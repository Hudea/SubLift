# Changelog

All notable changes to this project are documented in this file.

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
- Report: `docs/reports/phase6.6-cutover-gate.md`.

### Known limits

- Paddle remains Python-only until a native adapter exists.
- Limited GT inventory (single fixed clip, not vendored).
- Merged residual / timing edge cases tracked in progress.
