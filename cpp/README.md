# SubLift C++ tree (`cpp/`)

Native core / worker / CLI for **Phase 6**. Product default path is **C++ Native Core (`sublift_worker`)** after Phase 6.6 Cutover.  
Architecture and contracts: [`docs/cpp/`](../docs/cpp/).

## Requirements

- CMake ≥ 3.20
- C++20 compiler (AppleClang / Clang / GCC)
- Network on first configure (FetchContent downloads nlohmann/json; Catch2 only if tests ON)
- Optional: OpenCV 4.x (`core`+`imgproc`) for signature parity (feat-06101). Soft-discover: missing OpenCV disables signature, configure still succeeds. macOS: `brew install opencv@4` then reconfigure. Hard-require: `-DSUBLIFT_REQUIRE_OPENCV=ON`
- Optional: ONNX Runtime headers/library for Paddle Native. Product acceptance fixes the official
  ORT 1.28.0 dylib SHA rather than accepting an arbitrary same-version build.
- Optional: system `ffmpeg`/`ffprobe` on `PATH` (extractor integration + extract parity). Not required to build; pure/golden offline tests still run without them.

## Target graph

```text
sublift_cli ──► sublift_ffmpeg ──► sublift_core ──► nlohmann_json
sublift_worker ─┬► sublift_ffmpeg ──► sublift_core
                ├► sublift_vision_macos (OPTION OFF, macOS only)
                └► sublift_paddle (OPTION OFF, ONNX Runtime)
sublift_test_support ──► sublift_core
sublift_tests ──► test_support + Catch2 (+ core via PUBLIC)
```

| Target | Type | Role (6.0+) |
|---|---|---|
| `sublift_core` | STATIC | models/config/pipeline home (stubs: version) |
| `sublift_ffmpeg` | STATIC | subprocess extractor (stub) |
| `sublift_test_support` | STATIC | golden/parity helpers (stub) |
| `sublift_worker` | EXE | UDS worker shell |
| `sublift_cli` | EXE | native CLI shell |
| `sublift_vision_macos` | STATIC | Vision adapter; **off by default** |
| `sublift_paddle` | STATIC | PaddleOCR adapter (ONNX Runtime); **off by default** |

`sublift_core` must not depend on ObjC, Swift, Apple Vision, or ONNX Runtime.

## Options

| Option | Default | Meaning |
|---|---|---|
| `SUBLIFT_ENABLE_VISION` | OFF | Build `sublift_vision_macos` |
| `SUBLIFT_ENABLE_PADDLE` | OFF | Build `sublift_paddle` (needs system ONNX Runtime; macOS: `brew install onnxruntime`) |
| `SUBLIFT_PADDLE_MODEL_DIR` | `~/.cache/sublift/rapidocr-models` | PP-OCRv6 ONNX + `ppocrv6_dict.txt` (same cache as Python rapidocr) |
| `SUBLIFT_REQUIRE_PADDLE` | OFF | Fail configure if ONNX Runtime missing |
| `SUBLIFT_BUNDLE_ONNXRUNTIME` | ON | Copy the selected ORT shared library to build `lib/` and use a relative executable rpath |
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
| ONNX Runtime | `find_package`; PRIVATE to `sublift_paddle`. Paddle-enabled build copies the selected dylib/so beside build products; `sublift_core` remains ORT-free |
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
ctest --test-dir build/cpp -R vision --output-on-failure
```

### Apple Vision OCR integration & CI policy (Phase 6.4)

| Case | Behavior |
|---|---|
| `SUBLIFT_ENABLE_VISION=ON` (macOS) | Builds `sublift_vision_macos` with real Apple Vision (`Vision`, `CoreGraphics`, `Foundation`, `CoreText`). `is_vision_available()` returns `true`. `[vision][integration]` tests run in-memory CGImage text recognition |
| `SUBLIFT_ENABLE_VISION=OFF` (Default) | Uses stub (`vision_stub.cpp`), `is_vision_available()` returns `false`, `VisionOcrEngine` constructor throws `std::runtime_error` matching Python `RuntimeError` |
| Headless / CI Environment | Vision `VNRecognizeTextRequest` operates on in-memory `CGImageRef` pointers; does **not** require a GUI WindowServer session or screen recording permissions. Fully supported in headless CLI / CI runners |
| Catch2 Filter Tag | Use Catch tag `[vision][integration]` or `ctest -R vision` to filter Vision integration tests |
| Threading | `VisionOcrEngine::recognize` is **not thread-safe**; callers must serialize (no concurrent recognize) |
| Pixel path | Product/parity path is **RGB24**. BGR24/Gray8 are best-effort conversions only |
| C++ parity L0 vs L4 | C++ parity **L0** covers box/clamp/sort/empty structure via `dump_vision.py --check` + `[parity][vision]`; it runs from `./scripts/verify-standard.sh`, not Harness `./init.sh`. **L4** is live Vision text/conf 的可选报告；`dump_vision --live` 只保留说明性入口。 |
| Dual-matrix | `./scripts/verify-standard.sh` 在 macOS 默认以 **VISION=ON** 配置 Debug C++（`SUBLIFT_VERIFY_SKIP_VISION=1` 时改为 stub；非 macOS 也使用 stub），并运行 core/ffmpeg/mock 与 parity。若要聚焦 Vision smoke，可另行 `cmake -S cpp -B build/cpp -G Ninja -DSUBLIFT_ENABLE_VISION=ON` 后运行 `ctest -R 'vision|parity.*vision'`。 |

## sublift_worker CLI & Dual-Track Opt-in Guide (Phase 6.5)

`sublift_worker` is the standalone native C++ IPC worker binary delivering Unix Domain Socket framing (`>I` big-endian 4B header), JSON protocol DTOs, path mode, and frame mode processing.

### Usage

```bash
# Launch mock engine worker
./build/cpp/bin/sublift_worker --socket /tmp/sublift_worker.sock --engine mock

# Launch Apple Vision engine worker (when built with -DSUBLIFT_ENABLE_VISION=ON)
./build/cpp/bin/sublift_worker --socket /tmp/sublift_worker.sock --engine vision
```

### Dual-Track Opt-in & Cutover Policy

- **Default Product Runtime**: C++ Native Core (`sublift_worker`) is the **default runtime** for SubLift products (Phase 6.6 Cutover Default).
- **Rollback Path**: Host CLI / Swift GUI can fallback to Python worker by setting environment variable `SUBLIFT_RUNTIME=python` or passing `--runtime python`.
- **Paddle OCR**: Paddle available → C++ Worker stable product path; unavailable → explicit
  Python Paddle `paddle_override`. It never changes the requested engine.

## Runtime Resolution Policy & Toggles (Phase 6.6)

SubLift provides unified runtime resolution (`src/sublift/runtime.py` and `RuntimePolicy.swift`):

- **Default**: C++ Native Core (`sublift_worker`) for `vision`, `mock`, and available `paddle`.
- **Priority**: Explicit `--runtime python|cpp` Flag > Environment Variable `SUBLIFT_RUNTIME=python|cpp` > Product Default (`cpp`).
- **Engine Matrix Cross-Rules**:
  - `engine="paddle"` + C++ capability → C++ Worker; missing capability → Python Paddle
    with `resolved_via="paddle_override"`.
  - Explicit/env `runtime=python` remains the Paddle rollback/Oracle path.
  - `engine="vision"` or `"mock"` routes according to resolved runtime (`cpp` by default).

## Related docs

- [architecture.md](../docs/cpp/architecture.md) §2 target graph & §2.1 toolchain
- [phase6.0-bootstrap.md](../docs/cpp/phase6.0-bootstrap.md) — feat-06002 acceptance
