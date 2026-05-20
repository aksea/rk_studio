#pragma once

#include <map>
#include <memory>
#include <string>

#include <QObject>
#include <QtGui/qwindowdefs.h>

#include "rk_studio/domain/types.h"

#ifndef Q_MOC_RUN
#include "rk_studio/domain/session.h"
#include "rk_studio/media_core/v4l2_pipeline.h"
#endif

namespace rkinfra {
class GstAudioRecorder;
struct OutputStreamInfo;
}  // namespace rkinfra

namespace rkstudio::media {

class SessionWriter;
class V4l2Pipeline;

class MediaEngine : public QObject {
  Q_OBJECT

 public:
  explicit MediaEngine(QObject* parent = nullptr);
  ~MediaEngine() override;

  void LoadBoardConfig(const BoardConfig& board_config);
  void ApplySessionProfile(const SessionProfile& profile);
  bool StartPreview(std::string* err);
  bool StartRecording(std::string* err);
  void StopPreview();
  void StopRecording(bool ok = true);
  void StopAll();

  void BindPreviewWindow(const std::string& camera_id, WId window_id);
  void ObserveTelemetry(const TelemetryEvent& event);
  SessionWriter* session_writer() const { return session_writer_.get(); }
  const BoardConfig& board_config() const;
  const SessionProfile& session_profile() const;

 signals:
  void TelemetryObserved(rkstudio::TelemetryEvent event);
  void PreviewCameraFailed(QString camera_id, QString reason, bool fatal);
  void FatalCameraFailure();

 private:
  using CameraMap = std::map<std::string, std::unique_ptr<V4l2Pipeline>>;

  bool RebuildPipelines(bool recording, std::string* err);
  std::unique_ptr<V4l2Pipeline> BuildOnePipeline(
      const std::string& camera_id, bool recording, std::string* err);
  void StopPipelines();
  void EmitTelemetry(const TelemetryEvent& event);
  void OnCameraError(const std::string& camera_id, const std::string& reason, bool fatal);
  void FinalizeRecording(bool ok);
  bool StartAudioRecorder(std::string* err);
  void StopAudioRecorder();

  BoardConfig board_config_;
  SessionProfile session_profile_;
  CameraMap cameras_;
  std::map<std::string, WId> preview_window_ids_;
  std::unique_ptr<SessionWriter> session_writer_;
  std::unique_ptr<rkinfra::GstAudioRecorder> audio_recorder_;
};

}  // namespace rkstudio::media

Q_DECLARE_METATYPE(rkstudio::AppState)
Q_DECLARE_METATYPE(rkstudio::TelemetryEvent)
