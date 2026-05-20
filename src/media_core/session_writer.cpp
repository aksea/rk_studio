#include "rk_studio/media_core/session_writer.h"

#include <iostream>
#include <sstream>
#include <utility>

#include "rk_studio/infra/config_types.h"
#include "rk_studio/infra/runtime.h"
#include "rk_studio/infra/session_files.h"
#include "rk_studio/infra/telemetry.h"

namespace rkstudio::media {
namespace {

constexpr size_t kTelemetryQueueSize = 50'000;

rkinfra::AudioConfig ToInfraAudioConfig(const AudioSource& source) {
  rkinfra::AudioConfig config;
  config.id = source.id;
  config.device = source.device;
  config.rate = source.rate;
  config.channels = source.channels;
  return config;
}

rkinfra::StreamEvent ToInfraStreamEvent(const TelemetryEvent& event) {
  rkinfra::StreamEvent output;
  output.monotonic_ns = event.monotonic_ns;
  output.stream_id = event.stream_id;
  output.seq = event.seq;
  output.pts_ns = event.pts_ns;
  output.category = event.category;
  output.stage = event.stage;
  output.status = event.status;
  output.reason = event.reason;
  return output;
}

rkinfra::RecordingConfig BuildRecordingConfig(const BoardConfig& board_config, const SessionProfile& profile) {
  rkinfra::RecordingConfig config;
  config.record.output_dir = profile.output_dir;
  config.record.prefix = profile.prefix;
  config.encoder.gop = profile.gop;

  for (const auto& camera_id : EffectiveRecordCameraIds(profile)) {
    const CameraNodeSet* camera = FindCamera(board_config, camera_id);
    if (camera == nullptr) {
      continue;
    }
    rkinfra::VideoStreamConfig stream;
    stream.id = camera->id;
    stream.device = camera->record_device;
    stream.width = camera->record_width;
    stream.height = camera->record_height;
    stream.fps = camera->fps;
    stream.bitrate = camera->bitrate;
    stream.gop = profile.gop;
    stream.queue_capture_buffers = config.queue.video_capture_buffers;
    stream.queue_mux_max_time_ns = config.queue.video_mux_max_time_ns;
    stream.io_mode = camera->io_mode;
    stream.input_format = camera->input_format;
    config.video_streams.push_back(std::move(stream));
  }

  if (const AudioSource* audio = FindAudioSource(board_config, profile.audio_source)) {
    config.audio = ToInfraAudioConfig(*audio);
  }
  return config;
}

std::vector<std::string> BuildEnabledStreamIds(const rkinfra::RecordingConfig& config) {
  std::vector<std::string> ids;
  for (const auto& video : config.video_streams) {
    ids.push_back(video.id);
  }
  if (config.audio) {
    ids.push_back(config.audio->id);
  }
  return ids;
}

std::string BuildReferenceStreamId(const rkinfra::RecordingConfig& config) {
  if (!config.video_streams.empty()) {
    return config.video_streams.front().id;
  }
  if (config.audio) {
    return config.audio->id;
  }
  return {};
}

std::string TelemetryToJson(const TelemetryEvent& event) {
  std::ostringstream o;
  o << "{\"monotonic_ns\":" << event.monotonic_ns
    << ",\"stream_id\":\"" << rkinfra::JsonEscape(event.stream_id)
    << "\",\"seq\":" << event.seq
    << ",\"pts_ns\":" << event.pts_ns
    << ",\"category\":\"" << rkinfra::JsonEscape(event.category)
    << "\",\"stage\":\"" << rkinfra::JsonEscape(event.stage)
    << "\",\"status\":\"" << rkinfra::JsonEscape(event.status)
    << "\",\"reason\":\"" << rkinfra::JsonEscape(event.reason)
    << "\"}";
  return o.str();
}

}  // namespace

SessionWriter::SessionWriter() = default;

SessionWriter::~SessionWriter() {
  if (active()) {
    Finalize(false, {});
  }
}

bool SessionWriter::Initialize(const BoardConfig& board_config,
                               const SessionProfile& profile,
                               std::string* err) {
  recording_config_ = std::make_unique<rkinfra::RecordingConfig>(BuildRecordingConfig(board_config, profile));
  session_paths_ = std::make_unique<rkinfra::SessionPaths>(rkinfra::CreateSessionPaths(recording_config_->record));
  if (!rkinfra::EnsureSessionDirectory(*session_paths_, err)) {
    return false;
  }

  session_artifacts_ = SessionArtifacts{session_paths_->session_id, session_paths_->session_dir,
                                        session_paths_->session_dir / "studio.events.jsonl"};
  if (!studio_event_writer_.Open(session_artifacts_->studio_event_path, err)) {
    return false;
  }

  reference_stream_id_ = BuildReferenceStreamId(*recording_config_);
  telemetry_sink_ = std::make_unique<rkinfra::TelemetrySink>(
      kTelemetryQueueSize, session_paths_->sidecar_path.string(), recording_config_->sync,
      BuildEnabledStreamIds(*recording_config_),
      reference_stream_id_);
  if (!telemetry_sink_->Start(err)) {
    return false;
  }
  recording_started_utc_ = rkinfra::NowUtcIso8601();
  recording_start_monotonic_ns_ = rkinfra::ClockMonotonicNs();
  return true;
}

void SessionWriter::WriteEvent(const TelemetryEvent& event) {
  std::lock_guard<std::mutex> lock(event_mu_);
  if (session_artifacts_.has_value()) {
    studio_event_writer_.WriteLine(TelemetryToJson(event));
  }
}

bool SessionWriter::RecordSyncEvent(const TelemetryEvent& event) {
  if (telemetry_sink_) {
    telemetry_sink_->Record(ToInfraStreamEvent(event));
    return true;
  }
  return false;
}

void SessionWriter::WriteStartMeta(const std::vector<rkinfra::OutputStreamInfo>& outputs) {
  if (session_paths_ && recording_config_) {
    rkinfra::WriteSessionMeta(*session_paths_, *recording_config_, "starting", recording_started_utc_,
                              recording_start_monotonic_ns_, 0ULL, outputs, reference_stream_id_);
  }
}

void SessionWriter::Finalize(bool ok, const std::vector<rkinfra::OutputStreamInfo>& outputs) {
  if (telemetry_sink_ && session_paths_) {
    telemetry_sink_->Stop();
    rkinfra::WriteSyncReport(session_paths_->sync_path, telemetry_sink_->BuildSyncReport());
  }
  if (session_paths_ && recording_config_) {
    rkinfra::WriteSessionMeta(*session_paths_, *recording_config_, ok ? "stopped" : "error", recording_started_utc_,
                              recording_start_monotonic_ns_, 0ULL, outputs, reference_stream_id_);
  }

  telemetry_sink_.reset();
  recording_config_.reset();
  session_paths_.reset();
  session_artifacts_.reset();
  studio_event_writer_.Close();
}

bool SessionWriter::active() const {
  return session_artifacts_.has_value();
}

const rkinfra::SessionPaths* SessionWriter::session_paths() const {
  return session_paths_.get();
}

const rkinfra::RecordingConfig* SessionWriter::recording_config() const {
  return recording_config_.get();
}

rkinfra::TelemetrySink* SessionWriter::telemetry_sink() {
  return telemetry_sink_.get();
}

}  // namespace rkstudio::media
