# `ports/` — abstract interfaces only

Canonical Port headers:

- `ocr.hpp` — `IOcrEngine`
- `detector.hpp` — `IDetector`
- `extractor.hpp` — `IExtractor`, `FrameIOPlan`, …

Concrete engines/detectors live under `../adapters/`.

Any other `ports/*.hpp` files are **temporary re-exports** to `adapters/` for legacy includes; new code should include `sublift/adapters/…` directly.
