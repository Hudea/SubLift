#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "sublift/config.hpp"

namespace sublift::test_support {

/// Stub used by smoke tests.
[[nodiscard]] bool smoke_ok() noexcept;

struct OracleMeta {
  std::string oracle_commit;
  std::optional<std::string> oracle_branch;
  std::string config_fingerprint;
  int golden_schema_version{0};
};

struct ConfigGolden {
  OracleMeta oracle;
  Config config;
};

/// Load config golden envelope (schema v1). Throws std::runtime_error on error.
[[nodiscard]] ConfigGolden load_config_golden(const std::filesystem::path& path);

/// Field-by-field diff; nullopt if equal. Message includes oracle_commit + schema.
[[nodiscard]] std::optional<std::string> diff_config(
    const Config& candidate,
    const Config& golden,
    const OracleMeta* oracle = nullptr);

}  // namespace sublift::test_support
