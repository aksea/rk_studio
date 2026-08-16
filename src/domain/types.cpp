#include "rk_studio/domain/types.h"

#include <algorithm>

namespace rkstudio {

const CameraNodeSet* FindCamera(const BoardConfig& config, const std::string& id) {
  const auto it =
      std::find_if(config.cameras.begin(), config.cameras.end(), [&](const CameraNodeSet& camera) {
        return camera.id == id;
      });
  return it == config.cameras.end() ? nullptr : &(*it);
}

const AudioSource* FindAudioSource(const BoardConfig& config, const std::string& id) {
  const auto it =
      std::find_if(config.audio_sources.begin(), config.audio_sources.end(), [&](const AudioSource& source) {
        return source.id == id;
      });
  return it == config.audio_sources.end() ? nullptr : &(*it);
}

std::vector<std::string> EffectiveRecordCameraIds(const SessionProfile& profile) {
  return profile.record_cameras.empty() ? profile.preview_cameras : profile.record_cameras;
}

}  // namespace rkstudio
