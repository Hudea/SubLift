#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sublift/models.hpp"
#include "sublift/ocr.hpp"
#include "sublift/ports/paddle_geometry.hpp"

namespace sublift {

/// 返回 C++ PaddleOCR 引擎在当前运行时/构建中是否可用
[[nodiscard]] bool is_paddle_available() noexcept;

/// Diagnostic-only tensor capture. Values are copied only when a caller
/// explicitly supplies PaddleOcrOptions::stage_trace.
struct PaddleTensorTrace {
  std::vector<std::int64_t> shape;
  std::vector<float> values;
};

struct PaddleQuadTrace {
  float x0{0.0f};
  float y0{0.0f};
  float x1{0.0f};
  float y1{0.0f};
  float x2{0.0f};
  float y2{0.0f};
  float x3{0.0f};
  float y3{0.0f};
  float score{0.0f};
};

struct PaddleCropStageTrace {
  std::int32_t width{0};
  std::int32_t height{0};
  bool rotated_90{false};
  std::vector<std::uint8_t> rgb;
};

struct PaddleClsResultTrace {
  std::string label;
  double score{0.0};
  bool rotated_180{false};
};

struct PaddleDecodedTrace {
  std::vector<std::int32_t> ctc_tokens;
  std::string decoded_text;
  double decoded_confidence{0.0};
};

/// One recognize-call trace. It intentionally lives outside IOcrEngine so the
/// core interface stays provider-neutral. Timings are observational and are
/// excluded from deterministic golden comparisons.
struct PaddleStageTrace {
  std::int32_t schema_version{1};
  std::int32_t input_width{0};
  std::int32_t input_height{0};
  std::int32_t input_stride{0};
  std::string input_color_order{"RGB"};
  std::vector<std::uint8_t> input_rgb;
  std::int32_t working_width{0};
  std::int32_t working_height{0};
  std::vector<std::uint8_t> working_rgb;
  double global_ratio_h{1.0};
  double global_ratio_w{1.0};
  std::int32_t global_padding_top{0};
  std::int32_t global_padding_left{0};

  PaddleTensorTrace det_input;
  PaddleTensorTrace det_probability_map;
  std::vector<PaddleQuadTrace> det_quads;
  std::vector<PaddleCropStageTrace> crops;
  std::vector<PaddleTensorTrace> cls_inputs;
  std::vector<PaddleTensorTrace> cls_logits;
  std::vector<PaddleClsResultTrace> cls_results;
  std::vector<PaddleTensorTrace> rec_inputs;
  std::vector<PaddleTensorTrace> rec_logits;
  std::vector<PaddleDecodedTrace> decoded_results;
  std::vector<OcrLine> output_lines;

  bool cls_enabled{false};
  std::int32_t recognize_call_count{0};
  std::int32_t det_call_count{0};
  std::int32_t cls_call_count{0};
  std::int32_t rec_call_count{0};

  double global_preprocess_ms{0.0};
  double det_preprocess_ms{0.0};
  double det_infer_ms{0.0};
  double det_postprocess_ms{0.0};
  double crop_ms{0.0};
  double cls_preprocess_ms{0.0};
  double cls_infer_ms{0.0};
  double cls_postprocess_ms{0.0};
  double rec_preprocess_ms{0.0};
  double rec_infer_ms{0.0};
  double rec_decode_ms{0.0};
  double output_ms{0.0};

  void clear();
};

struct PaddleOcrOptions {
  std::string model_type{"small"};  // tiny / small / medium
  std::string model_root_dir{};     // 默认空，自动指向 ~/.cache/sublift/rapidocr-models
  // 0 keeps ONNX Runtime's default. The product defaults below are selected
  // by the 6.8 fixed-input sweep; the trace executable explicitly keeps the
  // Python-equivalent 0/6 settings for operator parity.
  std::int32_t intra_op_threads{4};
  std::int32_t cls_batch_size{6};
  // Keep the RapidOCR batch contract as the product default. A batch=1 sweep
  // was faster on one two-line microcase but changed a frozen Latin SRT hash.
  std::int32_t rec_batch_size{6};
  bool dump_stages{false};          // 调试：启用一次 recognize 的阶段捕获
  std::string dump_out_dir{};       // 保留兼容；落盘由独立 trace 工具负责
  PaddleStageTrace* stage_trace{nullptr};  // 非拥有指针；调用方保证生命周期
  // Diagnostic-only: replace Det postprocess output with frozen quads so the
  // crop/Cls/Rec operator chain can be audited independently of Det drift.
  const std::vector<PaddleQuadTrace>* det_quads_override{nullptr};
};

struct PaddleRuntimeStats {
  std::uint64_t recognize_calls{0};
  std::uint64_t det_boxes{0};
  std::uint64_t cls_batches{0};
  std::uint64_t rec_batches{0};
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
  [[nodiscard]] PaddleRuntimeStats runtime_stats() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sublift
