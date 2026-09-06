#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sublift {

// Paddle 执行后端（docs/design/gpu-execution.md，ADR-0041）。
//
// 区分三个概念：
//   - engine：识别算法，仍为 vision / paddle / mock；
//   - provider：Paddle 内部执行后端，第一版为 cpu / cuda；Vision 与 Mock 不接受；
//   - device_id：CUDA 进程可见设备的逻辑编号；CPU 不携带。
//
// 本头文件不依赖 Ort / CUDA，可被 Core / Server / Application 安全包含。
enum class PaddleProvider { Cpu, Cuda };

/// 已解析的 Paddle 执行配置。CPU 不携带设备；CUDA 携带容器内逻辑设备编号。
struct PaddleExecutionConfig {
  PaddleProvider provider{PaddleProvider::Cpu};
  std::optional<std::int32_t> device_id;

  friend bool operator==(const PaddleExecutionConfig&,
                         const PaddleExecutionConfig&) = default;
};

/// provider 的稳定字符串名（"cpu" / "cuda"）。
[[nodiscard]] std::string_view paddle_provider_name(PaddleProvider provider) noexcept;

/// 解析 provider 名（大小写不敏感、允许首尾 ASCII 空白）；未知/非法值返回 nullopt。
[[nodiscard]] std::optional<PaddleProvider> parse_paddle_provider(std::string_view name);

/// 执行配置解析结果：成功时 error 为空。
struct PaddleExecutionResolution {
  PaddleExecutionConfig config;
  std::string error;
  [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// 依据「显式参数 > 环境变量 > 默认值」解析执行配置并整体校验（fail-closed）。
///
/// - provider 未指定默认 cpu；仅接受 cpu / cuda，其余报错（无 auto 模式）。
/// - device_id：必须为非负整数；CUDA 未指定默认 0；CPU 指定设备即报错。
///
/// 任一来源传 nullopt 或空串都表示未提供。
[[nodiscard]] PaddleExecutionResolution resolve_paddle_execution(
    std::optional<std::string_view> explicit_provider,
    std::optional<std::string_view> explicit_device_id,
    std::optional<std::string_view> env_provider,
    std::optional<std::string_view> env_device_id);

/// 以紧凑形式描述执行配置，形如 "cpu" 或 "cuda:0"，供日志与快照使用。
/// 调用方应仅传入经 resolve/validate 通过的配置（CPU 不应携带 device_id）。
[[nodiscard]] std::string format_paddle_execution(const PaddleExecutionConfig& config);

}  // namespace sublift
