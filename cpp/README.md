# SubLift C++ tree (`cpp/`)

Native core / worker / CLI for **Phase 6**. Product default path remains **Python** until cutover.  
Architecture and contracts: [`docs/cpp/`](../docs/cpp/).

## Requirements

- CMake ≥ 3.20
- C++20 compiler (AppleClang / Clang / GCC)
- Network on first configure (FetchContent downloads nlohmann/json; Catch2 only if tests ON)
- Optional: OpenCV (`find_package` only; not linked in 6.0 stubs)
- Optional: system `ffmpeg` on `PATH` (used later by extractor; not required to build stubs)

## Target graph

```text
sublift_cli ──► sublift_ffmpeg ──► sublift_core ──► nlohmann_json
sublift_worker ─┬► sublift_ffmpeg ──► sublift_core
                └► sublift_vision_macos (OPTION OFF, macOS only)
sublift_test_support ──► sublift_core
sublift_tests ──► test_support + Catch2 (+ core via PUBLIC)
```

| Target | Type | Role (6.0) |
|---|---|---|
| `sublift_core` | STATIC | models/config/pipeline home (stubs: version) |
| `sublift_ffmpeg` | STATIC | subprocess extractor (stub) |
| `sublift_test_support` | STATIC | golden/parity helpers (stub) |
| `sublift_worker` | EXE | UDS worker shell |
| `sublift_cli` | EXE | native CLI shell |
| `sublift_vision_macos` | STATIC | Vision adapter; **off by default** |

`sublift_core` must not depend on ObjC, Swift, or Apple Vision.

## Options

| Option | Default | Meaning |
|---|---|---|
| `SUBLIFT_ENABLE_VISION` | OFF | Build `sublift_vision_macos` |
| `SUBLIFT_SANITIZE` | OFF | ASan+UBSan on Debug |
| `SUBLIFT_BUILD_TESTS` | ON | Catch2 + CTest |

## Dependency policy

| Dep | How |
|---|---|
| Catch2 | FetchContent, pin `v3.7.1` (only test framework) |
| nlohmann/json | FetchContent, pin `v3.11.3` |
| OpenCV | System `find_package` optional; not linked in 06002 |
| ffmpeg | System executable later; not vendored |

## Build & test

From **repository root** (out-of-source `build/cpp`):

```bash
cmake -S cpp -B build/cpp -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
```

Binaries land in `build/cpp/bin/` (`sublift_cli`, `sublift_worker`, `sublift_tests`).

Optional vision shell (macOS):

```bash
cmake -S cpp -B build/cpp -G Ninja -DSUBLIFT_ENABLE_VISION=ON
cmake --build build/cpp
```

## Related docs

- [architecture.md](../docs/cpp/architecture.md) §2 target graph & §2.1 toolchain
- [phase6.0-bootstrap.md](../docs/cpp/phase6.0-bootstrap.md) — feat-06002 acceptance
