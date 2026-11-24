#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <sublift/ports/ocr_engine.hpp>

namespace sublift::adapters::mock {

class MockOcrEngine : public IOcrEngine {
 public:
  explicit MockOcrEngine(std::string default_text = "MOCK SUBTITLE", double confidence = 0.95);
  explicit MockOcrEngine(OcrResult default_result);
  explicit MockOcrEngine(std::span<const OcrLine> lines);
  explicit MockOcrEngine(std::vector<OcrResult> sequence);

  ~MockOcrEngine() override = default;

  MockOcrEngine(MockOcrEngine&& other) noexcept;
  MockOcrEngine& operator=(MockOcrEngine&& other) noexcept;
  MockOcrEngine(const MockOcrEngine&) = delete;
  MockOcrEngine& operator=(const MockOcrEngine&) = delete;

  void set_default_text(std::string text, double confidence = 0.95);
  void set_default_result(OcrResult result);
  void set_lines(std::span<const OcrLine> lines);
  void set_sequence(std::vector<OcrResult> sequence);
  void queue_result(OcrResult result);
  void reset();

  [[nodiscard]] OcrResult recognize(const ImageView& image) override;
  [[nodiscard]] std::string engine_name() const override;

  [[nodiscard]] std::size_t call_count() const;
  [[nodiscard]] bool is_sequence_mode() const;
  [[nodiscard]] std::size_t sequence_size() const;

 private:
  mutable std::mutex mutex_;
  std::string default_text_{"MOCK SUBTITLE"};
  double default_confidence_{0.95};
  std::optional<OcrResult> default_result_{std::nullopt};
  std::vector<OcrResult> queued_results_{};
  std::optional<std::vector<OcrResult>> sequence_{std::nullopt};
  std::size_t next_result_idx_{0};
  std::size_t call_count_{0};
};

} // namespace sublift::adapters::mock

