#include "rk_studio/runtime/runtime_manager.h"

namespace rkstudio::runtime {

RuntimeManager::RuntimeManager(QObject* parent) : QObject(parent) {
  qRegisterMetaType<rkstudio::AppState>();
  qRegisterMetaType<rkstudio::TelemetryEvent>();

  media_engine_ = new media::MediaEngine(this);

  connect(media_engine_, &media::MediaEngine::TelemetryObserved,
          this, &RuntimeManager::TelemetryObserved);
  connect(media_engine_, &media::MediaEngine::PreviewCameraFailed,
          this, &RuntimeManager::PreviewCameraFailed);
  connect(media_engine_, &media::MediaEngine::FatalCameraFailure,
          this, &RuntimeManager::OnFatalCameraFailure);
}

RuntimeManager::~RuntimeManager() {
  StopAll();
}

void RuntimeManager::LoadBoardConfig(const BoardConfig& board_config) {
  media_engine_->LoadBoardConfig(board_config);
}

void RuntimeManager::ApplySessionProfile(const SessionProfile& profile) {
  media_engine_->ApplySessionProfile(profile);
}

bool RuntimeManager::StartPreview(std::string* err) {
  if (state_ == AppState::kRecording) {
    if (recording_with_preview_) {
      return true;
    }
    if (!media_engine_->StartPreview(err)) {
      return false;
    }
    recording_with_preview_ = true;
    // The primary state remains Recording, but the preview controls changed.
    emit StateChanged(state_);
    return true;
  }

  if (state_ != AppState::kIdle) {
    if (err) *err = "cannot start preview in current state";
    return false;
  }

  if (!media_engine_->StartPreview(err)) {
    EnterErrorState();
    return false;
  }

  SetState(AppState::kPreviewing);
  return true;
}

bool RuntimeManager::StartRecording(std::string* err) {
  if (state_ != AppState::kIdle && state_ != AppState::kPreviewing) {
    if (err) *err = "cannot start recording in current state";
    return false;
  }

  recording_with_preview_ = state_ == AppState::kPreviewing;

  if (!media_engine_->StartRecording(err)) {
    const bool preview_still_running = recording_with_preview_;
    recording_with_preview_ = false;
    SetState(preview_still_running ? AppState::kPreviewing : AppState::kError);
    return false;
  }

  SetState(AppState::kRecording);
  return true;
}

bool RuntimeManager::CapturePhotos(std::string* output_dir, std::string* err) {
  if (state_ != AppState::kIdle && state_ != AppState::kPreviewing) {
    if (err) *err = "cannot capture photos in current state";
    return false;
  }

  const AppState return_state = state_;
  SetState(AppState::kCapturing);

  std::string capture_error;
  const bool captured = media_engine_->CapturePhotos(output_dir, &capture_error);
  SetState(return_state);

  if (!captured) {
    if (err) *err = capture_error;
    return false;
  }
  return true;
}

void RuntimeManager::StopRecording() {
  if (state_ != AppState::kRecording) {
    return;
  }
  media_engine_->StopRecording(true);
  const bool preview_still_running = recording_with_preview_;
  recording_with_preview_ = false;
  SetState(preview_still_running ? AppState::kPreviewing : AppState::kIdle);
}

void RuntimeManager::StopPreview() {
  if (state_ == AppState::kRecording) {
    if (!recording_with_preview_) {
      return;
    }
    media_engine_->StopPreview();
    recording_with_preview_ = false;
    // The primary state remains Recording, but the preview controls changed.
    emit StateChanged(state_);
    return;
  }

  if (state_ != AppState::kPreviewing) {
    return;
  }
  media_engine_->StopPreview();
  SetState(AppState::kIdle);
}

void RuntimeManager::StopAll() {
  media_engine_->StopAll();
  recording_with_preview_ = false;
  SetState(AppState::kIdle);
}

void RuntimeManager::BindPreviewWindow(const std::string& camera_id, WId window_id) {
  media_engine_->BindPreviewWindow(camera_id, window_id);
}

const BoardConfig& RuntimeManager::board_config() const {
  return media_engine_->board_config();
}

const SessionProfile& RuntimeManager::session_profile() const {
  return media_engine_->session_profile();
}

bool RuntimeManager::preview_running() const {
  return state_ == AppState::kPreviewing ||
         (state_ == AppState::kRecording && recording_with_preview_);
}

void RuntimeManager::SetState(AppState state) {
  const bool changed = state_ != state;
  state_ = state;
  if (changed) {
    emit StateChanged(state_);
  }
}

void RuntimeManager::EnterErrorState() {
  SetState(AppState::kError);
}

void RuntimeManager::OnFatalCameraFailure() {
  if (state_ != AppState::kRecording) {
    return;
  }
  media_engine_->StopRecording(false);
  const bool preview_still_running = recording_with_preview_;
  recording_with_preview_ = false;
  SetState(preview_still_running ? AppState::kPreviewing : AppState::kError);
}

}  // namespace rkstudio::runtime
