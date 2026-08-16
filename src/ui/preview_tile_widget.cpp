#include "rk_studio/ui/preview_tile_widget.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QSize>
#include <QTimer>
#include <QVBoxLayout>

namespace rkstudio::ui {
namespace {

constexpr qreal kPreviewAspect = 16.0 / 9.0;

}  // namespace

PreviewTileWidget::PreviewTileWidget(QString camera_id, QWidget* parent)
    : QWidget(parent), camera_id_(std::move(camera_id)) {
  setStyleSheet("PreviewTileWidget { background: #1c1c1c; border: 2px solid #3b3b3b; border-radius: 10px; }");

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(6);

  title_ = new QLabel(camera_id_, this);
  title_->setStyleSheet("font-weight: 600; color: #f3f3f3;");
  status_ = new QLabel(QStringLiteral("未启动"), this);
  status_->setStyleSheet("color: #bdbdbd;");

  video_container_ = new QWidget(this);
  video_container_->setMinimumSize(320, 180);
  video_container_->installEventFilter(this);

  sink_host_ = new QFrame(video_container_);
  sink_host_->setFrameShape(QFrame::StyledPanel);
  sink_host_->setStyleSheet("background: #000;");
  sink_host_->setAttribute(Qt::WA_NativeWindow);
  sink_host_->setAttribute(Qt::WA_DontCreateNativeAncestors);
  sink_host_->setMinimumSize(320, 180);
  sink_host_->installEventFilter(this);
  installEventFilter(this);

  layout->addWidget(title_);
  layout->addWidget(video_container_, 1);
  layout->addWidget(status_);

  QTimer::singleShot(0, this, [this] {
    UpdateVideoGeometry();
  });
}

PreviewTileWidget::~PreviewTileWidget() = default;

QString PreviewTileWidget::camera_id() const {
  return camera_id_;
}

WId PreviewTileWidget::sink_window_id() {
  return sink_host_->winId();
}

void PreviewTileWidget::RebindSinkWindow() {
  const WId window_id = sink_host_->winId();
  QTimer::singleShot(0, this, [this, window_id] {
    if (sink_host_ != nullptr && sink_host_->winId() == window_id) {
      emit WindowRebound(camera_id_, window_id);
    }
  });
}

void PreviewTileWidget::UpdateVideoGeometry() {
  if (video_container_ == nullptr || sink_host_ == nullptr) {
    return;
  }
  const QSize size = video_container_->size();
  if (size.width() <= 0 || size.height() <= 0) {
    return;
  }

  int video_w = size.width();
  int video_h = static_cast<int>(std::round(static_cast<qreal>(video_w) / kPreviewAspect));
  if (video_h > size.height()) {
    video_h = size.height();
    video_w = static_cast<int>(std::round(static_cast<qreal>(video_h) * kPreviewAspect));
  }
  video_w = std::max(1, video_w);
  video_h = std::max(1, video_h);
  const int x = (size.width() - video_w) / 2;
  const int y = (size.height() - video_h) / 2;
  const QRect video_rect(x, y, video_w, video_h);
  if (sink_host_->geometry() != video_rect) {
    sink_host_->setGeometry(video_rect);
  }
}

void PreviewTileWidget::SetStatusText(const QString& text) {
  status_->setText(text);
}

bool PreviewTileWidget::eventFilter(QObject* watched, QEvent* event) {
  if ((watched == sink_host_ || watched == video_container_ || watched == this) &&
      (event->type() == QEvent::Resize ||
       event->type() == QEvent::Move ||
       event->type() == QEvent::Show ||
       event->type() == QEvent::Hide ||
       event->type() == QEvent::WindowStateChange)) {
    QTimer::singleShot(0, this, [this] {
      UpdateVideoGeometry();
    });
  }
  if (watched == sink_host_ &&
      (event->type() == QEvent::WinIdChange ||
       event->type() == QEvent::Show)) {
    RebindSinkWindow();
    return false;
  }
  return QWidget::eventFilter(watched, event);
}

}  // namespace rkstudio::ui
