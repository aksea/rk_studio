#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "rk_studio/domain/types.h"
#include "rk_studio/domain/session.h"

namespace rkinfra {
class TelemetrySink;
struct SessionPaths;
struct RecordingConfig;
struct OutputStreamInfo;
}  // namespace rkinfra

namespace rkstudio::media {

class SessionWriter {
 public:
  SessionWriter();
  ~SessionWriter();

  SessionWriter(const SessionWriter&) = delete;
  SessionWriter& operator=(const SessionWriter&) = delete;

  bool Initialize(const BoardConfig& board_config,
                  const SessionProfile& profile,
                  std::string* err);

  void WriteEvent(const TelemetryEvent& event);
  bool RecordSyncEvent(const TelemetryEvent& event);

  void WriteStartMeta(const std::vector<rkinfra::OutputStreamInfo>& outputs);
  void Finalize(bool ok, const std::vector<rkinfra::OutputStreamInfo>& outputs);

  bool active() const;
  const rkinfra::SessionPaths* session_paths() const;
  const rkinfra::RecordingConfig* recording_config() const;
  rkinfra::TelemetrySink* telemetry_sink();

 private:
  std::optional<SessionArtifacts> session_artifacts_;
  std::unique_ptr<rkinfra::TelemetrySink> telemetry_sink_;
  std::unique_ptr<rkinfra::SessionPaths> session_paths_;
  std::unique_ptr<rkinfra::RecordingConfig> recording_config_;
  JsonlFileWriter studio_event_writer_;
  std::mutex event_mu_;
  std::string recording_started_utc_;
  uint64_t recording_start_monotonic_ns_ = 0;
  std::string reference_stream_id_;
};

}  // namespace rkstudio::media
