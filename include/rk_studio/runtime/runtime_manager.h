#pragma once

#include <string>

#include <QObject>
#include <QtGui/qwindowdefs.h>

#include "rk_studio/domain/types.h"
#include "rk_studio/media_core/media_engine.h"

namespace rkstudio::runtime {

class RuntimeManager : public QObject {
  Q_OBJECT

 public:
  explicit RuntimeManager(QObject* parent = nullptr);
  ~RuntimeManager() override;

  void LoadBoardConfig(const BoardConfig& board_config);
  void ApplySessionProfile(const SessionProfile& profile);
  bool StartPreview(std::string* err);
  bool StartRecording(std::string* err);
  bool CapturePhotos(std::string* output_dir, std::string* err);
  void StopPreview();
  void StopRecording();
  void StopAll();

  void BindPreviewWindow(const std::string& camera_id, WId window_id);

  const BoardConfig& board_config() const;
  const SessionProfile& session_profile() const;
  AppState state() const { return state_; }
  bool preview_running() const;

 signals:
  void StateChanged(rkstudio::AppState state);
  void TelemetryObserved(rkstudio::TelemetryEvent event);
  void PreviewCameraFailed(QString camera_id, QString reason, bool fatal);

 private:
  void SetState(AppState state);
  void EnterErrorState();
  void OnFatalCameraFailure();

  media::MediaEngine* media_engine_ = nullptr;
  AppState state_ = AppState::kIdle;
  bool recording_with_preview_ = false;
};

}  // namespace rkstudio::runtime
