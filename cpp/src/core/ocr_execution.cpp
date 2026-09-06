#include "sublift/core/ocr_execution.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace sublift {

namespace {

std::string_view trim_view(std::string_view value) noexcept {
  // 解析只接受 ASCII 空白，不依赖 locale，保持跨机器行为稳定。
  const auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
  };
  while (!value.empty() && is_space(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_space(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

std::string to_lower(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

std::optional<std::int32_t> parse_device_id(std::string_view raw) {
  const std::string_view s = trim_view(raw);
  if (s.empty()) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10 + (c - '0');
    if (value > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
      return std::nullopt;
    }
  }
  return static_cast<std::int32_t>(value);
}

// 返回 nullopt 表示该来源未提供（缺省或空串）。
std::optional<std::string_view> non_empty(std::optional<std::string_view> source) {
  if (!source.has_value()) {
    return std::nullopt;
  }
  const std::string_view trimmed = trim_view(*source);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  return trimmed;
}

}  // namespace

std::string_view paddle_provider_name(PaddleProvider provider) noexcept {
  return provider == PaddleProvider::Cuda ? "cuda" : "cpu";
}

std::optional<PaddleProvider> parse_paddle_provider(std::string_view name) {
  const std::string lower = to_lower(trim_view(name));
  if (lower == "cpu") {
    return PaddleProvider::Cpu;
  }
  if (lower == "cuda") {
    return PaddleProvider::Cuda;
  }
  return std::nullopt;
}

PaddleExecutionResolution resolve_paddle_execution(
    std::optional<std::string_view> explicit_provider,
    std::optional<std::string_view> explicit_device_id,
    std::optional<std::string_view> env_provider,
    std::optional<std::string_view> env_device_id) {
  PaddleExecutionResolution out;

  // provider：显式参数 > 环境变量 > 默认 cpu
  auto provider_raw = non_empty(explicit_provider);
  if (!provider_raw.has_value()) {
    provider_raw = non_empty(env_provider);
  }

  PaddleProvider provider = PaddleProvider::Cpu;
  if (provider_raw.has_value()) {
    const auto parsed = parse_paddle_provider(*provider_raw);
    if (!parsed.has_value()) {
      out.error = "非法的 Paddle 执行后端 '" + std::string(*provider_raw) +
                  "'：仅支持 cpu / cuda";
      return out;
    }
    provider = *parsed;
  }

  // device_id：显式参数 > 环境变量
  auto device_raw = non_empty(explicit_device_id);
  if (!device_raw.has_value()) {
    device_raw = non_empty(env_device_id);
  }

  std::optional<std::int32_t> device_id;
  if (device_raw.has_value()) {
    const auto parsed_device = parse_device_id(*device_raw);
    if (!parsed_device.has_value()) {
      out.error = "非法的 Paddle 设备编号 '" + std::string(*device_raw) +
                  "'：必须为非负整数";
      return out;
    }
    device_id = *parsed_device;
  }

  // 组合校验：CPU 不接受设备；CUDA 缺省设备 0。
  if (provider == PaddleProvider::Cpu) {
    if (device_id.has_value()) {
      out.error = "CPU 执行后端不接受设备编号（device_id）";
      return out;
    }
  } else {
    if (!device_id.has_value()) {
      device_id = 0;
    }
  }

  out.config.provider = provider;
  out.config.device_id = device_id;
  return out;
}

std::string format_paddle_execution(const PaddleExecutionConfig& config) {
  std::string out{paddle_provider_name(config.provider)};
  if (config.device_id.has_value()) {
    out += ":" + std::to_string(*config.device_id);
  }
  return out;
}

}  // namespace sublift
