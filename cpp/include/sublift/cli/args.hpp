#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sublift::cli {

enum class ParseStatus { Ok, Help, Error };

struct ExtractArgs {
  std::filesystem::path video;
  std::filesystem::path output{"output.srt"};
  double fps{5.0};
  // ExtractionSpec default from contracts/fixtures/defaults.json (0.0 = no
  // high-confidence filter). The internal Pipeline Config keeps its
  // Oracle-compatible 0.5 default; this value is the request boundary.
  double confidence{0.0};
  // ADR-0042 optional det area budget; 0 keeps RapidOCR min-side 736.
  double det_pixel_budget{0.0};
  std::string engine{"vision"};
  std::string script{"auto"};
  std::optional<std::filesystem::path> worker;
};

struct ParseResult {
  ParseStatus status{ParseStatus::Error};
  ExtractArgs args{};
  std::string error;
};

/// Parse `extract` arguments. `argv[0]` is the program name; `argv[1]` is
/// expected to be `extract` when present. `--runtime` is a product error.
[[nodiscard]] ParseResult parse_extract_args(const std::vector<std::string>& argv);

[[nodiscard]] bool is_runtime_flag(std::string_view arg);

}  // namespace sublift::cli
