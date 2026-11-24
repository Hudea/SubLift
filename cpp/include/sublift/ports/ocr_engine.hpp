#pragma once

#include <span>
#include <string>
#include <vector>
#include <sublift/core/box.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/core/models.hpp>

namespace sublift {

struct OcrResult {
  std::string text;
  double confidence{0.0};
  std::vector<OcrLine> lines{};

  static OcrResult from_lines(std::span<const OcrLine> lines) {
    OcrResult res;
    if (lines.empty()) {
      return res;
    }
    double total_conf = 0.0;
    std::string combined;
    for (const auto& line : lines) {
      if (!combined.empty()) {
        combined += "\n";
      }
      combined += line.text;
      total_conf += line.confidence;
      res.lines.push_back(line);
    }
    res.text = combined;
    res.confidence = total_conf / static_cast<double>(lines.size());
    return res;
  }

  friend bool operator==(const OcrResult&, const OcrResult&) = default;
};

/**
 * @brief Abstract interface contract for OCR recognition engines.
 *
 * Concurrency / Thread Safety Contract:
 * - Implementations of IOcrEngine are NOT required to be thread-safe or re-entrant.
 * - Callers (such as SegmentRecognizer in the engine layer) MUST serialize all calls
 *   to recognize(), guaranteeing that at most one thread invokes recognize() at any time.
 * - Implementations receive a read-only borrowed ImageView and MUST NOT retain persistent
 *   references or pointers to its underlying pixel memory beyond the return of recognize().
 */
class IOcrEngine {
 public:
  virtual ~IOcrEngine() = default;

  IOcrEngine() = default;
  IOcrEngine(const IOcrEngine&) = delete;
  IOcrEngine& operator=(const IOcrEngine&) = delete;
  IOcrEngine(IOcrEngine&&) = default;
  IOcrEngine& operator=(IOcrEngine&&) = default;

  // 输入只读借用切片，避免深拷贝，内部严禁持久保留内存引用
  [[nodiscard]] virtual OcrResult recognize(const ImageView& image) = 0;

  [[nodiscard]] virtual std::string engine_name() const = 0;
};

} // namespace sublift
