#include "mediapipe/tracking/hand_tracker.h"

#include <algorithm>
#include <cmath>
#include <chrono>

#include "mediapipe/preprocess/image_ops.h"

namespace mediapipe_demo {

HandTracker::HandTracker(const PipelineConfig& config) : config_(config) {}

void HandTracker::Reset() {
  ResetTrackingState();
}

void HandTracker::ResetTrackingState() {
  tracking_roi_.reset();
  last_good_landmarks_.clear();
  lost_count_ = 0;
  fast_motion_cooldown_ = 0;
  filter_.Reset();
}

void HandTracker::ApplyConfig(const PipelineConfig& config) {
  config_ = config;
}

bool HandTracker::ShouldRunDetector(int frame_id) const {
  return !tracking_roi_.has_value() ||
         frame_id % std::max(config_.force_detect_interval, 1) == 0 ||
         fast_motion_cooldown_ > 0;
}

std::optional<RoiRect> HandTracker::CurrentRoi() const {
  return tracking_roi_;
}

const std::vector<cv::Point2f>& HandTracker::LastGoodLandmarks() const {
  return last_good_landmarks_;
}

TrackingMode HandTracker::UpdateFromDetection(const std::optional<RoiRect>& detected_roi,
                                              float detector_score) {
  if (!detected_roi.has_value()) {
    return tracking_roi_.has_value() ? TrackingMode::kTrack : TrackingMode::kNoHand;
  }
  if (!tracking_roi_.has_value()) {
    tracking_roi_ = detected_roi;
    last_good_landmarks_.clear();
    lost_count_ = 0;
    filter_.Reset();
    return TrackingMode::kDetect;
  }
  const float iou = RoiIou(*tracking_roi_, *detected_roi);
  if (detector_score >= 0.70f && iou < config_.detector_override_iou) {
    tracking_roi_ = detected_roi;
    last_good_landmarks_.clear();
    lost_count_ = 0;
    filter_.Reset();
    return TrackingMode::kRecover;
  }
  return TrackingMode::kTrack;
}

bool HandTracker::AcceptLandmarks(std::vector<cv::Point2f>* landmarks_xy,
                                  const RoiRect& roi,
                                  int frame_w,
                                  int frame_h,
                                  float* motion_norm) {
  if (landmarks_xy == nullptr || landmarks_xy->empty()) {
    return RejectLandmarks();
  }
  if (motion_norm != nullptr) {
    *motion_norm = 0.0f;
  }

  if (last_good_landmarks_.size() == landmarks_xy->size()) {
    float mean_disp = 0.0f;
    for (size_t i = 0; i < landmarks_xy->size(); ++i) {
      mean_disp += cv::norm((*landmarks_xy)[i] - last_good_landmarks_[i]);
    }
    mean_disp /= static_cast<float>(landmarks_xy->size());
    const float roi_diag = std::hypot(static_cast<float>(roi.x2 - roi.x1),
                                      static_cast<float>(roi.y2 - roi.y1)) + 1e-6f;
    const float motion = mean_disp / roi_diag;
    if (motion_norm != nullptr) {
      *motion_norm = motion;
    }
    if (motion >= config_.motion_high_thresh) {
      fast_motion_cooldown_ =
          std::max(fast_motion_cooldown_, config_.fast_motion_detect_frames);
    }
  }

  float in_frame_count = 0.0f;
  for (const auto& point : *landmarks_xy) {
    if (point.x >= 0.0f && point.x < static_cast<float>(frame_w) &&
        point.y >= 0.0f && point.y < static_cast<float>(frame_h)) {
      in_frame_count += 1.0f;
    }
  }
  const float in_frame_ratio =
      in_frame_count / static_cast<float>(std::max<size_t>(landmarks_xy->size(), 1));
  if (in_frame_ratio < 0.7f) {
    return RejectLandmarks();
  }

  float area_ratio = 0.0f;
  float tip_ratio = 0.0f;
  bool plausible = LandmarkPlausibility(*landmarks_xy, roi, &area_ratio, &tip_ratio);
  if (fast_motion_cooldown_ > 0) {
    plausible = plausible || (in_frame_ratio >= 0.9f);
  }
  if (!plausible) {
    return RejectLandmarks();
  }

  lost_count_ = 0;
  const float motion = motion_norm != nullptr ? *motion_norm : 0.0f;
  if (motion < config_.motion_high_thresh) {
    const double now_s = std::chrono::duration<double>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
    *landmarks_xy = filter_.Filter(*landmarks_xy, now_s);
  }

  tracking_roi_ = UpdateTrackRoi(*landmarks_xy, frame_w, frame_h);
  if (!tracking_roi_.has_value()) {
    ResetTrackingState();
    return false;
  }

  last_good_landmarks_ = *landmarks_xy;
  if (fast_motion_cooldown_ > 0) {
    --fast_motion_cooldown_;
  }
  return true;
}

void HandTracker::MarkLost() {
  RejectLandmarks();
}

std::optional<RoiRect> HandTracker::UpdateTrackRoi(const std::vector<cv::Point2f>& landmarks_xy,
                                                   int frame_w,
                                                   int frame_h) const {
  if (landmarks_xy.empty()) {
    return std::nullopt;
  }
  float x_min = landmarks_xy.front().x;
  float y_min = landmarks_xy.front().y;
  float x_max = landmarks_xy.front().x;
  float y_max = landmarks_xy.front().y;
  for (const auto& point : landmarks_xy) {
    x_min = std::min(x_min, point.x);
    y_min = std::min(y_min, point.y);
    x_max = std::max(x_max, point.x);
    y_max = std::max(y_max, point.y);
  }
  const float side = std::max(x_max - x_min, y_max - y_min) * config_.track_scale;
  return MakeSquareRoi((x_min + x_max) * 0.5f, (y_min + y_max) * 0.5f, side, frame_w, frame_h);
}

int HandTracker::FastMotionCooldown() const {
  return fast_motion_cooldown_;
}

float HandTracker::RoiIou(const RoiRect& a, const RoiRect& b) {
  const int xx1 = std::max(a.x1, b.x1);
  const int yy1 = std::max(a.y1, b.y1);
  const int xx2 = std::min(a.x2, b.x2);
  const int yy2 = std::min(a.y2, b.y2);
  const int inter_w = std::max(0, xx2 - xx1);
  const int inter_h = std::max(0, yy2 - yy1);
  const float inter = static_cast<float>(inter_w * inter_h);
  const float area_a = static_cast<float>(std::max(0, a.x2 - a.x1) * std::max(0, a.y2 - a.y1));
  const float area_b = static_cast<float>(std::max(0, b.x2 - b.x1) * std::max(0, b.y2 - b.y1));
  const float denom = area_a + area_b - inter;
  return denom > 0.0f ? inter / denom : 0.0f;
}

bool HandTracker::LandmarkPlausibility(const std::vector<cv::Point2f>& landmarks_xy,
                                       const RoiRect& roi,
                                       float* area_ratio,
                                       float* tip_ratio) {
  if (landmarks_xy.size() < 21) {
    if (area_ratio != nullptr) {
      *area_ratio = 0.0f;
    }
    if (tip_ratio != nullptr) {
      *tip_ratio = 0.0f;
    }
    return false;
  }

  const float roi_area = static_cast<float>(std::max(1, roi.x2 - roi.x1) *
                                            std::max(1, roi.y2 - roi.y1));
  float x_min = landmarks_xy.front().x;
  float y_min = landmarks_xy.front().y;
  float x_max = landmarks_xy.front().x;
  float y_max = landmarks_xy.front().y;
  for (const auto& point : landmarks_xy) {
    x_min = std::min(x_min, point.x);
    y_min = std::min(y_min, point.y);
    x_max = std::max(x_max, point.x);
    y_max = std::max(y_max, point.y);
  }

  const float hand_area = std::max(1.0f, x_max - x_min) * std::max(1.0f, y_max - y_min);
  const float area = hand_area / roi_area;
  const cv::Point2f wrist = landmarks_xy[0];
  const cv::Point2f middle_mcp = landmarks_xy[9];
  const float palm_len = cv::norm(middle_mcp - wrist) + 1e-6f;
  const int tip_ids[5] = {4, 8, 12, 16, 20};
  float max_tip_dist = 0.0f;
  for (const int tip_id : tip_ids) {
    max_tip_dist = std::max(max_tip_dist, static_cast<float>(cv::norm(landmarks_xy[tip_id] - wrist)));
  }
  const float tip = max_tip_dist / palm_len;

  if (area_ratio != nullptr) {
    *area_ratio = area;
  }
  if (tip_ratio != nullptr) {
    *tip_ratio = tip;
  }
  return area >= 0.10f && area <= 0.95f && tip >= 1.1f && tip <= 5.5f;
}

bool HandTracker::RejectLandmarks() {
  ++lost_count_;
  if (lost_count_ >= std::max(config_.max_lost_count, 1)) {
    ResetTrackingState();
  }
  return false;
}

}  // namespace mediapipe_demo
