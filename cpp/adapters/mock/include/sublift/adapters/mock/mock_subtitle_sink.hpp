#pragma once

#include <cstddef>
#include <mutex>
#include <vector>
#include <sublift/ports/subtitle_sink.hpp>

namespace sublift::adapters::mock {

class MockSubtitleSink : public ISubtitleSink {
 public:
  MockSubtitleSink() = default;
  ~MockSubtitleSink() override = default;

  MockSubtitleSink(MockSubtitleSink&& other) noexcept;
  MockSubtitleSink& operator=(MockSubtitleSink&& other) noexcept;
  MockSubtitleSink(const MockSubtitleSink&) = delete;
  MockSubtitleSink& operator=(const MockSubtitleSink&) = delete;

  void emit(const SubtitleEntry& entry) override;
  void flush() override;
  void close() override;

  [[nodiscard]] std::vector<SubtitleEntry> entries() const;
  [[nodiscard]] bool is_flushed() const;
  [[nodiscard]] bool is_closed() const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] bool empty() const;
  void clear();

 private:
  mutable std::mutex mutex_;
  std::vector<SubtitleEntry> entries_;
  bool flushed_{false};
  bool closed_{false};
};

} // namespace sublift::adapters::mock

