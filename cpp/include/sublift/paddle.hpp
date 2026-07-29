#pragma once

#include <memory>
#include <string>

#include "sublift/models.hpp"
#include "sublift/ocr.hpp"
#include "sublift/paddle_geometry.hpp"

namespace sublift {

/// 返回 C++ PaddleOCR 引擎在当前运行时/构建中是否可用
[[nodiscard]] bool is_paddle_available() noexcept;

struct PaddleOcrOptions {
  std::string model_type{"small"};  // tiny / small / medium
  std::string model_root_dir{};     // 默认空，自动指向 ~/.cache/sublift/rapidocr-models
  bool dump_stages{false};          // 调试：输出 10 阶段 Dump 摘要
  std::string dump_out_dir{};       // 调试 Dump 目录
};

class PaddleOcrEngine final : public IOcrEngine {
 public:
  explicit PaddleOcrEngine(PaddleOcrOptions options = {});
  ~PaddleOcrEngine() override;

  PaddleOcrEngine(PaddleOcrEngine&&) noexcept;
  PaddleOcrEngine& operator=(PaddleOcrEngine&&) noexcept;

  PaddleOcrEngine(const PaddleOcrEngine&) = delete;
  PaddleOcrEngine& operator=(const PaddleOcrEngine&) = delete;

  [[nodiscard]] OcrResult recognize(const ImageView& image) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sublift
