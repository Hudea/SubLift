#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sublift/models.hpp"
#include "sublift/ocr.hpp"

namespace sublift {

/// 返回 C++ PaddleOCR 引擎在当前运行时/构建中是否可用
[[nodiscard]] bool is_paddle_available() noexcept;

/// Product-facing paddle options.
struct PaddleOptions {
  std::string model_type{"small"};  // tiny / small / medium
  std::string model_root_dir{};
  std::int32_t intra_op_threads{4};
  std::int32_t cls_batch_size{6};
  std::int32_t rec_batch_size{6};
};

/// Backward-compatible alias used by existing call sites / parity tools.
using PaddleOcrOptions = PaddleOptions;

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
  [[nodiscard]] PaddleRuntimeStats runtime_stats() const noexcept;
  [[nodiscard]] static PaddleCapabilities probe_capabilities(
      const std::string& model_root_dir = "",
      const std::string& model_type = "small");

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sublift
