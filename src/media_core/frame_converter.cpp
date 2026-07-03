#include "rk_studio/media_core/frame_converter.h"

#include <atomic>
#include <cstring>
#include <iostream>
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

bool ConvertNv12BufferToRgbCpu(GstBuffer* buffer, const GstVideoInfo& info, cv::Mat* rgb) {
  if (buffer == nullptr || rgb == nullptr ||
      GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12) {
    return false;
  }

  GstVideoFrame frame;
  if (!gst_video_frame_map(&frame, const_cast<GstVideoInfo*>(&info), buffer, GST_MAP_READ)) {
    return false;
  }

  const int width = GST_VIDEO_INFO_WIDTH(&info);
  const int height = GST_VIDEO_INFO_HEIGHT(&info);
  const int y_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
  const int uv_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
  const uint8_t* y_plane = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
  const uint8_t* uv_plane = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
  if (width <= 0 || height <= 0 || y_plane == nullptr || uv_plane == nullptr) {
    gst_video_frame_unmap(&frame);
    return false;
  }

  cv::Mat tight(height * 3 / 2, width, CV_8UC1);
  for (int y = 0; y < height; ++y) {
    std::memcpy(tight.ptr<uint8_t>(y), y_plane + static_cast<size_t>(y) * y_stride, width);
  }
  for (int y = 0; y < height / 2; ++y) {
    std::memcpy(tight.ptr<uint8_t>(height + y), uv_plane + static_cast<size_t>(y) * uv_stride, width);
  }
  gst_video_frame_unmap(&frame);

  cv::cvtColor(tight, *rgb, cv::COLOR_YUV2RGB_NV12);
  return !rgb->empty();
}

}  // namespace

std::optional<vision::FrameRef> FrameConverter::ExtractNv12Frame(
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
  if (frame.dmabuf_fd < 0) {
    return std::nullopt;
  }
  frame.owned_data = HoldSampleRef(sample);
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

  if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12) {
    return std::nullopt;
  }

  cv::Mat rgb;
  static std::atomic_bool disable_rga{false};
  static std::atomic_bool logged_cpu_fallback{false};
  const int fd = ExtractDmabufFd(buffer);
  if (fd >= 0 && !disable_rga.load(std::memory_order_relaxed)) {
    if (!mediapipe_demo::ConvertNv12ToRgb(fd, w, h, stride, &rgb)) {
      disable_rga.store(true, std::memory_order_relaxed);
      if (!logged_cpu_fallback.exchange(true, std::memory_order_relaxed)) {
        std::cerr << "[mediapipe] RGA NV12->RGB failed; using CPU fallback\n";
      }
    }
  }
  if (rgb.empty() && !ConvertNv12BufferToRgbCpu(buffer, info, &rgb)) {
    return std::nullopt;
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
