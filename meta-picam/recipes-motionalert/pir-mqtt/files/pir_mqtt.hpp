#ifndef PIR_MQTT_HPP
#define PIR_MQTT_HPP

// SPDX-License-Identifier: MIT
#include <atomic>
#include <chrono>
#include <set>
#include <string>

namespace pir_mqtt {

// The camera module depends on this interface for runtime state, settings,
// motion-session control, logging, and MQTT event servicing.
extern std::atomic<bool> running;
extern std::string motion_source;
extern int cv_min_area;
extern int cv_diff_threshold;
extern int cv_sample_interval_ms;
extern double cv_background_learning_rate;
extern int cv_warmup_frames;
extern bool cv_debug_save_frames;

extern std::string rtsp_host;
extern int rtsp_port;
extern std::string rtsp_path;
extern int camera_width;
extern int camera_height;
extern int camera_fps;
extern std::string yolo_model_path;
extern int yolo_inference_interval_ms;
extern double yolo_confidence_threshold;
extern double yolo_nms_threshold;

void log_event(const std::string &message);
void pump_mqtt();

class MotionSessionTracker {
public:
    MotionSessionTracker();

    void on_edge(const std::string &detected_labels = {});
    void update_labels(const std::string &detected_labels);
    void tick();

    int edge_count() const;
    bool has_active_session() const;

private:
    void publish(const std::string &payload);
    static bool add_labels(std::set<std::string> &labels,
                           const std::string &detected_labels);
    void publish_motion_alert();

    std::chrono::steady_clock::time_point last_motion_;
    std::chrono::steady_clock::time_point last_accepted_edge_;
    std::chrono::steady_clock::time_point session_start_;
    std::chrono::steady_clock::time_point burst_start_;
    bool in_session_ = false;
    int edge_count_ = 0;
    int burst_count_ = 0;
    std::set<std::string> burst_labels_;
    std::set<std::string> session_labels_;
};

void run_pir_motion_detection(MotionSessionTracker &tracker);
void run_camera_analytics(MotionSessionTracker &tracker);

} // namespace pir_mqtt

#endif
