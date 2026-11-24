#include <sublift/adapters/mock/mock_subtitle_sink.hpp>
#include <utility>

namespace sublift::adapters::mock {

MockSubtitleSink::MockSubtitleSink(MockSubtitleSink&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  entries_ = std::move(other.entries_);
  flushed_ = other.flushed_;
  closed_ = other.closed_;
}

MockSubtitleSink& MockSubtitleSink::operator=(MockSubtitleSink&& other) noexcept {
  if (this != &other) {
    std::scoped_lock lock(mutex_, other.mutex_);
    entries_ = std::move(other.entries_);
    flushed_ = other.flushed_;
    closed_ = other.closed_;
  }
  return *this;
}

void MockSubtitleSink::emit(const SubtitleEntry& entry) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.push_back(entry);
}

void MockSubtitleSink::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  flushed_ = true;
}

void MockSubtitleSink::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  closed_ = true;
}

std::vector<SubtitleEntry> MockSubtitleSink::entries() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_;
}

bool MockSubtitleSink::is_flushed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return flushed_;
}

bool MockSubtitleSink::is_closed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return closed_;
}

std::size_t MockSubtitleSink::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

bool MockSubtitleSink::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.empty();
}

void MockSubtitleSink::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  flushed_ = false;
  closed_ = false;
}

} // namespace sublift::adapters::mock

