#include <filesystem>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "process_utils.hpp"
#include "sublift/adapters/ffmpeg.hpp"

namespace sublift::ffmpeg {

SourceFrameInfo probe_source_frame(const std::filesystem::path& video_path) {
  const std::string ffprobe = resolve_ffprobe_bin();
  const std::vector<std::string> cmd = {
      ffprobe,
      "-v",
      "error",
      "-select_streams",
      "v:0",
      "-show_entries",
      "stream=width,height,tags,side_data_list",
      "-of",
      "json",
      video_path.string(),
  };

  const auto res = detail::run_subprocess(cmd, std::chrono::milliseconds(60000));
  if (res.timed_out) {
    throw std::runtime_error("ffprobe 超时: " + video_path.string());
  }

  if (res.exit_code != 0) {
    const std::string tail = detail::read_stderr_tail(res.stderr_str, 2000);
    const std::string detail = tail.empty() ? "" : "：" + tail;
    throw std::runtime_error("ffprobe 失败（退出码 " +
                             std::to_string(res.exit_code) + "），path=" +
                             video_path.string() + detail);
  }

  nlohmann::json data;
  try {
    data = nlohmann::json::parse(res.stdout_str);
  } catch (const nlohmann::json::parse_error& exc) {
    throw std::runtime_error("ffprobe JSON 解析失败: " + video_path.string());
  }

  if (!data.contains("streams") || !data["streams"].is_array() || data["streams"].empty()) {
    throw std::runtime_error("未找到视频流: " + video_path.string());
  }

  const auto& stream = data["streams"][0];
  std::int32_t width = 0;
  std::int32_t height = 0;

  try {
    if (stream.contains("width") && stream["width"].is_number()) {
      width = stream["width"].get<std::int32_t>();
    } else if (stream.contains("width") && stream["width"].is_string()) {
      width = std::stoi(stream["width"].get<std::string>());
    } else {
      throw std::runtime_error("missing width");
    }

    if (stream.contains("height") && stream["height"].is_number()) {
      height = stream["height"].get<std::int32_t>();
    } else if (stream.contains("height") && stream["height"].is_string()) {
      height = std::stoi(stream["height"].get<std::string>());
    } else {
      throw std::runtime_error("missing height");
    }
  } catch (...) {
    throw std::runtime_error("ffprobe 缺少 width/height: " + video_path.string());
  }

  auto [ok, note] = assess_display_transform(stream);
  return SourceFrameInfo{
      .width = width,
      .height = height,
      .display_transform_ok = ok,
      .transform_note = note,
  };
}

std::int64_t probe_duration_ms(const std::filesystem::path& video_path) {
  std::error_code ec;
  if (!std::filesystem::exists(video_path, ec)) {
    return 0;
  }

  std::string ffprobe;
  try {
    ffprobe = resolve_ffprobe_bin();
  } catch (...) {
    return 0;
  }

  const std::vector<std::string> cmd = {
      ffprobe,
      "-v",
      "error",
      "-show_entries",
      "format=duration",
      "-of",
      "default=noprint_wrappers=1:nokey=1",
      video_path.string(),
  };

  try {
    const auto res = detail::run_subprocess(cmd, std::chrono::milliseconds(10000));
    if (res.exit_code == 0 && !res.stdout_str.empty()) {
      double val = std::stod(res.stdout_str);
      return static_cast<std::int64_t>(val * 1000.0);
    }
  } catch (...) {
    // Ignore exception and return 0 per Python contract
  }

  return 0;
}

VideoInfo probe_video(const std::filesystem::path& video_path) {
  const auto info = probe_source_frame(video_path);
  const auto duration_ms = probe_duration_ms(video_path);
  return VideoInfo{
      .width = info.width,
      .height = info.height,
      .duration_ms = duration_ms,
  };
}

}  // namespace sublift::ffmpeg
