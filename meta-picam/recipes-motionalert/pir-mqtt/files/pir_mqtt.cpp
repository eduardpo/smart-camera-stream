
/**
 * @file        pir_mqtt.cpp
 * @brief       Motion detection (PIR sensor or OpenCV background subtraction) with
 *              MQTT alerts. MediaMTX drives the camera and RTSP feed independently.
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
 * --- Architecture note ---
 * MediaMTX (a separate, always-on systemd service -- see mediamtx.yml) drives
 * the camera itself via its built-in `rpiCamera` source. The RTSP feed at
 * rtsp://<rtsp_host>:<rtsp_port>/<rtsp_path> is continuously live, independent
 * of motion state -- this program never manages MediaMTX or a GStreamer
 * push pipeline. It selects ONE of two motion-detection backends via the
 * `motion_source` config key:
 *   - "pir"    (default): watches a PIR sensor on GPIO.
 *   - "opencv": pulls frames from the already-live RTSP feed (never opens
 *               the camera device directly -- MediaMTX holds it exclusively)
 *               and classifies motion via a continuously-adapting background
 *               model, as a replacement for an unreliable/noisy PIR sensor.
 * Both backends feed the same MotionSessionTracker, so debounce, burst
 * confirmation, session timeout, and the max-session safety cap all behave
 * identically regardless of which one is active -- only how a raw "edge" is
 * detected differs between them.
 */

#include <iostream>
#include <fstream>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <getopt.h>
#include <mosquitto.h>
#include <gpiod.h>
#include <chrono>
#include <ctime>
#include <iomanip> // for std::put_time
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <opencv2/opencv.hpp>

#define MQTT_PAYLOAD_MOTION_DETECTED "motion_detected"
#define MQTT_PAYLOAD_MOTION_WAITING "motion_waiting"

static volatile bool running = true;

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
std::string snapshot_path = "/var/log/motion_snapshot.jpg";

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

// Which H.264 decoder element the OpenCV backend's GStreamer pipeline uses:
// "sw" (avdec_h264, software decode via libav -- mature, well-tested, but
// uses CPU continuously for the life of the process) or "hw" (v4l2h264dec,
// offloads to the RPi4's dedicated hardware decode block via bcm2835-codec
// -- much lower CPU/thermal cost, confirmed present on this board via
// dmesg, but a less mature GStreamer integration path). Default is "sw"
// since that's the one actually validated pixel-correct by testing; switch
// to "hw" once you've confirmed (e.g. via cv_debug_save_frames) that it
// doesn't introduce decode artifacts on your specific board/firmware.
std::string opencv_dec = "sw";

// RTSP feed served by MediaMTX (always-on, independent of motion state).
std::string rtsp_host = "127.0.0.1";
int rtsp_port = 8554;
std::string rtsp_path = "cam";

// mosq is global so any function can reach it without threading a pointer
// through every call site.
struct mosquitto *mosq = nullptr;

void log_event(const std::string& msg) {
    std::ofstream log(log_file, std::ios_base::app);
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    log << std::put_time(std::localtime(&now), "%c") << ": " << msg << std::endl;
}

void handle_signal(int sig) {
    log_event("Exiting on signal " + std::to_string(sig));
    running = false;
}

void mqtt_reconnect_loop(struct mosquitto *m) {
    while (mosquitto_connect(m, mqtt_host.c_str(), mqtt_port, 60) != MOSQ_ERR_SUCCESS) {
        log_event("MQTT reconnect to host '" + mqtt_host + "' failed, retrying in 3s...");
        sleep(3);
    }
    log_event("Connected to MQTT broker at " + mqtt_host + ":" + std::to_string(mqtt_port));
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
        else if (line.find("snapshot_path=") == 0) snapshot_path = line.substr(14);
        else if (line.find("cv_min_area=") == 0) cv_min_area = safe_stoi("cv_min_area", line.substr(12), cv_min_area);
        else if (line.find("cv_diff_threshold=") == 0) cv_diff_threshold = safe_stoi("cv_diff_threshold", line.substr(18), cv_diff_threshold);
        else if (line.find("cv_sample_interval_ms=") == 0) cv_sample_interval_ms = safe_stoi("cv_sample_interval_ms", line.substr(22), cv_sample_interval_ms);
        else if (line.find("cv_background_learning_rate=") == 0) cv_background_learning_rate = safe_stod("cv_background_learning_rate", line.substr(28), cv_background_learning_rate);
        else if (line.find("cv_warmup_frames=") == 0) cv_warmup_frames = safe_stoi("cv_warmup_frames", line.substr(17), cv_warmup_frames);
        else if (line.find("cv_debug_save_frames=") == 0) cv_debug_save_frames = (line.substr(21) == "1");
        else if (line.find("opencv_dec=") == 0) opencv_dec = line.substr(11);
        else if (line.find("rtsp_host=") == 0) rtsp_host = line.substr(10);
        else if (line.find("rtsp_port=") == 0) rtsp_port = safe_stoi("rtsp_port", line.substr(10), rtsp_port);
        else if (line.find("rtsp_path=") == 0) rtsp_path = line.substr(10);
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

// Grabs a single still frame from the already-live RTSP feed and writes it
// to snapshot_path. Pulls over RTSP rather than opening the camera device
// directly -- MediaMTX holds it exclusively via rpiCamera, so any direct
// device access here would simply fail to open (or worse, contend with
// MediaMTX or the OpenCV backend for it). Always uses software decode
// (avdec_h264): a rare, one-shot single-frame grab gets no meaningful
// benefit from hardware decode, only the (now largely ruled-out, but still
// not worth risking here) correctness uncertainty of the newer V4L2 path.
// jpegenc's snapshot=true property makes it emit EOS after the first
// encoded buffer, so the pipeline finishes cleanly and gst-launch exits on
// its own; the outer `timeout` is a safety net in case the RTSP handshake
// itself hangs.
void capture_snapshot() {
    std::string rtsp_url = "rtsp://" + rtsp_host + ":" + std::to_string(rtsp_port) + "/" + rtsp_path;
    std::string cmd =
        "timeout 5 gst-launch-1.0 -e rtspsrc location=" + rtsp_url + " protocols=tcp latency=200 "
        "! rtph264depay ! h264parse ! avdec_h264 ! videoconvert "
        "! jpegenc snapshot=true ! filesink location=" + snapshot_path +
        " > /dev/null 2>&1";

    int rc = system(cmd.c_str());
    if (rc == 0) {
        log_event("Snapshot saved to " + snapshot_path);
    } else {
        log_event("Snapshot capture failed (exit code " + std::to_string(rc) + ") pulling from " + rtsp_url);
    }
}

// ---------------------------------------------------------------------------
// Shared motion-session state machine
// ---------------------------------------------------------------------------
//
// Either backend just calls on_edge() whenever it detects a raw trigger (a
// GPIO rising edge, or a frame classified as containing motion), and tick()
// once per loop iteration regardless of whether an edge occurred. Handles:
//   - debounce_ms: reject edges arriving too soon after the last accepted one
//     (electrical/mechanical noise, or back-to-back frame hits from a single
//     real motion instant).
//   - confirm_count/confirm_window_ms: a burst of edges only gets promoted to
//     a real session once enough of them land within a short window -- a
//     lone spurious trigger never starts a session at all.
//   - motion_timeout_seconds: session ends once genuinely quiet.
//   - max_session_seconds: force-ends a session that's run suspiciously long
//     regardless of continued retriggers (stuck sensor / persistent false
//     source safety net). Evaluated in tick(), so it fires even if on_edge()
//     is being called continuously and the "gone quiet" branch never runs.
class MotionSessionTracker {
public:
    MotionSessionTracker() {
        auto now = std::chrono::steady_clock::now();
        last_motion_ = now - std::chrono::seconds(motion_timeout_seconds);
        last_accepted_edge_ = last_motion_;
        session_start_ = last_motion_;
        burst_start_ = last_motion_;
    }

    void on_edge() {
        auto now = std::chrono::steady_clock::now();
        auto since_last_accepted = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_accepted_edge_).count();
        if (since_last_accepted < debounce_ms) {
            return; // bounce/noise -- ignore entirely, don't even count it
        }

        last_accepted_edge_ = now;
        last_motion_ = now;
        edge_count_++;

        if (in_session_) {
            log_event("Motion continues (edge #" + std::to_string(edge_count_) + ")");
            return;
        }

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - burst_start_).count() > confirm_window_ms) {
            burst_start_ = now;
            burst_count_ = 1;
        } else {
            burst_count_++;
        }

        if (burst_count_ >= confirm_count) {
            in_session_ = true;
            session_start_ = now;
            log_event("Motion session started (edge #" + std::to_string(edge_count_) +
                       ", confirmed after " + std::to_string(burst_count_) + " edges)");
            publish(MQTT_PAYLOAD_MOTION_DETECTED);
            if (save_snapshot) std::thread(capture_snapshot).detach();
        } else {
            log_event("Motion candidate (edge #" + std::to_string(edge_count_) + ", " +
                       std::to_string(burst_count_) + "/" + std::to_string(confirm_count) +
                       " within " + std::to_string(confirm_window_ms) + "ms -- not yet confirmed)");
        }
    }

    void tick() {
        auto now = std::chrono::steady_clock::now();

        if (in_session_ &&
            std::chrono::duration_cast<std::chrono::seconds>(now - last_motion_).count() >= motion_timeout_seconds) {
            in_session_ = false;
            burst_count_ = 0;
            log_event("Motion session ended, now waiting");
            publish(MQTT_PAYLOAD_MOTION_WAITING);
            return;
        }

        if (in_session_ && max_session_seconds > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - session_start_).count() >= max_session_seconds) {
            in_session_ = false;
            burst_count_ = 0;
            log_event("Motion session FORCE-ENDED after max_session=" + std::to_string(max_session_seconds) +
                       "s of continuous retriggers -- check sensor/placement if this keeps happening");
            publish(MQTT_PAYLOAD_MOTION_WAITING);
        }
    }

    int edge_count() const { return edge_count_; }
    bool has_active_session() const { return in_session_; }

private:
    void publish(const char *payload) {
        int rc = mosquitto_publish(mosq, nullptr, mqtt_topic.c_str(), strlen(payload), payload, 0, false);
        if (rc != MOSQ_ERR_SUCCESS)
            log_event("MQTT publish failed: " + std::string(mosquitto_strerror(rc)));
    }

    std::chrono::steady_clock::time_point last_motion_, last_accepted_edge_, session_start_, burst_start_;
    bool in_session_ = false;
    int edge_count_ = 0;
    int burst_count_ = 0;
};

// ---------------------------------------------------------------------------
// Backend: PIR sensor on GPIO (libgpiod v1.x API -- kirkstone ships 1.6.3)
// ---------------------------------------------------------------------------
void run_pir_motion_detection() {
    struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
    if (!chip) {
        log_event("Failed to open /dev/gpiochip0");
        running = false;
        return;
    }

    struct gpiod_line *line = gpiod_chip_get_line(chip, gpio_line);
    if (!line) {
        log_event("Failed to get GPIO" + std::to_string(gpio_line) + " line handle");
        gpiod_chip_close(chip);
        running = false;
        return;
    }

    if (gpiod_line_request_both_edges_events(line, "pir_mqtt") != 0) {
        log_event("Failed to request GPIO" + std::to_string(gpio_line) + " for edge events");
        gpiod_chip_close(chip);
        running = false;
        return;
    }

    struct gpiod_line_event event;
    log_event("PIR motion detection started on GPIO" + std::to_string(gpio_line));

    MotionSessionTracker tracker;

    while (running) {
        struct timespec timeout = {0, 100000000}; // 100 ms
        int ret = gpiod_line_event_wait(line, &timeout);

        if (ret == 1) {
            if (gpiod_line_event_read(line, &event) == 0 &&
                event.event_type == GPIOD_LINE_EVENT_RISING_EDGE) {
                tracker.on_edge();
            }
        } else if (ret < 0) {
            log_event("GPIO edge wait error");
        }

        tracker.tick();

        mosquitto_loop_misc(mosq);
        mosquitto_loop_write(mosq, 1);
        mosquitto_loop_read(mosq, 1);
    }

    gpiod_line_release(line);
    gpiod_chip_close(chip);
    log_event("PIR motion detection stopped. Total accepted edges: " + std::to_string(tracker.edge_count()));
}

// ---------------------------------------------------------------------------
// Backend: OpenCV motion detection over RTSP (adaptive background model)
// ---------------------------------------------------------------------------
// Pulls frames from MediaMTX's own RTSP feed (never the camera device
// directly -- see the note on capture_snapshot). A frame counts as a raw
// "edge" when the largest contour against the running background estimate
// exceeds cv_min_area; that feeds the exact same MotionSessionTracker the
// PIR backend uses.

// Result of analyzing a single frame for motion.
struct FrameMotionResult {
    bool background_still_warming_up; // true while the running-average background model hasn't been established yet
    bool motion_detected;             // only meaningful when background_still_warming_up is false
    double largest_contour_area;
    cv::Mat debug_mask; // only populated when cv_debug_save_frames is true
};

// Runs the motion algorithm on one frame using a CONTINUOUSLY-ADAPTING
// background model (cv::accumulateWeighted), not a single static reference:
//   1. Grayscale + blur -- reduces sensitivity to per-pixel sensor noise and
//      small lighting flicker that would otherwise show up as spurious tiny
//      differences.
//   2. accumulateWeighted blends this frame into the running background at
//      rate cv_background_learning_rate -- the background slowly drifts to
//      track real lighting changes, but a genuine fast-moving subject still
//      stands out clearly against it in any single frame.
//   3. absdiff the CURRENT frame (not the background estimate itself)
//      against that background -- this is the key difference from a static
//      baseline: the reference is a smoothed average that's naturally more
//      resistant to single-frame codec/sensor noise, since that noise isn't
//      correlated frame-to-frame and gets averaged out over time.
//   4. Threshold to a clean mask, dilate to merge nearby changed pixels into
//      solid blobs, then find contours -- the largest contour's area is a
//      much stronger real-motion signal than total changed-pixel count.
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

// Builds the GStreamer pipeline string for the OpenCV backend's RTSP pull,
// choosing the decoder element based on opencv_dec:
//   "sw" -> avdec_h264 (software, mature/well-tested, higher CPU use)
//   "hw" -> v4l2h264dec (hardware-offloaded via bcm2835-codec, lower CPU,
//           newer/less battle-tested GStreamer integration path)
// Ends in `appsink` (what OpenCV's GStreamer backend reads from), matching
// the same rtspsrc/depay/parse chain capture_snapshot() already uses.
std::string build_opencv_gst_pipeline(const std::string &rtsp_url) {
    std::string common_head =
        "rtspsrc location=" + rtsp_url + " protocols=tcp latency=150 "
        "! rtph264depay ! h264parse ! ";
    std::string common_tail =
        " ! videoscale ! video/x-raw, width=640, height=480 "
        "! videoconvert ! video/x-raw, format=BGR "
        "! appsink drop=true max-buffers=1 sync=false";

    if (opencv_dec == "hw") {
        return common_head + "v4l2h264dec qos=false" + common_tail;
    }

    if (opencv_dec != "sw") {
        log_event("Config: unrecognized opencv_dec='" + opencv_dec + "', defaulting to 'sw'");
    }
    return common_head + "avdec_h264" + common_tail;
}

void run_opencv_motion_detection() {
    std::string rtsp_url = "rtsp://" + rtsp_host + ":" + std::to_string(rtsp_port) + "/" + rtsp_path;
    std::string gst_pipeline = build_opencv_gst_pipeline(rtsp_url);

    log_event("OpenCV motion detection using GStreamer pipeline : gst-launch-1.0 " + gst_pipeline);
    
    cv::VideoCapture cap(gst_pipeline, cv::CAP_GSTREAMER);
    if (!cap.isOpened()) {
        log_event("Failed to open RTSP feed for OpenCV motion detection via GStreamer (opencv_dec=" +
                   opencv_dec + "): " + rtsp_url +
                   " -- is MediaMTX running? (systemctl status mediamtx) Also confirm the opencv "
                   "build has PACKAGECONFIG 'gstreamer' enabled, and (if opencv_dec=hw) that "
                   "v4l2h264dec is registered (gst-inspect-1.0 | grep v4l2h264).");
        running = false;
        return;
    }

    log_event("OpenCV motion detection started, pulling from " + rtsp_url + " via GStreamer"
               " (opencv_dec=" + opencv_dec +
               " min_area=" + std::to_string(cv_min_area) +
               " diff_threshold=" + std::to_string(cv_diff_threshold) +
               " sample_interval_ms=" + std::to_string(cv_sample_interval_ms) +
               " background_learning_rate=" + std::to_string(cv_background_learning_rate) +
               " warmup_frames=" + std::to_string(cv_warmup_frames) + ")");

    MotionSessionTracker tracker;
    cv::Mat current_frame, background_accum;

    // Throttle how often we actually decode+analyze a frame -- presence
    // detection doesn't need full framerate, and skipping most frames saves
    // real CPU on hardware we already know has thermal headroom concerns.
    auto last_sample_time = std::chrono::steady_clock::now() - std::chrono::milliseconds(cv_sample_interval_ms);
    int consecutive_read_failures = 0;
    // Counts real decoded frames seen so far, purely to gate the warm-up
    // discard period below -- separate from motion_count/edge_count, which
    // only count frames that actually triggered the session tracker.
    int frames_decoded = 0;

    while (running) {
        auto now = std::chrono::steady_clock::now();
        bool time_to_sample = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sample_time).count()
                               >= cv_sample_interval_ms;

        if (!time_to_sample) {
            // Still drain the pipeline's internal buffer on skipped
            // iterations, so we don't fall behind the live stream even
            // though we're not decoding/analyzing every frame.
            cap.grab();
            tracker.tick();
            mosquitto_loop_misc(mosq);
            mosquitto_loop_write(mosq, 1);
            mosquitto_loop_read(mosq, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        last_sample_time = now;

        if (!cap.read(current_frame) || current_frame.empty()) {
            consecutive_read_failures++;
            // Only log the first failure and then periodically, not every
            // single one -- a genuinely down feed would otherwise flood the
            // log exactly like the motion_waiting/motion_detected bugs did.
            if (consecutive_read_failures == 1 || consecutive_read_failures % 50 == 0) {
                log_event("OpenCV: failed to read frame from RTSP feed (x" +
                           std::to_string(consecutive_read_failures) + "), retrying...");
            }
            tracker.tick();
            mosquitto_loop_misc(mosq);
            mosquitto_loop_write(mosq, 1);
            mosquitto_loop_read(mosq, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        consecutive_read_failures = 0;
        frames_decoded++;

        // Discard frames until the decoder has had a chance to settle past
        // its own startup warm-up -- letting the background model start
        // accumulating from a transient/garbled early frame would skew it
        // for a while (though, unlike the old static-baseline approach,
        // it would eventually self-correct as more frames blend in).
        if (frames_decoded <= cv_warmup_frames) {
            if (frames_decoded == cv_warmup_frames) {
                log_event("OpenCV: warm-up complete (" + std::to_string(cv_warmup_frames) +
                           " frames discarded), background model will start accumulating from the next frame");
            }
            tracker.tick();
            mosquitto_loop_misc(mosq);
            mosquitto_loop_write(mosq, 1);
            mosquitto_loop_read(mosq, 1);
            continue;
        }

        FrameMotionResult result = detect_motion_in_frame(current_frame, background_accum, /*background_needs_init=*/false);

        if (result.background_still_warming_up) {
            log_event("OpenCV: background model initialized");
        } else if (result.motion_detected) {
            if (cv_debug_save_frames) {
                cv::imwrite("/tmp/cv_debug_frame.jpg", current_frame);
                cv::imwrite("/tmp/cv_debug_mask.jpg", result.debug_mask);
                log_event("OpenCV: debug frame+mask saved to /tmp/cv_debug_frame.jpg, /tmp/cv_debug_mask.jpg "
                           "(largest_area=" + std::to_string(result.largest_contour_area) + ")");
            }
            tracker.on_edge();
        }

        tracker.tick();

        mosquitto_loop_misc(mosq);
        mosquitto_loop_write(mosq, 1);
        mosquitto_loop_read(mosq, 1);
    }

    cap.release();
    log_event("OpenCV motion detection stopped. Total accepted edges: " + std::to_string(tracker.edge_count()));
}

int main(int argc, char *argv[]) {
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

    if (!config_path.empty()) parse_config(config_path);
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
               " opencv_dec=" + opencv_dec +
               " cv_min_area=" + std::to_string(cv_min_area) +
               " cv_diff_threshold=" + std::to_string(cv_diff_threshold) +
               " cv_sample_interval_ms=" + std::to_string(cv_sample_interval_ms) +
               " cv_background_learning_rate=" + std::to_string(cv_background_learning_rate) +
               " cv_warmup_frames=" + std::to_string(cv_warmup_frames) +
               " cv_debug_save_frames=" + std::to_string(cv_debug_save_frames) +
               " rtsp=" + rtsp_host + ":" + std::to_string(rtsp_port) + "/" + rtsp_path);

    if (is_tcp_port_open(rtsp_host, rtsp_port)) {
        log_event("RTSP feed reachable at startup (" + rtsp_host + ":" + std::to_string(rtsp_port) + ")");
    } else {
        log_event("WARNING: RTSP feed not reachable at startup -- check 'systemctl status mediamtx'. " +
                   std::string(motion_source == "opencv"
                       ? "OpenCV motion detection cannot function without it and will fail to start."
                       : "PIR motion detection/MQTT will still work; viewing the stream will not until MediaMTX is up."));
    }

    mosquitto_lib_init();
    mosq = mosquitto_new(nullptr, true, nullptr);
    if (!mosq) {
        log_event("Failed to create mosquitto client");
        return 1;
    }

    int protocol_version = MQTT_PROTOCOL_V311;
    mosquitto_opts_set(mosq, MOSQ_OPT_PROTOCOL_VERSION, &protocol_version);
    mosquitto_reconnect_delay_set(mosq, 2, 10, false);

    mqtt_reconnect_loop(mosq);

    if (motion_source == "opencv") {
        run_opencv_motion_detection();
    } else {
        if (motion_source != "pir") {
            log_event("Config: unrecognized motion_source='" + motion_source + "', defaulting to 'pir'");
        }
        run_pir_motion_detection();
    }

    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    log_event("Shutdown complete.");
    return 0;
}