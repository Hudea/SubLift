# Parity 与历史 cutover 工具

> 本目录保留冻结 golden 的生成/复核与历史 Python↔C++ cutover 重放工具。
> Phase 13 起它们是隔离的可选离线工具，不是产品 runtime、Python 回退机制或
> Native 日常产品门。本文中的“双运行时”与“cutover”只描述历史复现能力。

## 历史 batch / cutover 入口

- **Golden script list (single source):** `golden_registry.py`
- **重放历史 correctness + runtime + GT：**
  `uv run python scripts/parity/check_cutover_gate.py --check --report-out /tmp/sublift_cutover_gate.md`
- **仅复核历史 parity（fail-fast）：**
  `uv run python scripts/parity/check_cutover_gate.py --check --parity-only --report-out /tmp/sublift_cutover_gate.md`

# Parity dump scripts

Frozen-oracle helpers for Native/Python parity. Contract: [`docs/cpp/parity-contract.md`](../../docs/cpp/parity-contract.md).

## Default Config golden (feat-06004)

```bash
# regenerate
uv run python scripts/parity/dump_config.py

# verify committed golden vs Python DEFAULT_CONFIG
uv run python scripts/parity/dump_config.py --check
```

Golden path: `benchmark/parity/goldens/config/default_config.v1.json`.

C++ loads the same file in Catch2 `[parity][config]` (offline; no Python at test time).

## Paddle E2E quality gate (feat-06805 / Phase 18 D02)

历史 Python product runtime 已从产品路径移除；该 gate 不再依赖活 Python 产品，而是用
`python-product-last` 历史 worktree 重放 Oracle（冻结 736），对 C++ 候选做质量门：

```bash
# 仅用于有意重建历史冻结资产；需要历史 Oracle worktree
uv run python scripts/parity/check_paddle_gate.py --freeze-oracle \
  --oracle-worktree /tmp/sublift-python-oracle

# 0.6MP 候选（默认 600000；传 0 表示 736 回归）；--check 失败即非零退出
uv run python scripts/parity/check_paddle_gate.py --check \
  --oracle-worktree /tmp/sublift-python-oracle \
  --candidate-det-pixel-budget 600000 \
  --report-out /tmp/sublift_paddle_quality_gate.md \
  --json-out /tmp/sublift_paddle_quality_gate.json
```

- 基线：`benchmark/parity/goldens/paddle/paddle_quality_baseline.v2.json`（历史
  `paddle_quality_baseline.v1.json` 保留为 544426e Oracle 基线）。
- 质量门：Oracle 相对门（timing F1/precision、CER macro/micro、usable recall、
  noise/empty、box IoU/recall）加冻结基线绝对门；det 几何与合并层不再要求整段 SRT
  逐字节相等，逐 source 仍记录输出 hash 以便审计。
- 历史 worktree：`git worktree add /tmp/sublift-python-oracle python-product-last`；
  依赖仓库 `.venv`。
- Manifest 位于 `benchmark/datasets/paddle_quality/manifest.v1.json`，冻结 3 个来源共
  614.272s 的输入/GT/生成 recipe 与 font hash。字体是生成输入：已冻结视频仍按 hash
  校验，宿主字体更新只记录 provenance drift 警告，不改变已提交资产。

## Paddle 历史 canonical performance 重放（feat-06806）

```bash
uv run python scripts/parity/check_paddle_perf.py --check \
  --candidate-worker /path/to/release/sublift_worker \
  --candidate-ort-library /path/to/official/libonnxruntime.1.dylib \
  --report-out /tmp/sublift_paddle_perf_gate.md \
  --json-out /tmp/sublift_paddle_perf_gate.json
```

Manifest 位于 `benchmark/datasets/paddle_performance/manifest.v1.json`，冻结 120s 输入、
预期 SRT、模型、官方 ORT dylib SHA 与 thread/batch 配置。历史门会先预热，再交错执行
Python/C++ 各 3 轮 CLI，取 wall median 并采样完整进程树 RSS，同时记录
OCR/Det box/Cls/Rec batch 计数。输入、模型、worker、ORT 指纹、统计或输出 hash
任一缺失均 FAIL；`--skip-runtime` 不构成验收。

## Paddle 历史 product cutover 重放（feat-06807）

```bash
uv run python scripts/parity/check_paddle_cutover.py --check \
  --worker build/cpp-rel/bin/sublift_worker \
  --ort-library .venv/lib/python3.12/site-packages/onnxruntime/capi/libonnxruntime.1.28.0.dylib \
  --report-out /tmp/sublift_paddle_cutover.md \
  --json-out /tmp/sublift_paddle_cutover.json
```

该工具重放当时的“默认 C++ → 强制 Python 回滚 → 再次默认 C++”三次
SRT SHA exact，并重放 ≥600s 连续长流、cancel/restart 与 macOS 相对 rpath 验收。
“强制 Python 回滚”只是历史证据场景，不是 ADR-0038 之后的产品合同。

## Signature golden

```bash
# regenerate fixtures (proves bgr_quirk RGB2GRAY vs BGR2GRAY trap)
uv run python scripts/parity/gen_signature_fixtures.py

# write golden (refuses dirty worktree unless --allow-dirty)
uv run python scripts/parity/dump_signature.py
# or: uv run python scripts/parity/dump_signature.py --allow-dirty

# verify core (config + per-fixture semantics/asset/sha/geometry/ts/fg/dhash)
uv run python scripts/parity/dump_signature.py --check
```

C++: `ctest` `[parity][signature]` loads the same fixtures, verifies `input_asset_sha256`, then L0/L1 compares candidate signatures.

OpenCV: soft-optional at CMake configure (`brew install opencv@4`); hard-require with `-DSUBLIFT_REQUIRE_OPENCV=ON`.

## Changepoint golden (feat-06102)

```bash
uv run python scripts/parity/gen_changepoint_fixtures.py
uv run python scripts/parity/dump_changepoint.py          # or --allow-dirty
uv run python scripts/parity/dump_changepoint.py --check
```

Golden: `benchmark/parity/goldens/events/changepoint.v1.json` (scenarios + L0 events).

Downstream: `dump_timeline.py` → segments, `dump_dedupe.py` → entries, `dump_line_select.py` → line-select.
