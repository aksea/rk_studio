#include "rk_studio/vision_core/vision_processor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "mediapipe/common/config.h"
#include "mediapipe/common/types.h"
#include "mediapipe/detector/palm_detector.h"
#include "mediapipe/landmark/hand_landmark.h"
#include "mediapipe/preprocess/image_ops.h"
#include "mediapipe/tracking/hand_tracker.h"

namespace rkstudio::vision {
namespace {

constexpr int kMaxHands = 1;

std::vector<cv::Point2f> ExtractLandmarkPoints(const mediapipe_demo::HandLandmarks& landmarks) {
  std::vector<cv::Point2f> points;
  points.reserve(landmarks.points.size());
  for (const auto& point : landmarks.points) {
    points.emplace_back(point.x, point.y);
  }
  return points;
}

std::vector<cv::Point3f> ExtractLandmarkPoints3D(const mediapipe_demo::HandLandmarks& landmarks) {
  std::vector<cv::Point3f> points;
  points.reserve(landmarks.points.size());
  for (const auto& point : landmarks.points) {
    points.push_back(point);
  }
  return points;
}

float EstimateHandAngleDeg(const std::vector<cv::Point2f>& landmarks_xy) {
  if (landmarks_xy.size() < 10) {
    return 0.0f;
  }
  const cv::Point2f v = landmarks_xy[9] - landmarks_xy[0];
  return static_cast<float>(std::atan2(v.y, v.x) * 180.0 / CV_PI);
}

float RotationToVerticalDeg(float angle_deg) {
  float rotation = angle_deg + 90.0f;
  while (rotation > 180.0f) {
    rotation -= 360.0f;
  }
  while (rotation < -180.0f) {
    rotation += 360.0f;
  }
  return rotation;
}

bool PointInImage(const cv::Point2f& point, const cv::Mat& image) {
  return point.x >= 0.0f && point.x < static_cast<float>(image.cols) &&
         point.y >= 0.0f && point.y < static_cast<float>(image.rows);
}

TrackingMode ConvertMode(mediapipe_demo::TrackingMode mode) {
  switch (mode) {
    case mediapipe_demo::TrackingMode::kDetect:
      return TrackingMode::kDetect;
    case mediapipe_demo::TrackingMode::kTrack:
      return TrackingMode::kTrack;
    case mediapipe_demo::TrackingMode::kRecover:
      return TrackingMode::kRecover;
    case mediapipe_demo::TrackingMode::kNoHand:
    default:
      return TrackingMode::kNoHand;
  }
}

cv::Mat ToRgbMat(const FrameRef& frame) {
  if (frame.mapped_ptr == nullptr || frame.width <= 0 || frame.height <= 0 || frame.stride <= 0) {
    return {};
  }

  if (frame.pixel_format == PixelFormat::kRgb) {
    return cv::Mat(frame.height, frame.width, CV_8UC3, const_cast<uint8_t*>(frame.mapped_ptr), frame.stride);
  }
  if (frame.pixel_format == PixelFormat::kNv12) {
    cv::Mat nv12(frame.height * 3 / 2, frame.width, CV_8UC1,
                 const_cast<uint8_t*>(frame.mapped_ptr), frame.stride);
    cv::Mat rgb;
    cv::cvtColor(nv12, rgb, cv::COLOR_YUV2RGB_NV12);
    return rgb;
  }
  if (frame.pixel_format == PixelFormat::kBgr) {
    cv::Mat bgr(frame.height, frame.width, CV_8UC3, const_cast<uint8_t*>(frame.mapped_ptr), frame.stride);
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    return rgb;
  }
  return {};
}

cv::Point2f RoiCenter(const mediapipe_demo::RoiRect& roi) {
  return {static_cast<float>(roi.x1 + roi.x2) * 0.5f, static_cast<float>(roi.y1 + roi.y2) * 0.5f};
}

float PointDistSq(const cv::Point2f& a, const cv::Point2f& b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return dx * dx + dy * dy;
}

float PointDist(const cv::Point2f& a, const cv::Point2f& b) {
  return std::sqrt(PointDistSq(a, b));
}

float Dot(const cv::Point2f& a, const cv::Point2f& b) {
  return a.x * b.x + a.y * b.y;
}

float ProjectFromWrist(const std::vector<cv::Point2f>& points, int index, const cv::Point2f& axis) {
  return Dot(points[index] - points[0], axis);
}

struct GestureRecognition {
  std::string gesture;
  float score = 0.0f;
};

GestureRecognition RecognizeHandGesture(const std::vector<cv::Point3f>& points3d) {
  GestureRecognition result;
  if (points3d.size() < 21) {
    return result;
  }

  std::vector<cv::Point2f> points;
  points.reserve(points3d.size());
  for (const auto& point : points3d) {
    points.emplace_back(point.x, point.y);
  }

  const cv::Point2f wrist = points[0];
  const cv::Point2f middle_mcp = points[9];
  const float palm_len = PointDist(wrist, middle_mcp);
  const float palm_width = PointDist(points[5], points[17]);
  const float palm_size = std::max({palm_len, palm_width, 1.0f});
  cv::Point2f palm_axis = middle_mcp - wrist;
  const float axis_len = std::max(PointDist(wrist, middle_mcp), 1.0f);
  palm_axis.x /= axis_len;
  palm_axis.y /= axis_len;

  const cv::Point2f palm_center =
      (wrist + points[5] + points[9] + points[13] + points[17]) * 0.2f;

  constexpr std::array<std::array<int, 3>, 4> kFingers = {{
      {{5, 6, 8}},
      {{9, 10, 12}},
      {{13, 14, 16}},
      {{17, 18, 20}},
  }};

  std::array<bool, 4> finger_extended{};
  std::array<bool, 4> finger_folded{};
  std::array<bool, 4> finger_strict_folded{};
  for (size_t i = 0; i < kFingers.size(); ++i) {
    const auto& finger = kFingers[i];
    const int mcp = finger[0];
    const int pip = finger[1];
    const int tip = finger[2];
    const float tip_proj = ProjectFromWrist(points, tip, palm_axis);
    const float pip_proj = ProjectFromWrist(points, pip, palm_axis);
    const bool extended_by_axis = tip_proj > pip_proj + 0.16f * palm_size;
    const bool extended_far_from_mcp = PointDist(points[tip], points[mcp]) > 0.62f * palm_size;
    const bool folded_by_axis = tip_proj < pip_proj + 0.06f * palm_size;
    const bool folded_near_palm = PointDist(points[tip], palm_center) < 0.72f * palm_size ||
                                  PointDist(points[tip], points[mcp]) < 0.55f * palm_size;
    finger_extended[i] = extended_by_axis && extended_far_from_mcp;
    finger_folded[i] = folded_by_axis || folded_near_palm;
    finger_strict_folded[i] = folded_by_axis && folded_near_palm;
  }

  const float thumb_to_palm = PointDist(points[4], palm_center);
  const float thumb_to_index_mcp = PointDist(points[4], points[5]);
  const float thumb_to_middle_mcp = PointDist(points[4], points[9]);
  const bool thumb_folded = thumb_to_palm < 0.60f * palm_size &&
                            thumb_to_index_mcp < 0.46f * palm_size &&
                            thumb_to_middle_mcp < 0.52f * palm_size;

  const bool index_extended = finger_extended[0];
  const bool middle_extended = finger_extended[1];
  const bool ring_extended = finger_extended[2];
  const bool pinky_extended = finger_extended[3];
  const int extended_count = static_cast<int>(index_extended) +
                             static_cast<int>(middle_extended) +
                             static_cast<int>(ring_extended) +
                             static_cast<int>(pinky_extended);
  const int folded_count = static_cast<int>(finger_strict_folded[0]) +
                           static_cast<int>(finger_strict_folded[1]) +
                           static_cast<int>(finger_strict_folded[2]) +
                           static_cast<int>(finger_strict_folded[3]);
  const bool middle_folded = finger_strict_folded[1];
  const bool ring_folded = finger_strict_folded[2];
  const bool pinky_folded = finger_strict_folded[3];

  const float thumb_index_gap = PointDist(points[4], points[8]);
  const float index_extension = ProjectFromWrist(points, 8, palm_axis) - ProjectFromWrist(points, 6, palm_axis);
  const float middle_extension = ProjectFromWrist(points, 12, palm_axis) - ProjectFromWrist(points, 10, palm_axis);
  const float ring_extension = ProjectFromWrist(points, 16, palm_axis) - ProjectFromWrist(points, 14, palm_axis);
  const float pinky_extension = ProjectFromWrist(points, 20, palm_axis) - ProjectFromWrist(points, 18, palm_axis);
  const float fingertip_gap = PointDist(points[8], points[12]);
  const bool index_isolated = index_extension > 0.18f * palm_size &&
                              middle_extension < 0.04f * palm_size &&
                              ring_extension < 0.04f * palm_size &&
                              pinky_extension < 0.04f * palm_size &&
                              fingertip_gap > 0.42f * palm_size;

  const bool back_three_fingers_folded =
      middle_folded && ring_folded && pinky_folded &&
      middle_extension < 0.04f * palm_size &&
      ring_extension < 0.04f * palm_size &&
      pinky_extension < 0.04f * palm_size;

  const bool back_three_fingers_extended =
      middle_extended && ring_extended && pinky_extended &&
      middle_extension > 0.14f * palm_size &&
      ring_extension > 0.14f * palm_size &&
      pinky_extension > 0.12f * palm_size;
  const bool thumb_index_touching = thumb_index_gap < 0.30f * palm_size;
  const bool index_curled_to_thumb = index_extension < 0.12f * palm_size ||
                                     thumb_index_gap < 0.24f * palm_size;

  if (thumb_index_touching && index_curled_to_thumb && back_three_fingers_extended) {
    result.gesture = "ok";
    result.score = std::clamp(0.70f +
                                  std::min(0.16f, (0.30f * palm_size - thumb_index_gap) /
                                                      std::max(palm_size, 1.0f)) +
                                  std::min(0.10f, middle_extension / std::max(palm_size, 1.0f)),
                              0.0f, 1.0f);
    return result;
  }

  if (index_extended && back_three_fingers_folded && thumb_folded && index_isolated) {
    result.gesture = "pointing";
    result.score = std::clamp(0.62f +
                                  std::min(0.18f, index_extension / std::max(palm_size, 1.0f)) +
                                  std::min(0.12f, fingertip_gap / std::max(palm_size, 1.0f)),
                              0.0f, 1.0f);
    return result;
  }

  if (thumb_folded && folded_count == 4) {
    result.gesture = "up";
    result.score = 0.86f;
    return result;
  }

  if (thumb_folded && extended_count >= 3) {
    result.gesture = "down";
    result.score = 0.84f;
    return result;
  }

  return result;
}

// Greedy match: assign detections to trackers by nearest ROI center.
// assignments[tracker_id] = index into detections, or -1 if unmatched.
std::array<int, kMaxHands> MatchDetectionsToTrackers(
    const std::vector<mediapipe_demo::PalmDetection>& detections,
    const std::vector<mediapipe_demo::RoiRect>& det_rois,
    const std::array<std::unique_ptr<mediapipe_demo::HandTracker>, kMaxHands>& trackers) {
  std::array<int, kMaxHands> assignments;
  assignments.fill(-1);
  std::vector<bool> det_used(detections.size(), false);

  // First pass: match trackers that have a current ROI to nearest detection
  for (int t = 0; t < kMaxHands; ++t) {
    const auto tracker_roi = trackers[t]->CurrentRoi();
    if (!tracker_roi.has_value()) {
      continue;
    }
    const cv::Point2f tc = RoiCenter(*tracker_roi);
    float best_dist = std::numeric_limits<float>::max();
    int best_d = -1;
    for (size_t d = 0; d < det_rois.size(); ++d) {
      if (det_used[d]) continue;
      const float dist = PointDistSq(tc, RoiCenter(det_rois[d]));
      if (dist < best_dist) {
        best_dist = dist;
        best_d = static_cast<int>(d);
      }
    }
    if (best_d >= 0) {
      assignments[t] = best_d;
      det_used[static_cast<size_t>(best_d)] = true;
    }
  }

  // Second pass: assign remaining detections to idle trackers
  for (size_t d = 0; d < det_rois.size(); ++d) {
    if (det_used[d]) continue;
    for (int t = 0; t < kMaxHands; ++t) {
      if (assignments[t] >= 0) continue;
      if (trackers[t]->CurrentRoi().has_value()) continue;
      assignments[t] = static_cast<int>(d);
      det_used[d] = true;
      break;
    }
  }

  return assignments;
}

}  // namespace

class MediapipeProcessor final : public IMediapipeProcessor {
 public:
  ~MediapipeProcessor() override { Stop(); }

  bool Start(const MediapipeProcessorConfig& config, std::string* err) override {
    Stop();

    config_ = config;
    pipeline_config_ = mediapipe_demo::PipelineConfig{};
    for (int i = 0; i < kMaxHands; ++i) {
      trackers_[i] = std::make_unique<mediapipe_demo::HandTracker>(pipeline_config_);
    }
    if (!detector_.LoadModel(config.detector_model)) {
      if (err) {
        *err = "failed to load detector model: " + config.detector_model;
      }
      for (auto& t : trackers_) t.reset();
      return false;
    }
    if (!landmark_.LoadModel(config.landmark_model)) {
      if (err) {
        *err = "failed to load landmark model: " + config.landmark_model;
      }
      for (auto& t : trackers_) t.reset();
      return false;
    }

    running_ = true;
    worker_ = std::thread([this] { RunLoop(); });
    return true;
  }

  void Submit(const VisionFrame& frame) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (!running_) {
      return;
    }

    pending_frames_.push_back(frame);
    while (pending_frames_.size() > std::max<size_t>(1, config_.queue_depth)) {
      pending_frames_.pop_front();
    }
    cv_.notify_one();
  }

  std::optional<MediapipeResult> PollResult() override {
    std::lock_guard<std::mutex> lock(mu_);
    if (results_.empty()) {
      return std::nullopt;
    }
    MediapipeResult result = std::move(results_.front());
    results_.pop_front();
    return result;
  }

  void Stop() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      running_ = false;
      pending_frames_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      results_.clear();
    }
    for (auto& t : trackers_) t.reset();
    frame_index_ = 1;
  }

 private:
  void RunLoop() {
    while (true) {
      VisionFrame frame;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return !running_ || !pending_frames_.empty(); });
        if (!running_ && pending_frames_.empty()) {
          break;
        }
        frame = std::move(pending_frames_.front());
        pending_frames_.pop_front();
      }

      MediapipeResult result = ProcessFrame(frame);
      {
        std::lock_guard<std::mutex> lock(mu_);
        results_.push_back(std::move(result));
        while (results_.size() > std::max<size_t>(1, config_.queue_depth * 2)) {
          results_.pop_front();
        }
      }
    }
  }

  HandResult ProcessOneHand(int hand_id,
                            mediapipe_demo::HandTracker& tracker,
                            mediapipe_demo::TrackingMode frame_mode,
                            const FrameRef& frame,
                            std::function<cv::Mat&()> ensure_rgb) {
    HandResult hand;
    hand.hand_id = hand_id;

    const std::optional<mediapipe_demo::RoiRect> current_roi = tracker.CurrentRoi();
    hand.tracking_mode = ConvertMode(frame_mode);

    if (!current_roi.has_value()) {
      tracker.MarkLost();
      hand.tracking_mode = TrackingMode::kNoHand;
      return hand;
    }

    mediapipe_demo::RoiRect roi_rect = *current_roi;
    hand.roi = RoiRect{roi_rect.x1, roi_rect.y1, roi_rect.x2, roi_rect.y2};
    if (!mediapipe_demo::IsUsableRoi(roi_rect, frame.width, frame.height)) {
      tracker.Reset();
      hand.tracking_mode = TrackingMode::kNoHand;
      hand.roi.reset();
      return hand;
    }

    const cv::Rect roi_cv(roi_rect.x1, roi_rect.y1, roi_rect.x2 - roi_rect.x1,
                           roi_rect.y2 - roi_rect.y1);
    const cv::Size landmark_size(224, 224);

    cv::Mat roi_for_landmark;
    cv::Mat inverse_affine;
    float align_rotation_deg = 0.0f;

    const auto& prev_landmarks = tracker.LastGoodLandmarks();
    if (pipeline_config_.enable_affine_align && prev_landmarks.size() == 21 &&
        !(pipeline_config_.affine_disable_on_fast_motion && tracker.FastMotionCooldown() > 0)) {
      std::vector<cv::Point2f> prev_local = prev_landmarks;
      for (auto& point : prev_local) {
        point.x -= static_cast<float>(roi_rect.x1);
        point.y -= static_cast<float>(roi_rect.y1);
      }

      cv::Mat roi_check = ensure_rgb();
      if (!roi_check.empty() &&
          roi_cv.x >= 0 && roi_cv.y >= 0 &&
          roi_cv.x + roi_cv.width <= roi_check.cols &&
          roi_cv.y + roi_cv.height <= roi_check.rows &&
          PointInImage(prev_local[0], roi_check(roi_cv)) &&
          PointInImage(prev_local[9], roi_check(roi_cv))) {
        const float angle_deg = EstimateHandAngleDeg(prev_local);
        align_rotation_deg = std::clamp(RotationToVerticalDeg(angle_deg),
                                        -pipeline_config_.affine_max_abs_deg,
                                        pipeline_config_.affine_max_abs_deg);
        if (std::abs(align_rotation_deg) > 1.0f) {
          cv::Mat roi = ensure_rgb()(roi_cv).clone();
          roi_for_landmark = mediapipe_demo::RotateRoi(roi, align_rotation_deg, &inverse_affine);
        }
      }
    }

    mediapipe_demo::PreprocessMeta lm_meta;
    std::optional<mediapipe_demo::HandLandmarks> landmarks;

    if (roi_for_landmark.empty()) {
      cv::Mat& rgb_frame = ensure_rgb();
      if (rgb_frame.empty() ||
          roi_cv.x < 0 || roi_cv.y < 0 ||
          roi_cv.x + roi_cv.width > rgb_frame.cols ||
          roi_cv.y + roi_cv.height > rgb_frame.rows) {
        tracker.MarkLost();
        hand.tracking_mode = TrackingMode::kNoHand;
        return hand;
      }
      roi_for_landmark = rgb_frame(roi_cv).clone();
    }
    cv::Mat lm_input = mediapipe_demo::LetterboxPadding(roi_for_landmark, landmark_size, &lm_meta);
    landmarks = landmark_.Infer(lm_input, lm_meta);

    if (!landmarks.has_value()) {
      tracker.MarkLost();
    } else {
      std::vector<cv::Point2f> roi_points = ExtractLandmarkPoints(*landmarks);
      if (!inverse_affine.empty()) {
        roi_points = mediapipe_demo::AffinePoints(roi_points, inverse_affine);
        for (auto& point : roi_points) {
          point.x = std::clamp(point.x, 0.0f, static_cast<float>(roi_cv.width - 1));
          point.y = std::clamp(point.y, 0.0f, static_cast<float>(roi_cv.height - 1));
        }
      }

      std::vector<cv::Point2f> global_points = roi_points;
      for (auto& point : global_points) {
        point.x += static_cast<float>(roi_rect.x1);
        point.y += static_cast<float>(roi_rect.y1);
      }

      float motion_norm = 0.0f;
      if (tracker.AcceptLandmarks(&global_points, roi_rect, frame.width, frame.height, &motion_norm)) {
        for (size_t i = 0; i < landmarks->points.size(); ++i) {
          landmarks->points[i].x = global_points[i].x;
          landmarks->points[i].y = global_points[i].y;
        }
        hand.landmarks.reserve(landmarks->points.size());
        for (const auto& point : landmarks->points) {
          hand.landmarks.push_back(Landmark3f{point.x, point.y, point.z});
        }
        const GestureRecognition gesture = RecognizeHandGesture(ExtractLandmarkPoints3D(*landmarks));
        hand.gesture = gesture.gesture;
        hand.gesture_score = gesture.score;
        hand.motion_norm = motion_norm;
        hand.rotation_deg = align_rotation_deg;
        hand.fast_motion_cooldown = tracker.FastMotionCooldown();
        hand.tracking_mode = ConvertMode(frame_mode);
      } else {
        hand.tracking_mode = tracker.CurrentRoi().has_value() ? TrackingMode::kTrack : TrackingMode::kNoHand;
      }
    }

    return hand;
  }

  MediapipeResult ProcessFrame(const VisionFrame& input) {
    auto started = std::chrono::steady_clock::now();
    const FrameRef& frame = input.rgb;

    cv::Mat rgb;
    auto ensure_rgb = [&]() -> cv::Mat& {
      if (rgb.empty()) {
        rgb = ToRgbMat(frame);
      }
      return rgb;
    };

    const cv::Size detector_size(192, 192);
    MediapipeResult result;
    result.camera_id = frame.camera_id;
    result.pts_ns = frame.pts_ns;
    result.frame_width = frame.width;
    result.frame_height = frame.height;

    // --- Detection phase: get up to 2 detections ---
    bool any_should_detect = false;
    for (int i = 0; i < kMaxHands; ++i) {
      if (trackers_[i]->ShouldRunDetector(frame_index_)) {
        any_should_detect = true;
        break;
      }
    }

    std::vector<mediapipe_demo::PalmDetection> detections;
    std::vector<mediapipe_demo::RoiRect> det_rois;

    if (any_should_detect) {
      mediapipe_demo::PreprocessMeta det_meta;

      cv::Mat& rgb_frame = ensure_rgb();
      if (!rgb_frame.empty()) {
        cv::Mat det_input = mediapipe_demo::LetterboxPadding(rgb_frame, detector_size, &det_meta);
        detections = detector_.InferMulti(det_input, det_meta, pipeline_config_.detector_score_threshold, kMaxHands);
      }

      std::vector<mediapipe_demo::PalmDetection> valid_detections;
      std::vector<mediapipe_demo::RoiRect> valid_det_rois;
      valid_detections.reserve(detections.size());
      valid_det_rois.reserve(detections.size());
      for (const auto& det : detections) {
        auto roi = mediapipe_demo::MakeRoiFromDetection(det.bbox, det_meta, frame.width, frame.height,
                                                         pipeline_config_.det_scale);
        if (roi.has_value()) {
          valid_detections.push_back(det);
          valid_det_rois.push_back(*roi);
        }
      }
      detections = std::move(valid_detections);
      det_rois = std::move(valid_det_rois);

      // Match detections to trackers
      auto assignments = MatchDetectionsToTrackers(detections, det_rois, trackers_);
      for (int t = 0; t < kMaxHands; ++t) {
        if (assignments[t] >= 0) {
          const size_t d = static_cast<size_t>(assignments[t]);
          trackers_[t]->UpdateFromDetection(det_rois[d], detections[d].score);
        }
      }
    }

    // --- Landmark phase: process each tracked hand ---
    for (int i = 0; i < kMaxHands; ++i) {
      const auto current_roi = trackers_[i]->CurrentRoi();
      if (!current_roi.has_value()) {
        continue;
      }
      mediapipe_demo::TrackingMode frame_mode = mediapipe_demo::TrackingMode::kTrack;
      HandResult hand = ProcessOneHand(i, *trackers_[i], frame_mode, frame, ensure_rgb);
      if (hand.tracking_mode != TrackingMode::kNoHand || !hand.landmarks.empty()) {
        result.hands.push_back(std::move(hand));
      }
    }

    auto finished = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = finished - started;
    result.fps = elapsed.count() > 0.0 ? static_cast<float>(1.0 / elapsed.count()) : 0.0f;
    result.ok = true;
    ++frame_index_;
    return result;
  }

  MediapipeProcessorConfig config_;
  mediapipe_demo::PipelineConfig pipeline_config_;
  mediapipe_demo::PalmDetector detector_;
  mediapipe_demo::HandLandmark landmark_;
  std::array<std::unique_ptr<mediapipe_demo::HandTracker>, kMaxHands> trackers_;

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<VisionFrame> pending_frames_;
  std::deque<MediapipeResult> results_;
  bool running_ = false;
  std::thread worker_;
  int frame_index_ = 1;
};

std::unique_ptr<IMediapipeProcessor> CreateMediapipeProcessor() {
  return std::make_unique<MediapipeProcessor>();
}

}  // namespace rkstudio::vision
