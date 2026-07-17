# Phase 4 ROI 数据通路 — 执行进度

## Phase 名称与目标

- **Phase**：`phase4-roi-data-path`
- **分支**：`opt/roi-data-path`（基线 `fdc2d1b`）
- **目标**：固定字幕区域的 ffmpeg crop-before-Python ROI 输出；同提交 full/roi A/B 与固定 GT 验收；≥10 分钟非 Zootopia 真实 GUI 长流体验验收。
- **非目标**：codec 级 ROI decode、OCR/打轴算法改动、质量泛化、CLI BottomCrop ROI。

## Feature 列表与依赖

| ID | 名称 | 依赖 | 状态 |
|---|---|---|---|
| feat-038 | 固定区域 FFmpeg ROI 输出通路 | feat-037 | in-progress |
| feat-039 | ROI A/B 性能与固定 GT 回归 | feat-038 | not-started |
| feat-040 | 非 Zootopia 长视频 GUI 真实验收 | feat-039 | not-started |

执行顺序：038 → 039 → 040（严格串行）。

## 当前执行状态

- **当前 Feature**：feat-038
- **当前步骤**：实现+测试通过；审查轮 1 Changes Required → 已修 Major → 等待轮 2
- **测试**：ruff/mypy 绿；pytest 全量绿（含 ROI 33+ 用例）；swift test 139 绿

## Feature Scope（feat-038）

### 目标

对 GUI path mode 与 benchmark 的有效固定 region，让 ffmpeg 在 rgb24 stdout 前 exact crop，Pipeline 以 frame-local 全幅 ROI 消费，避免 source 坐标二次裁剪。

### 必须实现

1. source-frame / frame-local / OCR-crop 坐标契约与 output_crop 严格校验
2. `FfmpegExtractor(output_crop=...)`：fps 后 crop exact=1，按 ROI 尺寸读 raw RGB
3. `RoiPassthroughDetector`：返回 `[0,0,w,h]`
4. Pipeline 全幅 ROI 透传（不二次 PIL crop）；计数 roi_passthrough / pipeline_crop
5. bridge path mode + 固定 region 自动 ROI
6. benchmark 内部 `frame_output_mode` full/roi A/B
7. 性能报告：output_mode、source/output 尺寸、source_region_box、raw_bytes、crop 计数
8. 自动回归测试（合成视频像素/尺寸/帧数/时间戳；坐标；取消；无 region；legacy；benchmark 报告）

### 明确不在范围

- codec ROI decode、缩放、JPEG、CLI BottomCrop ROI、旋转坐标支持（未验证则全帧回退）
- 改 OCR/打轴算法
- feat-039 的正式 A/B 硬门跑分
- feat-040 真实长视频 GUI

### 涉及模块

- `src/sublift/extractor/ffmpeg_extractor.py`
- `src/sublift/detector/`（新 RoiPassthroughDetector）
- `src/sublift/pipeline/core.py`
- `src/sublift/diagnostics/performance.py`
- `src/sublift/ipc/bridge.py`
- `benchmark/runner.py`, `benchmark/manifest.py`, 报告层
- 对应 tests

### 验收条件

见 `docs/plans/phase4-roi-data-path.md` §5 feat-038 完成定义 1–8。

## 已完成事项

- [x] 阅读 ARCHITECTURE、phase4 计划、ROI 设计、phase4.json、现有 extractor/pipeline/bridge/benchmark
- [x] 确认依赖顺序 038→039→040
- [x] feat-038 实现（extractor/detector/pipeline/perf/bridge/benchmark）
- [x] feat-038 测试（test_roi_output_path + 全量回归）
- [ ] feat-038 审查 Approved（轮 1: Changes Required，已修；轮 2 进行中）
- [ ] feat-038 commit

## 当前测试结果

- `uv run ruff check .` → All checks passed
- `uv run mypy src tests` → Success
- `uv run pytest` → 全绿（396+，ROI 增补后更多）
- `swift test`（apps/macos）→ 139 passed

## 审查轮次与结论

### 轮 1 — Changes Required
- Major1: 恒等 Display Matrix a33 误判 → 已修（rotation 优先 + 2^30）
- Major2: bridge 正向测试弱 → 已修（断言 output_crop + detector）
- Minor: set_detector API、文档、计数语义 → 已处理

### 轮 2 — 进行中

## 已知问题与阻塞

- feat-040 需要本地 ≥10 分钟非 Zootopia 真实硬字幕视频；若环境缺失将阻塞该 Feature（不阻塞 038/039）。
- 显式 ROI 越界必须报错；旋转未验证必须全帧回退。

## Commits

| Feature | Hash | 说明 |
|---|---|---|
| — | — | 尚未提交 |

## 下一步行动

1. 计划子代理产出 feat-038 可执行计划并主 Agent 审查
2. 实现 → 测试 → 审查 → 提交
3. 继续 feat-039、feat-040
