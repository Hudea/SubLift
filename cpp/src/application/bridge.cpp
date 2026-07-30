#include "bridge.hpp"

#include "sublift/bottom_crop_detector.hpp"
#include "sublift/fixed_detector.hpp"
#include "sublift/pipeline.hpp"
#include "sublift/roi_passthrough_detector.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

namespace sublift::worker {

namespace {

constexpr std::size_t kMaxJpegBytes = 20 * 1024 * 1024;
constexpr std::size_t kMaxImagePixels = 50'000'000;
constexpr std::size_t kMaxBase64JpegChars = ((kMaxJpegBytes + 2) / 3) * 4;

static const std::string kBase64Chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

static const std::array<int, 256> kBase64Table = [] {
  std::array<int, 256> table{};
  table.fill(-1);
  for (int i = 0; i < 64; i++) {
    table[static_cast<unsigned char>(kBase64Chars[i])] = i;
  }
  return table;
}();

void emit_perf_stats(const sublift::Pipeline& pipeline) {
  const char* enabled = std::getenv("SUBLIFT_PADDLE_PERF_DIAGNOSTICS");
  if (enabled == nullptr || std::string_view(enabled) != "1") {
    return;
  }
  std::cerr << "SUBLIFT_PERF_STATS"
            << " ocr_calls=" << pipeline.ocr_call_count() << '\n';
}

std::vector<std::uint8_t> decode_base64(std::string_view input) {
  if (input.size() > kMaxBase64JpegChars) {
    throw std::invalid_argument("JPEG base64 过大");
  }
  std::vector<std::uint8_t> out;
  out.reserve((input.size() * 3) / 4);

  int val = 0, valb = -8;
  for (unsigned char c : input) {
    if (c == '=') break;
    if (std::isspace(c)) continue;
    int idx = kBase64Table[c];
    if (idx == -1) {
      throw std::invalid_argument("base64 解码失败: 非法字符");
    }
    val = (val << 6) + idx;
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<std::uint8_t>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  if (out.size() > kMaxJpegBytes) {
    throw std::invalid_argument("JPEG 过大");
  }
  return out;
}

/// Decode JPEG (or other ImageIO-supported still image) bytes to RGB24.
/// Fail-closed: no solid-fill fallback for bad/non-image data.
sublift::ImageBuffer decode_frame_bytes(const std::vector<std::uint8_t>& bytes) {
  if (bytes.empty()) {
    throw std::invalid_argument("JPEG 解码失败: 空数据");
  }
  if (bytes.size() > kMaxJpegBytes) {
    throw std::invalid_argument("JPEG 解码失败: JPEG 过大");
  }
  if (bytes.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8 ||
      bytes[bytes.size() - 2] != 0xFF || bytes.back() != 0xD9) {
    throw std::invalid_argument("JPEG 解码失败: 仅支持 JPEG 数据");
  }

#if defined(__APPLE__)
  CFDataRef cf_data = CFDataCreateWithBytesNoCopy(
      kCFAllocatorDefault, bytes.data(), static_cast<CFIndex>(bytes.size()), kCFAllocatorNull);
  if (!cf_data) {
    throw std::invalid_argument("JPEG 解码失败: 无法创建 CFData");
  }

  CGImageSourceRef source = CGImageSourceCreateWithData(cf_data, nullptr);
  CFRelease(cf_data);
  if (!source) {
    throw std::invalid_argument("JPEG 解码失败: ImageIO 无法解析图像数据");
  }

  CFDictionaryRef properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr);
  if (!properties) {
    CFRelease(source);
    throw std::invalid_argument("JPEG 解码失败: ImageIO 无法读取尺寸");
  }
  CFNumberRef width_number = static_cast<CFNumberRef>(
      CFDictionaryGetValue(properties, kCGImagePropertyPixelWidth));
  CFNumberRef height_number = static_cast<CFNumberRef>(
      CFDictionaryGetValue(properties, kCGImagePropertyPixelHeight));
  long long metadata_width = 0;
  long long metadata_height = 0;
  const bool has_dimensions = width_number != nullptr && height_number != nullptr &&
      CFNumberGetValue(width_number, kCFNumberLongLongType, &metadata_width) &&
      CFNumberGetValue(height_number, kCFNumberLongLongType, &metadata_height);
  CFRelease(properties);
  if (!has_dimensions || metadata_width <= 0 || metadata_height <= 0 ||
      metadata_width > std::numeric_limits<std::int32_t>::max() ||
      metadata_height > std::numeric_limits<std::int32_t>::max() ||
      static_cast<unsigned long long>(metadata_width) >
          kMaxImagePixels / static_cast<unsigned long long>(metadata_height)) {
    CFRelease(source);
    throw std::invalid_argument("JPEG 解码失败: 图像尺寸超过限制");
  }

  CGImageRef cg_image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  CFRelease(source);
  if (!cg_image) {
    throw std::invalid_argument("JPEG 解码失败: ImageIO 无法创建 CGImage");
  }

  const std::size_t w = CGImageGetWidth(cg_image);
  const std::size_t h = CGImageGetHeight(cg_image);
  if (w == 0 || h == 0 || w > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
      h > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
      w > kMaxImagePixels / h || w > std::numeric_limits<std::size_t>::max() / (h * 4)) {
    CGImageRelease(cg_image);
    throw std::invalid_argument("JPEG 解码失败: 图像尺寸无效");
  }

  // CoreGraphics requires 32-bit (RGBA/RGBX) context. Create 32-bit temp storage.
  std::vector<std::uint8_t> temp_rgba(w * h * 4);
  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
  const uint32_t bitmap_info =
      static_cast<uint32_t>(kCGImageAlphaNoneSkipLast) | kCGBitmapByteOrderDefault;
  CGContextRef context = CGBitmapContextCreate(
      temp_rgba.data(), w, h, 8, w * 4, color_space, bitmap_info);
  CGColorSpaceRelease(color_space);

  if (!context) {
    CGImageRelease(cg_image);
    throw std::invalid_argument("JPEG 解码失败: 无法创建位图上下文");
  }

  CGContextDrawImage(context, CGRectMake(0, 0, static_cast<CGFloat>(w), static_cast<CGFloat>(h)),
                     cg_image);
  CGContextRelease(context);
  CGImageRelease(cg_image);

  // Convert RGBA -> RGB24
  sublift::ImageBuffer buf(
      static_cast<std::int32_t>(w), static_cast<std::int32_t>(h), sublift::PixelFormat::RGB24);
  std::uint8_t* dst = buf.data();
  for (std::size_t i = 0; i < w * h; ++i) {
    dst[i * 3 + 0] = temp_rgba[i * 4 + 0];
    dst[i * 3 + 1] = temp_rgba[i * 4 + 1];
    dst[i * 3 + 2] = temp_rgba[i * 4 + 2];
  }
  return buf;
#else
  throw std::invalid_argument("JPEG 解码失败: 当前平台未实现 ImageIO 解码");
#endif
}

}  // namespace

BridgeHandler::BridgeHandler(
    std::unique_ptr<sublift::application::IOcrEngineFactory> engine_factory,
    std::unique_ptr<sublift::application::IPathMediaServices> path_media)
    : engine_factory_(std::move(engine_factory)), path_media_(std::move(path_media)) {}

BridgeHandler::~BridgeHandler() {
  cancel_job();
}

void BridgeHandler::release_job_resources() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  // Pipeline first: it holds non-owning refs into detector/OCR.
  current_extractor_.reset();
  current_pipeline_.reset();
  job_detector_.reset();
  job_ocr_engine_.reset();
}

void BridgeHandler::cancel_job() {
  cancelled_ = true;
  std::shared_ptr<sublift::application::IStreamingExtractor> extractor;
  std::shared_ptr<sublift::Pipeline> pipeline;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    extractor = current_extractor_;
    pipeline = current_pipeline_;
  }
  if (extractor) extractor->cancel();
  if (pipeline) pipeline->cancel();

  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }

  release_job_resources();
  is_job_running_ = false;
  is_path_mode_ = false;
}

std::optional<ipc::Message> BridgeHandler::handle(const ipc::Message& msg, PushCallback push_cb) {
  auto safe_push_cb = [this, push_cb](const ipc::Message& m) {
    if (cancelled_) return;
    push_cb(m);
  };

  return std::visit(
      [this, &safe_push_cb](auto&& arg) -> std::optional<ipc::Message> {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, ipc::HelloMsg>) {
          return handle_hello(arg);
        } else if constexpr (std::is_same_v<T, ipc::StartJobMsg>) {
          return handle_start_job(arg, safe_push_cb);
        } else if constexpr (std::is_same_v<T, ipc::FrameMsg>) {
          return handle_frame(arg, safe_push_cb);
        } else if constexpr (std::is_same_v<T, ipc::FinalizeMsg>) {
          return handle_finalize(arg, safe_push_cb);
        } else if constexpr (std::is_same_v<T, ipc::CancelJobMsg>) {
          return handle_cancel(arg);
        } else {
          return std::nullopt;
        }
      },
      msg);
}

ipc::Message BridgeHandler::handle_hello(const ipc::HelloMsg& /*msg*/) {
  return ipc::ByeMsg{
      .protocol_version = 1,
      .runtime = "cpp",
      .engines = engine_factory_->supported_engines(),
      .capabilities = engine_factory_->capabilities(),
  };
}

std::optional<ipc::Message> BridgeHandler::handle_start_job(const ipc::StartJobMsg& msg,
                                                            PushCallback push_cb) {
  if (is_job_running_) {
    return ipc::DoneMsg{
        .video_id = msg.video_id,
        .ok = false,
        .error = "当前已有 Job 正在运行，无法并行启动新 Job",
    };
  }

  auto err = engine_factory_->validate_engine(msg.engine);
  if (err.has_value()) {
    return ipc::DoneMsg{
        .video_id = msg.video_id,
        .ok = false,
        .error = *err,
    };
  }

  const bool is_path = msg.video_path.has_value() && !msg.video_path->empty();

  if (is_path) {
    if (!std::filesystem::exists(*msg.video_path)) {
      return ipc::DoneMsg{
          .video_id = msg.video_id,
          .ok = false,
          .error = "视频文件不存在: " + *msg.video_path,
      };
    }
  }

  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }

  if (is_path) {
    is_job_running_ = true;
    is_path_mode_ = true;
    cancelled_ = false;
    current_video_id_ = msg.video_id;

    push_cb(ipc::ProgressMsg{
        .video_id = msg.video_id,
        .stage = "ready",
        .pct = 0.0,
        .eta_ms = 0,
    });

    worker_thread_ = std::thread(&BridgeHandler::run_path_mode, this, msg, push_cb);
    return std::nullopt;
  }

  // Frame mode: assemble fully before marking running / emitting ready so a
  // throw (e.g. vision construct failure) cannot leave sticky is_job_running_.
  try {
    sublift::Config cfg;
    cfg.confidence_threshold = msg.confidence_threshold;
    if (msg.enable_ssim_patrol.has_value()) {
      cfg.change_point.enable_ssim_patrol = *msg.enable_ssim_patrol;
    }
    if (msg.subtitle_profile.has_value()) {
      // CLI may send script-only profile (all geometry zero). Apply script to
      // Config.subtitle_script and only keep geometry when non-zero so Pipeline
      // can rebuild a full band profile from the crop.
      const auto& p = *msg.subtitle_profile;
      cfg.subtitle_script = p.script;
      const bool has_geometry =
          p.center_x != 0 || p.center_y != 0 || p.height != 0 || p.y_min != 0 || p.y_max != 0;
      if (has_geometry) {
        cfg.subtitle_profile = p;
      }
    } else if (msg.region_box.has_value() && msg.region_box->width > 0 &&
               msg.region_box->height > 0) {
      cfg.subtitle_profile = sublift::SubtitleProfile::from_crop(
          msg.region_box->width, msg.region_box->height, cfg.subtitle_script);
    }

    std::unique_ptr<sublift::IDetector> detector;
    if (msg.region_box.has_value() && msg.region_box->width > 0 && msg.region_box->height > 0) {
      sublift::FrameLocalBox box{
          .x = msg.region_box->x,
          .y = msg.region_box->y,
          .width = msg.region_box->width,
          .height = msg.region_box->height,
      };
      detector = std::make_unique<sublift::FixedRegionDetector>(box);
    } else {
      detector = std::make_unique<sublift::BottomCropDetector>(cfg.region_bottom_ratio);
    }

    auto ocr_engine = engine_factory_->create_engine();

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      job_detector_ = std::move(detector);
      job_ocr_engine_ = std::move(ocr_engine);
      current_pipeline_ =
          std::make_shared<sublift::Pipeline>(*job_detector_, *job_ocr_engine_, cfg);
    }

    is_job_running_ = true;
    is_path_mode_ = false;
    cancelled_ = false;
    current_video_id_ = msg.video_id;

    push_cb(ipc::ProgressMsg{
        .video_id = msg.video_id,
        .stage = "ready",
        .pct = 0.0,
        .eta_ms = 0,
    });

    return std::nullopt;
  } catch (const std::exception& e) {
    release_job_resources();
    is_job_running_ = false;
    is_path_mode_ = false;
    return ipc::DoneMsg{
        .video_id = msg.video_id,
        .ok = false,
        .error = std::string("Frame mode init error: ") + e.what(),
    };
  }
}

std::optional<ipc::Message> BridgeHandler::handle_frame(const ipc::FrameMsg& msg,
                                                        PushCallback push_cb) {
  if (cancelled_) {
    return ipc::ErrorMsg{.message = "job cancelled"};
  }

  std::shared_ptr<sublift::Pipeline> pipeline;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    pipeline = current_pipeline_;
  }

  if (!is_job_running_ || is_path_mode_ || !pipeline) {
    return ipc::ErrorMsg{
        .message = "frame received in invalid state (must call start_job with frame mode first)"};
  }
  if (msg.video_id != current_video_id_) {
    return ipc::ErrorMsg{.message = "frame video_id mismatch"};
  }

  try {
    std::vector<std::uint8_t> raw_bytes = decode_base64(msg.jpeg_bytes);
    sublift::ImageBuffer img = decode_frame_bytes(raw_bytes);
    sublift::Frame frame{.timestamp_ms = msg.ts_ms, .image = std::move(img)};

    auto evt = pipeline->feed(frame);

    push_cb(ipc::ProgressMsg{
        .video_id = current_video_id_,
        .stage = "processing",
        .pct = 0.5,
        .eta_ms = 0,
    });

    if (evt.has_value()) {
      sublift::SubtitleEntry entry = pipeline->ocr_segment(*evt);
      if (!cancelled_ && !entry.text.empty()) {
        push_cb(ipc::PushEntryMsg{
            .video_id = current_video_id_,
            .entry = entry,
        });
      }
    }
  } catch (const std::exception& e) {
    return ipc::ErrorMsg{.message = std::string("Frame processing error: ") + e.what()};
  }

  return std::nullopt;
}

std::optional<ipc::Message> BridgeHandler::handle_finalize(const ipc::FinalizeMsg& msg,
                                                           PushCallback push_cb) {
  if (cancelled_) {
    return ipc::DoneMsg{.video_id = msg.video_id, .ok = false, .error = "cancelled"};
  }

  std::shared_ptr<sublift::Pipeline> pipeline;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    pipeline = current_pipeline_;
  }

  if (!is_job_running_) {
    return ipc::DoneMsg{.video_id = msg.video_id, .ok = false, .error = "no running job to finalize"};
  }
  if (is_path_mode_) {
    return ipc::DoneMsg{
        .video_id = msg.video_id, .ok = false, .error = "path mode 不需要接收 finalize 消息"};
  }
  if (!pipeline) {
    return ipc::DoneMsg{
        .video_id = msg.video_id, .ok = false, .error = "frame mode pipeline not initialized"};
  }

  try {
    std::vector<sublift::SubtitleEntry> final_entries = pipeline->finalize();
    emit_perf_stats(*pipeline);

    push_cb(ipc::EntriesMsg{
        .video_id = msg.video_id,
        .entries = final_entries,
        .is_final = true,
    });

    push_cb(ipc::ProgressMsg{
        .video_id = msg.video_id,
        .stage = "done",
        .pct = 1.0,
        .eta_ms = 0,
    });

    release_job_resources();
    is_job_running_ = false;
    return ipc::DoneMsg{.video_id = msg.video_id, .ok = true};
  } catch (const std::exception& e) {
    release_job_resources();
    is_job_running_ = false;
    return ipc::DoneMsg{
        .video_id = msg.video_id,
        .ok = false,
        .error = std::string("Finalize error: ") + e.what(),
    };
  }
}

ipc::Message BridgeHandler::handle_cancel(const ipc::CancelJobMsg& msg) {
  cancel_job();
  return ipc::DoneMsg{
      .video_id = msg.video_id,
      .ok = false,
      .error = "cancelled",
  };
}

void BridgeHandler::run_path_mode(ipc::StartJobMsg msg, PushCallback push_cb) {
  const std::string video_id = msg.video_id;
  const std::string video_path = *msg.video_path;
  const double fps = msg.fps > 0.0 ? msg.fps : 1.0;

  // Always reclaim path-mode resources when the worker thread exits (success,
  // failure, or cancel), so Pipeline never outlives detector/OCR.
  struct PathJobGuard {
    BridgeHandler* self;
    ~PathJobGuard() {
      self->release_job_resources();
      self->is_job_running_ = false;
      self->is_path_mode_ = false;
    }
  } guard{this};

  try {
    sublift::Config cfg;
    cfg.confidence_threshold = msg.confidence_threshold;
    if (msg.enable_ssim_patrol.has_value()) {
      cfg.change_point.enable_ssim_patrol = *msg.enable_ssim_patrol;
    }
    if (msg.subtitle_profile.has_value()) {
      const auto& p = *msg.subtitle_profile;
      cfg.subtitle_script = p.script;
      const bool has_geometry =
          p.center_x != 0 || p.center_y != 0 || p.height != 0 || p.y_min != 0 || p.y_max != 0;
      if (has_geometry) {
        cfg.subtitle_profile = p;
      }
    } else if (msg.region_box.has_value() && msg.region_box->width > 0 &&
               msg.region_box->height > 0) {
      cfg.subtitle_profile = sublift::SubtitleProfile::from_crop(
          msg.region_box->width, msg.region_box->height, cfg.subtitle_script);
    }

    std::optional<sublift::SourceBox> region_box_src;
    if (msg.region_box.has_value()) {
      region_box_src = sublift::SourceBox{
          .x = msg.region_box->x,
          .y = msg.region_box->y,
          .width = msg.region_box->width,
          .height = msg.region_box->height,
      };
    }

    if (!path_media_) {
      push_cb(ipc::DoneMsg{
          .video_id = video_id,
          .ok = false,
          .error = "path media services not configured",
      });
      return;
    }

    sublift::FrameIOPlan plan =
        path_media_->plan_frame_io(video_path, region_box_src, "auto", cfg.region_bottom_ratio);

    std::unique_ptr<sublift::IDetector> detector;
    if (plan.output_crop.has_value()) {
      detector = std::make_unique<sublift::RoiPassthroughDetector>(plan.output_crop->width,
                                                                  plan.output_crop->height);
    } else {
      detector = std::make_unique<sublift::BottomCropDetector>(cfg.region_bottom_ratio);
    }

    auto ocr_engine = engine_factory_->create_engine();
    auto extractor = path_media_->create_extractor(fps, plan.output_crop);

    std::shared_ptr<sublift::Pipeline> pipeline;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      job_detector_ = std::move(detector);
      job_ocr_engine_ = std::move(ocr_engine);
      // Pipeline must be built only after detector/OCR are members (non-owning refs).
      current_pipeline_ =
          std::make_shared<sublift::Pipeline>(*job_detector_, *job_ocr_engine_, cfg);
      current_extractor_ = extractor;
      pipeline = current_pipeline_;
    }

    if (!cancelled_) {
      push_cb(ipc::ProgressMsg{
          .video_id = video_id,
          .stage = "processing",
          .pct = 0.0,
          .eta_ms = 0,
      });
    }

    const std::int64_t dur_ms = path_media_->probe_duration_ms(video_path);
    const double duration_s = static_cast<double>(dur_ms) / 1000.0;
    int estimated_total = static_cast<int>(duration_s * fps);
    if (estimated_total < 1) estimated_total = 1;

    int processed_count = 0;

    extractor->extract(
        video_path,
        [this, &pipeline, &push_cb, &video_id, &processed_count, estimated_total](
            const sublift::Frame& frame) -> bool {
          if (cancelled_) return false;

          auto evt = pipeline->feed(frame);
          processed_count++;

          if (!cancelled_ && (processed_count == 1 || processed_count % 5 == 0)) {
            double pct = std::min(
                static_cast<double>(processed_count) / static_cast<double>(estimated_total), 0.99);

            push_cb(ipc::ProgressMsg{
                .video_id = video_id,
                .stage = "processing",
                .pct = pct,
                .eta_ms = 0,
            });
          }

          if (evt.has_value()) {
            sublift::SubtitleEntry entry = pipeline->ocr_segment(*evt);
            if (!cancelled_ && !entry.text.empty()) {
              push_cb(ipc::PushEntryMsg{
                  .video_id = video_id,
                  .entry = entry,
              });
            }
          }

          return !cancelled_;
        });

    if (cancelled_) {
      return;
    }

    push_cb(ipc::ProgressMsg{
        .video_id = video_id,
        .stage = "finalizing",
        .pct = 0.99,
        .eta_ms = 0,
    });

    std::vector<sublift::SubtitleEntry> final_entries = pipeline->finalize();
    emit_perf_stats(*pipeline);

    if (!cancelled_) {
      push_cb(ipc::EntriesMsg{
          .video_id = video_id,
          .entries = final_entries,
          .is_final = true,
      });

      push_cb(ipc::ProgressMsg{
          .video_id = video_id,
          .stage = "done",
          .pct = 1.0,
          .eta_ms = 0,
      });

      push_cb(ipc::DoneMsg{
          .video_id = video_id,
          .ok = true,
      });
    }

  } catch (const std::exception& e) {
    if (!cancelled_) {
      push_cb(ipc::DoneMsg{
          .video_id = video_id,
          .ok = false,
          .error = std::string("Path mode error: ") + e.what(),
      });
    }
  }
}

}  // namespace sublift::worker
