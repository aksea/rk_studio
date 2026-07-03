#pragma once

#include <string>

namespace mediapipe_demo {

struct PipelineConfig {
  int camera_index = 71;
  std::string camera_device;
  std::string camera_backend = "v4l2";
  int camera_width = 1920;
  int camera_height = 1080;
  int camera_buffer_size = 2;
  std::string camera_fourcc = "NV12";
  float detector_score_threshold = 0.55f;
  int force_detect_interval = 6;
  float detector_override_iou = 0.25f;
  int max_lost_count = 5;
  float det_scale = 2.0f;
  float track_scale = 1.8f;
  bool enable_affine_align = true;
  float affine_max_abs_deg = 135.0f;
  float motion_high_thresh = 0.18f;
  int fast_motion_detect_frames = 6;
  bool affine_disable_on_fast_motion = true;
  int min_landmark_span_px = 28;
  float min_landmark_span_ratio = 0.045f;
  float roi_shrink_reject_ratio = 0.55f;
  bool enable_display = true;
  bool enable_profiling = false;
  bool enable_parallel_landmarks = true;
};

}  // namespace mediapipe_demo
