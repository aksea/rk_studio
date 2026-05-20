#include "rk_studio/media_core/vision_engine.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <utility>

#include <gst/gst.h>
#include <opencv2/imgproc.hpp>
#include <QMetaObject>

#include "rk_studio/infra/runtime.h"
#include "rk_studio/infra/session_files.h"
#include "rk_studio/infra/zenoh_publisher.h"
#include "rk_studio/media_core/frame_orientation.h"
#include "rk_studio/media_core/session_writer.h"
#include "rk_studio/vision_core/vision_processor.h"

namespace rkstudio::media {
namespace {

constexpr int kMediapipeFps = 15;
constexpr int kDefaultYoloFps = 5;
constexpr int kDefaultFaceExpressionFps = 10;
constexpr float kYoloPublishMinScore = 0.7f;

bool CanRunVisionPipeline(AppState state) {
  return state == AppState::kIdle ||
         state == AppState::kPreviewing ||
         state == AppState::kStreaming ||
         state == AppState::kRecording;
}

std::string DeriveSelfpathDevice(const std::string& mainpath_device, std::string* err) {
  const std::string prefix = "/dev/video";
  if (mainpath_device.rfind(prefix, 0) != 0 || mainpath_device.size() == prefix.size()) {
    if (err) *err = "cannot derive selfpath from camera device: " + mainpath_device;
    return {};
  }

  const std::string number = mainpath_device.substr(prefix.size());
  if (!std::all_of(number.begin(), number.end(),
                   [](unsigned char ch) { return std::isdigit(ch); })) {
    if (err) *err = "cannot derive selfpath from camera device: " + mainpath_device;
    return {};
  }

  return prefix + std::to_string(std::stoi(number) + 1);
}

std::string MediapipeResultToJson(const rkstudio::vision::MediapipeResult& r) {
  std::ostringstream o;
  o << "{\"camera_id\":\"" << rkinfra::JsonEscape(r.camera_id)
    << "\",\"pts_ns\":" << r.pts_ns
    << ",\"hands\":[";
  for (size_t h = 0; h < r.hands.size(); ++h) {
    if (h > 0) o << ',';
    const auto& hand = r.hands[h];
    o << "{\"id\":" << hand.hand_id;
    if (!hand.gesture.empty()) {
      o << ",\"gesture\":\"" << rkinfra::JsonEscape(hand.gesture) << "\"";
    }
    o << '}';
  }
  o << "]}";
  return o.str();
}

std::string YoloResultToJson(const rkstudio::vision::YoloResult& r) {
  std::ostringstream o;
  o << "{\"camera_id\":\"" << rkinfra::JsonEscape(r.camera_id)
    << "\",\"pts_ns\":" << r.pts_ns
    << ",\"size\":[" << r.frame_width << ',' << r.frame_height << ']'
    << ",\"objects\":[";
  for (size_t i = 0; i < r.detections.size(); ++i) {
    if (i > 0) o << ',';
    const auto& det = r.detections[i];
    o << "{\"class_id\":" << det.class_id;
    if (!det.class_name.empty()) {
      o << ",\"class_name\":\"" << rkinfra::JsonEscape(det.class_name) << '"';
    }
    o << ",\"score\":" << det.score
      << ",\"box\":[" << det.box.x1 << ',' << det.box.y1
      << ',' << det.box.x2 << ',' << det.box.y2 << "]}";
  }
  o << "]}";
  return o.str();
}

std::string FaceExpressionResultToJson(const rkstudio::vision::FaceExpressionResult& r) {
  std::ostringstream o;
  o << "{\"camera_id\":\"" << rkinfra::JsonEscape(r.camera_id)
    << "\",\"pts_ns\":" << r.pts_ns
    << ",\"size\":[" << r.frame_width << ',' << r.frame_height << ']'
    << ",\"faces\":[";
  for (size_t i = 0; i < r.faces.size(); ++i) {
    if (i > 0) o << ',';
    const auto& face = r.faces[i];
    o << "{\"id\":" << face.face_id
      << ",\"box\":[" << face.box.x1 << ',' << face.box.y1
      << ',' << face.box.x2 << ',' << face.box.y2 << ']'
      << ",\"expression\":\"" << rkinfra::JsonEscape(face.expression) << "\""
      << ",\"score\":" << face.expression_score
      << ",\"scores\":[";
    for (size_t s = 0; s < face.expression_scores.size(); ++s) {
      if (s > 0) o << ',';
      const auto& score = face.expression_scores[s];
      o << "{\"label\":\"" << rkinfra::JsonEscape(score.label)
        << "\",\"score\":" << score.score << '}';
    }
    o << "],\"landmarks\":[";
    for (size_t p = 0; p < face.landmarks.size(); ++p) {
      if (p > 0) o << ',';
      const auto& point = face.landmarks[p];
      o << '[' << point.x << ',' << point.y << ']';
    }
    o << "],\"aus\":[";
    for (size_t a = 0; a < face.action_units.size(); ++a) {
      if (a > 0) o << ',';
      const auto& au = face.action_units[a];
      o << "{\"name\":\"" << rkinfra::JsonEscape(au.name)
        << "\",\"score\":" << au.score << '}';
    }
    o << "]}";
  }
  o << "]}";
  return o.str();
}

rkstudio::vision::YoloResult FilterYoloResultForOutput(
    const rkstudio::vision::YoloResult& input) {
  rkstudio::vision::YoloResult output = input;
  output.detections.clear();
  for (const auto& det : input.detections) {
    if (det.score > kYoloPublishMinScore) {
      output.detections.push_back(det);
    }
  }
  return output;
}

std::optional<vision::FrameRef> RotateRgbFrame(
    std::optional<vision::FrameRef> frame,
    const std::string& orientation) {
  if (!frame.has_value() || !IsOriented(orientation) ||
      frame->pixel_format != vision::PixelFormat::kRgb ||
      frame->mapped_ptr == nullptr) {
    return frame;
  }

  cv::Mat input(frame->height, frame->width, CV_8UC3,
                const_cast<uint8_t*>(frame->mapped_ptr),
                frame->stride > 0 ? frame->stride : frame->width * 3);
  cv::Mat oriented = ApplyMatOrientation(input, orientation);
  if (oriented.empty()) {
    return frame;
  }

  auto holder = std::make_shared<cv::Mat>(std::move(oriented));
  frame->width = holder->cols;
  frame->height = holder->rows;
  frame->stride = static_cast<int>(holder->step[0]);
  frame->mapped_ptr = holder->data;
  frame->bytes_used = holder->total() * holder->elemSize();
  frame->dmabuf_fd = -1;
  frame->owned_data = holder;
  return frame;
}

std::optional<vision::FrameRef> RgbFrameToBgr(
    std::optional<vision::FrameRef> frame) {
  if (!frame.has_value() ||
      frame->pixel_format != vision::PixelFormat::kRgb ||
      frame->mapped_ptr == nullptr) {
    return frame;
  }
  cv::Mat rgb(frame->height, frame->width, CV_8UC3,
              const_cast<uint8_t*>(frame->mapped_ptr),
              frame->stride > 0 ? frame->stride : frame->width * 3);
  cv::Mat bgr;
  cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
  auto holder = std::make_shared<cv::Mat>(std::move(bgr));
  frame->stride = static_cast<int>(holder->step[0]);
  frame->pixel_format = vision::PixelFormat::kBgr;
  frame->mapped_ptr = holder->data;
  frame->bytes_used = holder->total() * holder->elemSize();
  frame->dmabuf_fd = -1;
  frame->owned_data = holder;
  return frame;
}

}  // namespace

VisionEngine::VisionEngine(QObject* parent) : QObject(parent) {
  qRegisterMetaType<rkstudio::vision::MediapipeResult>();
  qRegisterMetaType<rkstudio::vision::YoloResult>();
  qRegisterMetaType<rkstudio::vision::FaceExpressionResult>();

  mediapipe_poll_timer_ = new QTimer(this);
  mediapipe_poll_timer_->setInterval(30);
  connect(mediapipe_poll_timer_, &QTimer::timeout, this, &VisionEngine::PollMediapipeResults);

  yolo_poll_timer_ = new QTimer(this);
  yolo_poll_timer_->setInterval(50);
  connect(yolo_poll_timer_, &QTimer::timeout, this, &VisionEngine::PollYoloResults);

  face_expression_poll_timer_ = new QTimer(this);
  face_expression_poll_timer_->setInterval(50);
  connect(face_expression_poll_timer_, &QTimer::timeout, this, &VisionEngine::PollFaceExpressionResults);
}

VisionEngine::~VisionEngine() {
  StopAll();
}

void VisionEngine::LoadBoardConfig(const BoardConfig& board_config) {
  board_config_ = board_config;
}

void VisionEngine::ApplySessionProfile(const SessionProfile& profile) {
  session_profile_ = profile;
}

void VisionEngine::SetState(AppState state) {
  state_ = state;
}

void VisionEngine::SetSessionWriter(SessionWriter* session_writer) {
  session_writer_ = session_writer;
}

void VisionEngine::SetZenohPublisher(rkinfra::ZenohPublisher* zenoh_publisher) {
  zenoh_publisher_ = zenoh_publisher;
}

void VisionEngine::SetCallbacks(Callbacks callbacks) {
  callbacks_ = std::move(callbacks);
}

bool VisionEngine::ToggleMediapipe(bool enable, std::string* err) {
  const std::string mediapipe_cam_id = session_profile_.selected_mediapipe_camera;
  if (mediapipe_cam_id.empty()) {
    if (err) *err = "no Mediapipe camera configured";
    return false;
  }
  if (enable && yolo_enabled_ && mediapipe_cam_id == yolo_camera_id_) {
    if (err) *err = "Mediapipe and YOLO cannot use the same selfpath camera";
    return false;
  }
  if (enable && face_expression_enabled_ && mediapipe_cam_id == face_expression_camera_id_) {
    if (err) *err = "Mediapipe and face expression cannot use the same selfpath camera";
    return false;
  }

  mediapipe_enabled_ = enable;

  if (enable) {
    if (!mediapipe_processor_) {
      if (!StartMediapipeProcessor()) {
        mediapipe_enabled_ = false;
        if (err) *err = "failed to start Mediapipe processor";
        return false;
      }
    }
    if (CanRunVisionPipeline(state_)) {
      if (!StartMediapipePipeline(err)) {
        mediapipe_enabled_ = false;
        StopMediapipeProcessor();
        return false;
      }
    }
    if (session_writer_ && session_writer_->session_paths()) {
      session_writer_->OpenMediapipeWriter(nullptr);
    }
  } else {
    mediapipe_poll_timer_->stop();
    StopMediapipePipeline();
    StopMediapipeProcessor();
  }

  if (enable && mediapipe_processor_) {
    mediapipe_poll_timer_->start();
  }

  return true;
}

bool VisionEngine::ToggleYolo(bool enable, std::string* err) {
  const std::string yolo_cam_id = session_profile_.selected_yolo_camera;
  if (yolo_cam_id.empty()) {
    if (err) *err = "no YOLO camera configured";
    return false;
  }
  if (enable && mediapipe_enabled_ && yolo_cam_id == mediapipe_camera_id_) {
    if (err) *err = "Mediapipe and YOLO cannot use the same selfpath camera";
    return false;
  }
  if (enable && face_expression_enabled_ && yolo_cam_id == face_expression_camera_id_) {
    if (err) *err = "YOLO and face expression cannot use the same selfpath camera";
    return false;
  }

  yolo_enabled_ = enable;
  if (enable) {
    if (!yolo_processor_ && !StartYoloProcessor()) {
      yolo_enabled_ = false;
      if (err) *err = "failed to start YOLO processor";
      return false;
    }
    if (CanRunVisionPipeline(state_)) {
      if (!StartYoloPipeline(err)) {
        yolo_enabled_ = false;
        StopYoloProcessor();
        return false;
      }
    }
    if (yolo_processor_) {
      if (session_writer_ && session_writer_->session_paths()) {
        session_writer_->OpenYoloWriter(nullptr);
      }
      yolo_poll_timer_->start();
    }
  } else {
    yolo_poll_timer_->stop();
    StopYoloPipeline();
    StopYoloProcessor();
  }
  return true;
}

bool VisionEngine::ToggleFaceExpression(bool enable, std::string* err) {
  const std::string face_cam_id = session_profile_.selected_face_camera;
  if (face_cam_id.empty()) {
    if (err) *err = "no face expression camera configured";
    return false;
  }
  if (enable && mediapipe_enabled_ && face_cam_id == mediapipe_camera_id_) {
    if (err) *err = "face expression and Mediapipe cannot use the same selfpath camera";
    return false;
  }
  if (enable && yolo_enabled_ && face_cam_id == yolo_camera_id_) {
    if (err) *err = "face expression and YOLO cannot use the same selfpath camera";
    return false;
  }

  face_expression_enabled_ = enable;
  if (enable) {
    if (!face_expression_processor_ && !StartFaceExpressionProcessor()) {
      face_expression_enabled_ = false;
      if (err) *err = "failed to start face expression processor";
      return false;
    }
    if (CanRunVisionPipeline(state_)) {
      if (!StartFaceExpressionPipeline(err)) {
        face_expression_enabled_ = false;
        StopFaceExpressionProcessor();
        return false;
      }
    }
    if (face_expression_processor_) {
      if (session_writer_ && session_writer_->session_paths()) {
        session_writer_->OpenFaceExpressionWriter(nullptr);
      }
      face_expression_poll_timer_->start();
    }
  } else {
    face_expression_poll_timer_->stop();
    StopFaceExpressionPipeline();
    StopFaceExpressionProcessor();
  }
  return true;
}

bool VisionEngine::SyncForState(AppState state, std::string* err) {
  state_ = state;

  if (mediapipe_processor_ && session_writer_ && session_writer_->session_paths()) {
    session_writer_->OpenMediapipeWriter(nullptr);
  }
  if (yolo_processor_ && session_writer_ && session_writer_->session_paths()) {
    session_writer_->OpenYoloWriter(nullptr);
  }
  if (face_expression_processor_ && session_writer_ && session_writer_->session_paths()) {
    session_writer_->OpenFaceExpressionWriter(nullptr);
  }
  if (mediapipe_processor_) {
    if (!mediapipe_pipeline_) {
      if (!StartMediapipePipeline(err)) {
        StopPipelines();
        return false;
      }
    }
    mediapipe_poll_timer_->start();
  }
  if (yolo_processor_) {
    if (!yolo_pipeline_) {
      if (!StartYoloPipeline(err)) {
        StopPipelines();
        return false;
      }
    }
    yolo_poll_timer_->start();
  }
  if (face_expression_processor_) {
    if (!face_expression_pipeline_) {
      if (!StartFaceExpressionPipeline(err)) {
        StopPipelines();
        return false;
      }
    }
    face_expression_poll_timer_->start();
  }
  return true;
}

void VisionEngine::StopPipelines() {
  mediapipe_poll_timer_->stop();
  yolo_poll_timer_->stop();
  face_expression_poll_timer_->stop();
  StopMediapipePipeline();
  StopYoloPipeline();
  StopFaceExpressionPipeline();
}

void VisionEngine::StopAll() {
  mediapipe_enabled_ = false;
  yolo_enabled_ = false;
  face_expression_enabled_ = false;
  StopMediapipeProcessor();
  StopYoloProcessor();
  StopFaceExpressionProcessor();
  StopPipelines();
}

std::unique_ptr<V4l2Pipeline> VisionEngine::BuildMediapipePipeline(std::string* err) {
  if (!mediapipe_processor_ || mediapipe_camera_id_.empty()) {
    if (err) *err = "Mediapipe processor is not running";
    return nullptr;
  }
  return BuildVisionPipeline(
      mediapipe_camera_id_, "_mediapipe", kMediapipeFps,
      [this](GstSample* sample) { OnMediapipeSample(sample); }, err);
}

std::unique_ptr<V4l2Pipeline> VisionEngine::BuildVisionPipeline(
    const std::string& camera_id,
    const std::string& suffix,
    int fps,
    std::function<void(GstSample*)> sample_callback,
    std::string* err) {
  const CameraNodeSet* camera = FindCamera(board_config_, camera_id);
  if (camera == nullptr) {
    if (err) *err = "unknown vision camera id: " + camera_id;
    return nullptr;
  }

  const std::string selfpath_device = DeriveSelfpathDevice(camera->record_device, err);
  if (selfpath_device.empty()) {
    return nullptr;
  }

  auto pipeline = std::make_unique<V4l2Pipeline>();
  V4l2Pipeline::BuildOptions options;
  options.source.id = camera->id + suffix;
  options.source.device = selfpath_device;
  options.source.input_format = camera->input_format;
  options.source.io_mode = camera->io_mode;
  options.source.width = camera->preview_width;
  options.source.height = camera->preview_height;
  options.source.fps = fps;
  options.source.bitrate = camera->bitrate;
  options.record.session_dir = (session_writer_ && session_writer_->session_paths())
      ? session_writer_->session_paths()->session_dir
      : std::filesystem::path(session_profile_.output_dir);
  options.app_sink.enabled = true;
  options.app_sink.sample_callback = std::move(sample_callback);

  if (!pipeline->Build(
          options,
          [this](const TelemetryEvent& event) {
            if (callbacks_.emit_telemetry) callbacks_.emit_telemetry(event);
          },
          [this, camera_id](const std::string& reason, bool fatal) {
            QMetaObject::invokeMethod(
                this,
                [this, camera_id, reason, fatal] {
                  if (callbacks_.camera_error) {
                    callbacks_.camera_error(camera_id, reason, fatal);
                  }
                },
                Qt::QueuedConnection);
          },
          err)) {
    return nullptr;
  }

  return pipeline;
}

bool VisionEngine::StartMediapipePipeline(std::string* err) {
  StopMediapipePipeline();
  auto pipeline = BuildMediapipePipeline(err);
  if (!pipeline || !pipeline->Start(err)) {
    StopMediapipePipeline();
    return false;
  }
  mediapipe_pipeline_ = std::move(pipeline);
  std::cerr << "[mediapipe] capture pipeline started for " << mediapipe_camera_id_ << "\n";
  return true;
}

void VisionEngine::StopMediapipePipeline() {
  if (mediapipe_pipeline_) {
    mediapipe_pipeline_->Stop();
    mediapipe_pipeline_.reset();
    std::cerr << "[mediapipe] capture pipeline stopped\n";
  }
}

bool VisionEngine::StartMediapipeProcessor() {
  mediapipe_camera_id_.clear();
  const std::string& mediapipe_cam = session_profile_.selected_mediapipe_camera;
  if (!mediapipe_enabled_ || mediapipe_cam.empty() || !board_config_.mediapipe.has_value()) {
    return false;
  }

  const auto& mediapipe_hw = *board_config_.mediapipe;

  vision::MediapipeProcessorConfig config;
  config.detector_model = mediapipe_hw.detector_model;
  config.landmark_model = mediapipe_hw.landmark_model;
  config.queue_depth = 1;

  mediapipe_processor_ = vision::CreateMediapipeProcessor();
  std::string mediapipe_err;
  if (!mediapipe_processor_->Start(config, &mediapipe_err)) {
    std::cerr << "[mediapipe] failed to start processor: " << mediapipe_err << "\n";
    mediapipe_processor_.reset();
    return false;
  }

  mediapipe_camera_id_ = mediapipe_cam;
  std::cerr << "[mediapipe] processor started for " << mediapipe_camera_id_ << "\n";
  return true;
}

void VisionEngine::StopMediapipeProcessor() {
  mediapipe_poll_timer_->stop();
  StopMediapipePipeline();
  mediapipe_logged_path_ = false;
  if (mediapipe_processor_) {
    mediapipe_processor_->Stop();
    mediapipe_processor_.reset();
    std::cerr << "[mediapipe] processor stopped\n";
  }
  {
    std::lock_guard<std::mutex> lock(mediapipe_frame_mu_);
    mediapipe_camera_id_.clear();
  }
}

void VisionEngine::OnMediapipeSample(GstSample* sample) {
  if (!mediapipe_processor_ || !sample) {
    return;
  }

  std::string camera_id;
  {
    std::lock_guard<std::mutex> lock(mediapipe_frame_mu_);
    camera_id = mediapipe_camera_id_;
  }
  auto rgb_frame = frame_converter_.ConvertToRgbFrame(sample, camera_id);
  rgb_frame = RotateRgbFrame(std::move(rgb_frame), CameraOrientation(camera_id));
  if (!rgb_frame.has_value()) {
    return;
  }

  if (!mediapipe_logged_path_) {
    std::cerr << "[mediapipe] NV12->RGB path: RGA hardware\n";
    mediapipe_logged_path_ = true;
  }

  vision::VisionFrame vision_frame;
  vision_frame.rgb = *rgb_frame;
  mediapipe_processor_->Submit(vision_frame);
}

void VisionEngine::PollMediapipeResults() {
  if (!mediapipe_processor_) {
    return;
  }

  while (auto result = mediapipe_processor_->PollResult()) {
    std::string payload;
    const bool has_hands = result->ok && !result->hands.empty();
    if (session_writer_ && has_hands) {
      payload = MediapipeResultToJson(*result);
      session_writer_->WriteMediapipeLine(payload);
    }
    if (zenoh_publisher_ && zenoh_publisher_->active() &&
        zenoh_publisher_->result_publishing_enabled() && has_hands) {
      if (payload.empty()) {
        payload = MediapipeResultToJson(*result);
      }
      zenoh_publisher_->PublishMediapipe(result->camera_id, payload);
    }

    emit MediapipeResultReady(*result);
  }
}

bool VisionEngine::StartYoloProcessor() {
  yolo_camera_id_.clear();
  const std::string& yolo_cam = session_profile_.selected_yolo_camera;
  if (!yolo_enabled_ || yolo_cam.empty() || !board_config_.yolo.has_value()) {
    return false;
  }

  const auto& yolo_hw = *board_config_.yolo;
  vision::YoloProcessorConfig config;
  config.model = yolo_hw.model;
  config.queue_depth = 1;
  config.confidence_threshold = static_cast<float>(yolo_hw.confidence_threshold);
  config.nms_threshold = static_cast<float>(yolo_hw.nms_threshold);
  config.max_detections = yolo_hw.max_detections;
  config.class_names = yolo_hw.class_names;

  yolo_processor_ = vision::CreateYoloProcessor();
  std::string yolo_err;
  if (!yolo_processor_->Start(config, &yolo_err)) {
    std::cerr << "[yolo] failed to start processor: " << yolo_err << "\n";
    yolo_processor_.reset();
    return false;
  }

  yolo_camera_id_ = yolo_cam;
  std::cerr << "[yolo] processor started for " << yolo_camera_id_
            << " at " << yolo_hw.fps << "fps\n";
  return true;
}

void VisionEngine::StopYoloProcessor() {
  yolo_poll_timer_->stop();
  StopYoloPipeline();
  yolo_logged_path_ = false;
  if (yolo_processor_) {
    yolo_processor_->Stop();
    yolo_processor_.reset();
    std::cerr << "[yolo] processor stopped\n";
  }
  std::lock_guard<std::mutex> lock(yolo_frame_mu_);
  yolo_camera_id_.clear();
}

std::unique_ptr<V4l2Pipeline> VisionEngine::BuildYoloPipeline(std::string* err) {
  if (!yolo_processor_ || yolo_camera_id_.empty()) {
    if (err) *err = "YOLO processor is not running";
    return nullptr;
  }
  if (!board_config_.yolo.has_value()) {
    if (err) *err = "YOLO config is not available";
    return nullptr;
  }
  return BuildVisionPipeline(
      yolo_camera_id_, "_yolo",
      board_config_.yolo.has_value() && board_config_.yolo->fps > 0
          ? board_config_.yolo->fps
          : kDefaultYoloFps,
      [this](GstSample* sample) { OnYoloSample(sample); }, err);
}

std::unique_ptr<V4l2Pipeline> VisionEngine::BuildFaceExpressionPipeline(std::string* err) {
  if (!face_expression_processor_ || face_expression_camera_id_.empty()) {
    if (err) *err = "face expression processor is not running";
    return nullptr;
  }
  if (!board_config_.face_expression.has_value()) {
    if (err) *err = "face expression config is not available";
    return nullptr;
  }
  return BuildVisionPipeline(
      face_expression_camera_id_, "_face",
      board_config_.face_expression.has_value() && board_config_.face_expression->fps > 0
          ? board_config_.face_expression->fps
          : kDefaultFaceExpressionFps,
      [this](GstSample* sample) { OnFaceExpressionSample(sample); }, err);
}

bool VisionEngine::StartYoloPipeline(std::string* err) {
  StopYoloPipeline();
  auto pipeline = BuildYoloPipeline(err);
  if (!pipeline || !pipeline->Start(err)) {
    StopYoloPipeline();
    return false;
  }
  yolo_pipeline_ = std::move(pipeline);
  std::cerr << "[yolo] capture pipeline started for " << yolo_camera_id_ << "\n";
  return true;
}

void VisionEngine::StopYoloPipeline() {
  if (yolo_pipeline_) {
    yolo_pipeline_->Stop();
    yolo_pipeline_.reset();
    std::cerr << "[yolo] capture pipeline stopped\n";
  }
}

bool VisionEngine::StartFaceExpressionPipeline(std::string* err) {
  StopFaceExpressionPipeline();
  auto pipeline = BuildFaceExpressionPipeline(err);
  if (!pipeline || !pipeline->Start(err)) {
    StopFaceExpressionPipeline();
    return false;
  }
  face_expression_pipeline_ = std::move(pipeline);
  std::cerr << "[face] capture pipeline started for " << face_expression_camera_id_ << "\n";
  return true;
}

void VisionEngine::StopFaceExpressionPipeline() {
  if (face_expression_pipeline_) {
    face_expression_pipeline_->Stop();
    face_expression_pipeline_.reset();
    std::cerr << "[face] capture pipeline stopped\n";
  }
}

void VisionEngine::OnYoloSample(GstSample* sample) {
  if (!yolo_processor_ || !sample) {
    return;
  }

  std::string camera_id;
  {
    std::lock_guard<std::mutex> lock(yolo_frame_mu_);
    camera_id = yolo_camera_id_;
  }
  std::optional<vision::FrameRef> raw_frame;
  const std::string orientation = CameraOrientation(camera_id);
  if (IsOriented(orientation)) {
    raw_frame = frame_converter_.ConvertToRgbFrame(sample, camera_id);
    raw_frame = RotateRgbFrame(std::move(raw_frame), orientation);
    raw_frame = RgbFrameToBgr(std::move(raw_frame));
  } else {
    raw_frame = frame_converter_.ExtractNv12Frame(sample, camera_id, true);
  }
  if (!raw_frame.has_value()) {
    return;
  }

  if (!yolo_logged_path_) {
    std::cerr << "[yolo] input path: NV12 dmabuf\n";
    yolo_logged_path_ = true;
  }

  yolo_processor_->Submit(*raw_frame);
}

void VisionEngine::PollYoloResults() {
  if (!yolo_processor_) {
    return;
  }
  while (auto result = yolo_processor_->PollResult()) {
    const vision::YoloResult output_result = result->ok
        ? FilterYoloResultForOutput(*result)
        : *result;
    std::string payload;
    if (session_writer_ && output_result.ok && !output_result.detections.empty()) {
      payload = YoloResultToJson(output_result);
      session_writer_->WriteYoloLine(payload);
    }
    if (zenoh_publisher_ && zenoh_publisher_->active() &&
        zenoh_publisher_->result_publishing_enabled() &&
        output_result.ok && !output_result.detections.empty()) {
      if (payload.empty()) {
        payload = YoloResultToJson(output_result);
      }
      zenoh_publisher_->PublishYolo(output_result.camera_id, payload);
    }
    emit YoloResultReady(output_result);
  }
}

bool VisionEngine::StartFaceExpressionProcessor() {
  face_expression_camera_id_.clear();
  const std::string& face_cam = session_profile_.selected_face_camera;
  if (!face_expression_enabled_ || face_cam.empty() || !board_config_.face_expression.has_value()) {
    return false;
  }

  const auto& face_hw = *board_config_.face_expression;
  vision::FaceExpressionProcessorConfig config;
  config.detector_model = face_hw.detector_model;
  config.expression_model = face_hw.expression_model;
  config.expression_labels = face_hw.expression_labels;
  config.queue_depth = 1;
  config.confidence_threshold = static_cast<float>(face_hw.confidence_threshold);
  config.nms_threshold = static_cast<float>(face_hw.nms_threshold);
  config.expression_threshold = static_cast<float>(face_hw.expression_threshold);
  config.max_faces = face_hw.max_faces;

  face_expression_processor_ = vision::CreateFaceExpressionProcessor();
  std::string face_err;
  if (!face_expression_processor_->Start(config, &face_err)) {
    std::cerr << "[face] failed to start processor: " << face_err << "\n";
    face_expression_processor_.reset();
    return false;
  }

  face_expression_camera_id_ = face_cam;
  std::cerr << "[face] processor started for " << face_expression_camera_id_
            << " at " << face_hw.fps << "fps\n";
  return true;
}

void VisionEngine::StopFaceExpressionProcessor() {
  face_expression_poll_timer_->stop();
  StopFaceExpressionPipeline();
  face_expression_logged_path_ = false;
  if (face_expression_processor_) {
    face_expression_processor_->Stop();
    face_expression_processor_.reset();
    std::cerr << "[face] processor stopped\n";
  }
  std::lock_guard<std::mutex> lock(face_expression_frame_mu_);
  face_expression_camera_id_.clear();
}

void VisionEngine::OnFaceExpressionSample(GstSample* sample) {
  if (!face_expression_processor_ || !sample) {
    return;
  }

  std::string camera_id;
  {
    std::lock_guard<std::mutex> lock(face_expression_frame_mu_);
    camera_id = face_expression_camera_id_;
  }
  auto rgb_frame = frame_converter_.ConvertToRgbFrame(sample, camera_id);
  rgb_frame = RotateRgbFrame(std::move(rgb_frame), CameraOrientation(camera_id));
  if (!rgb_frame.has_value()) {
    return;
  }

  if (!face_expression_logged_path_) {
    std::cerr << "[face] NV12->RGB path: RGA hardware\n";
    face_expression_logged_path_ = true;
  }

  face_expression_processor_->Submit(*rgb_frame);
}

void VisionEngine::PollFaceExpressionResults() {
  if (!face_expression_processor_) {
    return;
  }
  while (auto result = face_expression_processor_->PollResult()) {
    std::string payload;
    const bool has_faces = result->ok && !result->faces.empty();
    if (session_writer_ && has_faces) {
      payload = FaceExpressionResultToJson(*result);
      session_writer_->WriteFaceExpressionLine(payload);
    }
    if (zenoh_publisher_ && zenoh_publisher_->active() &&
        zenoh_publisher_->result_publishing_enabled() && has_faces) {
      if (payload.empty()) {
        payload = FaceExpressionResultToJson(*result);
      }
      zenoh_publisher_->PublishFaceExpression(result->camera_id, payload);
    }
    emit FaceExpressionResultReady(*result);
  }
}

std::string VisionEngine::CameraOrientation(const std::string& camera_id) const {
  const CameraNodeSet* camera = FindCamera(board_config_, camera_id);
  return camera != nullptr ? camera->orientation : "normal";
}

}  // namespace rkstudio::media
