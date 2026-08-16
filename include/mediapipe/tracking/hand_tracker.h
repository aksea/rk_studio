#pragma once

#include <optional>
#include <vector>

#include "mediapipe/common/config.h"
#include "mediapipe/common/types.h"
#include "mediapipe/tracking/one_euro_filter.h"

namespace mediapipe_demo {

class HandTracker {
 public:
  explicit HandTracker(const PipelineConfig& config);

  void Reset();
  void ApplyConfig(const PipelineConfig& config);
  bool ShouldRunDetector(int frame_id) const;
  std::optional<RoiRect> CurrentRoi() const;
  const std::vector<cv::Point2f>& LastGoodLandmarks() const;

  TrackingMode UpdateFromDetection(const std::optional<RoiRect>& detected_roi,
                                   float detector_score);

  bool AcceptLandmarks(std::vector<cv::Point2f>* landmarks_xy,
                       const RoiRect& roi,
                       int frame_w,
                       int frame_h,
                       float* motion_norm);

  void MarkLost();
  int FastMotionCooldown() const;

 private:
  static float RoiIou(const RoiRect& a, const RoiRect& b);
  static bool LandmarkPlausibility(const std::vector<cv::Point2f>& landmarks_xy,
                                   const RoiRect& roi,
                                   float* area_ratio,
                                   float* tip_ratio);
  std::optional<RoiRect> UpdateTrackRoi(const std::vector<cv::Point2f>& landmarks_xy,
                                        int frame_w,
                                        int frame_h) const;
  void ResetTrackingState();
  bool RejectLandmarks();

  PipelineConfig config_;
  std::optional<RoiRect> tracking_roi_;
  std::vector<cv::Point2f> last_good_landmarks_;
  int lost_count_ = 0;
  int fast_motion_cooldown_ = 0;
  OneEuroFilter filter_{1.2f, 0.03f, 1.0f};
};

}  // namespace mediapipe_demo
