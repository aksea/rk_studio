#include "rk_studio/media_core/frame_converter.h"

#include <cstring>
#include <memory>
#include <utility>

#include <gst/allocators/gstdmabuf.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <linux/videodev2.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "mediapipe/preprocess/hw_preprocess.h"

namespace rkstudio::media {
namespace {

int ExtractDmabufFd(GstBuffer* buffer) {
  GstMemory* mem = gst_buffer_peek_memory(buffer, 0);
  if (mem != nullptr && gst_is_dmabuf_memory(mem)) {
    return gst_dmabuf_memory_get_fd(mem);
  }
  return -1;
}

uint32_t VideoFormatToFourcc(GstVideoFormat format) {
  switch (format) {
    case GST_VIDEO_FORMAT_NV12:
      return v4l2_fourcc('N', 'V', '1', '2');
    default:
      return 0;
  }
}

std::shared_ptr<void> HoldSampleRef(GstSample* sample) {
  if (sample == nullptr) {
    return {};
  }
  GstSample* ref = gst_sample_ref(sample);
  return std::shared_ptr<void>(ref, [](void* ptr) {
    gst_sample_unref(static_cast<GstSample*>(ptr));
  });
}

struct Nv12FrameHolder {
  std::shared_ptr<void> sample_ref;
  std::shared_ptr<cv::Mat> data;
};

}  // namespace

std::optional<vision::FrameRef> FrameConverter::ExtractNv12Frame(
    GstSample* sample,
    const std::string& camera_id,
    bool copy_dmabuf) const {
  if (sample == nullptr || camera_id.empty()) {
    return std::nullopt;
  }

  GstBuffer* buffer = gst_sample_get_buffer(sample);
  GstCaps* caps = gst_sample_get_caps(sample);
  if (!buffer || !caps) {
    return std::nullopt;
  }

  GstVideoInfo info;
  if (!gst_video_info_from_caps(&info, caps)) {
    return std::nullopt;
  }
  if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12) {
    return std::nullopt;
  }

  const int w = GST_VIDEO_INFO_WIDTH(&info);
  const int h = GST_VIDEO_INFO_HEIGHT(&info);
  const int stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
  const uint64_t pts_ns = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))
                              ? GST_BUFFER_PTS(buffer) : 0;

  vision::FrameRef frame;
  frame.camera_id = camera_id;
  frame.pts_ns = pts_ns;
  frame.width = w;
  frame.height = h;
  frame.stride = stride;
  frame.fourcc = VideoFormatToFourcc(GST_VIDEO_INFO_FORMAT(&info));
  frame.pixel_format = vision::PixelFormat::kNv12;
  frame.bytes_used = gst_buffer_get_size(buffer);
  frame.dmabuf_fd = ExtractDmabufFd(buffer);
  if (frame.dmabuf_fd >= 0 && !copy_dmabuf) {
    frame.owned_data = HoldSampleRef(sample);
    return frame;
  }

  GstVideoFrame video_frame;
  if (!gst_video_frame_map(&video_frame, &info, buffer, GST_MAP_READ)) {
    if (frame.dmabuf_fd >= 0) {
      frame.owned_data = HoldSampleRef(sample);
      return frame;
    }
    return std::nullopt;
  }

  auto nv12_holder = std::make_shared<cv::Mat>(h * 3 / 2, w, CV_8UC1);
  const auto* src_y = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0));
  const auto* src_uv = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 1));
  const int src_y_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0);
  const int src_uv_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 1);
  if (src_y == nullptr || src_uv == nullptr || src_y_stride <= 0 || src_uv_stride <= 0) {
    gst_video_frame_unmap(&video_frame);
    if (frame.dmabuf_fd >= 0) {
      frame.owned_data = HoldSampleRef(sample);
      return frame;
    }
    return std::nullopt;
  }
  for (int y = 0; y < h; ++y) {
    std::memcpy(nv12_holder->ptr<uint8_t>(y),
                src_y + static_cast<size_t>(y) * src_y_stride,
                w);
  }
  for (int y = 0; y < h / 2; ++y) {
    std::memcpy(nv12_holder->ptr<uint8_t>(h + y),
                src_uv + static_cast<size_t>(y) * src_uv_stride,
                w);
  }
  gst_video_frame_unmap(&video_frame);

  frame.stride = static_cast<int>(nv12_holder->step[0]);
  frame.mapped_ptr = nv12_holder->data;
  frame.bytes_used = nv12_holder->total() * nv12_holder->elemSize();
  if (frame.dmabuf_fd >= 0) {
    auto holder = std::make_shared<Nv12FrameHolder>();
    holder->sample_ref = HoldSampleRef(sample);
    holder->data = nv12_holder;
    frame.owned_data = holder;
  } else {
    frame.owned_data = nv12_holder;
  }
  return frame;
}

std::optional<vision::FrameRef> FrameConverter::ConvertToRgbFrame(
    GstSample* sample,
    const std::string& camera_id) const {
  if (sample == nullptr || camera_id.empty()) {
    return std::nullopt;
  }

  GstBuffer* buffer = gst_sample_get_buffer(sample);
  GstCaps* caps = gst_sample_get_caps(sample);
  if (!buffer || !caps) {
    return std::nullopt;
  }

  GstVideoInfo info;
  if (!gst_video_info_from_caps(&info, caps)) {
    return std::nullopt;
  }

  const int w = GST_VIDEO_INFO_WIDTH(&info);
  const int h = GST_VIDEO_INFO_HEIGHT(&info);
  const int stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
  const uint64_t pts_ns = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))
                              ? GST_BUFFER_PTS(buffer) : 0;

  cv::Mat rgb;
  const int fd = ExtractDmabufFd(buffer);
  if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12 ||
      fd < 0 ||
      !mediapipe_demo::ConvertNv12ToRgb(fd, w, h, stride, &rgb)) {
    GstVideoFrame video_frame;
    if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12 ||
        !gst_video_frame_map(&video_frame, &info, buffer, GST_MAP_READ)) {
      return std::nullopt;
    }

    const auto* src_y = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0));
    const auto* src_uv = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 1));
    const int src_y_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0);
    const int src_uv_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 1);
    if (src_y == nullptr || src_uv == nullptr || src_y_stride <= 0 || src_uv_stride <= 0) {
      gst_video_frame_unmap(&video_frame);
      return std::nullopt;
    }

    cv::Mat nv12(h * 3 / 2, w, CV_8UC1);
    for (int y = 0; y < h; ++y) {
      std::memcpy(nv12.ptr<uint8_t>(y),
                  src_y + static_cast<size_t>(y) * src_y_stride,
                  w);
    }
    for (int y = 0; y < h / 2; ++y) {
      std::memcpy(nv12.ptr<uint8_t>(h + y),
                  src_uv + static_cast<size_t>(y) * src_uv_stride,
                  w);
    }
    gst_video_frame_unmap(&video_frame);
    cv::cvtColor(nv12, rgb, cv::COLOR_YUV2RGB_NV12);
  }

  if (rgb.empty()) {
    return std::nullopt;
  }

  auto rgb_holder = std::make_shared<cv::Mat>(std::move(rgb));
  vision::FrameRef frame;
  frame.camera_id = camera_id;
  frame.pts_ns = pts_ns;
  frame.width = w;
  frame.height = h;
  frame.stride = static_cast<int>(rgb_holder->step[0]);
  frame.pixel_format = vision::PixelFormat::kRgb;
  frame.mapped_ptr = rgb_holder->data;
  frame.bytes_used = rgb_holder->total() * rgb_holder->elemSize();
  frame.owned_data = rgb_holder;
  return frame;
}

}  // namespace rkstudio::media
