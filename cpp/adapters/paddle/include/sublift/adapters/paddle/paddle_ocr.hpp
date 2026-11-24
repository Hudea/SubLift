#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sublift/core/image_view.hpp>
#include <sublift/core/models.hpp>
#include <sublift/ports/ocr_engine.hpp>

namespace sublift::adapters::paddle {

/// 返回 C++ PaddleOCR 引擎在当前运行时/构建中是否可用
[[nodiscard]] bool is_paddle_available() noexcept;

/// Process-level capability-probe cache observability.
struct PaddleProbeCacheStats {
  std::uint64_t hits{0};
  std::uint64_t misses{0};
  std::uint64_t stores{0};
};

[[nodiscard]] PaddleProbeCacheStats paddle_probe_cache_stats() noexcept;

/// Clear the capability-probe cache and reset its counters (for test isolation).
void reset_paddle_probe_cache_for_testing() noexcept;

/// Paddle 执行后端
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
[[nodiscard]] PaddleExecutionResolution resolve_paddle_execution(
    std::optional<std::string_view> explicit_provider,
    std::optional<std::string_view> explicit_device_id,
    std::optional<std::string_view> env_provider,
    std::optional<std::string_view> env_device_id);

/// 以紧凑形式描述执行配置，形如 "cpu" 或 "cuda:0"，供日志与快照使用。
[[nodiscard]] std::string format_paddle_execution(const PaddleExecutionConfig& config);

/// 校验执行配置在当前 Paddle 运行时是否可执行（fail-closed）。
/// 返回空字符串表示通过。
[[nodiscard]] std::string validate_paddle_execution(const PaddleExecutionConfig& config);

/// Det input scaling policy.
struct DetInputGeometry {
  double min_side{736.0};
  double pixel_budget{0.0};
  double short_side{0.0};
  double reference_width{0.0};

  friend constexpr bool operator==(const DetInputGeometry&,
                                   const DetInputGeometry&) = default;
};

inline constexpr double kProductDetPixelBudget = 600000.0;

[[nodiscard]] inline std::optional<std::string> validate_det_short_side(
    double short_side) {
  if (short_side == 0.0) {
    return std::nullopt;
  }
  if (!std::isfinite(short_side)) {
    return "det short side must be finite";
  }
  if (short_side < 64.0 || short_side > 4096.0) {
    return "det short side must be 0 or within [64, 4096]";
  }
  return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> validate_det_pixel_budget(
    double budget) {
  if (budget == 0.0) {
    return std::nullopt;
  }
  if (!std::isfinite(budget)) {
    return "det pixel budget must be finite";
  }
  if (budget < 10000.0 || budget > 16000000.0) {
    return "det pixel budget must be 0 or within [10000, 16000000]";
  }
  return std::nullopt;
}

/// Product-facing paddle options.
struct PaddleOptions {
  std::string model_type{"small"};  // tiny / small / medium
  std::string model_root_dir{};
  std::int32_t intra_op_threads{4};
  std::int32_t cls_batch_size{6};
  std::int32_t rec_batch_size{6};
  PaddleExecutionConfig execution{};
  DetInputGeometry det_geometry{
      .min_side = 736.0,
      .pixel_budget = kProductDetPixelBudget,
      .short_side = 0.0,
      .reference_width = 1920.0,
  };
};

struct PaddleCapabilities {
  bool available{false};
  std::string model_type{"small"};
  std::string model_root{};
  std::string detail{};
};

struct PaddleRuntimeStats {
  std::uint64_t recognize_calls{0};
  std::uint64_t det_boxes{0};
  std::uint64_t cls_batches{0};
  std::uint64_t rec_batches{0};
};

class PaddleOcrEngine final : public IOcrEngine {
 public:
  explicit PaddleOcrEngine(PaddleOptions options = {});
  ~PaddleOcrEngine() override;

  PaddleOcrEngine(PaddleOcrEngine&&) noexcept;
  PaddleOcrEngine& operator=(PaddleOcrEngine&&) noexcept;

  PaddleOcrEngine(const PaddleOcrEngine&) = delete;
  PaddleOcrEngine& operator=(const PaddleOcrEngine&) = delete;

  [[nodiscard]] OcrResult recognize(const ImageView& image) override;
  [[nodiscard]] std::string engine_name() const override;
  [[nodiscard]] PaddleRuntimeStats runtime_stats() const noexcept;
  [[nodiscard]] static PaddleCapabilities probe_capabilities(
      const std::string& model_root_dir = "",
      const std::string& model_type = "small");

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sublift::adapters::paddle
