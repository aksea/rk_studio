#pragma once

#include <opencv2/core.hpp>

#include "mediapipe/common/types.h"

namespace mediapipe_demo {

cv::Mat LetterboxPadding(const cv::Mat& image,
                         const cv::Size& target_size,
                         PreprocessMeta* meta);

cv::Mat PreprocessBgr(const cv::Mat& image,
                      const cv::Size& target_size,
                      PreprocessMeta* meta);

cv::Point2f MapPointFromInputToSource(const cv::Point2f& point,
                                      const PreprocessMeta& meta);

BBox MapNormBoxToSource(const BBox& norm_bbox, const PreprocessMeta& meta);

std::optional<RoiRect> MakeRoiFromDetection(const BBox& norm_bbox,
                                            const PreprocessMeta& meta,
                                            int frame_w,
                                            int frame_h,
                                            float scale);

std::optional<RoiRect> MakeSquareRoi(float cx,
                                     float cy,
                                     float side,
                                     int frame_w,
                                     int frame_h);

cv::Mat RotateRoi(const cv::Mat& roi, float rotation_deg, cv::Mat* inverse_affine);

std::vector<cv::Point2f> AffinePoints(const std::vector<cv::Point2f>& points,
                                      const cv::Mat& affine);

}  // namespace mediapipe_demo
