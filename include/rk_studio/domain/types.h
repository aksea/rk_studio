#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rkstudio {

enum class AppState {
  kIdle,
  kPreviewing,
  kRecording,
  kError,
};

struct CameraNodeSet {
  std::string id;
  std::string record_device;
  std::string input_format = "NV12";
  std::string io_mode = "dmabuf";
  int record_width = 1920;
  int record_height = 1080;
  int preview_width = 640;
  int preview_height = 360;
  int fps = 30;
  int bitrate = 8'000'000;
};

struct AudioSource {
  std::string id = "mic0";
  std::string device = "hw:0,0";
  int rate = 16'000;
  int channels = 2;
};

struct BoardConfig {
  std::vector<CameraNodeSet> cameras;
  std::vector<AudioSource> audio_sources;
  std::vector<std::string> sink_priority{"ximagesink", "glimagesink"};
};

struct SessionProfile {
  std::vector<std::string> preview_cameras;
  std::vector<std::string> record_cameras;
  std::string output_dir = "./records";
  std::string prefix = "session";
  std::string audio_source = "mic0";
  int preview_rows = 2;
  int preview_cols = 2;
  int gop = 30;
};

struct TelemetryEvent {
  uint64_t monotonic_ns = 0;
  std::string stream_id;
  uint64_t seq = 0;
  int64_t pts_ns = -1;
  std::string category;
  std::string stage;
  std::string status;
  std::string reason;
};

const CameraNodeSet* FindCamera(const BoardConfig& config, const std::string& id);
const AudioSource* FindAudioSource(const BoardConfig& config, const std::string& id);
std::vector<std::string> EffectiveRecordCameraIds(const SessionProfile& profile);

}  // namespace rkstudio
