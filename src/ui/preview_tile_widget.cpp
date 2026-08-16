#include "rk_studio/ui/preview_tile_widget.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <utility>

#include <QFont>
#include <QPainter>
#include <QPaintEvent>
#include <QPoint>
#include <QRectF>
#include <QSize>
#include <QTimer>
#include <QVBoxLayout>

namespace rkstudio::ui {
namespace {

constexpr qreal kPreviewAspect = 16.0 / 9.0;

class ResultOverlayWidget final : public QWidget {
 public:
  explicit ResultOverlayWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                   Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
  }

  void SetMediapipeResult(const vision::MediapipeResult& result) {
    mediapipe_result_ = result;
    update();
  }

  void ClearMediapipeResult() {
    mediapipe_result_.reset();
    update();
  }

  void SetYoloResult(const vision::YoloResult& result) {
    yolo_result_ = result;
    update();
  }

  void ClearYoloResult() {
    yolo_result_.reset();
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    DrawMediapipe(painter, rect());
    DrawYolo(painter, rect());
    DrawDebug(painter, rect());
  }

 private:
  static bool DebugOverlayEnabled() {
    return std::getenv("RK_STUDIO_OVERLAY_DEBUG") != nullptr;
  }

  static QRectF VideoContentRect(const QRect& target, int source_w, int source_h) {
    if (source_w <= 0 || source_h <= 0 || target.width() <= 0 || target.height() <= 0) {
      return QRectF(target);
    }
    // ximagesink keeps the source frame anchored at the top-left. Extra area
    // is black when the window is larger; the frame is clipped when smaller.
    return QRectF(target.left(), target.top(), source_w, source_h);
  }

  static QPointF MapPoint(const QRect& target,
                          int source_w,
                          int source_h,
                          float x,
                          float y) {
    const QRectF content = VideoContentRect(target, source_w, source_h);
    const float scale_x = static_cast<float>(content.width()) /
                          static_cast<float>(std::max(1, source_w));
    const float scale_y = static_cast<float>(content.height()) /
                          static_cast<float>(std::max(1, source_h));
    return QPointF(content.left() + x * scale_x, content.top() + y * scale_y);
  }

  std::pair<int, int> CurrentSourceSize() const {
    if (mediapipe_result_.has_value()) {
      return {mediapipe_result_->frame_width, mediapipe_result_->frame_height};
    }
    if (yolo_result_.has_value()) {
      return {yolo_result_->frame_width, yolo_result_->frame_height};
    }
    return {0, 0};
  }

  void DrawDebug(QPainter& painter, const QRect& target) {
    if (!DebugOverlayEnabled()) {
      return;
    }
    const auto [source_w, source_h] = CurrentSourceSize();
    const QRectF content = VideoContentRect(target, source_w, source_h);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 0, 0), 2));
    painter.drawRect(target.adjusted(1, 1, -2, -2));
    painter.setPen(QPen(QColor(0, 220, 255), 2));
    painter.drawRect(content.adjusted(1, 1, -2, -2));

    painter.setFont(QFont(painter.font().family(), 10, QFont::DemiBold));
    const QString label = QString("overlay %1x%2 src %3x%4")
                              .arg(target.width())
                              .arg(target.height())
                              .arg(source_w)
                              .arg(source_h);
    const QRect label_rect = painter.fontMetrics().boundingRect(label).adjusted(-6, -3, 6, 3);
    QRectF label_box(target.left() + 8, target.top() + 8,
                     label_rect.width(), label_rect.height());
    painter.fillRect(label_box, QColor(0, 0, 0, 170));
    painter.setPen(Qt::white);
    painter.drawText(label_box.adjusted(6, 0, -6, 0),
                     Qt::AlignVCenter | Qt::AlignLeft,
                     label);
  }

  void DrawMediapipe(QPainter& painter, const QRect& target) {
    if (!mediapipe_result_.has_value()) {
      return;
    }

    static const QColor kHandColors[] = {QColor(255, 160, 0), QColor(0, 160, 255)};
    static const QColor kLandmarkColors[] = {QColor(0, 255, 100), QColor(255, 100, 200)};

    const int source_w = mediapipe_result_->frame_width;
    const int source_h = mediapipe_result_->frame_height;
    for (size_t h = 0; h < mediapipe_result_->hands.size(); ++h) {
      const auto& hand = mediapipe_result_->hands[h];
      const QColor& roi_color = kHandColors[h % 2];
      const QColor& lm_color = kLandmarkColors[h % 2];

      if (hand.roi.has_value()) {
        painter.setPen(QPen(roi_color, 2));
        painter.setBrush(Qt::NoBrush);
        const auto& roi = *hand.roi;
        const QRectF roi_rect(
            MapPoint(target, source_w, source_h, static_cast<float>(roi.x1), static_cast<float>(roi.y1)),
            MapPoint(target, source_w, source_h, static_cast<float>(roi.x2), static_cast<float>(roi.y2)));
        const QRectF normalized = roi_rect.normalized();
        painter.drawRect(normalized);

        if (!hand.gesture.empty()) {
          painter.setFont(QFont(painter.font().family(), 10, QFont::DemiBold));
          const QString label = QString("%1  %2")
                                    .arg(QString::fromStdString(hand.gesture))
                                    .arg(hand.gesture_score, 0, 'f', 2);
          const QRect label_rect = painter.fontMetrics().boundingRect(label).adjusted(-4, -2, 4, 2);
          QRectF label_box(normalized.left(), normalized.top() - label_rect.height(),
                           label_rect.width(), label_rect.height());
          if (label_box.top() < target.top()) {
            label_box.moveTop(normalized.top());
          }
          painter.fillRect(label_box, roi_color);
          painter.setPen(Qt::black);
          painter.drawText(label_box.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
        }
      }

      painter.setPen(Qt::NoPen);
      painter.setBrush(lm_color);
      for (const auto& point : hand.landmarks) {
        painter.drawEllipse(MapPoint(target, source_w, source_h, point.x, point.y), 3.0, 3.0);
      }
    }
  }

  void DrawYolo(QPainter& painter, const QRect& target) {
    if (!yolo_result_.has_value()) {
      return;
    }

    static const QColor kBoxColors[] = {
        QColor(0, 220, 140),
        QColor(255, 180, 0),
        QColor(0, 180, 255),
        QColor(255, 90, 160),
    };

    painter.setFont(QFont(painter.font().family(), 10, QFont::DemiBold));
    const int source_w = yolo_result_->frame_width;
    const int source_h = yolo_result_->frame_height;
    for (size_t i = 0; i < yolo_result_->detections.size(); ++i) {
      const auto& det = yolo_result_->detections[i];
      const QColor color = kBoxColors[i % 4];
      const QRectF box(
          MapPoint(target, source_w, source_h, static_cast<float>(det.box.x1), static_cast<float>(det.box.y1)),
          MapPoint(target, source_w, source_h, static_cast<float>(det.box.x2), static_cast<float>(det.box.y2)));
      const QRectF normalized = box.normalized();

      painter.setPen(QPen(color, 2));
      painter.setBrush(Qt::NoBrush);
      painter.drawRect(normalized);

      const QString class_name = det.class_name.empty()
                                     ? QString("#%1").arg(det.class_id)
                                     : QString::fromStdString(det.class_name);
      const QString label = QString("%1  %2").arg(class_name).arg(det.score, 0, 'f', 2);
      const QRect label_rect = painter.fontMetrics().boundingRect(label).adjusted(-4, -2, 4, 2);
      QRectF label_box(normalized.left(), normalized.top() - label_rect.height(),
                       label_rect.width(), label_rect.height());
      if (label_box.top() < target.top()) {
        label_box.moveTop(normalized.top());
      }
      painter.fillRect(label_box, color);
      painter.setPen(Qt::black);
      painter.drawText(label_box.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
    }
  }

  std::optional<vision::MediapipeResult> mediapipe_result_;
  std::optional<vision::YoloResult> yolo_result_;
};

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

  // Keep the drawing layer outside the native video window. Painting a
  // translucent QWidget inside ximagesink's window causes X11 expose flicker.
  overlay_ = new ResultOverlayWidget();

  layout->addWidget(title_);
  layout->addWidget(video_container_, 1);
  layout->addWidget(status_);

  QTimer::singleShot(0, this, [this] {
    UpdateVideoGeometry();
    UpdateOverlayGeometry();
  });
}

PreviewTileWidget::~PreviewTileWidget() {
  if (tracked_window_ != nullptr) {
    tracked_window_->removeEventFilter(this);
    tracked_window_ = nullptr;
  }
  delete overlay_;
  overlay_ = nullptr;
}

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

void PreviewTileWidget::UpdateOverlayGeometry() {
  if (overlay_ == nullptr || sink_host_ == nullptr) {
    return;
  }
  UpdateVideoGeometry();
  QWidget* host_window = window();
  if (host_window != tracked_window_) {
    if (tracked_window_ != nullptr) {
      tracked_window_->removeEventFilter(this);
    }
    tracked_window_ = host_window;
    if (tracked_window_ != nullptr) {
      tracked_window_->installEventFilter(this);
    }
  }
  if (!isVisible() || !sink_host_->isVisible() || window() == nullptr ||
      window()->isMinimized()) {
    overlay_->hide();
    return;
  }

  const QRect global_rect(sink_host_->mapToGlobal(QPoint(0, 0)), sink_host_->size());
  if (overlay_->geometry() != global_rect) {
    overlay_->setGeometry(global_rect);
  }
  if (!overlay_->isVisible()) {
    overlay_->show();
  }
  overlay_->raise();
}

void PreviewTileWidget::SetStatusText(const QString& text) {
  status_->setText(text);
}

void PreviewTileWidget::SetMediapipeResult(const vision::MediapipeResult& result) {
  UpdateOverlayGeometry();
  if (auto* overlay = static_cast<ResultOverlayWidget*>(overlay_)) {
    overlay->SetMediapipeResult(result);
  }
}

void PreviewTileWidget::ClearMediapipeResult() {
  UpdateOverlayGeometry();
  if (auto* overlay = static_cast<ResultOverlayWidget*>(overlay_)) {
    overlay->ClearMediapipeResult();
  }
}

void PreviewTileWidget::SetYoloResult(const vision::YoloResult& result) {
  UpdateOverlayGeometry();
  if (auto* overlay = static_cast<ResultOverlayWidget*>(overlay_)) {
    overlay->SetYoloResult(result);
  }
}

void PreviewTileWidget::ClearYoloResult() {
  UpdateOverlayGeometry();
  if (auto* overlay = static_cast<ResultOverlayWidget*>(overlay_)) {
    overlay->ClearYoloResult();
  }
}

bool PreviewTileWidget::eventFilter(QObject* watched, QEvent* event) {
  if ((watched == sink_host_ || watched == video_container_ || watched == this || watched == tracked_window_) &&
      (event->type() == QEvent::Resize ||
       event->type() == QEvent::Move ||
       event->type() == QEvent::Show ||
       event->type() == QEvent::Hide ||
       event->type() == QEvent::WindowStateChange)) {
    QTimer::singleShot(0, this, [this] {
      UpdateOverlayGeometry();
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
