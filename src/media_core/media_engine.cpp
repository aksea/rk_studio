#include "rk_studio/media_core/media_engine.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <utility>

#include <gst/gst.h>
#include <QMetaObject>

#include "rk_studio/infra/config_types.h"
#include "rk_studio/infra/gst_audio_recorder.h"
#include "rk_studio/infra/gst_util.h"
#include "rk_studio/infra/runtime.h"
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

struct WarmupProbeContext {
  int remaining_buffers = 4;
};

struct PhotoResult {
  std::string camera_id;
  std::string device;
  std::filesystem::path file_path;
  int width = 0;
  int height = 0;
  uint64_t capture_monotonic_ns = 0;
  uint64_t bytes = 0;
  double elapsed_ms = 0.0;
  bool success = false;
  std::string error;
};

GstPadProbeReturn DropWarmupBuffers(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
  auto* context = static_cast<WarmupProbeContext*>(user_data);
  if (context == nullptr || !(info->type & GST_PAD_PROBE_TYPE_BUFFER)) {
    return GST_PAD_PROBE_OK;
  }
  if (context->remaining_buffers > 0) {
    --context->remaining_buffers;
    return GST_PAD_PROBE_DROP;
  }
  return GST_PAD_PROBE_REMOVE;
}

bool CaptureOnePng(const CameraNodeSet& camera,
                   const std::filesystem::path& output_path,
                   PhotoResult* result) {
  const auto started = std::chrono::steady_clock::now();
  result->camera_id = camera.id;
  result->device = camera.record_device;
  result->file_path = output_path;
  result->width = camera.record_width;
  result->height = camera.record_height;

  if (!rkinfra::IsNv12Format(camera.input_format)) {
    result->error = "photo capture currently requires NV12 input";
    return false;
  }

  std::string io_mode_error;
  const int io_mode = rkinfra::ToV4l2IoMode(camera.io_mode, &io_mode_error);
  if (io_mode < 0) {
    result->error = io_mode_error;
    return false;
  }

  GstElement* pipeline = gst_pipeline_new(("rk-photo-" + camera.id).c_str());
  GstElement* source = gst_element_factory_make("v4l2src", ("photo_source_" + camera.id).c_str());
  GstElement* caps_filter =
      gst_element_factory_make("capsfilter", ("photo_caps_" + camera.id).c_str());
  GstElement* queue = gst_element_factory_make("queue", ("photo_queue_" + camera.id).c_str());
  GstElement* convert =
      gst_element_factory_make("videoconvert", ("photo_convert_" + camera.id).c_str());
  GstElement* encoder = gst_element_factory_make("pngenc", ("photo_encoder_" + camera.id).c_str());
  GstElement* sink = gst_element_factory_make("filesink", ("photo_sink_" + camera.id).c_str());

  if (!pipeline || !source || !caps_filter || !queue || !convert || !encoder || !sink) {
    result->error = "failed to create sequential photo pipeline elements";
    if (source) gst_object_unref(source);
    if (caps_filter) gst_object_unref(caps_filter);
    if (queue) gst_object_unref(queue);
    if (convert) gst_object_unref(convert);
    if (encoder) gst_object_unref(encoder);
    if (sink) gst_object_unref(sink);
    if (pipeline) gst_object_unref(pipeline);
    return false;
  }

  rkinfra::SetPropertyIfExists(source, "device", camera.record_device.c_str());
  rkinfra::SetPropertyIfExists(source, "do-timestamp", TRUE);
  rkinfra::SetPropertyIfExists(source, "io-mode", io_mode);
  rkinfra::SetPropertyIfExists(source, "num-buffers", 20);

  GstCaps* caps = gst_caps_new_simple(
      "video/x-raw", "width", G_TYPE_INT, camera.record_width,
      "height", G_TYPE_INT, camera.record_height, "format", G_TYPE_STRING, "NV12",
      "framerate", GST_TYPE_FRACTION, camera.fps, 1, nullptr);
  g_object_set(G_OBJECT(caps_filter), "caps", caps, nullptr);
  gst_caps_unref(caps);

  rkinfra::SetPropertyIfExists(encoder, "snapshot", TRUE);
  rkinfra::SetPropertyIfExists(encoder, "compression-level", 3u);
  rkinfra::SetPropertyIfExists(queue, "max-size-buffers", 1u);
  rkinfra::SetPropertyIfExists(queue, "max-size-bytes", 0u);
  rkinfra::SetPropertyIfExists(queue, "max-size-time", static_cast<guint64>(0));
  rkinfra::SetPropertyIfExists(queue, "leaky", 2);
  rkinfra::SetPropertyIfExists(sink, "location", output_path.string().c_str());
  rkinfra::SetPropertyIfExists(sink, "sync", FALSE);

  gst_bin_add_many(GST_BIN(pipeline), source, caps_filter, queue, convert, encoder, sink, nullptr);
  if (!gst_element_link_many(source, caps_filter, queue, convert, encoder, sink, nullptr)) {
    result->error = "failed to link sequential photo pipeline";
    gst_object_unref(pipeline);
    return false;
  }

  WarmupProbeContext warmup;
  GstPad* caps_src_pad = gst_element_get_static_pad(caps_filter, "src");
  if (caps_src_pad == nullptr) {
    result->error = "failed to access photo pipeline source pad";
    gst_object_unref(pipeline);
    return false;
  }
  gst_pad_add_probe(caps_src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                    &DropWarmupBuffers, &warmup, nullptr);
  gst_object_unref(caps_src_pad);

  std::cerr << "[photo] capture " << camera.id << " device=" << camera.record_device
            << " size=" << camera.record_width << "x" << camera.record_height << "\n";

  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    result->error = "photo pipeline failed to enter PLAYING state";
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return false;
  }

  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* message = gst_bus_timed_pop_filtered(
      bus, 10 * GST_SECOND,
      static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

  bool completed = false;
  if (message == nullptr) {
    result->error = "photo capture timed out";
  } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    result->error = error != nullptr ? error->message : "unknown GStreamer error";
    g_clear_error(&error);
    g_free(debug);
  } else {
    completed = true;
  }

  if (message) gst_message_unref(message);
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);

  try {
    if (completed && std::filesystem::is_regular_file(output_path)) {
      result->bytes = std::filesystem::file_size(output_path);
      completed = result->bytes > 0;
    } else {
      completed = false;
    }
  } catch (const std::exception& ex) {
    result->error = ex.what();
    completed = false;
  }

  if (!completed && result->error.empty()) {
    result->error = "PNG file was not created";
  }
  result->capture_monotonic_ns = rkinfra::ClockMonotonicNs();
  result->elapsed_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  result->success = completed;
  return completed;
}

bool CreatePhotoDirectories(const SessionProfile& profile,
                            std::filesystem::path* final_dir,
                            std::filesystem::path* staging_dir,
                            std::string* err) {
  try {
    const std::filesystem::path root(profile.output_dir);
    std::filesystem::create_directories(root);
    const std::string base_name = profile.prefix + "-photo-" + rkinfra::NowLocalCompact();
    for (int suffix = 0; suffix < 100; ++suffix) {
      const std::string name = suffix == 0 ? base_name : base_name + "-" + std::to_string(suffix);
      const std::filesystem::path candidate = root / name;
      const std::filesystem::path staging(candidate.string() + ".partial");
      if (std::filesystem::exists(candidate) || std::filesystem::exists(staging)) {
        continue;
      }
      if (std::filesystem::create_directories(staging)) {
        *final_dir = candidate;
        *staging_dir = staging;
        return true;
      }
    }
    if (err) *err = "failed to allocate a unique photo directory";
  } catch (const std::exception& ex) {
    if (err) *err = std::string("failed to create photo directory: ") + ex.what();
  }
  return false;
}

bool WritePhotoManifest(const std::filesystem::path& path,
                        const std::vector<std::string>& expected_camera_ids,
                        const std::vector<PhotoResult>& results,
                        bool success,
                        const std::string& failure_reason) {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }

  out << "{\n";
  out << "  \"status\": \"" << (success ? "complete" : "partial") << "\",\n";
  out << "  \"mode\": \"sequential_full_resolution\",\n";
  out << "  \"instruction\": \"keep helmet and calibration board still until completion\",\n";
  out << "  \"created_utc\": \"" << rkinfra::JsonEscape(rkinfra::NowUtcIso8601()) << "\",\n";
  out << "  \"failure_reason\": \"" << rkinfra::JsonEscape(failure_reason) << "\",\n";
  out << "  \"expected_cameras\": [";
  for (size_t index = 0; index < expected_camera_ids.size(); ++index) {
    if (index > 0) out << ", ";
    out << "\"" << rkinfra::JsonEscape(expected_camera_ids[index]) << "\"";
  }
  out << "],\n";
  out << "  \"images\": [\n";
  for (size_t index = 0; index < results.size(); ++index) {
    const auto& result = results[index];
    out << "    {\n";
    out << "      \"sequence_index\": " << index << ",\n";
    out << "      \"camera_id\": \"" << rkinfra::JsonEscape(result.camera_id) << "\",\n";
    out << "      \"device\": \"" << rkinfra::JsonEscape(result.device) << "\",\n";
    out << "      \"file\": \"" << rkinfra::JsonEscape(result.file_path.filename().string()) << "\",\n";
    out << "      \"width\": " << result.width << ",\n";
    out << "      \"height\": " << result.height << ",\n";
    out << "      \"capture_monotonic_ns\": " << result.capture_monotonic_ns << ",\n";
    out << "      \"elapsed_ms\": " << result.elapsed_ms << ",\n";
    out << "      \"bytes\": " << result.bytes << ",\n";
    out << "      \"status\": \"" << (result.success ? "ok" : "error") << "\",\n";
    out << "      \"error\": \"" << rkinfra::JsonEscape(result.error) << "\"\n";
    out << "    }" << (index + 1 < results.size() ? "," : "") << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.good();
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

  return RebuildPreviewPipelines(err);
}

bool MediaEngine::StartRecording(std::string* err) {
  StopRecordPipelines();

  session_writer_ = std::make_unique<SessionWriter>();
  if (!session_writer_->Initialize(board_config_, session_profile_, err)) {
    session_writer_.reset();
    return false;
  }

  if (!RebuildRecordPipelines(err)) {
    FinalizeRecording(false);
    return false;
  }

  if (!StartAudioRecorder(err)) {
    FinalizeRecording(false);
    return false;
  }

  const auto outputs = CollectOutputs(record_cameras_, audio_recorder_.get());
  session_writer_->WriteStartMeta(outputs);
  return true;
}

bool MediaEngine::CapturePhotos(std::string* output_dir, std::string* err) {
  const std::vector<std::string> camera_ids = EffectiveRecordCameraIds(session_profile_);
  if (camera_ids.empty()) {
    if (err) *err = "session profile has no record cameras";
    return false;
  }
  for (const auto& camera_id : camera_ids) {
    const CameraNodeSet* camera = FindCamera(board_config_, camera_id);
    if (camera == nullptr) {
      if (err) *err = "unknown camera id: " + camera_id;
      return false;
    }
    if (camera->record_width != 1920 || camera->record_height != 1080) {
      if (err) {
        *err = "camera '" + camera_id + "' record resolution is not 1920x1080";
      }
      return false;
    }
  }

  std::filesystem::path final_dir;
  std::filesystem::path staging_dir;
  if (!CreatePhotoDirectories(session_profile_, &final_dir, &staging_dir, err)) {
    return false;
  }
  if (output_dir) *output_dir = staging_dir.string();

  std::vector<PhotoResult> results;
  std::string failure_reason;
  for (const auto& camera_id : camera_ids) {
    const CameraNodeSet* camera = FindCamera(board_config_, camera_id);
    PhotoResult result;
    const bool captured = CaptureOnePng(*camera, staging_dir / (camera_id + ".png"), &result);
    results.push_back(std::move(result));
    if (!captured) {
      failure_reason = camera_id + ": " + results.back().error;
      break;
    }
  }

  bool success = failure_reason.empty() && results.size() == camera_ids.size();
  if (!WritePhotoManifest(staging_dir / "capture.meta.json", camera_ids, results,
                          success, failure_reason)) {
    success = false;
    if (failure_reason.empty()) failure_reason = "failed to write capture.meta.json";
  }

  if (success) {
    try {
      std::filesystem::rename(staging_dir, final_dir);
      if (output_dir) *output_dir = final_dir.string();
      return true;
    } catch (const std::exception& ex) {
      failure_reason = std::string("failed to finalize photo directory: ") + ex.what();
    }
  }

  if (err) {
    *err = failure_reason + "; partial output: " + staging_dir.string();
  }
  return false;
}

void MediaEngine::StopPreview() {
  StopPreviewPipelines();
}

void MediaEngine::StopRecording(bool ok) {
  FinalizeRecording(ok);
}

void MediaEngine::StopAll() {
  if (session_writer_) {
    FinalizeRecording(true);
  }
  StopRecordPipelines();
  StopPreviewPipelines();
}

void MediaEngine::BindPreviewWindow(const std::string& camera_id, WId window_id) {
  preview_window_ids_[camera_id] = window_id;
  auto it = preview_cameras_.find(camera_id);
  if (it != preview_cameras_.end()) {
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
  options.source.id = recording ? camera->id : camera->id + "-preview";
  options.source.device = recording ? camera->record_device : camera->preview_device;
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
  options.app_sink.enabled = !recording && !options.preview.enabled;
  if (options.app_sink.enabled) {
    options.app_sink.sample_callback = [](GstSample*) {};
  }
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

bool MediaEngine::RebuildPreviewPipelines(std::string* err) {
  StopPreviewPipelines();
  for (const auto& camera_id : session_profile_.preview_cameras) {
    auto pipeline = BuildOnePipeline(camera_id, false, err);
    if (!pipeline || !pipeline->Start(err)) {
      StopPreviewPipelines();
      return false;
    }
    preview_cameras_.insert_or_assign(camera_id, std::move(pipeline));
  }
  return true;
}

bool MediaEngine::RebuildRecordPipelines(std::string* err) {
  StopRecordPipelines();
  for (const auto& camera_id : EffectiveRecordCameraIds(session_profile_)) {
    auto pipeline = BuildOnePipeline(camera_id, true, err);
    if (!pipeline || !pipeline->Start(err)) {
      StopRecordPipelines();
      return false;
    }
    record_cameras_.insert_or_assign(camera_id, std::move(pipeline));
  }
  return true;
}

void MediaEngine::StopPreviewPipelines() {
  for (auto& [id, pipeline] : preview_cameras_) {
    (void)id;
    pipeline->Stop();
  }
  preview_cameras_.clear();
}

void MediaEngine::StopRecordPipelines() {
  for (auto& [id, pipeline] : record_cameras_) {
    (void)id;
    pipeline->Stop();
  }
  record_cameras_.clear();
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
  const auto outputs = CollectOutputs(record_cameras_, audio_recorder_.get());

  StopAudioRecorder();
  StopRecordPipelines();

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
