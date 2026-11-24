#include <sublift/text/srt.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <sublift/text/cleaning.hpp>

namespace sublift::text {

std::string format_timestamp_srt(std::int64_t timestamp_ms) {
  if (timestamp_ms < 0) {
    timestamp_ms = 0;
  }
  const auto total_ms = timestamp_ms;
  const auto hours = total_ms / 3'600'000;
  const auto minutes = (total_ms % 3'600'000) / 60'000;
  const auto seconds = (total_ms % 60'000) / 1000;
  const auto millis = total_ms % 1000;

  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld",
                static_cast<long long>(hours),
                static_cast<long long>(minutes),
                static_cast<long long>(seconds),
                static_cast<long long>(millis));
  return std::string{buf};
}

bool is_blank_text(std::string_view text) {
  std::size_t i = 0;
  char32_t cp = 0;
  while (decode_utf8(text, i, cp)) {
    if (!is_space_cp(cp)) {
      return false;
    }
  }
  return true;
}

std::pair<std::string, std::size_t> format_srt_counted(
    const std::vector<SubtitleEntry>& entries) {
  if (entries.empty()) {
    return {"", 0};
  }

  std::vector<const SubtitleEntry*> ordered;
  ordered.reserve(entries.size());
  for (const auto& entry : entries) {
    ordered.push_back(&entry);
  }
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const auto* lhs, const auto* rhs) {
                     if (lhs->range.start_ms != rhs->range.start_ms) {
                       return lhs->range.start_ms < rhs->range.start_ms;
                     }
                     return lhs->range.end_ms < rhs->range.end_ms;
                   });

  std::string out;
  std::size_t idx = 1;
  for (const auto* entry : ordered) {
    if (is_blank_text(entry->text)) {
      continue;
    }

    const std::int64_t start = std::max<std::int64_t>(0, entry->range.start_ms);
    const std::int64_t end = std::max<std::int64_t>(start, entry->range.end_ms);
    out += std::to_string(idx++);
    out += '\n';
    out += format_timestamp_srt(start);
    out += " --> ";
    out += format_timestamp_srt(end);
    out += '\n';
    out += entry->text;
    out += "\n\n";
  }
  return {out, idx - 1};
}

std::string format_srt(const std::vector<SubtitleEntry>& entries) {
  return format_srt_counted(entries).first;
}

} // namespace sublift::text
