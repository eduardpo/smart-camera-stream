
/**
 * @file        pir_mqtt.cpp
 * @brief       MQTT motion-alert control and application entry point.
 * @author      Eduard Polyakov <eduardpo@gmail.com>
 * @date        2026-08-27
 * @version     1.0.0
 * @copyright   (c) 2026 Eduard Polyakov. All rights reserved.
 *              Licensed under the MIT License.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * This module handles configuration, MQTT alerts, motion-session policy, and
 * PIR GPIO input. Camera capture, OpenCV/YOLO analytics, and RTSP publishing
 * are isolated in camera_analytics.cpp and share this module's public API.
 */

#include <iostream>
#include <fstream>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <unistd.h>
#include <getopt.h>
#include <mosquitto.h>
#include <gpiod.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <exception>
#include <functional>
#include <set>
#include <sstream>
#include <vector>
#include <sys/stat.h>
#include <gst/gst.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <opencv2/core.hpp>

#include "pir_mqtt.hpp"

#define MQTT_PAYLOAD_MOTION_DETECTED "motion_detected"
#define MQTT_PAYLOAD_MOTION_WAITING "motion_waiting"

namespace pir_mqtt {

std::atomic<bool> running{true};

std::string mqtt_host = "192.168.1.38";
int mqtt_port = 1883;
std::string mqtt_topic = "pir/motion";
int gpio_line = 12;
std::string log_file = "/var/log/pir_mqtt.log";

// Which detection backend to use: "pir" or "opencv".
std::string motion_source = "pir";

// Shared session-state tuning (used by both backends).
int motion_timeout_seconds = 10; // seconds of no motion before declaring the session over (motion_waiting)
int debounce_ms = 300; // ignore raw edges arriving faster than this after an accepted one
int confirm_count = 3; // raw edges required within confirm_window_ms before a burst is promoted to a real session -- rejects sparse noise (e.g. a jittery PIR) without touching hardware
int confirm_window_ms = 4000; // rolling window for the above
int max_session_seconds = 1800; // safety cap: force-end a session after this long regardless of continued retriggers (0 disables)
bool save_snapshot = false;
const std::string snapshot_directory = "/var/log/motion_snapshot";
int maximum_snapshots = 200;
std::mutex snapshot_mutex;

// OpenCV backend tuning -- continuously-adapting background model
// (cv::accumulateWeighted), not a single static reference frame. A static
// baseline proved too fragile against ordinary H.264 compression noise
// (verified: false positives persisted identically across every decoder
// tried -- avdec_h264 software, v4l2h264dec hardware, and back to
// avdec_h264 -- which ruled out decoder choice as the cause and pointed at
// the algorithm itself).
int cv_min_area = 5000;           // minimum contour area (pixels) to count a frame as containing motion
int cv_diff_threshold = 25;       // absdiff threshold (0-255) separating "changed" from "unchanged" pixels
int cv_sample_interval_ms = 300;  // how often to analyze a frame (no need to run full framerate for presence detection)
double cv_background_learning_rate = 0.02; // cv::accumulateWeighted alpha: how fast the background model adapts per
                                            // frame. Lower = slower adaptation, more resistant to being fooled by
                                            // codec/sensor noise but slower to absorb genuine lighting changes.
                                            // Higher = adapts faster but a person standing still long enough could
                                            // eventually get absorbed into the background. 0.01-0.05 is a reasonable
                                            // starting range.
int cv_warmup_frames = 15; // decode and discard this many frames before the background model starts accumulating --
                            // lets the H.264 decoder (and camera AGC/AWB) finish settling after pipeline start.
bool cv_debug_save_frames = false; // if true, saves the current frame + diff mask to /tmp/cv_debug_*.jpg every time
                                    // motion is detected, for visual inspection when tuning alone isn't enough to
                                    // diagnose a persistent false-positive problem.

// RTSP feed served by MediaMTX (always-on, independent of motion state).
std::string rtsp_host = "127.0.0.1";
int rtsp_port = 8554;
std::string rtsp_path = "cam";
int camera_width = 1280;
int camera_height = 720;
int camera_fps = 15;
std::string yolo_model_path = "/usr/share/pir-mqtt/yolo11n.onnx";
int yolo_inference_interval_ms = 750;
double yolo_confidence_threshold = 0.5;
double yolo_nms_threshold = 0.45;

// mosq is global so any function can reach it without threading a pointer
// through every call site.
struct mosquitto *mosq = nullptr;
std::mutex log_mutex;

void log_event(const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::ofstream log(log_file, std::ios_base::app);
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    log << std::put_time(std::localtime(&now), "%c") << ": " << msg << std::endl;
}

void handle_signal(int sig) {
    (void)sig;
    running = false;
}

void mqtt_reconnect_loop(struct mosquitto *m) {
    // Startup is gated on MQTT so confirmed motion is never silently lost.
    while (mosquitto_connect(m, mqtt_host.c_str(), mqtt_port, 60) != MOSQ_ERR_SUCCESS) {
        log_event("MQTT reconnect to host '" + mqtt_host + "' failed, retrying in 3s...");
        sleep(3);
    }
    log_event("Connected to MQTT broker at " + mqtt_host + ":" + std::to_string(mqtt_port));
}

// Keep MQTT socket servicing with the MQTT module; the camera loop calls this
// between frames to avoid introducing another network thread.
void pump_mqtt() {
    mosquitto_loop_misc(mosq);
    mosquitto_loop_write(mosq, 1);
    mosquitto_loop_read(mosq, 1);
}

// A single malformed numeric line (empty value, stray whitespace, non-digit
// content) used to take the whole daemon down via an uncaught std::stoi
// exception -- systemd would then hit its restart-rate-limit and give up
// entirely. This keeps the existing default and logs exactly which config
// key/line failed to parse instead.
int safe_stoi(const std::string &key, const std::string &value, int fallback) {
    try {
        return std::stoi(value);
    } catch (const std::exception &e) {
        log_event("Config: '" + key + "' has invalid value '" + value +
                   "', keeping default " + std::to_string(fallback));
        return fallback;
    }
}

double safe_stod(const std::string &key, const std::string &value, double fallback) {
    try {
        return std::stod(value);
    } catch (const std::exception &e) {
        log_event("Config: '" + key + "' has invalid value '" + value +
                   "', keeping default " + std::to_string(fallback));
        return fallback;
    }
}

void parse_config(const std::string &path) {
    std::ifstream file(path);
    std::string line;
    // Apply only recognized key=value entries; absent or malformed values keep
    // their compiled defaults.
    while (getline(file, line)) {
        if (line.find("gpio_line=") == 0) gpio_line = safe_stoi("gpio_line", line.substr(10), gpio_line);
        else if (line.find("mqtt_host=") == 0) mqtt_host = line.substr(10);
        else if (line.find("mqtt_port=") == 0) mqtt_port = safe_stoi("mqtt_port", line.substr(10), mqtt_port);
        else if (line.find("mqtt_topic=") == 0) mqtt_topic = line.substr(11);
        else if (line.find("log_file=") == 0) log_file = line.substr(9);
        else if (line.find("motion_source=") == 0) motion_source = line.substr(14);
        else if (line.find("motion_timeout=") == 0) motion_timeout_seconds = safe_stoi("motion_timeout", line.substr(15), motion_timeout_seconds);
        else if (line.find("debounce_ms=") == 0) debounce_ms = safe_stoi("debounce_ms", line.substr(12), debounce_ms);
        else if (line.find("confirm_count=") == 0) confirm_count = safe_stoi("confirm_count", line.substr(14), confirm_count);
        else if (line.find("confirm_window_ms=") == 0) confirm_window_ms = safe_stoi("confirm_window_ms", line.substr(18), confirm_window_ms);
        else if (line.find("max_session=") == 0) max_session_seconds = safe_stoi("max_session", line.substr(12), max_session_seconds);
        else if (line.find("save_snapshot=") == 0) save_snapshot = (line.substr(14) == "1");
        else if (line.find("maximum_snapshots=") == 0) {
            const int configured_limit =
                safe_stoi("maximum_snapshots", line.substr(18), maximum_snapshots);
            if (configured_limit > 0) {
                maximum_snapshots = configured_limit;
            } else {
                log_event("Config: 'maximum_snapshots' must be positive, keeping " +
                          std::to_string(maximum_snapshots));
            }
        }
        else if (line.find("cv_min_area=") == 0) cv_min_area = safe_stoi("cv_min_area", line.substr(12), cv_min_area);
        else if (line.find("cv_diff_threshold=") == 0) cv_diff_threshold = safe_stoi("cv_diff_threshold", line.substr(18), cv_diff_threshold);
        else if (line.find("cv_sample_interval_ms=") == 0) cv_sample_interval_ms = safe_stoi("cv_sample_interval_ms", line.substr(22), cv_sample_interval_ms);
        else if (line.find("cv_background_learning_rate=") == 0) cv_background_learning_rate = safe_stod("cv_background_learning_rate", line.substr(28), cv_background_learning_rate);
        else if (line.find("cv_warmup_frames=") == 0) cv_warmup_frames = safe_stoi("cv_warmup_frames", line.substr(17), cv_warmup_frames);
        else if (line.find("cv_debug_save_frames=") == 0) cv_debug_save_frames = (line.substr(21) == "1");
        else if (line.find("rtsp_host=") == 0) rtsp_host = line.substr(10);
        else if (line.find("rtsp_port=") == 0) rtsp_port = safe_stoi("rtsp_port", line.substr(10), rtsp_port);
        else if (line.find("rtsp_path=") == 0) rtsp_path = line.substr(10);
        else if (line.find("camera_width=") == 0) camera_width = safe_stoi("camera_width", line.substr(13), camera_width);
        else if (line.find("camera_height=") == 0) camera_height = safe_stoi("camera_height", line.substr(14), camera_height);
        else if (line.find("camera_fps=") == 0) camera_fps = safe_stoi("camera_fps", line.substr(11), camera_fps);
        else if (line.find("yolo_model=") == 0) yolo_model_path = line.substr(11);
        else if (line.find("yolo_inference_interval_ms=") == 0) yolo_inference_interval_ms = safe_stoi("yolo_inference_interval_ms", line.substr(27), yolo_inference_interval_ms);
        else if (line.find("yolo_confidence_threshold=") == 0) yolo_confidence_threshold = safe_stod("yolo_confidence_threshold", line.substr(26), yolo_confidence_threshold);
        else if (line.find("yolo_nms_threshold=") == 0) yolo_nms_threshold = safe_stod("yolo_nms_threshold", line.substr(19), yolo_nms_threshold);
    }
}

// Startup visibility only -- does not gate or launch anything. MediaMTX is a
// fully independent systemd service; this is just so the log makes it obvious
// at a glance whether it was actually reachable when pir_mqtt came up.
bool is_tcp_port_open(const std::string &host, int port, int timeout_ms = 500) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        close(sock);
        return false;
    }

    bool ok = (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    close(sock);
    return ok;
}

bool parse_snapshot_filename(const std::string &filename, int &year, int &month,
                             int &day, int &index) {
    char trailing;
    if (std::sscanf(filename.c_str(), "%4d-%2d-%2d_%d.jpg%c",
                    &year, &month, &day, &index, &trailing) != 4) {
        return false;
    }
    return year > 0 && month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
           index > 0;
}

struct TemporarySnapshot {
    std::string path;

    ~TemporarySnapshot() {
        if (!path.empty()) {
            unlink(path.c_str());
        }
    }
};

// Snapshot the published stream (the live pipeline owns the camera), index
// files by date, and prune oldest indexed snapshots after a successful write.
void capture_snapshot() {
    // Serialize captures so two motion events cannot choose the same index or
    // compete for the hardware decoder.
    std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex);

    if (mkdir(snapshot_directory.c_str(), 0755) != 0 && errno != EEXIST) {
        log_event("Unable to create snapshot directory " + snapshot_directory +
                  ": " + std::strerror(errno));
        return;
    }

    std::unique_ptr<DIR, decltype(&closedir)> directory(
        opendir(snapshot_directory.c_str()), &closedir);
    if (!directory) {
        log_event("Unable to read snapshot directory " + snapshot_directory +
                  ": " + std::strerror(errno));
        return;
    }

    const std::time_t now = std::time(nullptr);
    std::tm local_time{};
    if (!localtime_r(&now, &local_time)) {
        log_event("Unable to determine local date for snapshot filename");
        return;
    }
    char date[11];
    if (std::strftime(date, sizeof(date), "%Y-%m-%d", &local_time) == 0) {
        log_event("Unable to format local date for snapshot filename");
        return;
    }

    int next_index = 1;
    std::vector<std::string> existing_snapshots;
    for (;;) {
        errno = 0;
        dirent *entry = readdir(directory.get());
        if (!entry) {
            if (errno != 0) {
                log_event("Unable to scan snapshot directory " + snapshot_directory +
                          ": " + std::strerror(errno));
                return;
            }
            break;
        }

        const std::string filename(entry->d_name);
        int year, month, day, index;
        if (!parse_snapshot_filename(filename, year, month, day, index)) {
            continue;
        }
        if (filename.compare(0, 10, date) == 0 && index >= next_index) {
            next_index = index + 1;
        }
        struct stat info {};
        const std::string path = snapshot_directory + "/" + filename;
        if (lstat(path.c_str(), &info) != 0) {
            log_event("Unable to inspect snapshot " + path + ": " +
                      std::strerror(errno));
            return;
        }
        if (!S_ISREG(info.st_mode)) {
            continue;
        }
        if (info.st_size == 0) {
            if (unlink(path.c_str()) != 0) {
                log_event("Unable to remove empty snapshot " + path + ": " +
                          std::strerror(errno));
                return;
            }
            log_event("Removed empty snapshot left by an incomplete capture: " + path);
            continue;
        }
        existing_snapshots.push_back(filename);
    }

    std::ostringstream indexed_name;
    indexed_name << date << "_" << std::setw(3) << std::setfill('0')
                 << next_index << ".jpg";
    const std::string snapshot_path = snapshot_directory + "/" + indexed_name.str();
    TemporarySnapshot temporary_snapshot{snapshot_path + ".tmp"};

    const std::string rtsp_url = "rtsp://" + rtsp_host + ":" +
                                 std::to_string(rtsp_port) + "/" + rtsp_path;
    constexpr GstClockTime snapshot_timeout = 5 * GST_SECOND;
    GError *parse_error = nullptr;
    GstElement *raw_pipeline = gst_parse_launch(
        "rtspsrc name=snapshot_source protocols=tcp latency=200 "
        "! rtph264depay ! h264parse ! v4l2h264dec ! videoconvert "
        "! jpegenc snapshot=true ! filesink name=snapshot_sink",
        &parse_error);
    std::unique_ptr<GstElement, decltype(&gst_object_unref)> pipeline(
        raw_pipeline, &gst_object_unref);
    if (parse_error) {
        const std::string message = parse_error->message;
        g_error_free(parse_error);
        log_event("Snapshot pipeline parse failed: " + message);
        return;
    }
    if (!pipeline) {
        log_event("Snapshot pipeline could not be created");
        return;
    }

    // Keep resources and decoder state scoped to this one-shot snapshot.
    struct PipelineShutdown {
        GstElement *pipeline;
        ~PipelineShutdown() {
            gst_element_set_state(pipeline, GST_STATE_NULL);
        }
    } shutdown{pipeline.get()};

    std::unique_ptr<GstElement, decltype(&gst_object_unref)> source(
        gst_bin_get_by_name(GST_BIN(pipeline.get()), "snapshot_source"),
        &gst_object_unref);
    std::unique_ptr<GstElement, decltype(&gst_object_unref)> sink(
        gst_bin_get_by_name(GST_BIN(pipeline.get()), "snapshot_sink"),
        &gst_object_unref);
    if (!source || !sink) {
        log_event("Snapshot pipeline is missing its RTSP source or file sink");
        return;
    }
    g_object_set(source.get(), "location", rtsp_url.c_str(), nullptr);
    g_object_set(sink.get(), "location", temporary_snapshot.path.c_str(), nullptr);

    if (gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        log_event("Unable to start snapshot pipeline from " + rtsp_url);
        return;
    }

    std::unique_ptr<GstBus, decltype(&gst_object_unref)> bus(
        gst_element_get_bus(pipeline.get()), &gst_object_unref);
    if (!bus) {
        log_event("Unable to get snapshot pipeline message bus");
        return;
    }
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus.get(), snapshot_timeout,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (!message) {
        log_event("Snapshot timed out waiting for a frame from " + rtsp_url);
        return;
    }

    const bool reached_eos = GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (!reached_eos) {
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        log_event(std::string("Snapshot pipeline error: ") +
                  (error ? error->message : "unknown GStreamer error") +
                  (debug ? std::string(" (") + debug + ")" : std::string()));
        if (error) {
            g_error_free(error);
        }
        g_free(debug);
    }
    gst_message_unref(message);
    gst_element_set_state(pipeline.get(), GST_STATE_NULL);

    if (!reached_eos) {
        return;
    }

    struct stat snapshot_info {};
    if (stat(temporary_snapshot.path.c_str(), &snapshot_info) != 0 ||
        !S_ISREG(snapshot_info.st_mode) || snapshot_info.st_size == 0) {
        log_event("Snapshot pipeline reached EOS without writing image data");
        return;
    }

    if (rename(temporary_snapshot.path.c_str(), snapshot_path.c_str()) != 0) {
        log_event("Unable to publish snapshot " + snapshot_path + ": " +
                  std::strerror(errno));
        return;
    }
    temporary_snapshot.path.clear();
    log_event("Snapshot saved to " + snapshot_path);

    existing_snapshots.push_back(indexed_name.str());
    std::sort(existing_snapshots.begin(), existing_snapshots.end());
    while (existing_snapshots.size() > static_cast<size_t>(maximum_snapshots)) {
        const std::string oldest =
            snapshot_directory + "/" + existing_snapshots.front();
        if (unlink(oldest.c_str()) != 0) {
            log_event("Unable to remove old snapshot " + oldest + ": " +
                      std::strerror(errno));
            break;
        }
        existing_snapshots.erase(existing_snapshots.begin());
    }
}

// MotionSessionTracker centralizes debouncing, confirmation, and session
// timeouts so GPIO and frame-based motion follow the same alert policy.
MotionSessionTracker::MotionSessionTracker() {
    auto now = std::chrono::steady_clock::now();
    last_motion_ = now - std::chrono::seconds(motion_timeout_seconds);
    last_accepted_edge_ = last_motion_;
    session_start_ = last_motion_;
    burst_start_ = last_motion_;
}

void MotionSessionTracker::on_edge(const std::string &detected_labels) {
    auto now = std::chrono::steady_clock::now();
    auto since_last_accepted =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_accepted_edge_).count();
    if (since_last_accepted < debounce_ms) {
        return;
    }

    last_accepted_edge_ = now;
    last_motion_ = now;
    edge_count_++;

    if (in_session_) {
        if (add_labels(session_labels_, detected_labels)) {
            publish_motion_alert();
        }
        log_event("Motion continues (edge #" + std::to_string(edge_count_) + ")");
        return;
    }

    // Accumulate candidate edges and their labels only inside the confirmation
    // window; a sparse trigger must not start a motion session.
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - burst_start_).count() >
        confirm_window_ms) {
        burst_start_ = now;
        burst_count_ = 1;
        burst_labels_.clear();
    } else {
        burst_count_++;
    }
    add_labels(burst_labels_, detected_labels);

    if (burst_count_ >= confirm_count) {
        in_session_ = true;
        session_start_ = now;
        session_labels_ = burst_labels_;
        log_event("Motion session started (edge #" + std::to_string(edge_count_) +
                  ", confirmed after " + std::to_string(burst_count_) + " edges)");
        publish_motion_alert();
        if (save_snapshot) {
            std::thread(capture_snapshot).detach();
        }
    } else {
        log_event("Motion candidate (edge #" + std::to_string(edge_count_) + ", " +
                  std::to_string(burst_count_) + "/" + std::to_string(confirm_count) +
                  " within " + std::to_string(confirm_window_ms) + "ms -- not yet confirmed)");
    }
}

void MotionSessionTracker::update_labels(const std::string &detected_labels) {
    if (in_session_ && add_labels(session_labels_, detected_labels)) {
        publish_motion_alert();
    }
}

void MotionSessionTracker::tick() {
    auto now = std::chrono::steady_clock::now();

    if (in_session_ &&
        std::chrono::duration_cast<std::chrono::seconds>(now - last_motion_).count() >=
            motion_timeout_seconds) {
        in_session_ = false;
        burst_count_ = 0;
        session_labels_.clear();
        log_event("Motion session ended, now waiting");
        publish(MQTT_PAYLOAD_MOTION_WAITING);
        return;
    }

    if (in_session_ && max_session_seconds > 0 &&
        std::chrono::duration_cast<std::chrono::seconds>(now - session_start_).count() >=
            max_session_seconds) {
        in_session_ = false;
        burst_count_ = 0;
        session_labels_.clear();
        log_event("Motion session FORCE-ENDED after max_session=" +
                  std::to_string(max_session_seconds) +
                  "s of continuous retriggers -- check sensor/placement if this keeps happening");
        publish(MQTT_PAYLOAD_MOTION_WAITING);
    }
}

int MotionSessionTracker::edge_count() const {
    return edge_count_;
}

bool MotionSessionTracker::has_active_session() const {
    return in_session_;
}

void MotionSessionTracker::publish(const std::string &payload) {
    int rc = mosquitto_publish(mosq, nullptr, mqtt_topic.c_str(),
                               static_cast<int>(payload.size()), payload.data(), 0, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        log_event("MQTT publish failed: " + std::string(mosquitto_strerror(rc)));
    }
}

bool MotionSessionTracker::add_labels(std::set<std::string> &labels,
                                     const std::string &detected_labels) {
    bool changed = false;
    for (size_t start = 0; start < detected_labels.size();) {
        const size_t end = detected_labels.find(',', start);
        const std::string label = detected_labels.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!label.empty()) {
            changed = labels.insert(label).second || changed;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return changed;
}

void MotionSessionTracker::publish_motion_alert() {
    std::string payload = MQTT_PAYLOAD_MOTION_DETECTED;
    if (!session_labels_.empty()) {
        payload += ":";
        bool first_label = true;
        for (const auto &label : session_labels_) {
            if (!first_label) {
                payload += ",";
            }
            payload += label;
            first_label = false;
        }
    }
    publish(payload);
}

// ---------------------------------------------------------------------------
// Backend: PIR sensor on GPIO (libgpiod v2 API)
// ---------------------------------------------------------------------------
void run_pir_motion_detection(MotionSessionTracker &tracker) {
    // Own the GPIO request and event buffer for the lifetime of the reader
    // thread so every error or shutdown releases the kernel resources.
    auto chip = std::unique_ptr<gpiod_chip, decltype(&gpiod_chip_close)>(
        gpiod_chip_open("/dev/gpiochip0"), &gpiod_chip_close);
    if (!chip) {
        log_event("Failed to open /dev/gpiochip0");
        running = false;
        return;
    }

    auto settings = std::unique_ptr<gpiod_line_settings, decltype(&gpiod_line_settings_free)>(
        gpiod_line_settings_new(), &gpiod_line_settings_free);
    if (!settings ||
        gpiod_line_settings_set_direction(settings.get(), GPIOD_LINE_DIRECTION_INPUT) != 0 ||
        gpiod_line_settings_set_edge_detection(settings.get(), GPIOD_LINE_EDGE_BOTH) != 0) {
        log_event("Failed to configure GPIO" + std::to_string(gpio_line) + " line settings");
        running = false;
        return;
    }

    auto line_config = std::unique_ptr<gpiod_line_config, decltype(&gpiod_line_config_free)>(
        gpiod_line_config_new(), &gpiod_line_config_free);
    const unsigned int line_offset = static_cast<unsigned int>(gpio_line);
    if (!line_config ||
        gpiod_line_config_add_line_settings(line_config.get(), &line_offset, 1, settings.get()) != 0) {
        log_event("Failed to configure GPIO" + std::to_string(gpio_line) + " line");
        running = false;
        return;
    }

    auto request_config = std::unique_ptr<gpiod_request_config, decltype(&gpiod_request_config_free)>(
        gpiod_request_config_new(), &gpiod_request_config_free);
    if (!request_config) {
        log_event("Failed to create GPIO request configuration");
        running = false;
        return;
    }
    gpiod_request_config_set_consumer(request_config.get(), "pir_mqtt");

    auto request = std::unique_ptr<gpiod_line_request, decltype(&gpiod_line_request_release)>(
        gpiod_chip_request_lines(chip.get(), request_config.get(), line_config.get()),
        &gpiod_line_request_release);
    if (!request) {
        log_event("Failed to request GPIO" + std::to_string(gpio_line) + " for edge events");
        running = false;
        return;
    }

    auto event_buffer = std::unique_ptr<gpiod_edge_event_buffer, decltype(&gpiod_edge_event_buffer_free)>(
        gpiod_edge_event_buffer_new(1), &gpiod_edge_event_buffer_free);
    if (!event_buffer) {
        log_event("Failed to create GPIO edge event buffer");
        running = false;
        return;
    }

    log_event("PIR motion detection started on GPIO" + std::to_string(gpio_line));

    while (running) {
        int ret = gpiod_line_request_wait_edge_events(request.get(), 100000000);
        if (ret < 0) {
            log_event("Failed waiting for GPIO" + std::to_string(gpio_line) + " edge events");
            running = false;
            break;
        }

        if (ret == 1) {
            ret = gpiod_line_request_read_edge_events(request.get(), event_buffer.get(), 1);
            if (ret < 0) {
                log_event("Failed reading GPIO" + std::to_string(gpio_line) + " edge event");
                running = false;
                break;
            }

            if (ret > 0) {
                struct gpiod_edge_event *event =
                    gpiod_edge_event_buffer_get_event(event_buffer.get(), 0);
                if (event &&
                    gpiod_edge_event_get_event_type(event) == GPIOD_EDGE_EVENT_RISING_EDGE) {
                    tracker.on_edge();
                }
            }
        }

        tracker.tick();

    }


    log_event("PIR motion detection stopped. Total accepted edges: " + std::to_string(tracker.edge_count()));
}


} // namespace pir_mqtt

int main(int argc, char *argv[]) {
    using namespace pir_mqtt;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    std::string config_path;
    int opt;
    while ((opt = getopt(argc, argv, "g:h:p:t:c:l:")) != -1) {
        switch (opt) {
            case 'g': gpio_line = std::stoi(optarg); break;
            case 'h': mqtt_host = optarg; break;
            case 'p': mqtt_port = std::stoi(optarg); break;
            case 't': mqtt_topic = optarg; break;
            case 'c': config_path = optarg; break;
            case 'l': log_file = optarg; break;
            default:
                std::cerr << "Usage: " << argv[0] << " [-c config] [-g gpio] [-h host] [-p port] [-t topic] [-l logfile]\n";
                return 1;
        }
    }

    if (!config_path.empty()) {
        parse_config(config_path);
    }
    if (camera_width <= 0 || camera_height <= 0 || camera_fps <= 0 ||
        cv_sample_interval_ms <= 0 || yolo_inference_interval_ms <= 0 ||
        yolo_confidence_threshold < 0.0 || yolo_confidence_threshold > 1.0 ||
        yolo_nms_threshold < 0.0 || yolo_nms_threshold > 1.0) {
        std::cerr << "Invalid camera, motion, or YOLO settings in configuration\n";
        return 1;
    }

    log_event("Config loaded: motion_source=" + motion_source +
               " gpio_line=" + std::to_string(gpio_line) +
               " mqtt=" + mqtt_host + ":" + std::to_string(mqtt_port) +
               " topic=" + mqtt_topic +
               " motion_timeout_s=" + std::to_string(motion_timeout_seconds) +
               " debounce_ms=" + std::to_string(debounce_ms) +
               " confirm_count=" + std::to_string(confirm_count) +
               " confirm_window_ms=" + std::to_string(confirm_window_ms) +
               " max_session_s=" + std::to_string(max_session_seconds) +
               " save_snapshot=" + std::to_string(save_snapshot) +
               " cv_min_area=" + std::to_string(cv_min_area) +
               " cv_diff_threshold=" + std::to_string(cv_diff_threshold) +
               " cv_sample_interval_ms=" + std::to_string(cv_sample_interval_ms) +
               " cv_background_learning_rate=" + std::to_string(cv_background_learning_rate) +
               " cv_warmup_frames=" + std::to_string(cv_warmup_frames) +
               " cv_debug_save_frames=" + std::to_string(cv_debug_save_frames) +
               " camera=" + std::to_string(camera_width) + "x" + std::to_string(camera_height) +
               "@" + std::to_string(camera_fps) + "fps" +
               " yolo_model=" + yolo_model_path +
               " yolo_interval_ms=" + std::to_string(yolo_inference_interval_ms) +
               " rtsp_publish=" + rtsp_host + ":" + std::to_string(rtsp_port) + "/" + rtsp_path);

    if (is_tcp_port_open(rtsp_host, rtsp_port)) {
        log_event("MediaMTX RTSP server reachable at startup (" + rtsp_host + ":" +
                  std::to_string(rtsp_port) + ")");
    } else {
        log_event("WARNING: MediaMTX RTSP server not reachable at startup -- publishing will fail "
                  "until it is available (check 'systemctl status mediamtx').");
    }

    // MQTT is initialized here; run_camera_analytics owns camera and
    // publishing, while the tracker is shared with the selected motion source.
    gst_init(nullptr, nullptr);
    mosquitto_lib_init();
    mosq = mosquitto_new(nullptr, true, nullptr);
    if (!mosq) {
        log_event("Failed to create mosquitto client");
        return 1;
    }
    if (mosquitto_threaded_set(mosq, true) != MOSQ_ERR_SUCCESS) {
        log_event("Failed to enable thread-safe MQTT client operation");
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    int protocol_version = MQTT_PROTOCOL_V311;
    mosquitto_opts_set(mosq, MOSQ_OPT_PROTOCOL_VERSION, &protocol_version);
    mosquitto_reconnect_delay_set(mosq, 2, 10, false);

    mqtt_reconnect_loop(mosq);

    int exit_code = 0;
    try {
        MotionSessionTracker tracker;
        run_camera_analytics(tracker);
    } catch (const cv::Exception &error) {
        log_event("OpenCV initialization/runtime error: " + std::string(error.what()));
        running = false;
        exit_code = 1;
    } catch (const std::exception &error) {
        log_event("Camera analytics startup/runtime error: " + std::string(error.what()));
        running = false;
        exit_code = 1;
    }

    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    log_event("Shutdown complete.");
    return exit_code;
}