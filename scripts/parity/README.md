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

## Paddle stage golden (feat-06802)

```bash
# 可选历史离线校验：不加载/下载 OCR 模型
uv run python scripts/parity/dump_paddle_stages.py --check

# 显式 live 双运行时重放：要求本机模型与 Paddle C++ trace executable
uv run python scripts/parity/dump_paddle_stages.py --check --runtime \
  --report-out /tmp/sublift_paddle_stages_live.json \
  --raw-dir /tmp/sublift_paddle_stages_raw

# 仅在有意更新冻结 Oracle/Candidate 时重建 v2 golden
uv run python scripts/parity/dump_paddle_stages.py --runtime
```

Golden: `benchmark/parity/goldens/paddle/paddle_stages.v2.json`。冻结环境、模型、
字典与参数位于 `freeze_paddle_manifest.json`；live 模式会在推理前核验实际文件
SHA256。产品路径默认不捕获 tensor，逐阶段大对象只存在于显式诊断工具。

## Paddle Det live gate (feat-06803)

```bash
# 历史 Candidate 复核：记录 Python/C++ ORT dylib SHA，并选择同构建/跨构建门
uv run python scripts/parity/check_paddle_det_parity.py --check \
  --report-out /tmp/sublift_paddle_det_gate.json

# 严格同 ORT 二进制复验时，显式给出 Candidate 动态库
uv run python scripts/parity/check_paddle_det_parity.py --check \
  --candidate-ort-library /path/to/libonnxruntime \
  --report-out /tmp/sublift_paddle_det_strict.json
```

硬门覆盖 Det input/probability tensor、quad IoU/坐标/score 和 empty 的
`0 box / 0 Rec`。同 ORT 二进制 probability `max_abs <= 1e-5`；同版本/provider
但不同二进制构建使用 `2.5e-5`，报告必须带双方 SHA256，box 门不放宽。

## Paddle Crop / Cls / Rec live gate (feat-06804)

```bash
uv run python scripts/parity/check_paddle_rec_parity.py --check \
  --report-out /tmp/sublift_paddle_rec_gate.json
```

该门使用冻结 Python Det quad 隔离下游算子，检查 perspective crop、Cls/Rec tensor、
batch call、CTC token/text、最终顺序与 AABB；历史端到端对照由 stage/quality 重放覆盖，
当前产品验收以 Native tests、golden 与 GT 为准。

## Paddle E2E quality gate (feat-06805)

```bash
# 缺外置真实源、生成素材、模型、Release worker 或 frozen baseline 时直接失败
uv run python scripts/parity/check_paddle_gate.py --check \
  --report-out /tmp/sublift_paddle_quality_gate.md \
  --json-out /tmp/sublift_paddle_quality_gate.json

# 仅用于有意重建历史冻结资产；不得用来设定新产品水位
uv run python scripts/parity/check_paddle_gate.py --freeze-oracle
```

Manifest 位于 `benchmark/datasets/paddle_quality/manifest.v1.json`，冻结 3 个来源共
614.272s 的输入/GT/生成 recipe 与 font hash。该历史门会重放 Python/C++ CLI，
同时检查当时的 Oracle 相对门、冻结 Python 绝对门、逐 clip noise/empty 与 Q0 live box；
不接受硬编码指标或缺依赖 skip。

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
