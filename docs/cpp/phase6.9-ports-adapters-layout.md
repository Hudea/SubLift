# Phase 6.9 — Ports / Adapters 分层（架构洁癖落地）

## 目标

对齐 `phase6.9-native-product-architecture.md` §7.2：

- **`include/sublift/ports/`**：仅抽象 Port（`IOcrEngine`、`IDetector`、`IExtractor` 及 DTO）
- **`include/sublift/adapters/`**：具体实现（Paddle / FFmpeg / Vision / detectors / mock）
- Application 通过工厂注入具体类型，不 `#include` 具体 detector 实现头

## 布局

```text
include/sublift/
├── ports/
│   ├── ocr.hpp              # IOcrEngine
│   ├── detector.hpp         # IDetector
│   └── extractor.hpp        # IExtractor, FrameIOPlan, …
├── adapters/
│   ├── paddle.hpp
│   ├── paddle_geometry.hpp
│   ├── ffmpeg.hpp
│   ├── vision.hpp
│   ├── mock_ocr.hpp
│   ├── fixed_detector.hpp
│   ├── bottom_crop_detector.hpp
│   └── roi_passthrough_detector.hpp
└── *.hpp                    # 兼容 re-export（→ adapters 或 ports）
```

`ports/` 不再保留任何具体实现 re-export。根目录 `include/sublift/*.hpp` 的少量历史
转发头暂作源码兼容，新产品代码直接包含 `sublift/adapters/…`。

## Detector 工厂

```text
IDetectorFactory（application）
  └─ DefaultDetectorFactory（worker / adapters）
       make_fixed / make_bottom_crop / make_roi_passthrough
```

Bridge 只依赖 `IDetector` + `IDetectorFactory`，不再 `make_unique` 具体 detector。

## 后续兼容清理

- 可在独立机械 feature 中删除根目录历史 re-export；不影响当前 Ports 边界。
