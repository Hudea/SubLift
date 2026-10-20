#include "sublift/cli/srt.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

namespace sublift::cli {

std::string format_srt_timestamp(std::int64_t ms) {
  if (ms < 0) ms = 0;
  const auto total_ms = ms;
  const auto hours = total_ms / 3'600'000;
  const auto minutes = (total_ms % 3'600'000) / 60'000;
  const auto seconds = (total_ms % 60'000) / 1000;
  const auto millis = total_ms % 1000;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld",
                static_cast<long long>(hours), static_cast<long long>(minutes),
                static_cast<long long>(seconds), static_cast<long long>(millis));
  return std::string{buf};
}

bool is_blank_text(std::string_view text) {
  for (unsigned char c : text) {
    if (!std::isspace(c)) return false;
  }
  return true;
}

std::pair<std::string, std::size_t> format_srt(
    const std::vector<sublift::SubtitleEntry>& entries) {
  if (entries.empty()) return {"", 0};

  std::vector<const sublift::SubtitleEntry*> ordered;
  ordered.reserve(entries.size());
  for (const auto& entry : entries) ordered.push_back(&entry);
  std::stable_sort(ordered.begin(), ordered.end(), [](const auto* lhs, const auto* rhs) {
    if (lhs->start_ms != rhs->start_ms) return lhs->start_ms < rhs->start_ms;
    return lhs->end_ms < rhs->end_ms;
  });

  std::string out;
  std::size_t idx = 1;
  for (const auto* entry : ordered) {
    if (is_blank_text(entry->text)) continue;

    const std::int64_t start = std::max<std::int64_t>(0, entry->start_ms);
    const std::int64_t end = std::max<std::int64_t>(start, entry->end_ms);
    out += std::to_string(idx++);
    out += '\n';
    out += format_srt_timestamp(start);
    out += " --> ";
    out += format_srt_timestamp(end);
    out += '\n';
    out += entry->text;
    out += "\n\n";
  }
  return {out, idx - 1};
}

}  // namespace sublift::cli
