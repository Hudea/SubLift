#include <sublift/adapters/mock/mock_ocr.hpp>
#include <utility>

namespace sublift::adapters::mock {

MockOcrEngine::MockOcrEngine(std::string default_text, double confidence)
    : default_text_(std::move(default_text)), default_confidence_(confidence) {}

MockOcrEngine::MockOcrEngine(OcrResult default_result)
    : default_text_(default_result.text),
      default_confidence_(default_result.confidence),
      default_result_(std::move(default_result)) {}

MockOcrEngine::MockOcrEngine(std::span<const OcrLine> lines)
    : default_result_(OcrResult::from_lines(lines)) {
  if (default_result_.has_value()) {
    default_text_ = default_result_->text;
    default_confidence_ = default_result_->confidence;
  }
}

MockOcrEngine::MockOcrEngine(std::vector<OcrResult> sequence) {
  set_sequence(std::move(sequence));
}

MockOcrEngine::MockOcrEngine(MockOcrEngine&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  default_text_ = std::move(other.default_text_);
  default_confidence_ = other.default_confidence_;
  default_result_ = std::move(other.default_result_);
  queued_results_ = std::move(other.queued_results_);
  sequence_ = std::move(other.sequence_);
  next_result_idx_ = other.next_result_idx_;
  call_count_ = other.call_count_;
}

MockOcrEngine& MockOcrEngine::operator=(MockOcrEngine&& other) noexcept {
  if (this != &other) {
    std::scoped_lock lock(mutex_, other.mutex_);
    default_text_ = std::move(other.default_text_);
    default_confidence_ = other.default_confidence_;
    default_result_ = std::move(other.default_result_);
    queued_results_ = std::move(other.queued_results_);
    sequence_ = std::move(other.sequence_);
    next_result_idx_ = other.next_result_idx_;
    call_count_ = other.call_count_;
  }
  return *this;
}

void MockOcrEngine::set_default_text(std::string text, double confidence) {
  std::lock_guard<std::mutex> lock(mutex_);
  default_text_ = std::move(text);
  default_confidence_ = confidence;
  default_result_ = std::nullopt;
}

void MockOcrEngine::set_default_result(OcrResult result) {
  std::lock_guard<std::mutex> lock(mutex_);
  default_text_ = result.text;
  default_confidence_ = result.confidence;
  default_result_ = std::move(result);
}

void MockOcrEngine::set_lines(std::span<const OcrLine> lines) {
  std::lock_guard<std::mutex> lock(mutex_);
  default_result_ = OcrResult::from_lines(lines);
  default_text_ = default_result_->text;
  default_confidence_ = default_result_->confidence;
}

void MockOcrEngine::set_sequence(std::vector<OcrResult> sequence) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!sequence.empty()) {
    const auto orig_size = sequence.size();
    while (sequence.size() < orig_size + 8) {
      sequence.push_back(sequence.back());
    }
  }
  sequence_ = std::move(sequence);
  next_result_idx_ = 0;
}

void MockOcrEngine::queue_result(OcrResult result) {
  std::lock_guard<std::mutex> lock(mutex_);
  queued_results_.push_back(std::move(result));
}

void MockOcrEngine::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  next_result_idx_ = 0;
  call_count_ = 0;
}

OcrResult MockOcrEngine::recognize(const ImageView& image) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++call_count_;

  if (sequence_.has_value()) {
    if (sequence_->empty()) {
      if (default_result_.has_value()) {
        return *default_result_;
      }
      OcrLine line{
          .text = default_text_,
          .confidence = default_confidence_,
          .box = OcrCropBox{0, 0, image.width(), image.height()},
      };
      return OcrResult{
          .text = default_text_,
          .confidence = default_confidence_,
          .lines = {std::move(line)},
      };
    }
    if (next_result_idx_ < sequence_->size()) {
      return (*sequence_)[next_result_idx_++];
    }
    return sequence_->back();
  }

  if (next_result_idx_ < queued_results_.size()) {
    return queued_results_[next_result_idx_++];
  }

  if (default_result_.has_value()) {
    return *default_result_;
  }

  OcrLine line{
      .text = default_text_,
      .confidence = default_confidence_,
      .box = OcrCropBox{0, 0, image.width(), image.height()},
  };

  return OcrResult{
      .text = default_text_,
      .confidence = default_confidence_,
      .lines = {std::move(line)},
  };
}

std::string MockOcrEngine::engine_name() const {
  return "MockOcrEngine";
}

std::size_t MockOcrEngine::call_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return call_count_;
}

bool MockOcrEngine::is_sequence_mode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sequence_.has_value();
}

std::size_t MockOcrEngine::sequence_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sequence_.has_value() ? sequence_->size() : 0;
}

} // namespace sublift::adapters::mock

