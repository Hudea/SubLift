#include "sublift/cli/runtime_guard.hpp"

#include <cctype>
#include <cstdlib>
#include <string>

namespace sublift::cli {
namespace {

[[nodiscard]] std::string trim_ascii(std::string_view in) {
  std::size_t begin = 0;
  std::size_t end = in.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(in[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1]))) {
    --end;
  }
  return std::string{in.substr(begin, end - begin)};
}

[[nodiscard]] std::string ascii_lower(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

}  // namespace

std::optional<std::string> product_runtime_env_error(std::string_view env_value) {
  const std::string trimmed = trim_ascii(env_value);
  if (trimmed.empty()) return std::nullopt;
  const std::string lower = ascii_lower(trimmed);
  if (lower == "cpp") return std::nullopt;
  if (lower == "python") {
    return "Error: SUBLIFT_RUNTIME=python is not a product option. Native is "
           "the only runtime. Roll back to a previous product version instead.";
  }
  return "Error: SUBLIFT_RUNTIME=" + trimmed +
         " is not a product option. Native is the only runtime.";
}

std::optional<std::string> product_runtime_env_error_from_process() {
  const char* env = std::getenv("SUBLIFT_RUNTIME");
  if (env == nullptr) return std::nullopt;
  return product_runtime_env_error(env);
}

}  // namespace sublift::cli
