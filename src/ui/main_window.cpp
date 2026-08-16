#include "rk_studio/ui/main_window.h"

#include <algorithm>
#include <map>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLayoutItem>
#include <QMessageBox>
#include <QStringList>
#include <QVBoxLayout>

#include "rk_studio/domain/config.h"

namespace rkstudio::ui {
namespace {

QString ResolveDefaultConfigPath(const QString& file_name) {
  const QStringList candidates{
      QDir::current().filePath(QStringLiteral("config/%1").arg(file_name)),
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../config/%1").arg(file_name)),
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("config/%1").arg(file_name)),
  };
  for (const QString& candidate : candidates) {
    const QFileInfo info(candidate);
    if (info.exists() && info.isFile()) {
      return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    }
  }
  return candidates.front();
}

void ClearLayoutWidgets(QWidget* container) {
  if (container == nullptr) {
    return;
  }
  QLayout* layout = container->layout();
  if (layout == nullptr) {
    return;
  }
  while (QLayoutItem* item = layout->takeAt(0)) {
    if (QWidget* widget = item->widget()) {
      widget->setParent(nullptr);
      delete widget;
    }
    delete item;
  }
  delete layout;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  runtime_manager_ = new runtime::RuntimeManager(this);

  BuildUi();
  board_config_path_ = ResolveDefaultConfigPath(QStringLiteral("board.toml"));
  profile_path_ = ResolveDefaultConfigPath(QStringLiteral("profile.toml"));
  connect(runtime_manager_, &runtime::RuntimeManager::StateChanged, this, &MainWindow::OnStateChanged);
  connect(runtime_manager_, &runtime::RuntimeManager::TelemetryObserved, this, &MainWindow::OnTelemetryObserved);
  connect(runtime_manager_, &runtime::RuntimeManager::PreviewCameraFailed, this, &MainWindow::OnPreviewFailure);

  if (QFileInfo::exists(board_config_path_) && QFileInfo::exists(profile_path_)) {
    LoadConfigFiles();
  } else {
    SetStatus(QStringLiteral("未找到默认配置，请确认 config 目录位置"));
    AppendLog(QStringLiteral("默认 board 配置: %1").arg(board_config_path_));
    AppendLog(QStringLiteral("默认 profile 配置: %1").arg(profile_path_));
  }
}

MainWindow::~MainWindow() = default;

void MainWindow::BuildUi() {
  resize(1440, 900);
  central_ = new QWidget(this);
  setCentralWidget(central_);

  auto* root = new QHBoxLayout(central_);
  root->setContentsMargins(16, 16, 16, 16);
  root->setSpacing(16);

  auto* side_panel = new QWidget(central_);
  side_panel->setFixedWidth(320);
  auto* side_layout = new QVBoxLayout(side_panel);
  side_layout->setSpacing(12);

  preview_button_ = new QPushButton(QStringLiteral("启动预览"), side_panel);
  record_button_ = new QPushButton(QStringLiteral("启动录制"), side_panel);
  photo_button_ = new QPushButton(QStringLiteral("一键拍照"), side_panel);
  record_button_->setEnabled(false);
  photo_button_->setEnabled(false);

  state_label_ = new QLabel(QStringLiteral("状态: Idle"), side_panel);
  summary_label_ = new QLabel(QStringLiteral("等待配置"), side_panel);
  summary_label_->setWordWrap(true);
  log_view_ = new QPlainTextEdit(side_panel);
  log_view_->setReadOnly(true);
  log_view_->setMaximumBlockCount(1000);

  side_layout->addWidget(preview_button_);
  side_layout->addWidget(record_button_);
  side_layout->addWidget(photo_button_);
  side_layout->addWidget(state_label_);
  side_layout->addWidget(summary_label_);
  side_layout->addWidget(log_view_, 1);

  grid_container_ = new QWidget(central_);
  root->addWidget(side_panel);
  root->addWidget(grid_container_, 1);

  connect(preview_button_, &QPushButton::clicked, this, &MainWindow::TogglePreview);
  connect(record_button_, &QPushButton::clicked, this, &MainWindow::ToggleRecording);
  connect(photo_button_, &QPushButton::clicked, this, &MainWindow::CapturePhotos);
}

void MainWindow::RebuildTiles() {
  ClearLayoutWidgets(grid_container_);
  tiles_.clear();

  const auto& profile = runtime_manager_->session_profile();
  auto* grid = new QGridLayout(grid_container_);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setSpacing(12);

  const int cols = std::max(1, profile.preview_cols);
  for (size_t i = 0; i < profile.preview_cameras.size(); ++i) {
    const QString camera_id = QString::fromStdString(profile.preview_cameras[i]);
    const int row = static_cast<int>(i / cols);
    const int col = static_cast<int>(i % cols);

    auto* tile = new PreviewTileWidget(camera_id, grid_container_);
    connect(tile, &PreviewTileWidget::WindowRebound, this, &MainWindow::OnTileRebound);
    grid->addWidget(tile, row, col);
    tiles_.insert_or_assign(camera_id, tile);
    runtime_manager_->BindPreviewWindow(profile.preview_cameras[i], tile->sink_window_id());
  }
}

void MainWindow::SetStatus(const QString& text) {
  summary_label_->setText(text);
}

void MainWindow::AppendLog(const QString& line) {
  log_view_->appendPlainText(line);
}

void MainWindow::LoadConfigFiles() {
  BoardConfig board_config;
  SessionProfile profile;
  std::string err;
  if (!LoadBoardConfig(board_config_path_.toStdString(), &board_config, &err) ||
      !LoadSessionProfile(profile_path_.toStdString(), &profile, &err)) {
    QMessageBox::critical(this, QStringLiteral("配置错误"), QString::fromStdString(err));
    return;
  }

  if (runtime_manager_->state() != AppState::kIdle) {
    runtime_manager_->StopAll();
  }
  runtime_manager_->LoadBoardConfig(board_config);
  runtime_manager_->ApplySessionProfile(profile);
  RebuildTiles();
  OnStateChanged(runtime_manager_->state());
  SetStatus(QString("已加载 %1 路摄像头").arg(profile.preview_cameras.size()));
}

void MainWindow::TogglePreview() {
  const AppState state = runtime_manager_->state();
  if (state == AppState::kPreviewing ||
      (state == AppState::kRecording && runtime_manager_->preview_running())) {
    runtime_manager_->StopPreview();
    RebuildTiles();
  } else if (state == AppState::kIdle || state == AppState::kRecording) {
    std::string err;
    if (!runtime_manager_->StartPreview(&err)) {
      QMessageBox::warning(this, QStringLiteral("启动失败"), QString::fromStdString(err));
    } else {
      RebuildTiles();
    }
  }
}

void MainWindow::ToggleRecording() {
  if (runtime_manager_->state() == AppState::kRecording) {
    runtime_manager_->StopRecording();
  } else if (runtime_manager_->state() == AppState::kPreviewing ||
             runtime_manager_->state() == AppState::kIdle) {
    std::string err;
    if (!runtime_manager_->StartRecording(&err)) {
      QMessageBox::warning(this, QStringLiteral("录制失败"), QString::fromStdString(err));
    }
  }
}

void MainWindow::CapturePhotos() {
  photo_button_->setText(QStringLiteral("拍照中，请保持棋盘静止…"));
  photo_button_->setEnabled(false);
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

  std::string output_dir;
  std::string err;
  if (!runtime_manager_->CapturePhotos(&output_dir, &err)) {
    QMessageBox::warning(this, QStringLiteral("拍照失败"), QString::fromStdString(err));
    AppendLog(QStringLiteral("[photo] 失败: %1").arg(QString::fromStdString(err)));
    return;
  }

  const QString path = QDir::toNativeSeparators(QString::fromStdString(output_dir));
  SetStatus(QStringLiteral("四路拍照完成：%1").arg(path));
  AppendLog(QStringLiteral("[photo] 已保存 %1").arg(path));
}

void MainWindow::OnStateChanged(rkstudio::AppState state) {
  struct StateRow {
    const char* label;
    QString preview_text;
    bool preview_enabled;
    QString record_text;
    bool record_enabled;
  };

  static const auto kTable = [] {
    std::map<rkstudio::AppState, StateRow> t;
    t[rkstudio::AppState::kIdle] = {
        "Idle",
        QStringLiteral("启动预览"), true,
        QStringLiteral("启动录制"), true};
    t[rkstudio::AppState::kPreviewing] = {
        "Previewing",
        QStringLiteral("关闭预览"), true,
        QStringLiteral("启动录制"), true};
    t[rkstudio::AppState::kCapturing] = {
        "Capturing",
        {}, false,
        {}, false};
    t[rkstudio::AppState::kRecording] = {
        "Recording",
        {}, true,
        QStringLiteral("停止录制"), true};
    t[rkstudio::AppState::kError] = {
        "Error",
        QStringLiteral("启动预览"), false,
        QStringLiteral("启动录制"), false};
    return t;
  }();

  const auto it = kTable.find(state);
  if (it == kTable.end()) return;
  const auto& row = it->second;

  if (state == rkstudio::AppState::kRecording) {
    preview_button_->setText(runtime_manager_->preview_running()
                                 ? QStringLiteral("关闭预览")
                                 : QStringLiteral("启动预览"));
  } else if (!row.preview_text.isEmpty()) {
    preview_button_->setText(row.preview_text);
  }
  preview_button_->setEnabled(row.preview_enabled);
  if (!row.record_text.isEmpty()) record_button_->setText(row.record_text);
  record_button_->setEnabled(row.record_enabled);
  photo_button_->setText(state == rkstudio::AppState::kCapturing
                             ? QStringLiteral("拍照中，请保持棋盘静止…")
                             : QStringLiteral("一键拍照"));
  photo_button_->setEnabled(state == rkstudio::AppState::kIdle ||
                            state == rkstudio::AppState::kPreviewing);
  state_label_->setText(QString("状态: %1").arg(row.label));
}

void MainWindow::OnTelemetryObserved(rkstudio::TelemetryEvent event) {
  if (event.status == "ok") {
    if (++telemetry_ok_skip_counter_ % 30 != 0) {
      return;
    }
  }
  AppendLog(QString("[%1] %2/%3 %4 %5")
                .arg(QString::fromStdString(event.category))
                .arg(QString::fromStdString(event.stream_id))
                .arg(QString::fromStdString(event.stage))
                .arg(QString::fromStdString(event.status))
                .arg(QString::fromStdString(event.reason)));
}

void MainWindow::OnPreviewFailure(QString camera_id, QString reason, bool fatal) {
  if (const auto it = tiles_.find(camera_id); it != tiles_.end()) {
    it->second->SetStatusText(fatal ? QStringLiteral("故障: %1").arg(reason)
                                    : QStringLiteral("预览异常: %1").arg(reason));
  }
}

void MainWindow::OnTileRebound(QString camera_id, WId window_id) {
  runtime_manager_->BindPreviewWindow(camera_id.toStdString(), window_id);
}

}  // namespace rkstudio::ui
