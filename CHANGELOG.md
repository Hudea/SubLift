# Changelog

All notable changes to this project are documented in this file.

## [Unreleased] — Phase 13 D04: Python-free product gate

- Product verification is `./scripts/verify-product.sh`. It does not install or run Python, and it does not skip required checks when python/uv/.venv are missing.
- The gate runs CTest, Swift tests, Web Vitest/build, Native resource/ORT/capability probes, CLI mock and paddle extract, and a Native Server job that exports SRT.
- `./scripts/verify-standard.sh` remains the transitional Oracle/parity gate.

## [Unreleased] — Phase 13 D03: Native model and ORT resource loop

- Paddle PP-OCRv6-small files and ONNX Runtime are pinned by `native-resources/manifest.v1.json` (SHA-256).
- `sublift resources install` writes a temporary file, verifies the digest, then atomically replaces the destination. A failed fetch is not a usable bundle.
- CMake and `ResourceLocator` reject ONNX Runtime from `.venv` / `site-packages`. macOS Homebrew 1.28.0 and official GitHub linux/darwin archives are the allowed sources.
- Default model cache is `~/.cache/sublift/models/ppocrv6-small`. SHA-matching files in the historical RapidOCR cache may still be reused; new installs do not write there.

## [Unreleased] — Phase 13 D02: close dual product runtime

- Product CLI is Native `build/cpp/bin/sublift`. `--runtime` and `SUBLIFT_RUNTIME=python` fail closed; they no longer select a Python implementation.
- macOS starts only `sublift_worker`. It does not launch `python -m sublift.ipc.server`, inject `PYTHONPATH`, or treat `.venv` as a product locator.
- Missing Native worker, engine, or capability returns a structured error. There is no silent engine switch and no Python fallback.
- The Python `sublift extract` console entry prints a Native CLI redirect and exits 2. Isolated Oracle/benchmark tools remain until later Phase 13 deliverables.

## [6.6] — 2026-07-28 — Cutover: default C++ runtime for vision/mock

### Product default

- **vision / mock** product path now defaults to the **C++** `sublift_worker` (CLI + macOS GUI).
- **paddle** always routes to the **Python** IPC worker (no silent fallback to vision/mock).
- Priority: explicit CLI/GUI flag → `SUBLIFT_RUNTIME` env → product default (`cpp`).

### Native CLI (no uv required for vision/mock)

- Build artifacts: `build/cpp/bin/sublift` (and `sublift_cli`) — `extract <video> [options]`.
- Spawns sibling `sublift_worker` over UDS; writes SRT.
- `uv run sublift` remains the **oracle / paddle / rollback** Python entry.

### Rollback (one-key)

```bash
export SUBLIFT_RUNTIME=python   # CLI + GUI resolve to Python worker
# or per-invocation:
uv run sublift extract <video> --runtime python -o out.srt
```

Python tree is **not** deleted; oracle, benchmark, and paddle keep working.

### Quality / performance (cutover gate)

- Correctness: 10 parity goldens (`scripts/parity/check_cutover_gate.py --check`).
- Runtime (mock path-mode): cancel ≤1s (hard), restart ≤5s (hard), wall ≤×1.30+0.05s, RSS ≤×1.50.
- GT L3: fixed Zootopia waterline wired; live measurement when `debug/Zootopia_clip_1080p.mp4` is present; otherwise **WAIVED** per ADR-0022 (report must not claim full publish-contract).
- Evidence: `docs/phases/phase6.json` (Phase 6.6 cutover entries).

### Known limits

- Paddle remains Python-only until a native adapter exists.
- Limited GT inventory (single fixed clip, not vendored).
- Merged residual / timing edge cases tracked in progress.
