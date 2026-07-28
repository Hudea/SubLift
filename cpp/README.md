# SubLift C++ tree (`cpp/`)

Native core / worker / CLI for **Phase 6**. Product default path remains **Python** until cutover.  
Architecture and contracts: [`docs/cpp/`](../docs/cpp/).

## Requirements

- CMake ≥ 3.20
- C++20 compiler (AppleClang / Clang / GCC)
- Network on first configure (FetchContent downloads nlohmann/json; Catch2 only if tests ON)
- Optional: OpenCV 4.x (`core`+`imgproc`) for signature parity (feat-06101). Soft-discover: missing OpenCV disables signature, configure still succeeds. macOS: `brew install opencv@4` then reconfigure. Hard-require: `-DSUBLIFT_REQUIRE_OPENCV=ON`
- Optional: system `ffmpeg`/`ffprobe` on `PATH` (extractor integration + extract parity). Not required to build; pure/golden offline tests still run without them.

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
| `SUBLIFT_ENABLE_OPENCV` | ON | Prefer signature pipeline when OpenCV is found |
| `SUBLIFT_REQUIRE_OPENCV` | OFF | Fail configure if OpenCV missing |

## Dependency policy

| Dep | How |
|---|---|
| Catch2 | FetchContent, pin `v3.7.1` (only test framework) |
| nlohmann/json | FetchContent, pin `v3.11.3` |
| OpenCV | System `find_package` soft-optional (`core`/`imgproc`); PRIVATE to `sublift_core` when found. Prefer **opencv@4** (parity with Python `cv2` 4.x) |
| ffmpeg / ffprobe | System executables (subprocess only; no libav). Not vendored |

## Build & test

From **repository root** (out-of-source `build/cpp`):

```bash
cmake -S cpp -B build/cpp -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
```

Binaries land in `build/cpp/bin/` (`sublift_cli`, `sublift_worker`, `sublift_tests`).

### ffmpeg / ffprobe integration skip policy (Phase 6.3)

| Case | Behavior |
|---|---|
| `ffmpeg`/`ffprobe` resolve OK (`sublift::ffmpeg::available()`) | Probe, full/ROI extract, and `[parity][extractor][extract]` run live |
| Not on PATH / not resolvable | Those tests **runtime-skip** via Catch `WARN` + early `return` (case still reports **PASS** so `ctest` stays green offline) |
| Pure contracts + `[parity][extractor][pure]` | Always run; no spawn |
| Mid-flight cancel | Unit-only (`extractor_full_test`); not locked in frozen golden (timing-sensitive) |

There is **no** `SUBLIFT_REQUIRE_FFMPEG` hard-fail yet; optional future: CTest `LABELS integration` + env to fail when extract L0 never ran. CI without ffmpeg still validates pure + offline golden load.

Optional vision shell (macOS):

```bash
cmake -S cpp -B build/cpp -G Ninja -DSUBLIFT_ENABLE_VISION=ON
cmake --build build/cpp
```

## Related docs

- [architecture.md](../docs/cpp/architecture.md) §2 target graph & §2.1 toolchain
- [phase6.0-bootstrap.md](../docs/cpp/phase6.0-bootstrap.md) — feat-06002 acceptance
