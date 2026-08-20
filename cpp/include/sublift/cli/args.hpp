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
  double confidence{0.5};
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
