# `ports/` — abstract interfaces only

Canonical Port headers:

- `ocr.hpp` — `IOcrEngine`
- `detector.hpp` — `IDetector`
- `extractor.hpp` — `IExtractor`, `FrameIOPlan`, …

Concrete engines/detectors live under `../adapters/`.
Legacy compatibility re-exports, where still required, live at `include/sublift/*.hpp`;
`ports/` itself never exposes concrete adapters.
