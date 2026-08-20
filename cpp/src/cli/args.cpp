#include "sublift/cli/args.hpp"

#include <cstdlib>
#include <sstream>

namespace sublift::cli {
namespace {

[[nodiscard]] std::string unknown_option_error(std::string_view arg) {
  if (is_runtime_flag(arg)) {
    return "Error: --runtime is not a product option. Native is the only "
           "runtime. Roll back to a previous product version instead of "
           "selecting python.";
  }
  std::ostringstream oss;
  oss << "Error: unknown option " << arg;
  return oss.str();
}

}  // namespace

bool is_runtime_flag(std::string_view arg) {
  return arg == "--runtime" || arg == "-runtime";
}

ParseResult parse_extract_args(const std::vector<std::string>& argv) {
  ParseResult out;
  ExtractArgs args;
  std::optional<std::string> positional;

  // argv[0]=prog, argv[1]=extract (optional if caller already sliced)
  std::size_t i = 0;
  if (i < argv.size()) {
    ++i;  // skip program
  }
  if (i < argv.size() && argv[i] == "extract") {
    ++i;
  }

  for (; i < argv.size(); ++i) {
    const std::string_view arg = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argv.size()) {
        out.status = ParseStatus::Error;
        out.error = std::string("Error: ") + name + " requires a value";
        return nullptr;
      }
      return argv[++i].c_str();
    };

    if (arg == "-o" || arg == "--output") {
      const char* v = need("--output");
      if (!v) return out;
      args.output = v;
    } else if (arg == "--fps") {
      const char* v = need("--fps");
      if (!v) return out;
      args.fps = std::atof(v);
    } else if (arg == "--confidence") {
      const char* v = need("--confidence");
      if (!v) return out;
      args.confidence = std::atof(v);
    } else if (arg == "--engine") {
      const char* v = need("--engine");
      if (!v) return out;
      args.engine = v;
    } else if (arg == "--script") {
      const char* v = need("--script");
      if (!v) return out;
      args.script = v;
    } else if (arg == "--worker") {
      const char* v = need("--worker");
      if (!v) return out;
      args.worker = std::filesystem::path{v};
    } else if (arg == "-h" || arg == "--help") {
      out.status = ParseStatus::Help;
      out.args = args;
      return out;
    } else if (!arg.empty() && arg[0] == '-') {
      out.status = ParseStatus::Error;
      out.error = unknown_option_error(arg);
      return out;
    } else {
      if (positional.has_value()) {
        out.status = ParseStatus::Error;
        out.error = std::string("Error: unexpected argument ") + std::string{arg};
        return out;
      }
      positional = std::string{arg};
    }
  }

  if (!positional.has_value()) {
    out.status = ParseStatus::Error;
    out.error = "Error: extract requires <video> path";
    return out;
  }
  args.video = *positional;

  if (args.engine != "mock" && args.engine != "vision" && args.engine != "paddle") {
    out.status = ParseStatus::Error;
    out.error = "Error: --engine must be mock|vision|paddle";
    return out;
  }
  if (args.script != "auto" && args.script != "cjk" && args.script != "latin") {
    out.status = ParseStatus::Error;
    out.error = "Error: --script must be auto|cjk|latin";
    return out;
  }

  out.status = ParseStatus::Ok;
  out.args = std::move(args);
  return out;
}

}  // namespace sublift::cli
