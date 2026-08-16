#include "mediapipe/preprocess/image_ops.h"

#include <algorithm>

#include <opencv2/imgproc.hpp>

namespace mediapipe_demo {

namespace {

constexpr int kMinRoiSide = 32;

}  // namespace

cv::Mat LetterboxPadding(const cv::Mat& image,
                         const cv::Size& target_size,
                         PreprocessMeta* meta) {
  const float scale = std::min(static_cast<float>(target_size.width) / image.cols,
                               static_cast<float>(target_size.height) / image.rows);
  const int new_w = static_cast<int>(std::round(image.cols * scale));
  const int new_h = static_cast<int>(std::round(image.rows * scale));

  cv::Mat resized;
  cv::resize(image, resized, cv::Size(new_w, new_h), 0.0, 0.0, cv::INTER_LINEAR);

  const int pad_left = (target_size.width - new_w) / 2;
  const int pad_top = (target_size.height - new_h) / 2;
  const int pad_right = target_size.width - new_w - pad_left;
  const int pad_bottom = target_size.height - new_h - pad_top;

  cv::Mat padded;
  cv::copyMakeBorder(resized,
                     padded,
                     pad_top,
                     pad_bottom,
                     pad_left,
                     pad_right,
                     cv::BORDER_CONSTANT,
                     cv::Scalar(0, 0, 0));

  if (meta != nullptr) {
    meta->scale = scale;
    meta->pad_left = static_cast<float>(pad_left);
    meta->pad_top = static_cast<float>(pad_top);
    meta->input_w = static_cast<float>(target_size.width);
    meta->input_h = static_cast<float>(target_size.height);
    meta->src_w = static_cast<float>(image.cols);
    meta->src_h = static_cast<float>(image.rows);
  }

  return padded;
}

cv::Mat PreprocessBgr(const cv::Mat& image,
                      const cv::Size& target_size,
                      PreprocessMeta* meta) {
  cv::Mat rgb;
  cv::cvtColor(image, rgb, cv::COLOR_BGR2RGB);
  return LetterboxPadding(rgb, target_size, meta);
}

cv::Point2f MapPointFromInputToSource(const cv::Point2f& point,
                                      const PreprocessMeta& meta) {
  const float x = std::clamp((point.x - meta.pad_left) / std::max(meta.scale, 1e-6f),
                             0.0f,
                             meta.src_w - 1.0f);
  const float y = std::clamp((point.y - meta.pad_top) / std::max(meta.scale, 1e-6f),
                             0.0f,
                             meta.src_h - 1.0f);
  return cv::Point2f(x, y);
}

BBox MapNormBoxToSource(const BBox& norm_bbox, const PreprocessMeta& meta) {
  const cv::Point2f top_left =
      MapPointFromInputToSource(cv::Point2f(norm_bbox.x1 * meta.input_w,
                                            norm_bbox.y1 * meta.input_h),
                                meta);
  const cv::Point2f bottom_right =
      MapPointFromInputToSource(cv::Point2f(norm_bbox.x2 * meta.input_w,
                                            norm_bbox.y2 * meta.input_h),
                                meta);
  return BBox{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
}

std::optional<RoiRect> MakeRoiFromDetection(const BBox& norm_bbox,
                                            const PreprocessMeta& meta,
                                            int frame_w,
                                            int frame_h,
                                            float scale) {
  const BBox mapped = MapNormBoxToSource(norm_bbox, meta);
  const float box_w = mapped.x2 - mapped.x1;
  const float box_h = mapped.y2 - mapped.y1;
  const float cx = mapped.x1 + box_w * 0.5f;
  const float cy = mapped.y1 + box_h * 0.5f;
  return MakeSquareRoi(cx, cy, std::max(box_w, box_h) * scale, frame_w, frame_h);
}

std::optional<RoiRect> MakeSquareRoi(float cx,
                                     float cy,
                                     float side,
                                     int frame_w,
                                     int frame_h) {
  if (frame_w <= 1 || frame_h <= 1) {
    return std::nullopt;
  }

  const int side_px = std::min(std::min(frame_w, frame_h),
                               std::max(kMinRoiSide, static_cast<int>(std::round(side))));
  const float half = static_cast<float>(side_px) * 0.5f;
  int x1 = static_cast<int>(std::round(cx - half));
  int y1 = static_cast<int>(std::round(cy - half));
  x1 = std::clamp(x1, 0, std::max(0, frame_w - side_px));
  y1 = std::clamp(y1, 0, std::max(0, frame_h - side_px));

  RoiRect roi;
  roi.x1 = x1;
  roi.y1 = y1;
  roi.x2 = std::min(frame_w, x1 + side_px);
  roi.y2 = std::min(frame_h, y1 + side_px);
  if (roi.x2 - roi.x1 < kMinRoiSide || roi.y2 - roi.y1 < kMinRoiSide) {
    return std::nullopt;
  }
  return roi;
}

cv::Mat RotateRoi(const cv::Mat& roi, float rotation_deg, cv::Mat* inverse_affine) {
  const cv::Point2f center(roi.cols * 0.5f, roi.rows * 0.5f);
  cv::Mat affine = cv::getRotationMatrix2D(center, rotation_deg, 1.0);
  cv::Mat rotated;
  cv::warpAffine(roi,
                 rotated,
                 affine,
                 roi.size(),
                 cv::INTER_LINEAR,
                 cv::BORDER_CONSTANT,
                 cv::Scalar(0, 0, 0));
  if (inverse_affine != nullptr) {
    cv::invertAffineTransform(affine, *inverse_affine);
  }
  return rotated;
}

std::vector<cv::Point2f> AffinePoints(const std::vector<cv::Point2f>& points,
                                      const cv::Mat& affine) {
  std::vector<cv::Point2f> transformed;
  cv::transform(points, transformed, affine);
  return transformed;
}

}  // namespace mediapipe_demo
