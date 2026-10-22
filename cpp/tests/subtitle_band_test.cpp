#include <catch2/catch_test_macros.hpp>

#include "sublift/subtitle_band.hpp"

#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using sublift::SubtitleBandLine;
using sublift::SubtitleBandResult;

SubtitleBandLine line(int frame, double x, double y, double width, double height,
                      std::string text, double confidence = 0.95) {
  SubtitleBandLine out;
  out.frame_index = frame;
  out.x = x;
  out.y = y;
  out.width = width;
  out.height = height;
  out.text = std::move(text);
  out.confidence = confidence;
  return out;
}

void append_subtitle_frames(std::vector<SubtitleBandLine>& lines, int frames,
                            double y, double height, const char* prefix) {
  for (int frame = 0; frame < frames; ++frame) {
    lines.push_back(line(frame, 0.30, y, 0.40, height,
                         std::string(prefix) + std::to_string(frame)));
  }
}

std::vector<SubtitleBandLine> load_fixture(const std::string& name) {
  const std::string path = std::string(SUBLIFT_BAND_FIXTURE_DIR) + "/" + name +
                           ".json";
  std::ifstream in(path);
  REQUIRE(in.good());
  nlohmann::json root;
  in >> root;
  std::vector<SubtitleBandLine> lines;
  for (const auto& frame : root.at("frames")) {
    const int frame_index = frame.at("frame_index").get<int>();
    for (const auto& item : frame.at("lines")) {
      lines.push_back(line(frame_index, item.at("x").get<double>(),
                           item.at("y").get<double>(),
                           item.at("width").get<double>(),
                           item.at("height").get<double>(),
                           item.at("text").get<std::string>(),
                           item.at("confidence").get<double>()));
    }
  }
  return lines;
}

}  // namespace

TEST_CASE("watermark domain detection is conservative", "[subtitle_band]") {
  using sublift::contains_watermark_domain;
  REQUIRE(contains_watermark_domain("98Tang.COM"));
  REQUIRE(contains_watermark_domain("暖暖的猫 98Tang.COM"));
  REQUIRE(contains_watermark_domain("www.example.com"));
  REQUIRE(contains_watermark_domain("https://example.com/x"));
  REQUIRE_FALSE(contains_watermark_domain("你好，世界"));
  REQUIRE_FALSE(contains_watermark_domain("ZPD 警局"));
  REQUIRE_FALSE(contains_watermark_domain("等等..."));
  REQUIRE_FALSE(contains_watermark_domain("3.14"));
  REQUIRE_FALSE(contains_watermark_domain("No.1"));
}

TEST_CASE("single centered subtitle line produces tight adaptive band",
          "[subtitle_band]") {
  std::vector<SubtitleBandLine> lines;
  for (int frame = 0; frame < 8; ++frame) {
    if (frame == 3 || frame == 7) {
      continue;  // 字幕空档：锚点应依赖间歇性而非常驻
    }
    lines.push_back(line(frame, 0.30, 0.79, 0.40, 0.06, "第" + std::to_string(frame) + "句"));
  }
  sublift::SubtitleBandConfig config;
  config.sample_frames_hint = 8;
  const auto result = sublift::detect_subtitle_band(lines, config);
  REQUIRE(result.detected);
  REQUIRE(result.x == 0.0);
  REQUIRE(result.width == 1.0);
  REQUIRE(result.y > 0.75);
  REQUIRE(result.y < 0.80);
  REQUIRE(result.height > 0.07);
  REQUIRE(result.height < 0.12);
  REQUIRE(result.diagnostics.anchor_support_frames == 6);
  REQUIRE(result.diagnostics.sample_frames == 8);
  REQUIRE(result.preview_text.find("第") != std::string::npos);
}

TEST_CASE("multi-line subtitle expands band across line gap",
          "[subtitle_band]") {
  std::vector<SubtitleBandLine> lines;
  for (int frame = 0; frame < 6; ++frame) {
    lines.push_back(line(frame, 0.30, 0.76, 0.40, 0.06, "上句"));
    lines.push_back(line(frame, 0.32, 0.83, 0.36, 0.06, "下句"));
  }
  const auto result = sublift::detect_subtitle_band(lines);
  REQUIRE(result.detected);
  REQUIRE(result.y < 0.76);
  REQUIRE(result.y + result.height > 0.89);
  REQUIRE(result.height < 0.22);
}

TEST_CASE("side persistent watermark is excluded and X trims safely",
          "[subtitle_band]") {
  std::vector<SubtitleBandLine> lines;
  for (int frame = 0; frame < 8; ++frame) {
    if (frame < 6) {
      lines.push_back(line(frame, 0.30, 0.79, 0.40, 0.06, "正常字幕"));
    }
    lines.push_back(line(frame, 0.02, 0.86, 0.12, 0.045, "98Tang.COM", 0.9));
    lines.push_back(line(frame, 0.03, 0.90, 0.10, 0.045, "暖暖的猫", 0.9));
  }
  const auto result = sublift::detect_subtitle_band(lines);
  REQUIRE(result.detected);
  REQUIRE(result.diagnostics.excluded_watermark_lines >= 8);
  REQUIRE(result.diagnostics.side_watermark_trimmed);
  REQUIRE(result.x > 0.15);
  REQUIRE(result.x + result.width < 0.85);
  REQUIRE(result.width <= 0.65);
  REQUIRE(result.preview_text.find("正常字幕") != std::string::npos);
  REQUIRE(result.preview_text.find("98Tang") == std::string::npos);
}

TEST_CASE("scrolling URL watermark does not become the anchor",
          "[subtitle_band]") {
  std::vector<SubtitleBandLine> lines;
  const double wm_x[8] = {0.10, 0.30, 0.50, 0.70, 0.20, 0.60, 0.40, 0.80};
  for (int frame = 0; frame < 8; ++frame) {
    if (frame < 6) {
      lines.push_back(line(frame, 0.30, 0.79, 0.40, 0.06, "第" + std::to_string(frame) + "句台词"));
    }
    lines.push_back(line(frame, wm_x[frame], 0.80, 0.20, 0.045,
                         "暖暖的猫 98Tang.COM", 0.9));
  }
  const auto result = sublift::detect_subtitle_band(lines);
  REQUIRE(result.detected);
  REQUIRE(result.diagnostics.anchor_frame_index >= 0);
  REQUIRE(result.preview_text.find("98Tang") == std::string::npos);
  REQUIRE(result.diagnostics.excluded_watermark_lines >= 1);
  REQUIRE(result.y < 0.82);
}

TEST_CASE("oversized logo box is rejected", "[subtitle_band]") {
  std::vector<SubtitleBandLine> lines;
  for (int frame = 0; frame < 8; ++frame) {
    lines.push_back(line(frame, 0.40, 0.55, 0.20, 0.28, "DisNep", 0.99));
    if (frame < 6) {
      lines.push_back(line(frame, 0.30, 0.79, 0.40, 0.06, "字幕内容"));
    }
  }
  const auto result = sublift::detect_subtitle_band(lines);
  REQUIRE(result.detected);
  REQUIRE(result.y > 0.72);
  REQUIRE(result.height < 0.20);
}

TEST_CASE("empty input fails closed", "[subtitle_band]") {
  const auto result = sublift::detect_subtitle_band({});
  REQUIRE_FALSE(result.detected);
  REQUIRE(result.diagnostics.reason == "empty_input");
}

TEST_CASE("only URL lines fail closed without returning a watermark box",
          "[subtitle_band]") {
  std::vector<SubtitleBandLine> lines;
  for (int frame = 0; frame < 8; ++frame) {
    lines.push_back(line(frame, 0.35, 0.79, 0.30, 0.06, "www.example.com", 0.9));
  }
  const auto result = sublift::detect_subtitle_band(lines);
  REQUIRE_FALSE(result.detected);
  REQUIRE(result.diagnostics.reason == "no_subtitle_anchor");
  REQUIRE(result.x == 0.0);
  REQUIRE(result.width == 1.0);
}

TEST_CASE("single-frame playhead mode detects one line", "[subtitle_band]") {
  const std::vector<SubtitleBandLine> lines = {
      line(0, 0.30, 0.79, 0.40, 0.06, "当前帧字幕")};
  const auto result = sublift::detect_subtitle_band(lines);
  REQUIRE(result.detected);
  REQUIRE(result.diagnostics.sample_frames == 1);
  REQUIRE(result.y > 0.74);
  REQUIRE(result.y < 0.80);
  REQUIRE(result.width == 1.0);
}

TEST_CASE("playhead on watermark-only frame fails closed", "[subtitle_band]") {
  // 单帧重检落在无字幕、只有水印的画面：不能把水印当字幕区返回。
  std::vector<SubtitleBandLine> lines;
  lines.push_back(line(0, 0.02, 0.86, 0.12, 0.045, "暖暖的猫", 0.95));
  lines.push_back(line(0, 0.02, 0.90, 0.12, 0.045, "98Tang.COM", 0.95));
  const auto result = sublift::detect_subtitle_band(lines);
  REQUIRE_FALSE(result.detected);
  REQUIRE(result.diagnostics.reason == "no_subtitle_anchor");
}

TEST_CASE("persistent watermark-only track fails closed", "[subtitle_band]") {
  // 全片只有常驻水印（无字幕）：所有候选都是常驻短文本，不能选出锚点。
  std::vector<SubtitleBandLine> lines;
  for (int frame = 0; frame < 8; ++frame) {
    lines.push_back(line(frame, 0.03, 0.86, 0.12, 0.045, "暖暖的猫", 0.95));
    lines.push_back(line(frame, 0.03, 0.90, 0.12, 0.045, "98Tang.COM", 0.95));
  }
  const auto result = sublift::detect_subtitle_band(lines);
  REQUIRE_FALSE(result.detected);
  REQUIRE(result.diagnostics.reason == "no_subtitle_anchor");
}

TEST_CASE("letterboxed lower subtitle still detected", "[subtitle_band]") {
  std::vector<SubtitleBandLine> lines;
  for (int frame = 0; frame < 6; ++frame) {
    lines.push_back(line(frame, 0.32, 0.86, 0.36, 0.05, "字母框字幕"));
  }
  const auto result = sublift::detect_subtitle_band(lines);
  REQUIRE(result.detected);
  REQUIRE(result.y > 0.82);
  REQUIRE(result.y + result.height <= 1.0);
}

// ---- 录制 fixture 黄金（D01 采集；期望来自原型标定，容差 0.02-0.03） ----

TEST_CASE("fixture watermark_band_32s golden band", "[subtitle_band][golden]") {
  const auto lines = load_fixture("watermark_band_32s");
  const auto result = sublift::detect_subtitle_band(lines);
  REQUIRE(result.detected);
  REQUIRE(result.y > 0.78);
  REQUIRE(result.y < 0.84);
  REQUIRE(result.height > 0.09);
  REQUIRE(result.height < 0.16);
  REQUIRE(result.preview_text.find("字幕") != std::string::npos);
  REQUIRE(result.diagnostics.excluded_watermark_lines >= 1);
}

TEST_CASE("fixture scrolling_watermark_32s golden band",
          "[subtitle_band][golden]") {
  const auto lines = load_fixture("scrolling_watermark_32s");
  const auto result = sublift::detect_subtitle_band(lines);
  REQUIRE(result.detected);
  REQUIRE(result.y > 0.77);
  REQUIRE(result.y < 0.84);
  REQUIRE(result.height > 0.09);
  REQUIRE(result.height < 0.17);
  REQUIRE(result.preview_text.find("98Tang") == std::string::npos);
}

TEST_CASE("fixture band_1080p golden band", "[subtitle_band][golden]") {
  const auto lines = load_fixture("band_1080p");
  const auto result = sublift::detect_subtitle_band(lines);
  REQUIRE(result.detected);
  REQUIRE(result.y > 0.84);
  REQUIRE(result.y < 0.92);
  REQUIRE(result.height > 0.08);
  REQUIRE(result.height < 0.18);
}

TEST_CASE("fixture Zootopia golden band", "[subtitle_band][golden]") {
  const auto lines = load_fixture("Zootopia_clip_1080p");
  const auto result = sublift::detect_subtitle_band(lines);
  REQUIRE(result.detected);
  REQUIRE(result.y > 0.72);
  REQUIRE(result.y < 0.79);
  REQUIRE(result.height > 0.10);
  REQUIRE(result.height < 0.18);
}
