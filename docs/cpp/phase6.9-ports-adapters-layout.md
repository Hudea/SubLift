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

`ports/<concrete>.hpp` 暂保留 **一行 re-export** 到 adapters，避免一次性改光历史 include；新代码应直接 `#include "sublift/adapters/…"`。

## Detector 工厂

```text
IDetectorFactory（application）
  └─ DefaultDetectorFactory（worker / adapters）
       make_fixed / make_bottom_crop / make_roi_passthrough
```

Bridge 只依赖 `IDetector` + `IDetectorFactory`，不再 `make_unique` 具体 detector。

## 非本轮

- nlohmann 从 protocol public 头移除（PIMPL，另开）
- 删除全部兼容 re-export
