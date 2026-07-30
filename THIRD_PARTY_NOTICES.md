# Third-party notices

## PaddleOCR / RapidOCR algorithm compatibility

The native Paddle adapter in `cpp/src/paddle/ppocr_det_preprocess.*` and
`cpp/src/paddle/ppocr_db_postprocess.*` independently adapts the observable
preprocessing and DB postprocessing behavior of:

- PaddleOCR, copyright 2020 PaddlePaddle Authors;
- RapidOCR 3.9.2, maintained by RapidAI.

Those upstream implementations are provided under the Apache License 2.0:
<https://www.apache.org/licenses/LICENSE-2.0>.

SubLift's implementation uses its own C++ types and target boundaries. It does
not embed Python, call RapidOCR as a subprocess, or vendor the upstream
repositories. Python RapidOCR remains a test oracle and explicit product
fallback.

The DB unclip compatibility implementation also independently reproduces the
observable integer offset semantics of pyclipper 1.4.0 / Angus Johnson's
Clipper 6.4.2. pyclipper is MIT-licensed and the Clipper core is distributed
under the Boost Software License 1.0. SubLift does not vendor either codebase.
