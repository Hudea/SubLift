#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace sublift::paddle_detail {

/// 模型规格枚举：tiny / small / medium
enum class ModelType {
  Tiny,
  Small,
  Medium
};

/// 1. 字符串转 ModelType 枚举（不区分大小写），非法规格抛出 std::invalid_argument
ModelType parse_model_type(std::string_view name);

/// 2. ModelType 转规范小写字符串
std::string model_type_to_string(ModelType type);

/// 3. 解析用户主目录波浪号 (~) 扩展
std::filesystem::path expand_user_path(const std::filesystem::path& path);

/// 4. 解析 Paddle 模型缓存目录：
/// 优先级：自定义 custom_dir > 环境变量 SUBLIFT_PADDLE_MODEL_DIR > 默认 ~/.cache/sublift/rapidocr-models
std::filesystem::path resolve_model_dir(const std::string& custom_dir = "");

/// 5. 预期模型路径组合
struct ModelPaths {
  std::filesystem::path det_path;
  std::filesystem::path cls_path;
  std::filesystem::path rec_path;
  std::filesystem::path keys_path;
};

/// 获取特定 ModelType 对应的预期模型文件路径
ModelPaths get_expected_model_paths(const std::filesystem::path& model_dir, ModelType model_type);

/// 校验模型路径组合是否存在，若存在返回 true；若缺失填入 error_msg 说明
bool validate_model_paths(const ModelPaths& paths, std::string* error_msg = nullptr);

}  // namespace sublift::paddle_detail
