#pragma once

#include <optional>
#include <string>

#include "rk_studio/vision_core/vision_types.h"

struct _GstSample;
typedef struct _GstSample GstSample;

namespace rkstudio::media {

class FrameConverter {
 public:
  std::optional<vision::FrameRef> ExtractNv12Frame(
      GstSample* sample,
      const std::string& camera_id,
      bool copy_dmabuf = false) const;

  std::optional<vision::FrameRef> ConvertToRgbFrame(
      GstSample* sample,
      const std::string& camera_id) const;
};

}  // namespace rkstudio::media
