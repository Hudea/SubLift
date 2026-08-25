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

/// 展开波浪号路径（~ -> $HOME）
inline std::filesystem::path expand_tilde(const std::string& raw) {
  if (raw.empty()) return {};
  if (raw[0] == '~') {
    const char* home = std::getenv("HOME");
    if (home) {
      if (raw.size() == 1) return std::filesystem::path(home);
      if (raw[1] == '/') return std::filesystem::path(home) / raw.substr(2);
      return std::filesystem::path(home) / raw.substr(1);
    }
  }
  return std::filesystem::path(raw);
}

/// 允许访问的合法媒体视频扩展名白名单（大小写不敏感）
inline bool is_allowed_media_extension(const std::filesystem::path& p) {
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  // 注意：".ts" (MPEG-TS) 刻意不在白名单内——它与 TypeScript 源码扩展名冲突，
  // 在 CORS 放行的部署下等于开放任意源码文件读取；浏览器 <video> 也无法原生
  // 播放 MPEG-TS，收益远低于风险。需要处理 .ts 视频时请先 remux 为 .mp4。
  static const std::vector<std::string_view> kAllowedExtensions = {
      ".mp4", ".m4v", ".mov", ".webm", ".mkv",
      ".avi", ".flv", ".wmv", ".m2ts",
      ".mpg", ".mpeg", ".vob", ".3gp", ".ogv"
  };

  for (const auto& allowed : kAllowedExtensions) {
    if (ext == allowed) return true;
  }
  return false;
}

/// 解析、规范化并校验视频文件路径，防御 LFI 任意文件读取、空字节注入、非法扩展名穿越与缓存越权访问。
/// @param input_path 用户输入的原始路径
/// @param allowed_root 安全沙箱根目录（若未指定，则检查环境变量 SUBLIFT_ALLOWED_MEDIA_ROOT；未配置则 fail-closed 拒绝）
/// @return 规范化后的绝对路径（canonical path）
/// @throws PathSecurityException 若路径为空、包含空字符、不存在、不是普通文件、扩展名非法、超出沙箱根目录或属于 .sublift_cache 内部文件
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

  // 沙箱根目录校验（显式传入或环境变量）
  std::optional<std::filesystem::path> root = allowed_root;
  if (!root.has_value() || root->empty()) {
    if (const char* env_root = std::getenv("SUBLIFT_ALLOWED_MEDIA_ROOT"); env_root && *env_root) {
      root = std::filesystem::path(env_root);
    }
  }

  // fail-closed：工作区未配置或沙箱根未指定时，严格拒绝访问任何路径
  if (!root.has_value() || root->empty()) {
    throw PathSecurityException("工作区或沙箱根目录未配置 (fail-closed)");
  }

  std::error_code root_ec;
  std::filesystem::path canonical_root = std::filesystem::canonical(*root, root_ec);
  if (root_ec) {
    throw PathSecurityException("沙箱根目录无法解析 (fail-closed): " + root->string());
  }

  std::filesystem::path expanded = expand_tilde(input_path);
  std::filesystem::path target_p = expanded;
  if (target_p.is_relative()) {
    target_p = canonical_root / target_p;
  }

  std::error_code ec;
  // 展开软链接与 '..' 相对路径以防穿越
  std::filesystem::path canonical_p = std::filesystem::canonical(target_p, ec);
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

  // 拒绝直接访问 .sublift_cache 内部缓存文件
  for (const auto& part : canonical_p) {
    if (part.filename() == ".sublift_cache") {
      throw PathSecurityException("安全策略限制：禁止直接访问 .sublift_cache 内部缓存文件");
    }
  }

  // 沙箱根目录越界校验
  auto [root_end, _] = std::mismatch(
      canonical_root.begin(), canonical_root.end(),
      canonical_p.begin(), canonical_p.end());
  if (root_end != canonical_root.end()) {
    throw PathSecurityException("安全策略限制：禁止访问沙箱外部文件");
  }

  return canonical_p;
}

/// 允许保存的字幕输出扩展名白名单（大小写不敏感）
inline bool is_allowed_subtitle_extension(const std::filesystem::path& p) {
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  static const std::vector<std::string_view> kAllowedExtensions = {
      ".srt", ".vtt", ".ass", ".ssa", ".sub", ".txt"
  };
  for (const auto& allowed : kAllowedExtensions) {
    if (ext == allowed) return true;
  }
  return false;
}

/// 解析、规范化并校验字幕输出路径，防御目录穿越与越界写入。
/// @param input_path 用户输入的原始输出路径
/// @param allowed_root 安全沙箱根目录（若未指定，则检查环境变量 SUBLIFT_ALLOWED_MEDIA_ROOT；未配置则 fail-closed 拒绝）
/// @return 规范化后的绝对输出路径
/// @throws PathSecurityException 若路径为空、包含空字符、扩展名非法、超出沙箱根目录或写入 .sublift_cache
inline std::filesystem::path resolve_and_validate_output_path(
    const std::string& input_path,
    const std::optional<std::filesystem::path>& allowed_root = std::nullopt) {
  if (input_path.empty()) {
    throw PathSecurityException("输出路径参数不能为空");
  }

  // 防御空字节截断攻击
  if (input_path.find('\0') != std::string::npos) {
    throw PathSecurityException("非法路径包含空字符");
  }

  // 沙箱根目录校验
  std::optional<std::filesystem::path> root = allowed_root;
  if (!root.has_value() || root->empty()) {
    if (const char* env_root = std::getenv("SUBLIFT_ALLOWED_MEDIA_ROOT"); env_root && *env_root) {
      root = std::filesystem::path(env_root);
    }
  }

  // fail-closed：工作区未配置或沙箱根未指定时，严格拒绝写入任何路径
  if (!root.has_value() || root->empty()) {
    throw PathSecurityException("工作区或沙箱根目录未配置 (fail-closed)");
  }

  std::error_code root_ec;
  std::filesystem::path canonical_root = std::filesystem::canonical(*root, root_ec);
  if (root_ec) {
    throw PathSecurityException("沙箱根目录无法解析 (fail-closed): " + root->string());
  }

  std::filesystem::path expanded = expand_tilde(input_path);
  std::filesystem::path target_p = expanded;
  if (target_p.is_relative()) {
    target_p = canonical_root / target_p;
  }

  // 扩展名白名单校验
  if (!is_allowed_subtitle_extension(target_p)) {
    throw PathSecurityException("不受支持的字幕文件扩展名: " + target_p.extension().string());
  }

  // 展开并规范化父目录路径以防穿越
  std::filesystem::path parent_p = target_p.parent_path();
  std::error_code ec;
  std::filesystem::path canonical_parent = std::filesystem::weakly_canonical(parent_p, ec);
  if (ec) {
    throw PathSecurityException("父级目录无法解析: " + parent_p.string());
  }

  // 组合规范化后的目标路径
  std::filesystem::path normalized_target = canonical_parent / target_p.filename();

  // 拒绝写入 .sublift_cache 内部目录
  for (const auto& part : normalized_target) {
    if (part.filename() == ".sublift_cache") {
      throw PathSecurityException("安全策略限制：禁止直接写入 .sublift_cache 内部缓存文件");
    }
  }

  // 沙箱根目录越界校验
  auto [root_end, _] = std::mismatch(
      canonical_root.begin(), canonical_root.end(),
      normalized_target.begin(), normalized_target.end());
  if (root_end != canonical_root.end()) {
    throw PathSecurityException("安全策略限制：禁止写入沙箱外部目录");
  }

  return normalized_target;
}

}  // namespace sublift::server
