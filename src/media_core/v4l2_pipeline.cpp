#include "rk_studio/media_core/v4l2_pipeline.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

#include <gst/video/video-event.h>
#include <gst/video/video.h>
#include <gst/video/videooverlay.h>

#include "rk_studio/infra/runtime.h"
#include "rk_studio/infra/gst_util.h"

namespace rkstudio::media {
namespace {

constexpr guint kQueueLeakyDownstream = 2;

template <typename T>
struct GstUnrefDeleter {
  void operator()(T* ptr) const {
    if (ptr != nullptr) {
      gst_object_unref(ptr);
    }
  }
};

GstCaps* MakeSourceCaps(const V4l2Pipeline::SourceOptions& source) {
  return gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, source.width,
                             "height", G_TYPE_INT, source.height, "format",
                             G_TYPE_STRING, "NV12", nullptr);
}

GstCaps* MakeRateCaps(const V4l2Pipeline::SourceOptions& source) {
  return gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, source.width,
                             "height", G_TYPE_INT, source.height, "format",
                             G_TYPE_STRING, "NV12", "framerate",
                             GST_TYPE_FRACTION, source.fps, 1, nullptr);
}

std::string BranchSummary(const V4l2Pipeline::BuildOptions& options) {
  std::string branches;
  if (options.preview.enabled) {
    branches += branches.empty() ? "preview" : "+preview";
  }
  if (options.record.enabled) {
    branches += branches.empty() ? "record" : "+record";
  }
  if (options.app_sink.enabled) {
    branches += branches.empty() ? "appsink" : "+appsink";
  }
  return branches.empty() ? "none" : branches;
}

}  // namespace

V4l2Pipeline::V4l2Pipeline() = default;

V4l2Pipeline::~V4l2Pipeline() {
  Stop();
}

bool V4l2Pipeline::Build(const BuildOptions& options,
                          TelemetryCallback telemetry_callback,
                          ErrorCallback error_callback,
                          std::string* err) {
  Stop();
  options_ = options;
  telemetry_callback_ = std::move(telemetry_callback);
  error_callback_ = std::move(error_callback);

  resolved_device_ = options_.source.device;

  std::cerr << "[v4l2] build " << options_.source.id
            << " device=" << resolved_device_
            << " format=" << options_.source.input_format
            << " size=" << options_.source.width << "x" << options_.source.height
            << " fps=" << options_.source.fps << "/1"
            << " io=" << options_.source.io_mode
            << " branches=" << BranchSummary(options_)
            << "\n";

  if (options_.record.enabled) {
    record_output_path_ = (options_.record.session_dir / (options_.source.id + ".mkv")).string();
  } else {
    record_output_path_.clear();
  }

  return BuildPipeline(err);
}

bool V4l2Pipeline::BuildPipeline(std::string* err) {
  if (!rkinfra::IsNv12Format(options_.source.input_format)) {
    if (err != nullptr) {
      *err = "V4l2Pipeline only supports NV12 input for " + options_.source.id;
    }
    return false;
  }

  const int branch_count = (options_.preview.enabled ? 1 : 0) +
                           (options_.record.enabled ? 1 : 0) +
                           (options_.app_sink.enabled ? 1 : 0);
  if (branch_count != 1) {
    if (err != nullptr) {
      *err = "V4l2Pipeline expects exactly one branch for " + options_.source.id;
    }
    return false;
  }

  pipeline_ = gst_pipeline_new(("rk-studio-" + options_.source.id).c_str());
  source_ = gst_element_factory_make("v4l2src", ("source_" + options_.source.id).c_str());
  source_caps_ = gst_element_factory_make("capsfilter", ("source_caps_" + options_.source.id).c_str());

  rate_filter_ = gst_element_factory_make("videorate", ("videorate_" + options_.source.id).c_str());
  rate_caps_ = gst_element_factory_make("capsfilter", ("rate_caps_" + options_.source.id).c_str());

  if (options_.preview.enabled) {
    preview_convert_ = gst_element_factory_make("videoconvert", ("preview_convert_" + options_.source.id).c_str());
    if (!CreatePreviewSink(err)) {
      return false;
    }
  }

  if (options_.app_sink.enabled) {
    appsink_queue_ = gst_element_factory_make("queue", ("appsink_queue_" + options_.source.id).c_str());
    app_sink_ = gst_element_factory_make("appsink", ("app_sink_" + options_.source.id).c_str());
  }

  if (options_.record.enabled) {
    encoder_ = gst_element_factory_make("mpph265enc", ("encoder_" + options_.source.id).c_str());
    parser_ = gst_element_factory_make("h265parse", ("parser_" + options_.source.id).c_str());
    mux_ = gst_element_factory_make("matroskamux", ("mux_" + options_.source.id).c_str());
    record_sink_ = gst_element_factory_make("filesink", ("record_sink_" + options_.source.id).c_str());
  }

  if (!pipeline_ || !source_ || !source_caps_ ||
      !rate_filter_ || !rate_caps_ ||
      (options_.preview.enabled && (!preview_convert_ || !preview_sink_)) ||
      (options_.record.enabled && (!encoder_ || !parser_ || !mux_ || !record_sink_)) ||
      (options_.app_sink.enabled && (!appsink_queue_ || !app_sink_))) {
    if (err != nullptr) {
      *err = "failed to create camera pipeline elements for " + options_.source.id;
    }
    return false;
  }

  rkinfra::SetPropertyIfExists(source_, "device", resolved_device_.c_str());
  rkinfra::SetPropertyIfExists(source_, "do-timestamp", TRUE);
  std::string io_mode_error;
  const int io_mode = rkinfra::ToV4l2IoMode(options_.source.io_mode, &io_mode_error);
  if (io_mode < 0) {
    if (err != nullptr) {
      *err = io_mode_error;
    }
    return false;
  }
  rkinfra::SetPropertyIfExists(source_, "io-mode", io_mode);

  GstCaps* source_caps = MakeSourceCaps(options_.source);
  g_object_set(G_OBJECT(source_caps_), "caps", source_caps, nullptr);
  gst_caps_unref(source_caps);

  rkinfra::SetPropertyIfExists(rate_filter_, "drop-only", TRUE);
  GstCaps* rate_caps = MakeRateCaps(options_.source);
  g_object_set(G_OBJECT(rate_caps_), "caps", rate_caps, nullptr);
  gst_caps_unref(rate_caps);

  if (options_.preview.enabled) {
    rkinfra::SetPropertyIfExists(preview_sink_, "sync", FALSE);
  }

  if (options_.app_sink.enabled) {
    g_object_set(G_OBJECT(app_sink_), "emit-signals", FALSE, "max-buffers", 2u, "drop", TRUE, "sync", FALSE, nullptr);
    g_object_set(G_OBJECT(appsink_queue_), "leaky", kQueueLeakyDownstream, "max-size-buffers", 2u,
                 "max-size-bytes", 0u, "max-size-time", static_cast<guint64>(0), nullptr);
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = &V4l2Pipeline::OnAppSinkSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(app_sink_), &callbacks, this, nullptr);
  }

  if (options_.record.enabled) {
    rkinfra::SetPropertyIfExists(encoder_, "bps", static_cast<guint>(options_.source.bitrate));
    rkinfra::SetPropertyIfExists(encoder_, "gop", options_.record.gop);
    rkinfra::SetPropertyIfExists(encoder_, "header-mode", 1);
    rkinfra::SetPropertyIfExists(record_sink_, "location", record_output_path_.c_str());
  }

  // Add elements to pipeline.
  gst_bin_add_many(GST_BIN(pipeline_), source_, source_caps_, nullptr);
  gst_bin_add_many(GST_BIN(pipeline_), rate_filter_, rate_caps_, nullptr);
  if (options_.preview.enabled) {
    gst_bin_add_many(GST_BIN(pipeline_), preview_convert_, preview_sink_, nullptr);
  }
  if (options_.app_sink.enabled) {
    gst_bin_add_many(GST_BIN(pipeline_), appsink_queue_, app_sink_, nullptr);
  }
  if (options_.record.enabled) {
    gst_bin_add_many(GST_BIN(pipeline_), encoder_, parser_, mux_, record_sink_, nullptr);
  }

  // Link source chain: v4l2src -> capsfilter -> videorate -> rate caps.
  // source_tail tracks the last element in the source chain for linking the output branch.
  GstElement* source_tail = nullptr;
  const bool link_ok = gst_element_link_many(source_, source_caps_, rate_filter_, rate_caps_, nullptr);
  source_tail = rate_caps_;
  if (!link_ok) {
    if (err != nullptr) {
      *err = "failed to link source chain for " + options_.source.id;
    }
    return false;
  }

  auto LinkRecordChain = [&]() -> bool {
    if (!gst_element_link_many(encoder_, parser_, nullptr) || !gst_element_link(mux_, record_sink_)) {
      if (err) *err = "failed to link record chain for " + options_.source.id;
      return false;
    }
    GstPad* mux_pad = gst_element_get_request_pad(mux_, "video_%u");
    GstPad* parser_src = gst_element_get_static_pad(parser_, "src");
    const bool mux_ok = mux_pad && parser_src && gst_pad_link(parser_src, mux_pad) == GST_PAD_LINK_OK;
    if (parser_src) gst_object_unref(parser_src);
    if (mux_pad) gst_object_unref(mux_pad);
    if (!mux_ok) {
      if (err) *err = "failed to link parser → mux for " + options_.source.id;
      return false;
    }
    return true;
  };

  if (options_.app_sink.enabled) {
    if (!gst_element_link_many(source_tail, appsink_queue_, app_sink_, nullptr)) {
      if (err) *err = "failed to link appsink branch for " + options_.source.id;
      return false;
    }
  } else if (options_.preview.enabled) {
    if (!gst_element_link_many(source_tail, preview_convert_, preview_sink_, nullptr)) {
      if (err) *err = "failed to link preview branch for " + options_.source.id;
      return false;
    }
  } else if (options_.record.enabled) {
    if (!gst_element_link(source_tail, encoder_) || !LinkRecordChain()) return false;
  }

  InstallProbes(source_tail);
  return true;
}

bool V4l2Pipeline::CreatePreviewSink(std::string* err) {
  for (const auto& sink_name : options_.preview.sink_priority) {
    preview_sink_ = gst_element_factory_make(sink_name.c_str(), ("preview_sink_" + options_.source.id).c_str());
    if (preview_sink_ != nullptr) {
      return true;
    }
  }
  if (err != nullptr) {
    *err = "failed to create preview sink for " + options_.source.id;
  }
  return false;
}

void V4l2Pipeline::InstallProbes(GstElement* source_tail) {
  using GstPadPtr = std::unique_ptr<GstPad, GstUnrefDeleter<GstPad>>;

  GstPadPtr pad(gst_element_get_static_pad(source_tail, "src"));
  if (!pad) {
    return;
  }
  auto ctx = std::make_unique<ProbeContext>();
  ctx->self = this;
  ctx->stream_id = options_.source.id;
  ctx->seq = &seq_;
  gst_pad_add_probe(pad.get(), GST_PAD_PROBE_TYPE_BUFFER, &V4l2Pipeline::OnCaptureProbe, ctx.get(), nullptr);
  probe_contexts_.push_back(std::move(ctx));
}

bool V4l2Pipeline::Start(std::string* err) {
  if (pipeline_ == nullptr) {
    if (err != nullptr) {
      *err = "pipeline not built for " + options_.source.id;
    }
    return false;
  }

  stop_requested_.store(false);
  bus_done_.store(false);
  base_time_ns_.store(GST_CLOCK_TIME_NONE);

  using GstClockPtr = std::unique_ptr<GstClock, GstUnrefDeleter<GstClock>>;
  GstClockPtr clock(gst_system_clock_obtain());
  if (!clock) {
    if (err != nullptr) {
      *err = "failed to obtain GStreamer clock";
    }
    return false;
  }
  g_object_set(clock.get(), "clock-type", GST_CLOCK_TYPE_MONOTONIC, nullptr);
  gst_pipeline_use_clock(GST_PIPELINE(pipeline_), clock.get());

  gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  const GstStateChangeReturn state = gst_element_get_state(pipeline_, nullptr, nullptr, 5 * GST_SECOND);
  if (state == GST_STATE_CHANGE_FAILURE) {
    if (err != nullptr) {
      *err = "pipeline failed to enter PLAYING state for " + options_.source.id;
    }
    return false;
  }

  base_time_ns_.store(gst_element_get_base_time(pipeline_));
  BindPreviewOverlay();
  bus_thread_ = std::thread([this] { RunBusLoop(); });

  return true;
}

void V4l2Pipeline::Stop() {
  stop_requested_.store(true);

  if (pipeline_ != nullptr) {
    gst_element_send_event(pipeline_, gst_event_new_eos());
  }

  if (bus_thread_.joinable()) {
    bus_thread_.join();
  }

  if (pipeline_ != nullptr) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
  }

  pipeline_ = nullptr;
  source_ = nullptr;
  source_caps_ = nullptr;
  rate_filter_ = nullptr;
  rate_caps_ = nullptr;
  preview_convert_ = nullptr;
  preview_sink_ = nullptr;
  encoder_ = nullptr;
  parser_ = nullptr;
  mux_ = nullptr;
  record_sink_ = nullptr;
  appsink_queue_ = nullptr;
  app_sink_ = nullptr;
  base_time_ns_.store(GST_CLOCK_TIME_NONE);
  probe_contexts_.clear();
}

void V4l2Pipeline::SetPreviewWindow(WId window_id) {
  options_.preview.window_id = window_id;
  BindPreviewOverlay();
}

const std::string& V4l2Pipeline::camera_id() const {
  return options_.source.id;
}

const std::string& V4l2Pipeline::resolved_device() const {
  return resolved_device_;
}

std::string V4l2Pipeline::record_output_path() const {
  return record_output_path_;
}

void V4l2Pipeline::RunBusLoop() {
  using GstBusPtr = std::unique_ptr<GstBus, GstUnrefDeleter<GstBus>>;

  GstBusPtr bus(pipeline_ != nullptr ? gst_element_get_bus(pipeline_) : nullptr);
  if (!bus) {
    Fail("missing_bus", options_.record.enabled);
    bus_done_.store(true);
    return;
  }

  while (!stop_requested_.load()) {
    GstMessage* message =
        gst_bus_timed_pop_filtered(bus.get(), 200 * GST_MSECOND,
                                   static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    if (message == nullptr) {
      continue;
    }

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError* error = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      const std::string reason = error != nullptr ? error->message : "unknown";
      g_clear_error(&error);
      g_free(debug);
      gst_message_unref(message);
      Fail(reason, options_.record.enabled);
      bus_done_.store(true);
      return;
    }

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
      gst_message_unref(message);
      break;
    }
    gst_message_unref(message);
  }

  bus_done_.store(true);
}

void V4l2Pipeline::EmitTelemetry(TelemetryEvent event) {
  if (telemetry_callback_) {
    telemetry_callback_(std::move(event));
  }
}

void V4l2Pipeline::Fail(const std::string& reason, bool fatal) {
  TelemetryEvent event;
  event.monotonic_ns = rkinfra::ClockMonotonicNs();
  event.stream_id = options_.source.id;
  event.category = "media";
  event.stage = "pipeline";
  event.status = "error";
  event.reason = reason;
  EmitTelemetry(std::move(event));

  if (error_callback_) {
    error_callback_(reason, fatal);
  }
}

void V4l2Pipeline::BindPreviewOverlay() {
  if (!options_.preview.enabled || preview_sink_ == nullptr || options_.preview.window_id == 0) {
    return;
  }
  if (!GST_IS_VIDEO_OVERLAY(preview_sink_)) {
    return;
  }
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(preview_sink_),
                                      static_cast<guintptr>(options_.preview.window_id));
  gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(preview_sink_),
                                         0,
                                         0,
                                         options_.source.width,
                                         options_.source.height);
  gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(preview_sink_), FALSE);
  gst_video_overlay_expose(GST_VIDEO_OVERLAY(preview_sink_));
}

GstPadProbeReturn V4l2Pipeline::OnCaptureProbe(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
  auto* ctx = static_cast<ProbeContext*>(user_data);
  if (ctx == nullptr || ctx->self == nullptr || !(info->type & GST_PAD_PROBE_TYPE_BUFFER)) {
    return GST_PAD_PROBE_OK;
  }

  GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  if (buffer == nullptr) {
    return GST_PAD_PROBE_OK;
  }

  const GstClockTime pts = GST_BUFFER_PTS(buffer);
  const GstClockTime base = ctx->self->base_time_ns_.load();

  TelemetryEvent event;
  event.stream_id = ctx->stream_id;
  event.seq = ctx->seq->fetch_add(1) + 1;
  event.category = "media";
  event.stage = "capture";
  event.status = GST_CLOCK_TIME_IS_VALID(pts) ? "ok" : "missing";
  event.pts_ns = GST_CLOCK_TIME_IS_VALID(pts) ? static_cast<int64_t>(pts) : -1;
  event.monotonic_ns = GST_CLOCK_TIME_IS_VALID(base) && GST_CLOCK_TIME_IS_VALID(pts)
                           ? static_cast<uint64_t>(base + pts)
                           : rkinfra::ClockMonotonicNs();
  if (!GST_CLOCK_TIME_IS_VALID(pts)) {
    event.reason = "invalid_pts";
  }
  ctx->self->EmitTelemetry(std::move(event));
  return GST_PAD_PROBE_OK;
}

GstFlowReturn V4l2Pipeline::OnAppSinkSample(GstAppSink* sink, gpointer user_data) {
  auto* self = static_cast<V4l2Pipeline*>(user_data);
  if (!self || !self->options_.app_sink.sample_callback) {
    return GST_FLOW_OK;
  }

  GstSample* sample = gst_app_sink_pull_sample(sink);
  if (!sample) {
    return GST_FLOW_OK;
  }

  self->options_.app_sink.sample_callback(sample);
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

}  // namespace rkstudio::media
