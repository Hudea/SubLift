# SubLift C++ 优化完整回归测试报告

- **测试时间**：2026-07-29（约 10:52–11:05 CST）
- **仓库 / 分支**：`SubLift_CPP` · `refactor/cpp`
- **Commit**：`53f53a4`（`fix(phase6.6): 加固 Worker 并补齐 sanitizer 对照`）
- **目的**：以发现问题为主，跑通基线 + 产品路径 + cutover（含固定 GT 视频）
- **测试视频来源**：主仓库 `SubLift/debug/`（本仓库 `debug/` 为符号链接，未复制大文件）
- **原始日志**：`/tmp/sublift_full_test_2026-07-29/`

---

## 1. 环境

| 项 | 值 |
|---|---|
| OS | Darwin 26.5.2 (macOS) |
| Python (uv) | 3.12.13 |
| CMake | 4.4.0 |
| ffmpeg | 8.1 (`ffmpeg-full`) |
| OpenCV | 4.14.0 (`opencv@4`) |
| C++ 配置 | `Debug` + `SUBLIFT_REQUIRE_OPENCV=ON` + **`SUBLIFT_ENABLE_VISION=ON`** |
| GT 视频 | `debug/Zootopia_clip_1080p.mp4` → 主仓 1080p / ~254s |
| 冒烟视频 | `debug/Zootopia_clip_test_2min.mp4` → 主仓 2min / 1080p |
| GT SRT | `benchmark/fixtures/Zootopia_clip_1080p_gt.srt`（87 条） |

---

## 2. 执行矩阵与结果总览

| # | 套件 | 结果 | 说明 |
|---|---|---|---|
| A1 | `ruff check .` | ✅ PASS | 0 error |
| A2 | `mypy src tests` | ✅ PASS | 70 files |
| A3 | `pytest`（默认，非 integration） | ✅ PASS | **508 passed**, 23 deselected |
| B1 | cmake configure VISION=ON | ✅ PASS | OpenCV + Vision 开启 |
| B2 | cmake build **product bins** | ✅ PASS | `sublift` / `sublift_worker` 可链接 |
| B3 | cmake build **`sublift_tests`** | ❌ **FAIL** | 见问题 **P1** |
| B4 | ctest（旧 binary / 部分） | ⚠️ 不可信 | 测试二进制未因 VISION=ON 重建成功；曾显示 152/152，**不能**当作 VISION=ON 验收 |
| C1 | 10× parity `dump_*.py --check` | ✅ PASS | 全绿 |
| C2 | `pytest tests/ipc/test_cpp_worker.py` | ✅ PASS | 9/9 |
| D1 | cutover `--check`（无 PYTHONPATH，有 GT） | ❌ **FAIL** | 见问题 **P2** |
| D2 | cutover + `PYTHONPATH=.`（有 GT） | ✅ PASS | GT L3 **实测**过水位；报告见 `phase6.6-cutover-gate-fulltest-2026-07-29.md` |
| E1 | 原生 CLI mock/vision 2min | ✅ 功能 PASS | 见性能 **P3** |
| E2 | Python CLI default(cpp)/python/paddle 2min | ✅ 功能 PASS | paddle 正确强制 Python |
| E3 | 全片 254s vision：C++ vs Python | ✅ 文本一致 | 见 **P3/P4** |

**结论一句话：** 正确性（parity + 同配置下 C++/Python 产品 SRT 一致 + 固定 ROI 的 GT L3）整体健康；**发布与工程门禁存在真实缺陷**（VISION 测试编不过、cutover GT 导入路径、产品路径墙钟显著慢于 Python、默认路径空字幕条目）。

---

## 3. 发现问题（按严重度）

### P1 — Critical：`SUBLIFT_ENABLE_VISION=ON` 时 `sublift_tests` 编译失败

**现象**

```
cpp/tests/worker_path_mode_test.cpp:276:18:
error: no member named 'is_vision_available' in namespace 'sublift'
```

**原因**

- `worker_path_mode_test.cpp` 在 `#if SUBLIFT_ENABLE_VISION` 分支调用 `sublift::is_vision_available()`
- **未** `#include "sublift/vision.hpp"`（同目录 `worker_protocol_test.cpp` 已正确 include）
- 产品二进制（`sublift` / `sublift_worker`）仍可链接；**Catch2 测试目标失败**

**影响**

- 产品路径需要 VISION=ON，但 **无法在同配置下跑通完整 ctest**
- 易误判：若沿用 VISION=OFF 旧 `sublift_tests`，ctest 仍可能全绿，掩盖 vision 集成测缺失

**建议修复**

```cpp
#include "sublift/vision.hpp"
```

（仅 `worker_path_mode_test.cpp`；确认无其它 VISION 条件编译缺头文件。）

---

### P2 — High：GT 视频存在时 cutover 门默认失败（`No module named 'benchmark'`）

**现象**

```text
[3/3] 正在评估 GT L3 水位门禁...
  [failed] GT L3 measurement error: No module named 'benchmark'
```

**复现**

```bash
# 失败（init.sh 等价调用）
uv run --extra vision --extra paddle \
  python scripts/parity/check_cutover_gate.py --check

# 成功
PYTHONPATH=. uv run --extra vision --extra paddle \
  python scripts/parity/check_cutover_gate.py --check
```

**原因**

- 以脚本方式执行时 `sys.path[0]` = `scripts/parity/`，**不含仓库根**
- `run_gt_l3_check()` 内 `from benchmark.diagnostics import ...` 依赖仓库根在 path 上
- `pyproject.toml` 的 `pythonpath = ["src", "."]` **只作用于 pytest**，不作用于 `python scripts/...`
- 无 GT 视频时走 ADR-0022 waiver，**从不 import benchmark → 缺陷被隐藏**

**影响**

- 一旦机器放上 `debug/Zootopia_clip_1080p.mp4`，`./init.sh` 的 cutover 段会 **硬失败**
- 有资产时门禁反而比无资产更红（与「有资产应更严」一致，但失败原因是工程 bug，不是质量）

**建议修复**

在 `check_cutover_gate.py` 顶部（import 前）插入：

```python
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))
```

或 `init.sh` 统一 `PYTHONPATH=.`；前者更稳妥。

---

### P3 — High：产品路径墙钟相对 Python 严重回退（cutover 微基准未覆盖）

| 场景 | C++ 路径 | Python runtime | 比值 (C++/Py) |
|---|---:|---:|---:|
| 2min · mock · 全默认 | **46.4s**（native） | **7.2s** | **~6.4×** |
| 2min · vision · cjk | **54.0s**（native / py→cpp） | **15.3s** | **~3.5×** |
| **254s 全片 · vision · cjk** | **119.5s**（native） | **32.4s** | **~3.7×** |
| cutover 微任务 wall | 0.126s | 0.108s | 1.16×（门禁 PASS） |
| cutover GT L3（固定 `region_box`） | **19.5s** | — | 仅 C++ worker 测 |

**对照**

- 纯 ffmpeg 抽 2min @5fps raw RGB → `/dev/null`：**~2.8s**  
  → C++ mock 46s 远超解码本身，瓶颈在 **抽帧/管线/帧搬运** 一侧，而非 OCR  alone。

**影响**

- 产品默认已是 C++，用户体感会比 Python 回滚路径 **慢约 3–4×（vision）/ 6×（mock）**
- cutover runtime 门只测极短 IPC job，**无法发现全片回归**
- RSS 优势仍明显（cutover：C++ ~15MB vs Py ~110MB）

**建议**

1. 为 cutover 增加「固定片全链路 wall」门（至少 2min mock + 全片 vision 或 ROI 全片），阈值需重新标定  
2. 剖析 C++ extractor：是否缺 ROI crop-before-decode、是否同步阻塞、是否多余拷贝  
3. 对比 GT L3 固定 ROI 仅 19.5s vs 默认 CLI 119s：默认路径是否未走与 Python 同等的 ROI 优化

---

### P4 — Medium：默认产品路径导出大量**空文本字幕**（C++/Python 一致）

| 输出 | 总条数 | 空文本 | 非空 | 与对端一致 |
|---|---:|---:|---:|---|
| native C++ vision 全片 | 109 | **22** | 87 | 与 Python 逐条 exact |
| Python vision 全片 | 109 | **22** | 87 | |
| GT | 87 | 0 | 87 | — |
| cutover GT L3（固定 region） | — | **0**（门禁） | — | F1/CER 过水位 |

**说明**

- 非 C++ 回归：Python `--runtime python` 同样 22 条空 cue  
- 但 **cutover GT L3 使用固定 `region_box=[0,848,1920,87]`**，不测默认 bottom-crop CLI  
- 默认路径若按 GT 水位计 `text_empty`，会远超阈值（max 1）

**建议**

- 产品默认应 `drop_empty_text` 或在 export 前过滤空条目  
- cutover 增加「默认 CLI 配置」质量门，避免仅固定 ROI 装绿

---

### P5 — Medium：VISION=ON 与 init 默认构建矩阵不一致

- `init.sh` / 文档：默认 **VISION=OFF** 构建 + L0 vision 几何 golden  
- 产品默认 engine 是 **vision** → 需要 VISION=ON 的 worker  
- 本次在 VISION=ON 下才能做真实 recognize 冒烟；且触发 P1  
- 风险：CI/init 绿 ≠ 产品 vision worker 同配置可测

**建议**

- macOS init 增加第二段：`SUBLIFT_ENABLE_VISION=ON` 构建 + 关键 vision/worker 测试  
- 或默认 ON（评估 stub 策略）

---

### P6 — Low / 观察：2min 片 OCR 噪声（双端一致）

- 如 `跟胡尼克`、偶发 `™` 等（2min native/python 均可见）  
- 全片非空 87 条 C++≡Python，且固定 ROI 的 GT L3 CER=3.2% 过水位  
- 属 Vision/后处理既有行为，**非 C++ 独有**；保留为质量债

---

### P7 — Low：cutover 失败时仍可能覆盖「成功报告」

- 无 PYTHONPATH 的失败跑把结果写回默认/指定 report  
- 操作上需注意：失败 run 会覆盖先前 PASS 报告（本次已用 `PYTHONPATH=.` 重写 fulltest 报告）

---

## 4. 通过项（证据摘要）

### 4.1 Python 基线

- ruff / mypy / pytest：**全绿**（508 tests）

### 4.2 Parity（Oracle golden）

10/10 dump `--check` PASS：config, signature, changepoint, timeline, dedupe, line_select, pipeline, extractor, vision, ipc_session

### 4.3 Worker e2e

`tests/ipc/test_cpp_worker.py` 9/9：handshake、path、frame、cancel、bye、oversized base64、engine mismatch、paddle reject

### 4.4 Cutover（`PYTHONPATH=.` + GT 视频）

| 门 | 结果 |
|---|---|
| 10 golden | ✅ |
| Wall / Cancel / Restart / RSS | ✅（微任务） |
| GT L3 live | ✅ timing_f1=0.9767, precision=0.9882, usable=0.9195, cer_macro=0.0320, noise=0, empty=0（耗时 19.5s） |

详见：`docs/reports/phase6.6-cutover-gate-fulltest-2026-07-29.md`

### 4.5 产品路径功能

| 路径 | 2min | 全片 254s |
|---|---|---|
| native `sublift` mock | ✅ 3 条 mock 段 | — |
| native `sublift` vision | ✅ 50 条（含 2 空） | ✅ 109 条（22 空 + 87 非空） |
| `uv run sublift` 默认 cpp vision | ✅ 与 native 一致 | — |
| `uv run sublift --runtime python` vision | ✅ | ✅ 与 C++ **87 非空 exact + 22 空 exact** |
| paddle | ✅ 强制 Python；46 条（2min） | — |

**正确性亮点：** 同配置下 C++ 与 Python 全片 vision SRT **逐条一致**（含空条目），说明迁移在默认 CLI 语义上对齐良好。

### 4.6 引擎矩阵

- paddle 打印「暂仅支持 Python runtime，自动切回」——符合契约，无静默 vision/mock

---

## 5. 未覆盖 / 未在本轮硬验

| 项 | 原因 |
|---|---|
| ASan + OpenCV worker 全绿 | 已知 TBB 退出 134（06607）；本轮未重跑 sanitizer |
| GUI Swift 冒烟 | 未启 GUI |
| 长流 ≥10min | 最长 254s |
| Linux core-only | 本机 macOS only |
| Paddle 质量水位 | 仅功能路由；未对 GT 打 F1 |

---

## 6. 原始产物路径

```text
/tmp/sublift_full_test_2026-07-29/
  00_env.txt
  01_python_baseline.txt
  02_cpp_build_ctest.txt          # 含首次 build 失败痕迹
  02b_rebuild_vision_verify.txt   # P1 完整编译错误
  03_parity_worker.txt
  04_cutover_gate.txt             # P2 首次失败
  04b_cutover_gt_fixpath.txt     # PYTHONPATH 修复后 PASS
  04c_cutover_no_pythonpath.txt   # P2 再确认
  05_cli_smoke_2min.txt
  06_ffmpeg_baseline.txt
  07_full_254s_vision.txt
  cli_outputs/
    native_mock_2min.srt
    native_vision_2min.srt
    py_*_2min.srt
    native_vision_full.srt
    py_vision_full.srt

docs/reports/
  phase6.6-cutover-gate-fulltest-2026-07-29.md   # GT L3 PASS 报告
  full-regression-test-2026-07-29.md             # 本文件
```

仓库内 `debug/*.mp4|mkv` 为指向主仓的符号链接（**勿提交** 除非团队约定；`.gitignore` 通常已忽略 `debug/` 大文件）。

---

## 7. 建议修复优先级（面向「发现问题 → 收口」）

| 优先级 | ID | 动作 | 预期收益 |
|---|---|---|---|
| P0 | P1 | 补 `vision.hpp` include，VISION=ON 下 ctest 全绿 | 恢复产品配置可测性 |
| P0 | P2 | cutover 脚本注入 `REPO_ROOT` 到 `sys.path` | 有 GT 资产时 init/门禁不再假失败 |
| P1 | P3 | 剖析并修复 C++ 全片墙钟；cutover 增加全链路 wall 样本 | 产品默认不劣于 Python |
| P1 | P4 | 默认过滤空文本；GT 门增加默认 CLI 配置 | 导出质量与水位一致 |
| P2 | P5 | init 双矩阵（VISION ON/OFF） | 避免「init 绿、产品配置红」 |
| P3 | P6 | OCR 噪声另案（非阻塞 cutover） | 体验打磨 |

---

## 8. 总评

| 维度 | 评分 | 说明 |
|---|---|---|
| 算法 / golden parity | **健康** | 10/10 + 全片 C++≡Python |
| 固定 ROI GT 质量 | **健康** | L3 全指标过冻结水位 |
| 工程可构建可测（VISION=ON） | **不健康** | 测试目标编不过（P1） |
| 门禁脚本在有资产机器 | **不健康** | benchmark 导入失败（P2） |
| 产品默认性能 | **不健康** | ~3.7× 慢于 Python（P3） |
| 产品默认导出洁净度 | **有债** | 22 条空字幕，双端一致（P4） |

**本轮最有价值的发现：** 不是「C++ 识别错了」，而是 **(1) 工程门在 VISION=ON / 有 GT 时自己绊倒；(2) 默认产品路径性能与空字幕问题被微基准和固定 ROI 门挡住。** 正确性迁移本身在同配置对照下表现扎实。
