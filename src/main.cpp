#include <cstdlib>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#include <gst/gst.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include "rk_studio/domain/config.h"
#include "rk_studio/runtime/runtime_manager.h"
#include "rk_studio/ui/main_window.h"

namespace {

enum class RunMode {
  kGui,
  kCaptureOnce,
  kCaptureWithPreview,
  kRecordWithPreviewSmoke,
};

QString ResolveDefaultConfigPath(const QString& file_name) {
  const QStringList candidates{
      QDir::current().filePath(QStringLiteral("config/%1").arg(file_name)),
      QDir(QCoreApplication::applicationDirPath())
          .filePath(QStringLiteral("../config/%1").arg(file_name)),
      QDir(QCoreApplication::applicationDirPath())
          .filePath(QStringLiteral("config/%1").arg(file_name)),
  };
  for (const QString& candidate : candidates) {
    const QFileInfo info(candidate);
    if (info.exists() && info.isFile()) {
      return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath()
                                                : info.canonicalFilePath();
    }
  }
  return candidates.front();
}

RunMode RemoveHeadlessArgument(int* argc, char** argv) {
  RunMode mode = RunMode::kGui;
  int write_index = 1;
  for (int read_index = 1; read_index < *argc; ++read_index) {
    if (std::strcmp(argv[read_index], "--capture-once") == 0) {
      mode = RunMode::kCaptureOnce;
      continue;
    }
    if (std::strcmp(argv[read_index], "--capture-with-preview") == 0) {
      mode = RunMode::kCaptureWithPreview;
      continue;
    }
    if (std::strcmp(argv[read_index], "--record-with-preview-smoke") == 0) {
      mode = RunMode::kRecordWithPreviewSmoke;
      continue;
    }
    argv[write_index++] = argv[read_index];
  }
  *argc = write_index;
  argv[write_index] = nullptr;
  return mode;
}

int RunHeadless(int argc, char** argv, RunMode mode) {
  QCoreApplication app(argc, argv);
  rkstudio::BoardConfig board_config;
  rkstudio::SessionProfile profile;
  std::string err;
  if (!rkstudio::LoadBoardConfig(
          ResolveDefaultConfigPath(QStringLiteral("board.toml")).toStdString(),
          &board_config, &err) ||
      !rkstudio::LoadSessionProfile(
          ResolveDefaultConfigPath(QStringLiteral("profile.toml")).toStdString(),
          &profile, &err)) {
    std::cerr << "configuration error: " << err << "\n";
    return 2;
  }

  rkstudio::runtime::RuntimeManager runtime;
  runtime.LoadBoardConfig(board_config);
  runtime.ApplySessionProfile(profile);

  if (mode == RunMode::kCaptureWithPreview ||
      mode == RunMode::kRecordWithPreviewSmoke) {
    if (!runtime.StartPreview(&err)) {
      std::cerr << "preview failed: " << err << "\n";
      return 3;
    }
  }

  if (mode == RunMode::kRecordWithPreviewSmoke) {
    if (!runtime.StartRecording(&err)) {
      std::cerr << "recording failed: " << err << "\n";
      return 4;
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    runtime.StopRecording();
    std::cout << "preview+record smoke complete\n";
    return 0;
  }

  std::string output_dir;
  if (!runtime.CapturePhotos(&output_dir, &err)) {
    std::cerr << "photo capture failed: " << err << "\n";
    return 5;
  }
  std::cout << output_dir << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  setenv("QT_XCB_GL_INTEGRATION", "none", 0);

  const RunMode mode = RemoveHeadlessArgument(&argc, argv);

  gst_init(&argc, &argv);

  if (mode != RunMode::kGui) {
    return RunHeadless(argc, argv, mode);
  }

  QApplication app(argc, argv);
  rkstudio::ui::MainWindow window;
  window.show();
  return app.exec();
}
