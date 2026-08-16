#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sublift::server {

class PathSecurityException : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// 允许访问的合法媒体视频扩展名白名单（大小写不敏感）
inline bool is_allowed_media_extension(const std::filesystem::path& p) {
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  static const std::vector<std::string_view> kAllowedExtensions = {
      ".mp4", ".m4v", ".mov", ".webm", ".mkv",
      ".avi", ".flv", ".wmv", ".ts", ".m2ts",
      ".mpg", ".mpeg", ".vob", ".3gp", ".ogv"
  };

  for (const auto& allowed : kAllowedExtensions) {
    if (ext == allowed) return true;
  }
  return false;
}

/// 解析、规范化并校验视频文件路径，防御 LFI 任意文件读取、空字节注入与非法扩展名穿越。
/// @param input_path 用户输入的原始路径
/// @param allowed_root 可选的安全沙箱根目录（若未指定，则检查环境变量 SUBLIFT_ALLOWED_MEDIA_ROOT）
/// @return 规范化后的绝对路径（canonical path）
/// @throws PathSecurityException 若路径为空、包含空字符、不存在、不是普通文件、扩展名非法或超出沙箱根目录
inline std::filesystem::path resolve_and_validate_media_path(
    const std::string& input_path,
    const std::optional<std::filesystem::path>& allowed_root = std::nullopt) {
  if (input_path.empty()) {
    throw PathSecurityException("路径参数不能为空");
  }

  // 防御空字节截断攻击
  if (input_path.find('\0') != std::string::npos) {
    throw PathSecurityException("非法路径包含空字符");
  }

  std::error_code ec;
  std::filesystem::path raw_p(input_path);

  // 展开软链接与 '..' 相对路径以防穿越
  std::filesystem::path canonical_p = std::filesystem::canonical(raw_p, ec);
  if (ec) {
    throw PathSecurityException("文件不存在或无法解析: " + input_path);
  }

  if (!std::filesystem::is_regular_file(canonical_p, ec)) {
    throw PathSecurityException("指定路径不是常规媒体文件: " + canonical_p.string());
  }

  // 扩展名白名单校验（拒绝 /etc/passwd, 代码文件, 数据库文件等）
  if (!is_allowed_media_extension(canonical_p)) {
    throw PathSecurityException("不受支持的媒体文件类型: " + canonical_p.extension().string());
  }

  // 沙箱根目录校验（若配置）
  std::optional<std::filesystem::path> root = allowed_root;
  if (!root.has_value()) {
    if (const char* env_root = std::getenv("SUBLIFT_ALLOWED_MEDIA_ROOT"); env_root && *env_root) {
      root = std::filesystem::path(env_root);
    }
  }

  if (root.has_value() && !root->empty()) {
    std::filesystem::path canonical_root = std::filesystem::canonical(*root, ec);
    if (!ec) {
      auto [root_end, _] = std::mismatch(canonical_root.begin(), canonical_root.end(), canonical_p.begin());
      if (root_end != canonical_root.end()) {
        throw PathSecurityException("安全策略限制：禁止访问沙箱外部文件");
      }
    }
  }

  return canonical_p;
}

}  // namespace sublift::server
