#include "rk_studio/media_core/media_engine.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <utility>

#include <QMetaObject>

#include "rk_studio/infra/config_types.h"
#include "rk_studio/infra/gst_audio_recorder.h"
#include "rk_studio/infra/session_files.h"
#include "rk_studio/media_core/session_writer.h"

namespace rkstudio::media {
namespace {

bool Contains(const std::vector<std::string>& items, const std::string& value) {
  return std::find(items.begin(), items.end(), value) != items.end();
}

std::vector<rkinfra::OutputStreamInfo> CollectOutputs(
    const std::map<std::string, std::unique_ptr<V4l2Pipeline>>& cameras,
    const rkinfra::GstAudioRecorder* audio_recorder) {
  std::vector<rkinfra::OutputStreamInfo> outputs;
  for (const auto& [camera_id, pipeline] : cameras) {
    (void)camera_id;
    const std::string path = pipeline->record_output_path();
    if (path.empty()) {
      continue;
    }
    rkinfra::OutputStreamInfo output;
    output.id = pipeline->camera_id();
    output.type = "video";
    output.device = pipeline->resolved_device();
    output.codec = "h265";
    output.output_path = path;
    outputs.push_back(std::move(output));
  }
  if (audio_recorder != nullptr) {
    outputs.push_back(audio_recorder->stream_output());
  }
  return outputs;
}

TelemetryEvent FromInfraStreamEvent(const rkinfra::StreamEvent& event) {
  TelemetryEvent output;
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

}  // namespace

MediaEngine::MediaEngine(QObject* parent) : QObject(parent) {
  qRegisterMetaType<rkstudio::TelemetryEvent>();
}

MediaEngine::~MediaEngine() {
  StopAll();
}

void MediaEngine::LoadBoardConfig(const BoardConfig& board_config) {
  board_config_ = board_config;
}

void MediaEngine::ApplySessionProfile(const SessionProfile& profile) {
  session_profile_ = profile;
}

bool MediaEngine::StartPreview(std::string* err) {
  if (board_config_.cameras.empty()) {
    if (err != nullptr) {
      *err = "board config is empty";
    }
    return false;
  }
  if (session_profile_.preview_cameras.empty()) {
    if (err != nullptr) {
      *err = "session profile preview_cameras is empty";
    }
    return false;
  }

  return RebuildPipelines(false, err);
}

bool MediaEngine::StartRecording(std::string* err) {
  StopPipelines();

  session_writer_ = std::make_unique<SessionWriter>();
  if (!session_writer_->Initialize(board_config_, session_profile_, err)) {
    session_writer_.reset();
    return false;
  }

  if (!RebuildPipelines(true, err)) {
    FinalizeRecording(false);
    return false;
  }

  if (!StartAudioRecorder(err)) {
    FinalizeRecording(false);
    return false;
  }

  const auto outputs = CollectOutputs(cameras_, audio_recorder_.get());
  session_writer_->WriteStartMeta(outputs);
  return true;
}

void MediaEngine::StopPreview() {
  StopPipelines();
}

void MediaEngine::StopRecording(bool ok) {
  FinalizeRecording(ok);
}

void MediaEngine::StopAll() {
  if (session_writer_) {
    FinalizeRecording(true);
  }
  StopPipelines();
}

void MediaEngine::BindPreviewWindow(const std::string& camera_id, WId window_id) {
  preview_window_ids_[camera_id] = window_id;
  auto it = cameras_.find(camera_id);
  if (it != cameras_.end()) {
    it->second->SetPreviewWindow(window_id);
  }
}

void MediaEngine::ObserveTelemetry(const TelemetryEvent& event) {
  EmitTelemetry(event);
}

const BoardConfig& MediaEngine::board_config() const {
  return board_config_;
}

const SessionProfile& MediaEngine::session_profile() const {
  return session_profile_;
}

std::unique_ptr<V4l2Pipeline> MediaEngine::BuildOnePipeline(
    const std::string& camera_id, bool recording, std::string* err) {
  const CameraNodeSet* camera = FindCamera(board_config_, camera_id);
  if (camera == nullptr) {
    if (err != nullptr) {
      *err = "unknown camera id: " + camera_id;
    }
    return nullptr;
  }

  auto pipeline = std::make_unique<V4l2Pipeline>();
  V4l2Pipeline::BuildOptions options;
  options.source.id = camera->id;
  options.source.device = camera->record_device;
  options.source.input_format = camera->input_format;
  options.source.io_mode = camera->io_mode;
  options.source.width = recording ? camera->record_width : camera->preview_width;
  options.source.height = recording ? camera->record_height : camera->preview_height;
  options.source.fps = camera->fps;
  options.source.bitrate = camera->bitrate;
  options.preview.sink_priority = board_config_.sink_priority;
  options.record.session_dir = (session_writer_ && session_writer_->session_paths())
      ? session_writer_->session_paths()->session_dir
      : std::filesystem::path(session_profile_.output_dir);
  if (const auto it = preview_window_ids_.find(camera_id); it != preview_window_ids_.end()) {
    options.preview.window_id = it->second;
  }
  options.preview.enabled = !recording
                             && preview_window_ids_.count(camera_id) > 0
                             && Contains(session_profile_.preview_cameras, camera_id);
  options.record.enabled = recording && Contains(EffectiveRecordCameraIds(session_profile_), camera_id);
  options.record.gop = session_profile_.gop;

  if (!pipeline->Build(
          options, [this](const TelemetryEvent& event) { EmitTelemetry(event); },
          [this, camera_id](const std::string& reason, bool fatal) {
            QMetaObject::invokeMethod(
                this,
                [this, camera_id, reason, fatal] { OnCameraError(camera_id, reason, fatal); },
                Qt::QueuedConnection);
          },
          err)) {
    return nullptr;
  }

  return pipeline;
}

bool MediaEngine::RebuildPipelines(bool recording, std::string* err) {
  StopPipelines();

  const std::vector<std::string> camera_ids =
      recording ? EffectiveRecordCameraIds(session_profile_) : session_profile_.preview_cameras;
  for (const auto& camera_id : camera_ids) {
    auto pipeline = BuildOnePipeline(camera_id, recording, err);
    if (!pipeline || !pipeline->Start(err)) {
      StopPipelines();
      return false;
    }
    cameras_.insert_or_assign(camera_id, std::move(pipeline));
  }

  return true;
}

void MediaEngine::StopPipelines() {
  for (auto& [id, pipeline] : cameras_) {
    (void)id;
    pipeline->Stop();
  }
  cameras_.clear();
}

void MediaEngine::EmitTelemetry(const TelemetryEvent& event) {
  if (session_writer_) {
    session_writer_->WriteEvent(event);

    const bool record_sync_event =
        event.category == "audio" ||
        (event.category == "media" && Contains(EffectiveRecordCameraIds(session_profile_), event.stream_id) &&
         (event.stage == "capture" || event.stage == "queue"));
    if (record_sync_event) {
      session_writer_->RecordSyncEvent(event);
    }
  }
  emit TelemetryObserved(event);
}

void MediaEngine::OnCameraError(const std::string& camera_id, const std::string& reason, bool fatal) {
  emit PreviewCameraFailed(QString::fromStdString(camera_id), QString::fromStdString(reason), fatal);
  if (fatal) {
    emit FatalCameraFailure();
  }
}

void MediaEngine::FinalizeRecording(bool ok) {
  const auto outputs = CollectOutputs(cameras_, audio_recorder_.get());

  StopAudioRecorder();
  StopPipelines();

  if (session_writer_) {
    session_writer_->Finalize(ok, outputs);
    session_writer_.reset();
  }
}

bool MediaEngine::StartAudioRecorder(std::string* err) {
  const auto* config = session_writer_ ? session_writer_->recording_config() : nullptr;
  if (!config || !config->audio.has_value() || !session_writer_->session_paths()) {
    audio_recorder_.reset();
    return true;
  }

  audio_recorder_ = std::make_unique<rkinfra::GstAudioRecorder>(
      *config->audio, config->queue.audio_mux_max_time_ns,
      [this](rkinfra::StreamEvent event) {
        event.category = "audio";
        EmitTelemetry(FromInfraStreamEvent(event));
      },
      session_writer_->session_paths()->session_dir);

  std::string audio_err;
  if (!audio_recorder_->Build(&audio_err) || !audio_recorder_->Start(&audio_err)) {
    if (err) *err = audio_err;
    audio_recorder_.reset();
    return false;
  }
  return true;
}

void MediaEngine::StopAudioRecorder() {
  if (!audio_recorder_) return;
  audio_recorder_->RequestStop();
  audio_recorder_->Stop();
  audio_recorder_.reset();
}

}  // namespace rkstudio::media
