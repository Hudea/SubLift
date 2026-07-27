# Parity 契约：Oracle 冻结与比较规则

> 状态：`feat-06004` 必须落地本文件可执行子集；6.1+ 每个算法 feat 增补 golden。  
> **禁止**将 Oracle 定义为「当时的 origin/main 尖端」而不带 commit 与环境钉扎。

## 1. Oracle 定义（冻结，不可漂移）

每一次正式 golden / cutover 报告必须声明：

| 字段 | 含义 | 示例 |
|---|---|---|
| `oracle_commit` | Python 参考实现的 **精确 git SHA** | `7a19bf9…` |
| `oracle_branch` | 可选说明 | `main` |
| `python_version` | `python -V` | 3.12.x |
| `opencv_version` | `cv2.__version__` | 4.x.x |
| `numpy_version` | | |
| `ffmpeg_version` | `ffmpeg -version` 首行 | |
| `macos_version` | 若涉及 Vision | 15.x |
| `vision_note` | Vision 不可版本钉时记录 OS + 机型 | |
| `input_asset_sha256` | 视频/帧素材哈希 | |
| `config_fingerprint` | 完整 `Config` 序列化哈希或嵌入 golden | |
| `golden_schema_version` | 本文件 schema 版本 | `1` |

**规则：**

1. 更新算法或依赖后若需新 Oracle：先 **bump `oracle_commit` + 重生成 golden**，不得静默以新 main 覆盖旧解释。
2. Candidate（C++）在报告中记录自己的 `candidate_commit`、编译器、OpenCV、sanitize 开关。
3. CI/本地 parity 失败时，diff 工具必须打印双方 commit 与 schema version。

### 1.1 推荐清单文件

```text
benchmark/parity/manifests/<name>.json
  oracle_commit, versions..., inputs[], goldens[]
```

大文件可 git-lfs 或本地 cache；manifest 进库。

## 2. Golden 应保存的中间量（不只是 SRT）

按模块递进；**6.1 起尽量齐全**，cutover 门需要 L2+：

| 产物 | 内容 | 层 |
|---|---|---|
| `signature.jsonl` | 每帧 `timestamp_ms`, `fg_ratio`, `dhash`, 可选 SSIM 中间量 | L0/L1 |
| `events.jsonl` | `StateEvent` 序列（类型、时间、原因） | L0 |
| `segments.jsonl` | 段 `start_ms/end_ms`、代表帧时间戳列表 | L0 |
| `ocr_decisions.jsonl` | 每段：是否调用 OCR、代表帧 ts、早停原因、选中行索引 | L0 |
| `ocr_lines.jsonl` | Mock/固定引擎下的 lines；Vision 见 L4 | L0/L4 |
| `entries.jsonl` | 去重前/后 `SubtitleEntry` | L0/L2 |
| `detection_hash` | 与现有 benchmark 同算法 | L2 |
| `out.srt` | 最终导出 | L3 辅助 |
| `metrics.json` | F1/CER/usable… | L3 |

**原则：** 最终 SRT 一致但中间决策不一致 → **parity 失败**（避免两错抵消）。

## 3. 比较层级（L0–L4）

| 层 | 对象 | 规则 |
|---|---|---|
| **L0** | 整数、枚举、事件类型、dHash、段边界 ms、调用次数、代表帧 ts | **exact** |
| **L1** | `fg_ratio`、SSIM 等 float | 字段级 **epsilon**（见 §4） |
| **L2** | `detection_hash`、段集合、OCR 调用决策（Mock） | **exact** |
| **L3** | 固定 GT：F1/precision/CER/usable/noise/empty | **不低于冻结水位**（阈值见 overview / benchmark） |
| **L4** | Vision 文本 | 允许引擎微差；**坐标变换与排序规则**仍 L0；质量走 L3 |

## 4. 字段级 exact / epsilon（schema v1）

| 字段 | 规则 |
|---|---|
| 所有 `*_ms` 时间戳 | exact int |
| `dhash` / hamming | exact |
| `fg_ratio` | `abs_rel`：`abs(a-b) <= 1e-9 + 1e-6*max(abs(a),abs(b))`（初值；若 Oracle 证明需放宽，**改 schema 版本**） |
| SSIM | 同上，默认 `1e-6` rel |
| `confidence` | exact 若来自 Mock；Vision 不入 L0 |
| 字符串 text（Mock/line_select 纯函数） | exact NFC 后比较（见 §5） |
| box x,y,w,h | exact int（应用 round 契约后） |

未列字段默认 **exact**；新增字段必须更新本表并 bump `golden_schema_version`。

## 5. 语言 / 数值语义契约

### 5.1 Rounding

- Python 3 `round` 为 **银行家舍入**（banker's rounding，.5 向偶数）。
- C++ **禁止**对 parity 路径使用 `std::round` 假装等价。
- 契约：Vision box 等路径实现 **`bankers_round`**（与 Python 3 一致），单测锁定 `.5` 用例。

### 5.2 浮点序列化

- Golden JSON：float 用足够精度（建议 17 位 g 或 decimal 字符串）；compare 时按 §4 epsilon，不按字符串相等。
- 禁止在 golden 中混用 locale 相关格式。

### 5.3 Unicode

- 文本比较前：NFC normalize（与 Python `unicodedata.normalize("NFC")` 对齐）。
- 不在 parity 中隐式 strip 除非 Python 参考路径也 strip。

### 5.4 必须复现的历史行为（非 bugfix 窗口）

| 行为 | 说明 |
|---|---|
| 色域怪癖 | Pipeline `RGB→BGR` 后 signature 侧按历史 `RGB2GRAY` 路径处理——**按现状复现** |
| 时间戳 | `timestamp_ms = int(frame_index / fps * 1000)`，非容器 PTS |
| Vision box | 归一化 bottom-left → 像素 top-left；bankers_round + clamp + 行排序 |
| 空文本 | `drop_empty_text` 默认 false 等 Config 语义 |

修复类改动必须独立 PR + 新 oracle_commit，不得藏进迁移。

## 6. Harness 布局（06004）

```text
scripts/parity/              # Python dump（依赖 oracle 环境）
  dump_config.py
  dump_signature.py          # 6.1+
  ...
cpp/tests/parity/            # C++ load + compare
docs/cpp/parity-contract.md  # 本文
benchmark/parity/            # manifests + 小 golden（或 gitignore 大件）
```

最小 06004 交付：

1. 本文定稿（schema v1）
2. dump **完整默认 Config** JSON + C++ 读入比对（exact 字段）
3. `oracle_commit` 写入该 golden 旁 metadata
4. 文档：如何为 signature 加下一条 golden

## 7. 与现有 Benchmark 的关系

- **不复制**整套 Python 诊断实现到 C++。
- Candidate 接入方式（cutover 门）：
  1. C++ 导出与 Python 相同的 `entries` / 中间 JSONL；或
  2. CLI/`--runtime python|cpp` 后 **复用**现有 `benchmark/` 对齐与指标脚本。
- 优先 (2) 做 L3；L0–L2 用本 harness。

## 8. schema 版本

- 当前：`golden_schema_version = 1`
- 任何 epsilon 或字段集合变更 → 版本 +1 并迁移说明

## 9. 扩展：增加 signature golden（6.1 指南）

1. 冻结 oracle：同一 `oracle_commit` 策略；输入帧或视频记 `input_asset_sha256`。
2. Python：`scripts/parity/dump_signature.py`
   - 固定小 fixture + Config
   - 调用 Python signature 参考实现
   - 写出 `signature.jsonl`：每行 `{timestamp_ms, fg_ratio, dhash, ...}`
   - envelope `kind` 可为 `"signature"` 或旁路 metadata
3. 比较：L0 exact 对 `dhash`/`timestamp_ms`；L1 epsilon 对 `fg_ratio`（§4）
4. C++：实现 candidate 后加 `cpp/tests/parity/signature_parity_test.cpp`
5. 字段/epsilon 变更才 bump `golden_schema_version`
6. 禁止：无 commit 的「用当前 main 跑一下」

## 10. Config golden 路径（schema v1 可执行）

- 文件：`benchmark/parity/goldens/config/default_config.v1.json`
- 顶层：`golden_schema_version`（1）、`kind`（`config`）、`oracle`、`config`
- `oracle` 必填：`oracle_commit`、`config_fingerprint`（`sha256:` + 规范 JSON 的 SHA-256）
- `config`：与 `src/sublift/config.py` / `sublift::Config` 全字段对齐；未知键拒绝
- 比较层：Config 体 **L0 exact**（默认值无需 L1 epsilon）
- 工具：`scripts/parity/dump_config.py`（写 / `--check`）；C++ `load_config_golden` + Catch2 `[parity]`
