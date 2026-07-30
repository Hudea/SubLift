#include "sublift/models/model_bundle.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace sublift::models {

ModelType parse_model_type(std::string_view name) {
  std::string lower_name;
  lower_name.reserve(name.size());
  for (char c : name) {
    lower_name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }

  if (lower_name == "tiny") {
    return ModelType::Tiny;
  }
  if (lower_name == "small") {
    return ModelType::Small;
  }
  if (lower_name == "medium") {
    return ModelType::Medium;
  }

  throw std::invalid_argument("未知 model_type: '" + std::string(name) +
                              "'，可选: 'medium', 'small', 'tiny'");
}

std::string model_type_to_string(ModelType type) {
  switch (type) {
    case ModelType::Tiny:
      return "tiny";
    case ModelType::Small:
      return "small";
    case ModelType::Medium:
      return "medium";
  }
  return "small";
}

std::filesystem::path expand_user_path(const std::filesystem::path& path) {
  std::string p_str = path.string();
  if (p_str.empty() || p_str[0] != '~') {
    return path;
  }

  const char* home = std::getenv("HOME");
  if (!home || std::string_view(home).empty()) {
    home = std::getenv("USERPROFILE");
  }

  if (!home) {
    return path;
  }

  return std::filesystem::path(std::string(home) + p_str.substr(1));
}

std::filesystem::path resolve_model_dir(const std::string& custom_dir) {
  if (!custom_dir.empty()) {
    return expand_user_path(custom_dir);
  }

  const char* env_dir = std::getenv("SUBLIFT_PADDLE_MODEL_DIR");
  if (env_dir && !std::string_view(env_dir).empty()) {
    return expand_user_path(env_dir);
  }

  return expand_user_path("~/.cache/sublift/rapidocr-models");
}

ModelPaths get_expected_model_paths(const std::filesystem::path& model_dir, ModelType model_type) {
  const std::string suffix = model_type_to_string(model_type);
  ModelPaths paths;
  paths.det_path = model_dir / ("PP-OCRv6_det_" + suffix + ".onnx");
  paths.cls_path = model_dir / "ch_ppocr_mobile_v2.0_cls_mobile.onnx";
  paths.rec_path = model_dir / ("PP-OCRv6_rec_" + suffix + ".onnx");
  if (model_type == ModelType::Tiny) {
    paths.keys_path = model_dir / "ppocrv6_tiny_dict.txt";
  } else {
    paths.keys_path = model_dir / "ppocrv6_dict.txt";
  }
  return paths;
}

bool validate_model_paths(const ModelPaths& paths, std::string* error_msg) {
  std::vector<std::string> missing;
  if (!std::filesystem::exists(paths.det_path)) {
    missing.push_back(paths.det_path.filename().string());
  }
  if (!std::filesystem::exists(paths.cls_path)) {
    missing.push_back(paths.cls_path.filename().string());
  }
  if (!std::filesystem::exists(paths.rec_path)) {
    missing.push_back(paths.rec_path.filename().string());
  }
  bool keys_ok = std::filesystem::exists(paths.keys_path);
  if (!keys_ok) {
    auto alt = paths.keys_path.parent_path() / "ppocrv6_dict.txt";
    keys_ok = std::filesystem::exists(alt);
    if (!keys_ok) {
      missing.push_back(paths.keys_path.filename().string());
    }
  }

  if (missing.empty()) {
    return true;
  }

  if (error_msg) {
    *error_msg = "缺少 Paddle 模型文件: ";
    for (size_t i = 0; i < missing.size(); ++i) {
      if (i > 0) *error_msg += ", ";
      *error_msg += missing[i];
    }
  }
  return false;
}

}  // namespace sublift::models
