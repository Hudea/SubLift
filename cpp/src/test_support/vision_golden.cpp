#include <fstream>
#include <sstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

#include "sublift/test_support.hpp"

namespace sublift::test_support {

namespace {

static OcrCropBox parse_box(const nlohmann::json& j) {
  return OcrCropBox{
      .x = j.at("x").get<std::int32_t>(),
      .y = j.at("y").get<std::int32_t>(),
      .width = j.at("width").get<std::int32_t>(),
      .height = j.at("height").get<std::int32_t>(),
  };
}

static OcrLine parse_ocr_line(const nlohmann::json& j) {
  return OcrLine{
      .text = j.at("text").get<std::string>(),
      .confidence = j.at("confidence").get<double>(),
      .box = parse_box(j.at("box")),
  };
}

}  // namespace

VisionGolden load_vision_golden(const std::filesystem::path& path) {
  std::ifstream ifs(path);
  if (!ifs.is_open()) {
    throw std::runtime_error("Failed to open vision golden file: " + path.string());
  }

  try {
    nlohmann::json root;
    ifs >> root;

    if (!root.is_object()) {
      throw std::runtime_error("Invalid vision golden: root is not an object");
    }

    if (root.at("golden_schema_version").get<int>() != 1) {
      throw std::runtime_error("Unsupported golden_schema_version in " + path.string());
    }

    if (root.at("kind").get<std::string>() != "vision") {
      throw std::runtime_error("Invalid kind in vision golden: " + root.at("kind").get<std::string>());
    }

    VisionGolden g;
    const auto& o = root.at("oracle");
    g.oracle.oracle_commit = o.at("oracle_commit").get<std::string>();
    if (o.contains("oracle_branch") && !o.at("oracle_branch").is_null()) {
      g.oracle.oracle_branch = o.at("oracle_branch").get<std::string>();
    }

    if (o.contains("macos_version")) {
      g.macos_version = o.at("macos_version").get<std::string>();
    }
    if (o.contains("vision_available")) {
      g.vision_available = o.at("vision_available").get<bool>();
    }
    if (o.contains("vision_note")) {
      g.vision_note = o.at("vision_note").get<std::string>();
    }

    if (root.contains("default_recognition_languages")) {
      for (const auto& lang : root.at("default_recognition_languages")) {
        g.default_languages.push_back(lang.get<std::string>());
      }
    }

    if (root.contains("cases")) {
      for (const auto& c : root.at("cases")) {
        std::string kind = c.at("kind").get<std::string>();
        if (kind == "normalized_box_to_pixel") {
          VisionBoxTestCase tc;
          tc.name = c.at("name").get<std::string>();
          tc.nx = c.at("nx").get<double>();
          tc.ny = c.at("ny").get<double>();
          tc.nw = c.at("nw").get<double>();
          tc.nh = c.at("nh").get<double>();
          tc.image_width = c.at("image_width").get<std::int32_t>();
          tc.image_height = c.at("image_height").get<std::int32_t>();
          tc.expected_box = parse_box(c.at("expected_box"));
          g.box_cases.push_back(tc);
        } else if (kind == "clamp_box") {
          VisionClampTestCase tc;
          tc.name = c.at("name").get<std::string>();
          tc.x = c.at("x").get<std::int32_t>();
          tc.y = c.at("y").get<std::int32_t>();
          tc.w = c.at("w").get<std::int32_t>();
          tc.h = c.at("h").get<std::int32_t>();
          tc.image_width = c.at("image_width").get<std::int32_t>();
          tc.image_height = c.at("image_height").get<std::int32_t>();
          tc.expected_box = parse_box(c.at("expected_box"));
          g.clamp_cases.push_back(tc);
        } else if (kind == "line_sorting") {
          VisionSortTestCase tc;
          tc.name = c.at("name").get<std::string>();
          for (const auto& l : c.at("input_lines")) {
            tc.input_lines.push_back(parse_ocr_line(l));
          }
          for (const auto& l : c.at("expected_lines")) {
            tc.expected_lines.push_back(parse_ocr_line(l));
          }
          g.sort_cases.push_back(tc);
        } else if (kind == "empty_result") {
          VisionEmptyResultCase tc;
          tc.name = c.at("name").get<std::string>();
          const auto& exp = c.at("expected");
          tc.expected_text = exp.at("text").get<std::string>();
          tc.expected_confidence = exp.at("confidence").get<double>();
          tc.expected_line_count = exp.at("lines").size();
          g.empty_result_cases.push_back(tc);
        }
      }
    }

    return g;
  } catch (const std::exception& e) {
    throw std::runtime_error("Error parsing vision golden " + path.string() + ": " + e.what());
  }
}

}  // namespace sublift::test_support
