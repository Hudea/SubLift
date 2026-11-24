#pragma once

#include <sublift/core/subtitle.hpp>

namespace sublift {

class ISubtitleSink {
 public:
  virtual ~ISubtitleSink() = default;

  virtual void emit(const SubtitleEntry& entry) = 0;
  virtual void write(const SubtitleItem& item) { emit(item); }
  virtual void flush() = 0;
  virtual void close() = 0;
};

} // namespace sublift
