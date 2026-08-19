# ROI 数据通路设计（Phase 4）

> 本文定义固定区域 crop-before-Python 的历史架构契约。feat-038 已实现该通路；
> 任务与验收证据见 [phase4.json](../phases/phase4.json)。

## 1. 术语与非承诺

本设计的准确名称是 **FFmpeg ROI 输出（crop-before-Python）**：

~~~text
压缩视频
  → ffmpeg 仍可能重建完整编码帧
  → fps 采样
  → crop 到字幕 ROI
  → 仅 ROI rgb24 经 stdout 进入 Python
~~~

它不等同于 codec 级 ROI decode。收益来自减少 RGB 输出、管道传输、PIL
Image.frombytes、PIL 裁剪与下游图像处理；不承诺按 ROI 面积倍数降低 codec 解码或
整体 wall time。

## 2. 坐标契约

BoundingBox 不是天然“绝对坐标”。每个边界必须声明坐标空间，禁止把同一个数值
Box 跨空间复用。

| 名称 | 原点 / 单位 | 生产者与消费者 |
|---|---|---|
| source-frame | 与当前全帧 FfmpegExtractor 输出一致的画面左上 / 像素 | GUI start_job.region_box、benchmark manifest、ffmpeg crop 参数、诊断报告 |
| frame-local | 当前 extractor 输出图像左上 / 像素 | Frame.image、ROI Pipeline 的 Region.box、signature |
| ocr-crop | 传给 OCR 图像左上 / 像素 | OcrLine.box、SubtitleProfile |

固定 source ROI 为 R = [rx, ry, rw, rh] 时，ROI 图像中的像素 L 与 source-frame
像素 S 的关系是：

~~~text
Lx = Sx - rx
Ly = Sy - ry
0 ≤ Lx < rw，0 ≤ Ly < rh
~~~

Phase 4 不缩放，因此 source-frame 到 frame-local 仅是平移；OCR crop 与
frame-local 在 ROI passthrough 路径中是同一张图。

canonical 例子：

| 项 | 值 |
|---|---|
| source_region_box | [0, 848, 1920, 87] |
| Frame.image.size | (1920, 87) |
| ROI Pipeline Region.box | [0, 0, 1920, 87] |
| SubtitleProfile / OcrLine | 继续相对 (0, 0) 的 ROI 输入，不重映射 |

## 3. 路由与数据流

~~~text
Swift / benchmark
  source_region_box（source-frame）
      │
      ├─ FfmpegExtractor(output_crop=source_region_box)
      │     └─ fps=<fps>,crop=<w>:<h>:<x>:<y>:exact=1
      │         └─ Frame.image（frame-local ROI）
      │
      └─ RoiPassthroughDetector(width, height)
            └─ Region([0, 0, width, height])
                  └─ Pipeline：直接使用整张 ROI
~~~

| 路由 | extractor | detector | 结果 |
|---|---|---|---|
| GUI path mode + 有效 fixed region | ROI raw RGB | RoiPassthroughDetector | 自动 ROI |
| benchmark frame_output_mode=roi | ROI raw RGB | RoiPassthroughDetector | 实验组 |
| benchmark frame_output_mode=full | 全帧 raw RGB | FixedRegionDetector(source box) | 对照组 |
| 无 region 的 path mode | 全帧 raw RGB | BottomCropDetector | 保持现状 |
| legacy Swift frame mode | Swift 全帧 JPEG | 现有 detector | 保持现状 |

RoiPassthroughDetector 的存在是语义边界：它只消费 frame-local 尺寸，永不接收
source_region_box。不能将 FixedRegionDetector(source_region_box) 直接注入 ROI
Frame，否则 Pipeline 会把 source Y 再应用到局部图，造成二次裁剪或越界。

## 4. 模块职责

| 模块 | Phase 4 职责 | 不负责 |
|---|---|---|
| FfmpegExtractor | 探测、校验 source output crop、构建 filter、以 ROI 尺寸读 raw RGB、保留取消语义 | codec 级 ROI decode、坐标静默修正 |
| detector/RoiPassthroughDetector | 对 ROI Frame 返回局部全幅 Region | 解释 source 坐标 |
| Pipeline | full-size local Region 时直接透传图像；其余路径维持原 crop | 选择是否启用 ROI |
| bridge | 将同一 source region 分别交给 extractor 和诊断；将 local detector 注入 Pipeline | 新增 GUI 用户开关 |
| benchmark | 提供 full / roi 内部 A/B，联合质量与性能报告 | 以单次旧报告替代对照 |
| Swift GUI | 继续发送 source region 与局部 SubtitleProfile；有效固定 region 时默认受益 | 发送帧或管理 ROI 开关 |

## 5. Extractor 契约

目标接口为 FfmpegExtractor(fps, output_crop=None, performance_recorder=None)。

### 5.1 校验

在 spawn ffmpeg 前，先以 ffprobe 获得 source-frame 宽高并校验：

- x ≥ 0、y ≥ 0、width > 0、height > 0；
- x + width ≤ source_width，y + height ≤ source_height；
- 当前 GUI 坐标与 ffmpeg source-frame 存在非恒等旋转/显示变换且未经验证时，不启用 ROI。

显式 output_crop 非法时立即报任务错误，错误文字必须包含 source 尺寸和请求区域；
不得 clamp，也不得退回全帧。

### 5.2 Filter 与帧读取

有效 ROI 使用：

~~~text
-vf "fps=<fps>,format=rgb24,crop=<width>:<height>:<x>:<y>:exact=1"
-pix_fmt rgb24 -vcodec rawvideo
~~~

fps 在 crop 前，避免对未采样帧做无用裁剪；``format=rgb24`` 再 crop，使裁剪发生在
与 full 路径 Python crop 相同的 RGB 空间，避免 yuv420 色度对齐导致 detection_hash
漂移；exact=1 防止坐标/尺寸被悄悄改写。每帧大小必须为
output_width × output_height × 3，构造出的 PIL 图尺寸必须完全相等。

### 5.3 取消与错误

ROI 不新增线程或队列。FfmpegExtractor.cancel 仍负责终止已启动的 ffmpeg；
bridge 的 finally 仍负责回收 extractor / worker。ffprobe 期间没有承诺额外的
可中断性。

## 6. Pipeline 局部图不变量

RoiPassthroughDetector 返回的 Region 必须刚好等于 Frame.image 的全幅。Pipeline 的
crop 帮助函数应检测这个条件并直接返回原图，而不是再次创建等尺寸 PIL crop。

| 不变量 | 目的 |
|---|---|
| ROI Region = [0, 0, image.width, image.height] | 防止 source / local 坐标混用 |
| pipeline_crop_count = 0（ROI 路径） | 无二次 PIL crop 的硬不变量 |
| full_frame_passthrough_count ≥ frame_count | 几何全幅零拷贝次数（feed+OCR 会累加；非 ROI 专属） |
| SubtitleProfile 不重新映射 | 保持 feat-034 的 OCR 行级选择语义 |
| OcrLine.box 仍相对 OCR 输入 | 保持 OCR Protocol 不变 |

## 7. 诊断与可比性

PerformanceSummary / benchmark 报告应区分并记录：

- source_width / source_height；
- output_mode = full_rgb 或 roi_rgb；
- source_region_box（仅 roi）；
- output_width / output_height；
- raw_bytes_per_frame、raw_output_bytes、frame_count；
- full_frame_passthrough_count、pipeline_crop_count；
- extract_wait 仍表示 decode + fps/filter + RGB + pipe 等待，绝不命名为纯 decode。

full 和 roi 只能在同提交、同机、同 manifest、同环境元数据下直接比较。任何 ROI
性能结论必须随同次 detection_hash 与质量 gates 一起归档。

## 8. 自动测试不变量

1. 越界、零尺寸、奇数坐标 / 尺寸的 output_crop 具有确定的校验或 exact 行为。
2. 合成视频的 ROI 像素等于全帧输出后对同一区域裁剪的像素；帧数与时间戳一致。
3. ROI 输入下 source box 不进入 Pipeline 图像裁剪；profile 的局部几何不偏移。
4. 无 region、legacy frame mode、BottomCrop 与取消/重启既有测试不回归。
5. benchmark A/B 报告含完整 output metadata，trace 仍可解析。
6. canonical ROI 三次结果必须与同次 full 对照 detection_hash 完全一致。

## 9. 后置边界

缩放、JPEG / 其他像素格式、CLI BottomCrop ROI、旋转坐标支持、动态区域、并发和
codec ROI decode 均不属于本设计的第一轮实现。
