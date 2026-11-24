#pragma once

#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace sublift::adapters::ffmpeg::detail {

/// 判断显示/旋转变换是否恒等且可信任
[[nodiscard]] std::pair<bool, std::optional<std::string>> assess_display_transform(
    const nlohmann::json& stream_json);

/// 尽力识别恒等 Display Matrix
[[nodiscard]] bool is_identity_display_matrix(const nlohmann::json& side_data_json);

}  // namespace sublift::adapters::ffmpeg::detail
