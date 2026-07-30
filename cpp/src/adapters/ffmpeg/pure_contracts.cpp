#include "sublift/adapters/ffmpeg.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sublift::ffmpeg {

namespace {

std::string format_fps(double fps) {
  if (std::isnan(fps) || std::isinf(fps)) {
    std::ostringstream oss;
    oss << fps;
    return oss.str();
  }
  std::ostringstream oss;
  oss << fps;
  std::string s = oss.str();
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find('E') == std::string::npos) {
    s += ".0";
  }
  return s;
}

std::string to_lower(std::string_view str) {
  std::string result{str};
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

std::string trim(std::string_view str) {
  size_t start = str.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  size_t end = str.find_last_not_of(" \t\n\r");
  return std::string{str.substr(start, end - start + 1)};
}

}  // namespace

void validate_output_crop(const SourceBox& crop, std::int32_t source_width,
                            std::int32_t source_height) {
  const auto x = crop.x;
  const auto y = crop.y;
  const auto w = crop.width;
  const auto h = crop.height;

  const std::string requested = "[" + std::to_string(x) + ", " + std::to_string(y) +
                                ", " + std::to_string(w) + ", " + std::to_string(h) +
                                "]";
  const std::string source =
      std::to_string(source_width) + "x" + std::to_string(source_height);

  if (x < 0 || y < 0) {
    throw std::invalid_argument("output_crop 的 x/y 不能为负：source=" + source +
                                " requested=" + requested);
  }
  if (w <= 0 || h <= 0) {
    throw std::invalid_argument("output_crop 的 width/height 必须为正：source=" +
                                source + " requested=" + requested);
  }
  if (static_cast<std::int64_t>(x) + static_cast<std::int64_t>(w) >
          static_cast<std::int64_t>(source_width) ||
      static_cast<std::int64_t>(y) + static_cast<std::int64_t>(h) >
          static_cast<std::int64_t>(source_height)) {
    throw std::invalid_argument("output_crop 越界：source=" + source +
                                " requested=" + requested);
  }
}

std::string build_output_vf(double fps, const SourceBox* crop) {
  const std::string fps_str = format_fps(fps);
  if (crop == nullptr) {
    return "fps=" + fps_str;
  }
  return "fps=" + fps_str +
         ",format=rgb24,crop=" + std::to_string(crop->width) + ":" +
         std::to_string(crop->height) + ":" + std::to_string(crop->x) + ":" +
         std::to_string(crop->y) + ":exact=1";
}

std::string build_output_vf(double fps, const std::optional<SourceBox>& crop) {
  return build_output_vf(fps, crop.has_value() ? &crop.value() : nullptr);
}

std::pair<bool, std::optional<std::string>> assess_display_transform(
    const nlohmann::json& stream_json) {
  if (stream_json.contains("tags") && stream_json["tags"].is_object()) {
    const auto& tags = stream_json["tags"];
    if (tags.contains("rotate")) {
      std::string rotate_str;
      if (tags["rotate"].is_string()) {
        rotate_str = tags["rotate"].get<std::string>();
      } else if (tags["rotate"].is_number()) {
        rotate_str = tags["rotate"].dump();
      }
      rotate_str = trim(rotate_str);
      if (!rotate_str.empty() && rotate_str != "0") {
        std::string note = "stream.tags.rotate=";
        if (tags["rotate"].is_string()) {
          note += "'" + tags["rotate"].get<std::string>() + "'";
        } else {
          note += rotate_str;
        }
        return {false, note};
      }
    }
  }

  if (!stream_json.contains("side_data_list") || stream_json["side_data_list"].is_null()) {
    return {true, std::nullopt};
  }

  const auto& side_data = stream_json["side_data_list"];
  if (!side_data.is_array()) {
    return {false, "stream.side_data_list 不可解析"};
  }

  for (const auto& item : side_data) {
    if (!item.is_object()) {
      return {false, "stream.side_data_list 项不可解析"};
    }
    std::string side_type;
    if (item.contains("side_data_type")) {
      if (item["side_data_type"].is_string()) {
        side_type = item["side_data_type"].get<std::string>();
      } else {
        side_type = item["side_data_type"].dump();
      }
    }
    const std::string lower_type = to_lower(side_type);
    if (lower_type.find("display matrix") == std::string::npos &&
        lower_type.find("displaymatrix") == std::string::npos) {
      continue;
    }

    if (is_identity_display_matrix(item)) {
      continue;
    }
    return {false, "non-identity display matrix: " + side_type};
  }

  return {true, std::nullopt};
}

bool is_identity_display_matrix(const nlohmann::json& side_data_json) {
  if (!side_data_json.is_object()) {
    return false;
  }

  if (side_data_json.contains("rotation") && !side_data_json["rotation"].is_null()) {
    try {
      double rot = 0.0;
      if (side_data_json["rotation"].is_number()) {
        rot = side_data_json["rotation"].get<double>();
      } else if (side_data_json["rotation"].is_string()) {
        rot = std::stod(side_data_json["rotation"].get<std::string>());
      } else {
        return false;
      }
      return std::abs(rot) <= 1e-3;
    } catch (...) {
      return false;
    }
  }

  const nlohmann::json* matrix_ptr = nullptr;
  if (side_data_json.contains("displaymatrix") &&
      !side_data_json["displaymatrix"].is_null()) {
    matrix_ptr = &side_data_json["displaymatrix"];
  } else if (side_data_json.contains("matrix") &&
             !side_data_json["matrix"].is_null()) {
    matrix_ptr = &side_data_json["matrix"];
  }

  if (matrix_ptr == nullptr || matrix_ptr->is_null()) {
    return false;
  }

  std::vector<double> values;
  if (matrix_ptr->is_string()) {
    std::string s = matrix_ptr->get<std::string>();
    std::replace(s.begin(), s.end(), '\n', ' ');
    std::istringstream iss(s);
    double val = 0.0;
    while (iss >> val) {
      values.push_back(val);
    }
  } else if (matrix_ptr->is_array()) {
    for (const auto& el : *matrix_ptr) {
      if (el.is_number()) {
        values.push_back(el.get<double>());
      } else if (el.is_string()) {
        try {
          values.push_back(std::stod(el.get<std::string>()));
        } catch (...) {
          return false;
        }
      } else {
        return false;
      }
    }
  } else {
    return false;
  }

  if (values.size() < 9) {
    return false;
  }

  constexpr double float_id[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  bool is_float_identity = true;
  for (int i = 0; i < 9; ++i) {
    if (std::abs(values[i] - float_id[i]) > 1e-3) {
      is_float_identity = false;
      break;
    }
  }
  if (is_float_identity) {
    return true;
  }

  constexpr double fixed_id[9] = {65536.0, 0.0, 0.0, 0.0, 65536.0, 0.0, 0.0, 0.0, 1073741824.0};
  double max_abs = 0.0;
  for (int i = 0; i < 9; ++i) {
    max_abs = std::max(max_abs, std::abs(values[i]));
  }

  if (max_abs > 16.0) {
    for (int i = 0; i < 9; ++i) {
      if (std::abs(values[i] - fixed_id[i]) > 1.0) {
        return false;
      }
    }
    return true;
  }

  return false;
}

}  // namespace sublift::ffmpeg
