#include <sublift/adapters/ffmpeg/ffmpeg_frame_source.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <sstream>
#include <vector>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "process_utils.hpp"

namespace sublift::adapters::ffmpeg {

namespace {

SourceFrameInfo parse_source_frame_json(const std::string& stdout_str,
                                        const std::filesystem::path& video_path);

std::optional<int> decoded_quarter_turn(const nlohmann::json& stream) {
  std::optional<double> tag_rotation;
  if (stream.contains("tags") && stream["tags"].is_object() && stream["tags"].contains("rotate")) {
    const auto& tag = stream["tags"]["rotate"];
    try {
      const double angle = tag.is_string() ? std::stod(tag.get<std::string>()) : tag.get<double>();
      if (!std::isfinite(angle)) return std::nullopt;
      tag_rotation = angle;
    } catch (...) { return std::nullopt; }
  }
  if (!stream.contains("side_data_list") || stream["side_data_list"].is_null()) {
    if (tag_rotation && std::abs(std::remainder(*tag_rotation, 360.0)) > 0.001) return std::nullopt;
    return 0;
  }
  const auto& side = stream["side_data_list"];
  if (!side.is_array()) return std::nullopt;
  bool seen = false;
  int turns = 0;
  for (const auto& item : side) {
    if (!item.is_object() || !item.contains("side_data_type") || !item["side_data_type"].is_string()) return std::nullopt;
    const auto type = item["side_data_type"].get<std::string>();
    if (type != "Display Matrix" && type != "Display matrix") continue;
    if (seen || !item.contains("displaymatrix") || !item["displaymatrix"].is_string()) return std::nullopt;
    seen = true;
    std::istringstream input(item["displaymatrix"].get<std::string>());
    std::string line;
    std::vector<long long> values;
    while (std::getline(input, line)) {
      if (line.empty()) continue;
      const auto colon = line.find(':');
      if (colon == std::string::npos) return std::nullopt;
      std::istringstream row(line.substr(colon + 1));
      long long a, b, c;
      std::string extra;
      if (!(row >> a >> b >> c) || (row >> extra)) return std::nullopt;
      values.insert(values.end(), {a, b, c});
    }
    if (values.size() != 9) return std::nullopt;
    constexpr std::array<std::array<long long, 9>, 4> rotations{{
      {{65536,0,0,0,65536,0,0,0,1073741824}},
      {{0,-65536,0,65536,0,0,0,0,1073741824}},
      {{-65536,0,0,0,-65536,0,0,0,1073741824}},
      {{0,65536,0,-65536,0,0,0,0,1073741824}},
    }};
    bool found = false;
    for (std::size_t i = 0; i < rotations.size(); ++i) {
      if (std::equal(values.begin(), values.end(), rotations[i].begin())) {
        turns = static_cast<int>(i); found = true; break;
      }
    }
    if (!found) return std::nullopt;
    if (item.contains("rotation") && !item["rotation"].is_null()) {
      try {
        const auto& value = item["rotation"];
        const double angle = value.is_string() ? std::stod(value.get<std::string>()) : value.get<double>();
        if (!std::isfinite(angle) || std::abs(std::remainder(angle - turns * 90.0, 360.0)) > 0.001)
          return std::nullopt;
      } catch (...) { return std::nullopt; }
    }
  }
  if (tag_rotation && std::abs(std::remainder(*tag_rotation - turns * 90.0, 360.0)) > 0.001)
    return std::nullopt;
  return turns;
}

struct MediaTiming {
  std::int64_t duration_ms{0};
  std::int64_t start_ms{0};
};

MediaTiming parse_media_timing(const nlohmann::json& data) {
  if (!data.contains("format") || !data["format"].is_object()) return {};
  const auto& format = data["format"];
  auto seconds = [&](const char* key) -> std::optional<double> {
    if (!format.contains(key) || !format[key].is_string()) return std::nullopt;
    try {
      const double value = std::stod(format[key].get<std::string>());
      if (!std::isfinite(value) || std::abs(value) > 1000000000.0) return std::nullopt;
      return value;
    } catch (...) { return std::nullopt; }
  };
  const double start = seconds("start_time").value_or(0.0);
  const auto reported_duration = seconds("duration");
  double duration = reported_duration.value_or(0.0);
  const auto name = format.value("format_name", std::string{});
  if (reported_duration && (name == "matroska,webm" || name == "matroska" || name == "webm") &&
      duration - start > 0.0) {
    duration -= start;
  }
  return {
    .duration_ms = duration > 0 ? static_cast<std::int64_t>(std::llround(duration * 1000.0)) : 0,
    .start_ms = static_cast<std::int64_t>(std::llround(start * 1000.0))
  };
}

double parse_fps_from_stream(const nlohmann::json& stream) {
  auto parse_fraction = [](const std::string& frac_str) -> std::optional<double> {
    auto pos = frac_str.find('/');
    if (pos == std::string::npos) {
      try {
        double val = std::stod(frac_str);
        if (std::isfinite(val) && val > 0.0) return val;
      } catch (...) {}
      return std::nullopt;
    }
    try {
      double num = std::stod(frac_str.substr(0, pos));
      double den = std::stod(frac_str.substr(pos + 1));
      if (den > 0.0 && num > 0.0 && std::isfinite(num / den)) {
        return num / den;
      }
    } catch (...) {}
    return std::nullopt;
  };

  if (stream.contains("r_frame_rate") && stream["r_frame_rate"].is_string()) {
    if (auto fps = parse_fraction(stream["r_frame_rate"].get<std::string>()); fps.has_value()) {
      return *fps;
    }
  }
  if (stream.contains("avg_frame_rate") && stream["avg_frame_rate"].is_string()) {
    if (auto fps = parse_fraction(stream["avg_frame_rate"].get<std::string>()); fps.has_value()) {
      return *fps;
    }
  }
  return 0.0;
}

SourceFrameInfo parse_source_frame_json(const std::string& stdout_str,
                                        const std::filesystem::path& video_path) {
  nlohmann::json data;
  try {
    data = nlohmann::json::parse(stdout_str);
  } catch (const nlohmann::json::parse_error&) {
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

  const auto turns = decoded_quarter_turn(stream);
  std::int32_t aspect_num = 1, aspect_den = 1;
  bool aspect_ok = true;
  if (stream.contains("sample_aspect_ratio") && !stream["sample_aspect_ratio"].is_null()) {
    if (!stream["sample_aspect_ratio"].is_string()) {
      aspect_ok = false;
    } else {
      const auto value = stream["sample_aspect_ratio"].get<std::string>();
      if (value != "N/A" && value != "0:1" && value != "0:0") {
        std::istringstream ratio(value);
        long long num = 0, den = 0; char colon = 0, extra = 0;
        if ((ratio >> num >> colon >> den) && colon == ':' && !(ratio >> extra) &&
            num > 0 && den > 0 && num <= std::numeric_limits<std::int32_t>::max() &&
            den <= std::numeric_limits<std::int32_t>::max()) {
          aspect_num = static_cast<std::int32_t>(num); aspect_den = static_cast<std::int32_t>(den);
        } else {
          aspect_ok = false;
        }
      }
    }
  }
  const bool ok = turns.has_value() && aspect_ok;
  std::int64_t relative_video_start_ms = 0;
  if (stream.contains("start_time") && stream["start_time"].is_string()) {
    try {
      const double video_start = std::stod(stream["start_time"].get<std::string>());
      if (std::isfinite(video_start) && std::abs(video_start) <= 1000000000.0) {
        const auto format_start = parse_media_timing(data).start_ms;
        relative_video_start_ms = std::max<std::int64_t>(
            0, static_cast<std::int64_t>(std::llround(video_start * 1000.0)) - format_start);
      }
    } catch (...) {}
  }
  if (turns && (*turns == 1 || *turns == 3)) {
    std::swap(width, height);
    std::swap(aspect_num, aspect_den);
  }
  const std::optional<std::string> note = ok ? std::nullopt :
      std::optional<std::string>(aspect_ok ? "不支持的显示矩阵、旋转或镜像变换" : "像素宽高比无效或不受支持");
  return SourceFrameInfo{
      .width = width,
      .height = height,
      .display_transform_ok = ok,
      .transform_note = note,
      .pixel_aspect_num = aspect_num,
      .pixel_aspect_den = aspect_den,
      .relative_video_start_ms = relative_video_start_ms,
  };
}

}  // namespace

SourceFrameInfo probe_source_frame(const std::filesystem::path& video_path) {
  const std::string ffprobe = resolve_ffprobe_bin();
  const std::vector<std::string> cmd = {
      ffprobe,
      "-v",
      "error",
      "-select_streams",
      "v:0",
      "-show_entries",
      "stream=width,height,sample_aspect_ratio,start_time:stream_tags:stream_side_data:format=start_time",
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

  return parse_source_frame_json(res.stdout_str, video_path);
}

SourceAndDuration probe_source_and_duration(const std::filesystem::path& video_path) {
  std::error_code ec;
  if (!std::filesystem::exists(video_path, ec)) {
    throw std::runtime_error("视频文件不存在: " + video_path.string());
  }

  std::string ffprobe;
  try {
    ffprobe = resolve_ffprobe_bin();
  } catch (const std::exception&) {
    return SourceAndDuration{
        .source = SourceFrameInfo{},
        .duration_ms = 0,
    };
  }

  const std::vector<std::string> cmd = {
      ffprobe,
      "-v",
      "error",
      "-select_streams",
      "v:0",
      "-show_entries",
      "stream=codec_name,pix_fmt,width,height,r_frame_rate,avg_frame_rate,sample_aspect_ratio,start_time:stream_tags:stream_side_data:format=format_name,start_time,duration",
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

  SourceFrameInfo source = parse_source_frame_json(res.stdout_str, video_path);

  MediaTiming timing;
  double fps = 0.0;
  std::string codec_name;
  std::string pix_fmt;
  try {
    auto j = nlohmann::json::parse(res.stdout_str);
    timing = parse_media_timing(j);
    if (j.contains("streams") && j["streams"].is_array() && !j["streams"].empty()) {
      const auto& s0 = j["streams"][0];
      fps = parse_fps_from_stream(s0);
      if (s0.contains("codec_name") && s0["codec_name"].is_string()) {
        codec_name = s0["codec_name"].get<std::string>();
      }
      if (s0.contains("pix_fmt") && s0["pix_fmt"].is_string()) {
        pix_fmt = s0["pix_fmt"].get<std::string>();
      }
    }
  } catch (...) {
    timing = {};
  }

  return SourceAndDuration{
      .source = source,
      .duration_ms = timing.duration_ms,
      .format_start_ms = timing.start_ms,
      .fps = fps,
      .codec_name = std::move(codec_name),
      .pix_fmt = std::move(pix_fmt),
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
      "format=format_name,start_time,duration",
      "-of",
      "json",
      video_path.string(),
  };

  try {
    const auto res = detail::run_subprocess(cmd, std::chrono::milliseconds(10000));
    if (res.exit_code == 0 && !res.stdout_str.empty()) {
      return parse_media_timing(nlohmann::json::parse(res.stdout_str)).duration_ms;
    }
  } catch (...) {}

  return 0;
}

VideoInfo probe_video(const std::filesystem::path& video_path) {
  const auto combined = probe_source_and_duration(video_path);
  return VideoInfo{
      .width = combined.source.width,
      .height = combined.source.height,
      .duration_ms = combined.duration_ms,
      .fps = combined.fps,
      .display_transform_ok = combined.source.display_transform_ok,
      .transform_note = combined.source.transform_note,
  };
}

}  // namespace sublift::adapters::ffmpeg
