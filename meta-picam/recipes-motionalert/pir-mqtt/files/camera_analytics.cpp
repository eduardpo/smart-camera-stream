/**
 * @file camera_analytics.cpp
 * @brief Camera frame acquisition, motion/object analytics, and RTSP publishing.
 *
 * This module owns the camera and streaming pipeline. MQTT/session policy and
 * GPIO input remain in pir_mqtt.cpp; the shared declarations are in pir_mqtt.hpp.
 */
// SPDX-License-Identifier: MIT
#include "pir_mqtt.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <gst/video/video.h>
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>

namespace pir_mqtt {
// ---------------------------------------------------------------------------
// Camera capture, analytics, hardware encoding, and RTSP publishing
// ---------------------------------------------------------------------------

// Result of analyzing a single frame for motion.
struct FrameMotionResult {
    bool background_still_warming_up; // true while the running-average background model hasn't been established yet
    bool motion_detected;             // only meaningful when background_still_warming_up is false
    double largest_contour_area;
    cv::Mat debug_mask; // only populated when cv_debug_save_frames is true
};

struct ObjectDetection {
    cv::Rect box;
    float confidence;
    const char *label;
};

// Inference runs off the capture thread. A single replaceable pending frame
// keeps latency bounded when the model cannot keep up with camera FPS.
class ObjectInferenceWorker {
public:
    ObjectInferenceWorker(const std::string &model_path, int interval_ms,
                          double confidence_threshold, double nms_threshold)
        : interval_ms_(interval_ms),
          confidence_threshold_(static_cast<float>(confidence_threshold)),
          nms_threshold_(static_cast<float>(nms_threshold)) {
        net_ = cv::dnn::readNetFromONNX(model_path);
        if (net_.empty()) {
            throw std::runtime_error("OpenCV DNN loaded an empty YOLO model: " + model_path);
        }
        net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        worker_ = std::thread(&ObjectInferenceWorker::run, this);
        log_event("YOLO person/cat inference worker started (model=" + model_path +
                  ", interval_ms=" + std::to_string(interval_ms_) +
                  ", confidence_threshold=" + std::to_string(confidence_threshold_) + ")");
    }

    ~ObjectInferenceWorker() {
        stop();
    }

    ObjectInferenceWorker(const ObjectInferenceWorker &) = delete;
    ObjectInferenceWorker &operator=(const ObjectInferenceWorker &) = delete;

    void submit(const cv::Mat &frame) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (now - last_submit_ < std::chrono::milliseconds(interval_ms_)) {
            return;
        }
        pending_frame_ = frame.clone();
        frame_pending_ = true;
        last_submit_ = now;
        condition_.notify_one();
    }

    std::vector<ObjectDetection> detections() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return detections_;
    }

    std::string failure() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failure_;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    std::vector<ObjectDetection> infer(const cv::Mat &frame) {
        constexpr int input_size = 640;
        // Letterboxing preserves object proportions; reverse the same scale
        // and padding below to map model coordinates back to camera pixels.
        const float scale = std::min(static_cast<float>(input_size) / frame.cols,
                                     static_cast<float>(input_size) / frame.rows);
        const int resized_width = static_cast<int>(std::round(frame.cols * scale));
        const int resized_height = static_cast<int>(std::round(frame.rows * scale));
        const int pad_left = (input_size - resized_width) / 2;
        const int pad_top = (input_size - resized_height) / 2;
        const float scale_x = static_cast<float>(resized_width) / frame.cols;
        const float scale_y = static_cast<float>(resized_height) / frame.rows;

        cv::Mat resized;
        cv::resize(frame, resized, cv::Size(resized_width, resized_height));
        cv::Mat letterboxed(input_size, input_size, CV_8UC3, cv::Scalar(114, 114, 114));
        resized.copyTo(letterboxed(cv::Rect(pad_left, pad_top, resized_width, resized_height)));
        cv::Mat blob = cv::dnn::blobFromImage(
            letterboxed, 1.0 / 255.0, cv::Size(input_size, input_size),
            cv::Scalar(), true, false);
        net_.setInput(blob);
        cv::Mat output = net_.forward();

        if (output.dims != 3 || output.size[0] != 1 || output.size[1] != 84) {
            throw std::runtime_error("Unexpected YOLO11 ONNX output tensor shape");
        }

        const int prediction_count = output.size[2];
        cv::Mat predictions(84, prediction_count, CV_32F, output.ptr<float>());
        cv::Mat rows;
        cv::transpose(predictions, rows);

        constexpr int class_ids[] = {0, 15};
        constexpr const char *class_names[] = {"person", "cat"};
        const cv::Rect frame_bounds(0, 0, frame.cols, frame.rows);
        std::vector<ObjectDetection> result;

        for (size_t class_index = 0; class_index < 2; ++class_index) {
            std::vector<cv::Rect> boxes;
            std::vector<float> scores;
            boxes.reserve(prediction_count);
            scores.reserve(prediction_count);

            for (int i = 0; i < prediction_count; ++i) {
                const float *row = rows.ptr<float>(i);
                const float score = row[4 + class_ids[class_index]];
                if (score < confidence_threshold_) {
                    continue;
                }

                const float left = (row[0] - row[2] / 2.0f - pad_left) / scale_x;
                const float top = (row[1] - row[3] / 2.0f - pad_top) / scale_y;
                const int x = static_cast<int>(std::floor(left));
                const int y = static_cast<int>(std::floor(top));
                const int right = static_cast<int>(std::ceil(
                    (row[0] + row[2] / 2.0f - pad_left) / scale_x));
                const int bottom = static_cast<int>(std::ceil(
                    (row[1] + row[3] / 2.0f - pad_top) / scale_y));
                const cv::Rect box = cv::Rect(x, y, right - x, bottom - y) & frame_bounds;
                if (box.area() > 0) {
                    boxes.push_back(box);
                    scores.push_back(score);
                }
            }

            std::vector<int> selected;
            cv::dnn::NMSBoxes(boxes, scores, confidence_threshold_, nms_threshold_, selected);
            for (int index : selected) {
                result.push_back({boxes[index], scores[index], class_names[class_index]});
            }
        }

        return result;
    }

    void run() {
        std::vector<ObjectDetection> previous_detections;
        while (true) {
            cv::Mat frame;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || frame_pending_; });
                if (stopping_) {
                    return;
                }
                frame = std::move(pending_frame_);
                frame_pending_ = false;
            }

            try {
                auto current_detections = infer(frame);
                std::vector<ObjectDetection> confirmed_detections;
                // Require the same class and an overlapping box in consecutive
                // inferences to suppress one-off YOLO false positives.
                for (const auto &current : current_detections) {
                    for (const auto &previous : previous_detections) {
                        if (current.label != previous.label) {
                            continue;
                        }
                        const int intersection = (current.box & previous.box).area();
                        const int combined = current.box.area() + previous.box.area() - intersection;
                        if (combined > 0 &&
                            static_cast<float>(intersection) / combined >= 0.5f) {
                            confirmed_detections.push_back(current);
                            break;
                        }
                    }
                }
                previous_detections = std::move(current_detections);
                std::lock_guard<std::mutex> lock(mutex_);
                detections_ = std::move(confirmed_detections);
            } catch (const cv::Exception &error) {
                std::lock_guard<std::mutex> lock(mutex_);
                failure_ = "OpenCV YOLO inference failed: " + std::string(error.what());
                return;
            } catch (const std::exception &error) {
                std::lock_guard<std::mutex> lock(mutex_);
                failure_ = error.what();
                return;
            }
        }
    }

    cv::dnn::Net net_;
    int interval_ms_;
    float confidence_threshold_;
    float nms_threshold_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    cv::Mat pending_frame_;
    std::vector<ObjectDetection> detections_;
    std::string failure_;
    std::thread worker_;
    std::chrono::steady_clock::time_point last_submit_{};
    bool frame_pending_ = false;
    bool stopping_ = false;
};

// Compare blurred grayscale frames with a slowly adapting background. The
// largest thresholded contour, rather than total pixel noise, is the motion
// signal; the first frame only initializes the background model.
FrameMotionResult detect_motion_in_frame(const cv::Mat &current_frame, cv::Mat &background_accum,
                                          bool background_needs_init) {
    cv::Mat gray_frame, blurred_frame;
    cv::cvtColor(current_frame, gray_frame, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray_frame, blurred_frame, cv::Size(21, 21), 0);

    if (background_accum.empty() || background_needs_init) {
        // accumulateWeighted requires a float accumulator matching the
        // input's channel count; blurred_frame is 8-bit single-channel.
        blurred_frame.convertTo(background_accum, CV_32F);
        return {/*background_still_warming_up=*/true, /*motion_detected=*/false, /*largest_contour_area=*/0.0, cv::Mat()};
    }

    cv::accumulateWeighted(blurred_frame, background_accum, cv_background_learning_rate);

    cv::Mat background_8u;
    background_accum.convertTo(background_8u, CV_8U);

    cv::Mat frame_difference, threshold_frame;
    cv::absdiff(blurred_frame, background_8u, frame_difference);
    cv::threshold(frame_difference, threshold_frame, cv_diff_threshold, 255, cv::THRESH_BINARY);
    cv::dilate(threshold_frame, threshold_frame, cv::Mat(), cv::Point(-1, -1), 2);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(threshold_frame, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    double largest_area = 0;
    for (const auto &contour : contours) {
        double area = cv::contourArea(contour);
        if (area > largest_area) largest_area = area;
    }

    return {/*background_still_warming_up=*/false, largest_area >= cv_min_area, largest_area,
            cv_debug_save_frames ? threshold_frame : cv::Mat()};
}

struct GstObjectUnref {
    void operator()(GstElement *object) const {
        if (object) {
            gst_object_unref(object);
        }
    }
};

using GstElementPtr = std::unique_ptr<GstElement, GstObjectUnref>;

// Centralize parse errors and ownership for the two independently managed
// capture and publisher pipelines.
GstElementPtr parse_gstreamer_pipeline(const std::string &description, const std::string &name) {
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(description.c_str(), &error);
    if (error) {
        std::string message = error->message;
        g_error_free(error);
        if (pipeline) {
            gst_object_unref(pipeline);
        }
        throw std::runtime_error(name + " pipeline parse failed: " + message);
    }
    if (!pipeline) {
        throw std::runtime_error(name + " pipeline could not be created");
    }
    return GstElementPtr(pipeline);
}

bool log_pipeline_error(GstElement *pipeline, const std::string &name) {
    GstBus *bus = gst_element_get_bus(pipeline);
    if (!bus) {
        return false;
    }
    GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    if (!message) {
        return false;
    }

    GError *error = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    log_event(name + " pipeline error: " +
              (error ? error->message : "unknown GStreamer error") +
              (debug ? " (" + std::string(debug) + ")" : ""));
    if (error) {
        g_error_free(error);
    }
    g_free(debug);
    gst_message_unref(message);
    return true;
}

cv::Mat frame_from_sample(GstSample *sample) {
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGR) {
        throw std::runtime_error("Camera appsink returned invalid or non-BGR video caps");
    }

    GstVideoFrame mapped;
    if (!gst_video_frame_map(&mapped, &info, buffer, GST_MAP_READ)) {
        throw std::runtime_error("Unable to map camera frame from GStreamer");
    }
    // Clone before unmapping: the appsink buffer is returned to GStreamer
    // immediately after this helper returns.
    cv::Mat view(GST_VIDEO_FRAME_HEIGHT(&mapped), GST_VIDEO_FRAME_WIDTH(&mapped),
                 CV_8UC3, GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0),
                 static_cast<size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0)));
    cv::Mat frame = view.clone();
    gst_video_frame_unmap(&mapped);
    return frame;
}

GstFlowReturn push_annotated_frame(GstAppSrc *source, const cv::Mat &frame,
                                   guint64 frame_number) {
    if (frame.type() != CV_8UC3 || frame.cols % 2 != 0 || frame.rows % 2 != 0) {
        throw std::runtime_error("Annotated frame must be even-sized 8-bit BGR for I420 encoding");
    }
    cv::Mat i420_frame;
    cv::cvtColor(frame, i420_frame, cv::COLOR_BGR2YUV_I420);
    GstVideoInfo info;
    if (!gst_video_info_set_format(&info, GST_VIDEO_FORMAT_I420, frame.cols, frame.rows)) {
        throw std::runtime_error("Unable to describe I420 output frame");
    }
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, info.size, nullptr);
    if (!buffer) {
        throw std::runtime_error("Unable to allocate GStreamer output frame");
    }
    GstVideoFrame mapped;
    if (!gst_video_frame_map(&mapped, &info, buffer, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        throw std::runtime_error("Unable to map GStreamer output frame");
    }
    const size_t luma_size = static_cast<size_t>(frame.cols) * frame.rows;
    const size_t chroma_size = luma_size / 4;
    const size_t source_offsets[] = {0, luma_size, luma_size + chroma_size};
    const int plane_widths[] = {frame.cols, frame.cols / 2, frame.cols / 2};
    const int plane_heights[] = {frame.rows, frame.rows / 2, frame.rows / 2};
    // OpenCV's I420 image is tightly packed; copy rows into GStreamer's
    // negotiated plane strides rather than assuming both layouts match.
    for (guint plane = 0; plane < 3; ++plane) {
        auto *destination = static_cast<guint8 *>(
            GST_VIDEO_FRAME_PLANE_DATA(&mapped, plane));
        const gint destination_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, plane);
        const guint8 *source = i420_frame.data + source_offsets[plane];
        for (int row = 0; row < plane_heights[plane]; ++row) {
            std::memcpy(destination + static_cast<size_t>(row) * destination_stride,
                        source + static_cast<size_t>(row) * plane_widths[plane],
                        plane_widths[plane]);
        }
    }
    gst_video_frame_unmap(&mapped);
    GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(frame_number, GST_SECOND, camera_fps);
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, camera_fps);
    return gst_app_src_push_buffer(source, buffer);
}

void run_camera_analytics(MotionSessionTracker &tracker) {
    cv::setNumThreads(2);
    ObjectInferenceWorker inference(yolo_model_path, yolo_inference_interval_ms,
                                    yolo_confidence_threshold, yolo_nms_threshold);

    const std::string rtsp_url = "rtsp://" + rtsp_host + ":" +
                                 std::to_string(rtsp_port) + "/" + rtsp_path;
    const std::string capture_description =
        "libcamerasrc ! video/x-raw,width=" + std::to_string(camera_width) +
        ",height=" + std::to_string(camera_height) + ",framerate=" +
        std::to_string(camera_fps) +
        "/1 ! videoconvert ! video/x-raw,format=BGR "
        "! appsink name=camera_sink sync=false max-buffers=1 drop=true";
    const std::string encoder_description =
        "appsrc name=annotated_src ! "
        // Drop queued old frames instead of accumulating latency under load.
        "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
        "video/x-raw,format=I420 ! v4l2h264enc name=hardware_encoder ! "
        "video/x-h264,profile=(string)baseline,level=(string)4 ! "
        "h264parse config-interval=-1 ! rtspclientsink name=rtsp_sink";

    log_event("Starting libcamera capture at " + std::to_string(camera_width) + "x" +
              std::to_string(camera_height) + "@" + std::to_string(camera_fps) +
              "fps; publishing annotated H.264 to " + rtsp_url);

    GstElementPtr capture_pipeline = parse_gstreamer_pipeline(capture_description, "Camera capture");
    GstElementPtr encoder_pipeline = parse_gstreamer_pipeline(encoder_description, "Encoder/publisher");
    GstElementPtr camera_sink(gst_bin_get_by_name(GST_BIN(capture_pipeline.get()), "camera_sink"));
    GstElementPtr annotated_src(gst_bin_get_by_name(GST_BIN(encoder_pipeline.get()), "annotated_src"));
    GstElementPtr rtsp_sink(gst_bin_get_by_name(GST_BIN(encoder_pipeline.get()), "rtsp_sink"));
    GstElementPtr encoder(gst_bin_get_by_name(GST_BIN(encoder_pipeline.get()), "hardware_encoder"));
    if (!camera_sink || !annotated_src || !rtsp_sink || !encoder) {
        throw std::runtime_error("Required GStreamer elements were not created; check libcamerasrc, "
                                 "v4l2h264enc, and rtspclientsink plugin packages");
    }

    // The encoder requires explicit I420 dimensions and timing from appsrc.
    GstCaps *caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "I420",
        "width", G_TYPE_INT, camera_width,
        "height", G_TYPE_INT, camera_height,
        "framerate", GST_TYPE_FRACTION, camera_fps, 1,
        nullptr);
    gst_app_src_set_caps(GST_APP_SRC(annotated_src.get()), caps);
    gst_caps_unref(caps);
    g_object_set(annotated_src.get(),
                 "is-live", TRUE,
                 "format", GST_FORMAT_TIME,
                 "block", FALSE,
                 nullptr);
    g_object_set(rtsp_sink.get(), "location", rtsp_url.c_str(), nullptr);
    GParamSpec *protocols_property =
        g_object_class_find_property(G_OBJECT_GET_CLASS(rtsp_sink.get()), "protocols");
    if (!protocols_property || !G_TYPE_IS_FLAGS(G_PARAM_SPEC_VALUE_TYPE(protocols_property))) {
        throw std::runtime_error("rtspclientsink has no transport protocol setting");
    }
    gst_util_set_object_arg(G_OBJECT(rtsp_sink.get()), "protocols", "tcp");
    GValue protocols = G_VALUE_INIT;
    g_value_init(&protocols, G_PARAM_SPEC_VALUE_TYPE(protocols_property));
    g_object_get_property(G_OBJECT(rtsp_sink.get()), "protocols", &protocols);
    const bool tcp_enabled =
        (g_value_get_flags(&protocols) & GST_RTSP_LOWER_TRANS_TCP) != 0;
    g_value_unset(&protocols);
    if (!tcp_enabled) {
        throw std::runtime_error("rtspclientsink did not accept the requested TCP transport");
    }
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(encoder.get()), "bitrate")) {
        g_object_set(encoder.get(), "bitrate", 1500000U, nullptr);
    }

    if (gst_element_set_state(encoder_pipeline.get(), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        log_pipeline_error(encoder_pipeline.get(), "Encoder/publisher");
        throw std::runtime_error("Unable to start the hardware encoder and RTSP publisher");
    }
    if (gst_element_set_state(capture_pipeline.get(), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        log_pipeline_error(capture_pipeline.get(), "Camera capture");
        throw std::runtime_error("Unable to start libcamerasrc");
    }

    std::thread pir_thread;
    if (motion_source != "opencv") {
        if (motion_source != "pir") {
            log_event("Config: unrecognized motion_source='" + motion_source + "', defaulting to 'pir'");
        }
        pir_thread = std::thread(run_pir_motion_detection, std::ref(tracker));
    }

    log_event("Camera analytics pipeline running: OpenCV motion_source=" + motion_source +
              ", YOLO model=" + yolo_model_path);
    cv::Mat background_accum;
    auto last_motion_sample = std::chrono::steady_clock::now() -
                              std::chrono::milliseconds(cv_sample_interval_ms);
    guint64 frame_number = 0;
    int warmup_frames_seen = 0;
    bool motion_background_initialized = false;
    bool pipeline_ok = true;

    while (running) {
        if (log_pipeline_error(capture_pipeline.get(), "Camera capture") ||
            log_pipeline_error(encoder_pipeline.get(), "Encoder/publisher")) {
            pipeline_ok = false;
            break;
        }
        const std::string inference_failure = inference.failure();
        if (!inference_failure.empty()) {
            log_event(inference_failure);
            pipeline_ok = false;
            break;
        }

        GstSample *sample = gst_app_sink_try_pull_sample(
            GST_APP_SINK(camera_sink.get()), 100 * GST_MSECOND);
        if (!sample) {
            if (motion_source == "opencv") {
                tracker.tick();
            }
            pump_mqtt();
            continue;
        }

        cv::Mat frame;
        try {
            frame = frame_from_sample(sample);
        } catch (const std::exception &error) {
            gst_sample_unref(sample);
            log_event(error.what());
            pipeline_ok = false;
            break;
        }
        gst_sample_unref(sample);
        if (frame.empty() || frame.cols != camera_width || frame.rows != camera_height) {
            log_event("Camera produced a frame with unexpected dimensions");
            pipeline_ok = false;
            break;
        }

        // Keep capture/publishing at camera rate while inference samples at its
        // configured slower interval.
        inference.submit(frame);
        const auto detections = inference.detections();
        std::string detection_labels;
        for (const auto &detection : detections) {
            if (!detection_labels.empty()) {
                detection_labels += ",";
            }
            detection_labels += detection.label;
        }
        if (motion_source == "opencv") {
            tracker.update_labels(detection_labels);
            ++warmup_frames_seen;
            const auto now = std::chrono::steady_clock::now();
            if (warmup_frames_seen > cv_warmup_frames &&
                now - last_motion_sample >= std::chrono::milliseconds(cv_sample_interval_ms)) {
                last_motion_sample = now;
                const FrameMotionResult result =
                    detect_motion_in_frame(frame, background_accum, false);
                if (result.background_still_warming_up) {
                    if (!motion_background_initialized) {
                        log_event("OpenCV motion background model initialized");
                        motion_background_initialized = true;
                    }
                } else if (result.motion_detected) {
                    if (cv_debug_save_frames) {
                        cv::imwrite("/tmp/cv_debug_frame.jpg", frame);
                        cv::imwrite("/tmp/cv_debug_mask.jpg", result.debug_mask);
                    }
                    tracker.on_edge(detection_labels);
                }
            }
            tracker.tick();
        }

        for (const auto &detection : detections) {
            cv::rectangle(frame, detection.box, cv::Scalar(0, 255, 0), 2);
            char label[48];
            std::snprintf(label, sizeof(label), "%s %.2f",
                          detection.label, detection.confidence);
            const int label_y = std::max(18, detection.box.y - 6);
            cv::putText(frame, label, cv::Point(detection.box.x, label_y),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
        }

        GstFlowReturn flow = GST_FLOW_ERROR;
        try {
            flow = push_annotated_frame(
                GST_APP_SRC(annotated_src.get()), frame, frame_number++);
        } catch (const std::exception &error) {
            log_event(error.what());
            pipeline_ok = false;
            break;
        }
        if (flow != GST_FLOW_OK) {
            log_event("Unable to push annotated frame to the hardware encoder (flow=" +
                      std::to_string(flow) + ")");
            pipeline_ok = false;
            break;
        }
        pump_mqtt();
    }

    running = false;
    if (pir_thread.joinable()) {
        pir_thread.join();
    }
    gst_element_set_state(capture_pipeline.get(), GST_STATE_NULL);
    gst_element_set_state(encoder_pipeline.get(), GST_STATE_NULL);
    log_event("Camera analytics pipeline stopped");
    if (!pipeline_ok) {
        throw std::runtime_error("Camera analytics pipeline stopped after an error");
    }
}

} // namespace pir_mqtt
