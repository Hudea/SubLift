# SubLift C++ tree (`cpp/`)

Native core、worker、CLI 与 Web server。产品默认路径使用 **C++ Native Runtime**。
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

当前 targets、依赖方向和目录所有权见
[`docs/cpp/native-architecture.md`](../docs/cpp/native-architecture.md)。CMake 文件是可执行依赖图的真源。

## Options

| Option | Default | Meaning |
|---|---|---|
| `SUBLIFT_ENABLE_VISION` | OFF | Build `sublift_vision_macos` |
| `SUBLIFT_ENABLE_PADDLE` | OFF | Build `sublift_paddle` (needs system ONNX Runtime; macOS: `brew install onnxruntime`) |
| `SUBLIFT_PADDLE_MODEL_DIR` | `~/.cache/sublift/models/ppocrv6-small` | PP-OCRv6-small ONNX + `ppocrv6_dict.txt`，SHA 由 `native-resources/manifest.v1.json` 约束 |
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

### ffmpeg / ffprobe integration skip policy

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

### Apple Vision OCR integration & CI policy

| Case | Behavior |
|---|---|
| `SUBLIFT_ENABLE_VISION=ON` (macOS) | Builds `sublift_vision_macos` with real Apple Vision (`Vision`, `CoreGraphics`, `Foundation`, `CoreText`). `is_vision_available()` returns `true`. `[vision][integration]` tests run in-memory CGImage text recognition |
| `SUBLIFT_ENABLE_VISION=OFF` (Default) | Uses stub (`vision_stub.cpp`), `is_vision_available()` returns `false`, `VisionOcrEngine` constructor throws an explicit `std::runtime_error` |
| Headless / CI Environment | Vision `VNRecognizeTextRequest` operates on in-memory `CGImageRef` pointers; does **not** require a GUI WindowServer session or screen recording permissions. Fully supported in headless CLI / CI runners |
| Catch2 Filter Tag | Use Catch tag `[vision][integration]` or `ctest -R vision` to filter Vision integration tests |
| Threading | `VisionOcrEngine::recognize` is **not thread-safe**; callers must serialize (no concurrent recognize) |
| Pixel path | Product/parity path is **RGB24**. BGR24/Gray8 are best-effort conversions only |
| C++ parity L0 vs L4 | C++ `[parity][vision]` 离线读取已提交 golden，覆盖 box/clamp/sort/empty 结构。过渡期 `verify-standard.sh` 仍额外运行历史 `dump_vision.py --check`；它不是 Native-only 产品门。**L4** live Vision text/conf 只作可选报告。 |
| Dual-matrix | `./scripts/verify-standard.sh` 在 macOS 默认以 **VISION=ON** 配置 Debug C++（`SUBLIFT_VERIFY_SKIP_VISION=1` 时改为 stub；非 macOS 也使用 stub），并运行 core/ffmpeg/mock 与 parity。若要聚焦 Vision smoke，可另行 `cmake -S cpp -B build/cpp -G Ninja -DSUBLIFT_ENABLE_VISION=ON` 后运行 `ctest -R 'vision|parity.*vision'`。 |

## sublift_worker CLI

`sublift_worker` is the standalone native C++ IPC worker binary delivering Unix Domain Socket framing (`>I` big-endian 4B header), JSON protocol DTOs, path mode, and frame mode processing.

### Usage

```bash
# Launch mock engine worker
./build/cpp/bin/sublift_worker --socket /tmp/sublift_worker.sock --engine mock

# Launch Apple Vision engine worker (when built with -DSUBLIFT_ENABLE_VISION=ON)
./build/cpp/bin/sublift_worker --socket /tmp/sublift_worker.sock --engine vision
```

### Runtime policy

- **Only Product Runtime**: C++ Native Core (`sublift_worker`).
- **Fail-closed**: 请求的 Worker、引擎、模型或 capability 不可用时明确失败，不自动改引擎或进入 Python。
- **Rollback**: 回滚到上一版已验收 Native artifact、release/tag 或 Git revision，不在同一版本切换 runtime。

## Engine 与 capability 解析

产品只解析目标 Native 进程的 engine 与 capability：

- `vision`、`mock` 与 `paddle` 只能在当前 Worker 实际声明 capability 时选择。
- 缺失 capability 时明确失败，不存在自动 `paddle_override` 或其他引擎替换。
- 产品接口不接受 `--runtime` / `SUBLIFT_RUNTIME` 作为实现选择。

产品宿主（Native CLI、macOS、Web）已不再通过 `RuntimePolicy` / `SUBLIFT_RUNTIME` 选择
Python。`src/sublift/runtime.py` 与 Python IPC Server 仍可能存在于工作树，属于待移除隔离
工具债务，不属于本 C++ tree 的产品合同，也不得新增消费者。

## Related docs

- [Native architecture](../docs/cpp/native-architecture.md)
- [Runtime contract](../docs/cpp/runtime-contract.md)
- [Worker IPC contract](../docs/cpp/worker-ipc-contract.md)
- [Parity contract](../docs/cpp/parity-contract.md)
