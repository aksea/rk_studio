#pragma once

#include <map>

#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>

#include "rk_studio/runtime/runtime_manager.h"
#include "rk_studio/ui/preview_tile_widget.h"

namespace rkstudio::ui {

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 private slots:
  void LoadConfigFiles();
  void TogglePreview();
  void ToggleRecording();
  void OnStateChanged(rkstudio::AppState state);
  void OnTelemetryObserved(rkstudio::TelemetryEvent event);
  void OnPreviewFailure(QString camera_id, QString reason, bool fatal);
  void OnTileRebound(QString camera_id, WId window_id);

 private:
  void BuildUi();
  void RebuildTiles();
  void SetStatus(const QString& text);
  void AppendLog(const QString& line);

  runtime::RuntimeManager* runtime_manager_ = nullptr;
  QString board_config_path_;
  QString profile_path_;

  QWidget* central_ = nullptr;
  QWidget* grid_container_ = nullptr;
  QLabel* state_label_ = nullptr;
  QLabel* summary_label_ = nullptr;
  QPushButton* preview_button_ = nullptr;
  QPushButton* record_button_ = nullptr;
  QPlainTextEdit* log_view_ = nullptr;
  std::map<QString, PreviewTileWidget*> tiles_;
  int telemetry_ok_skip_counter_ = 0;
};

}  // namespace rkstudio::ui
