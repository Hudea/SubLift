#!/usr/bin/env python3
"""
scripts/test_check_dependencies.py - Comprehensive Unit & Negative Tests for Dependency Checker

Validates that check_dependencies.py catches architectural violations deterministically:
  1. Negative test: timing including text headers fails.
  2. Negative test: text including timing headers fails.
  3. Negative test: core including adapter headers fails.
  4. Negative test: features public header including <opencv2/core.hpp> fails.
  5. Negative test: core public header including <opencv2/opencv.hpp> fails.
  6. Negative test: engine including features headers fails.
  7. Negative test: engine including adapters headers fails.
  8. Negative test: core public header including <onnxruntime_cxx_api.h> fails.
  9. Negative test: core implementation file including <opencv2/core.hpp> fails.
  10. Negative test: adapter including engine headers fails.
  11. Positive test: features src implementation privately including OpenCV passes.
  12. Positive test: timing including features headers passes.
  13. Positive test: engine including timing and text headers passes.
  14. Positive test: commented-out violation does not trigger failure.
  15. Positive test: real project cpp/ directory passes cleanly with 0 violations.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

# Import the checker directly as a module as well
SCRIPTS_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPTS_DIR.parent
CHECKER_SCRIPT = SCRIPTS_DIR / "check_dependencies.py"

sys.path.insert(0, str(SCRIPTS_DIR))
import check_dependencies  # noqa: E402


class TestCheckDependencies(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.test_root = Path(self.temp_dir.name)

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def run_checker_cli(self, target_dir: Path) -> subprocess.CompletedProcess[str]:
        """Runs check_dependencies.py as a subprocess CLI command."""
        return subprocess.run(
            [sys.executable, str(CHECKER_SCRIPT), str(target_dir)],
            capture_output=True,
            text=True,
        )

    # -------------------------------------------------------------------------
    # Negative Test 1: timing including text
    # -------------------------------------------------------------------------
    def test_timing_including_text_fails(self) -> None:
        timing_header = self.test_root / "cpp" / "include" / "sublift" / "timing" / "bad_timing.hpp"
        timing_header.parent.mkdir(parents=True, exist_ok=True)
        timing_header.write_text(
            "#pragma once\n"
            "#include <sublift/core/frame.hpp>\n"
            "#include <sublift/text/consensus.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Checker must fail when timing includes text")
        self.assertIn("RULE-2", res.stdout)
        self.assertIn("Rule 2 (Mutual Exclusion)", res.stdout)
        self.assertIn("bad_timing.hpp", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 2: text including timing
    # -------------------------------------------------------------------------
    def test_text_including_timing_fails(self) -> None:
        text_source = self.test_root / "cpp" / "src" / "text" / "bad_text.cpp"
        text_source.parent.mkdir(parents=True, exist_ok=True)
        text_source.write_text(
            "#include <sublift/text/cleaning.hpp>\n"
            "#include <sublift/timing/changepoint.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Checker must fail when text includes timing")
        self.assertIn("RULE-2", res.stdout)
        self.assertIn("Rule 2 (Mutual Exclusion)", res.stdout)
        self.assertIn("bad_text.cpp", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 3: core including adapters
    # -------------------------------------------------------------------------
    def test_core_including_adapters_fails(self) -> None:
        core_header = self.test_root / "cpp" / "include" / "sublift" / "core" / "bad_core.hpp"
        core_header.parent.mkdir(parents=True, exist_ok=True)
        core_header.write_text(
            "#pragma once\n"
            "#include <sublift/adapters/mock/mock_ocr.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Checker must fail when core includes adapters")
        self.assertTrue(
            "RULE-1" in res.stdout or "RULE-7" in res.stdout,
            "Expected RULE-1 or RULE-7 for core including adapters",
        )
        self.assertIn("bad_core.hpp", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 4: features public header including <opencv2/core.hpp>
    # -------------------------------------------------------------------------
    def test_features_public_header_including_opencv_fails(self) -> None:
        feat_header = self.test_root / "cpp" / "include" / "sublift" / "features" / "bad_mask.hpp"
        feat_header.parent.mkdir(parents=True, exist_ok=True)
        feat_header.write_text(
            "#pragma once\n"
            "#include <opencv2/core.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(
            res.returncode, 0, "Checker must fail when features public header includes opencv2"
        )
        self.assertIn("RULE-4", res.stdout)
        self.assertIn("Rule 4 (3rd-Party Type Leakage)", res.stdout)
        self.assertIn("bad_mask.hpp", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 5: core public header including <opencv2/opencv.hpp>
    # -------------------------------------------------------------------------
    def test_core_public_header_including_opencv_fails(self) -> None:
        core_header = self.test_root / "cpp" / "include" / "sublift" / "core" / "bad_box.hpp"
        core_header.parent.mkdir(parents=True, exist_ok=True)
        core_header.write_text(
            "#pragma once\n"
            "#include <opencv2/opencv.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(
            res.returncode, 0, "Checker must fail when core public header includes opencv2"
        )
        self.assertIn("RULE-4", res.stdout)
        self.assertIn("Rule 4 (3rd-Party Type Leakage)", res.stdout)
        self.assertIn("bad_box.hpp", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 6: engine including features headers
    # -------------------------------------------------------------------------
    def test_engine_including_features_fails(self) -> None:
        engine_src = self.test_root / "cpp" / "src" / "engine" / "bad_engine.cpp"
        engine_src.parent.mkdir(parents=True, exist_ok=True)
        engine_src.write_text(
            "#include <sublift/engine/session.hpp>\n"
            "#include <sublift/features/mask.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Checker must fail when engine includes features")
        self.assertTrue("RULE-3" in res.stdout or "RULE-5" in res.stdout)
        self.assertIn("bad_engine.cpp", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 7: engine including adapters headers
    # -------------------------------------------------------------------------
    def test_engine_including_adapters_fails(self) -> None:
        engine_header = self.test_root / "cpp" / "include" / "sublift" / "engine" / "bad_session.hpp"
        engine_header.parent.mkdir(parents=True, exist_ok=True)
        engine_header.write_text(
            "#pragma once\n"
            "#include <sublift/adapters/mock/mock_frame_source.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Checker must fail when engine includes adapters")
        self.assertTrue("RULE-1" in res.stdout or "RULE-5" in res.stdout)
        self.assertIn("bad_session.hpp", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 8: core header including ONNX Runtime
    # -------------------------------------------------------------------------
    def test_core_header_including_onnx_runtime_fails(self) -> None:
        core_header = self.test_root / "cpp" / "include" / "sublift" / "core" / "bad_onnx.hpp"
        core_header.parent.mkdir(parents=True, exist_ok=True)
        core_header.write_text(
            "#pragma once\n"
            "#include <onnxruntime_cxx_api.h>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Checker must fail when core includes onnxruntime")
        self.assertIn("RULE-4", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 9: core implementation including OpenCV
    # -------------------------------------------------------------------------
    def test_core_implementation_including_opencv_fails(self) -> None:
        core_src = self.test_root / "cpp" / "src" / "core" / "bad_core.cpp"
        core_src.parent.mkdir(parents=True, exist_ok=True)
        core_src.write_text(
            "#include <sublift/core/geometry.hpp>\n"
            "#include <opencv2/core.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(
            res.returncode, 0, "Checker must fail when core src file includes opencv2"
        )
        self.assertIn("RULE-4", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 10: adapter including engine
    # -------------------------------------------------------------------------
    def test_adapter_including_engine_fails(self) -> None:
        adapter_src = self.test_root / "cpp" / "adapters" / "mock" / "src" / "bad_adapter.cpp"
        adapter_src.parent.mkdir(parents=True, exist_ok=True)
        adapter_src.write_text(
            "#include <sublift/ports/frame_source.hpp>\n"
            "#include <sublift/engine/session.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Checker must fail when adapter includes engine")
        self.assertIn("RULE-6", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 11: features src privately including OpenCV is ALLOWED
    # -------------------------------------------------------------------------
    def test_features_private_src_including_opencv_passes(self) -> None:
        feat_src = self.test_root / "cpp" / "src" / "features" / "valid_mask.cpp"
        feat_src.parent.mkdir(parents=True, exist_ok=True)
        feat_src.write_text(
            "#include <opencv2/core.hpp>\n"
            "#include <opencv2/imgproc.hpp>\n"
            "#include <sublift/features/mask.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, f"Private OpenCV include in src/features must pass: {res.stdout}")
        self.assertIn("[CLEAN]", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 12: timing including features is ALLOWED
    # -------------------------------------------------------------------------
    def test_timing_including_features_passes(self) -> None:
        timing_src = self.test_root / "cpp" / "src" / "timing" / "valid_timing.cpp"
        timing_src.parent.mkdir(parents=True, exist_ok=True)
        timing_src.write_text(
            "#include <sublift/core/image_view.hpp>\n"
            "#include <sublift/features/dhash.hpp>\n"
            "#include <sublift/features/visual_analysis.hpp>\n"
            "#include <sublift/timing/changepoint.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, f"Timing including features must pass: {res.stdout}")
        self.assertIn("[CLEAN]", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 13: engine including timing and text is ALLOWED
    # -------------------------------------------------------------------------
    def test_engine_orchestrating_timing_and_text_passes(self) -> None:
        engine_src = self.test_root / "cpp" / "src" / "engine" / "valid_orchestration.cpp"
        engine_src.parent.mkdir(parents=True, exist_ok=True)
        engine_src.write_text(
            "#include <sublift/engine/session.hpp>\n"
            "#include <sublift/ports/frame_source.hpp>\n"
            "#include <sublift/timing/changepoint.hpp>\n"
            "#include <sublift/text/dedupe.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, f"Engine coordinating timing and text must pass: {res.stdout}")
        self.assertIn("[CLEAN]", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 14: Commented-out violation does not trigger error
    # -------------------------------------------------------------------------
    def test_commented_out_violation_ignored(self) -> None:
        timing_src = self.test_root / "cpp" / "src" / "timing" / "commented.cpp"
        timing_src.parent.mkdir(parents=True, exist_ok=True)
        timing_src.write_text(
            "// #include <sublift/text/consensus.hpp>\n"
            "/*\n"
            " * #include <sublift/adapters/mock/mock_ocr.hpp>\n"
            " */\n"
            "#include <sublift/timing/segment.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, f"Commented includes must not fail: {res.stdout}")
        self.assertIn("[CLEAN]", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 15: adapter paddle public header including OpenCV fails
    # -------------------------------------------------------------------------
    def test_adapter_paddle_public_header_including_opencv_fails(self) -> None:
        paddle_header = (
            self.test_root
            / "cpp"
            / "adapters"
            / "paddle"
            / "include"
            / "sublift"
            / "adapters"
            / "paddle"
            / "paddle_ocr.hpp"
        )
        paddle_header.parent.mkdir(parents=True, exist_ok=True)
        paddle_header.write_text(
            "#pragma once\n"
            "#include <opencv2/core.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Public adapter header including OpenCV must fail")
        self.assertIn("RULE-4", res.stdout)
        self.assertIn("Zero 3rd-party types may leak into public contracts", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 16: adapter paddle public header including ONNX Runtime fails
    # -------------------------------------------------------------------------
    def test_adapter_paddle_public_header_including_onnx_fails(self) -> None:
        paddle_header = (
            self.test_root
            / "cpp"
            / "adapters"
            / "paddle"
            / "include"
            / "sublift"
            / "adapters"
            / "paddle"
            / "paddle_ocr.hpp"
        )
        paddle_header.parent.mkdir(parents=True, exist_ok=True)
        paddle_header.write_text(
            "#pragma once\n"
            "#include <onnxruntime_cxx_api.h>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Public adapter header including ONNX Runtime must fail")
        self.assertIn("RULE-4", res.stdout)
        self.assertIn("Zero 3rd-party types may leak into public contracts", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 17: adapter ffmpeg public header including FFmpeg fails
    # -------------------------------------------------------------------------
    def test_adapter_ffmpeg_public_header_including_ffmpeg_fails(self) -> None:
        ffmpeg_header = (
            self.test_root
            / "cpp"
            / "adapters"
            / "ffmpeg"
            / "include"
            / "sublift"
            / "adapters"
            / "ffmpeg"
            / "ffmpeg_frame_source.hpp"
        )
        ffmpeg_header.parent.mkdir(parents=True, exist_ok=True)
        ffmpeg_header.write_text(
            "#pragma once\n"
            "#include <libavcodec/avcodec.h>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Public adapter header including FFmpeg must fail")
        self.assertIn("RULE-4", res.stdout)
        self.assertIn("Zero 3rd-party types may leak into public contracts", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 18: adapter ffmpeg src including FFmpeg C header fails
    # -------------------------------------------------------------------------
    def test_adapter_ffmpeg_src_including_ffmpeg_fails(self) -> None:
        ffmpeg_src = self.test_root / "cpp" / "adapters" / "ffmpeg" / "src" / "ffmpeg_frame_source.cpp"
        ffmpeg_src.parent.mkdir(parents=True, exist_ok=True)
        ffmpeg_src.write_text(
            "#include <libavformat/avformat.h>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "FFmpeg src including libav* must fail")
        self.assertIn("RULE-4", res.stdout)
        self.assertIn("prohibits libav* C library header inclusion", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 19: adapter ffmpeg src including OpenCV fails
    # -------------------------------------------------------------------------
    def test_adapter_ffmpeg_src_including_opencv_fails(self) -> None:
        ffmpeg_src = self.test_root / "cpp" / "adapters" / "ffmpeg" / "src" / "probe.cpp"
        ffmpeg_src.parent.mkdir(parents=True, exist_ok=True)
        ffmpeg_src.write_text(
            "#include <opencv2/core.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "FFmpeg src including OpenCV must fail")
        self.assertIn("RULE-4", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 20: adapter ffmpeg src including ONNX Runtime fails
    # -------------------------------------------------------------------------
    def test_adapter_ffmpeg_src_including_onnx_fails(self) -> None:
        ffmpeg_src = self.test_root / "cpp" / "adapters" / "ffmpeg" / "src" / "probe.cpp"
        ffmpeg_src.parent.mkdir(parents=True, exist_ok=True)
        ffmpeg_src.write_text(
            "#include <onnxruntime_cxx_api.h>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "FFmpeg src including ONNX Runtime must fail")
        self.assertIn("RULE-4", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 21: features src including ONNX Runtime fails
    # -------------------------------------------------------------------------
    def test_features_src_including_onnx_fails(self) -> None:
        feat_src = self.test_root / "cpp" / "src" / "features" / "bad_onnx_feat.cpp"
        feat_src.parent.mkdir(parents=True, exist_ok=True)
        feat_src.write_text(
            "#include <onnxruntime_cxx_api.h>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Features src including ONNX Runtime must fail")
        self.assertIn("RULE-4", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 22: adapter paddle src privately including OpenCV & ONNX Runtime passes
    # -------------------------------------------------------------------------
    def test_adapter_paddle_src_including_opencv_and_onnx_passes(self) -> None:
        paddle_src = self.test_root / "cpp" / "adapters" / "paddle" / "src" / "paddle_ocr.cpp"
        paddle_src.parent.mkdir(parents=True, exist_ok=True)
        paddle_src.write_text(
            "#include <sublift/core/frame.hpp>\n"
            "#include <opencv2/core.hpp>\n"
            "#include <opencv2/imgproc.hpp>\n"
            "#include <onnxruntime_cxx_api.h>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, f"Private OpenCV/ONNX includes in adapters/paddle/src must pass: {res.stdout}")
        self.assertIn("[CLEAN]", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 23: adapter paddle private header in src/ passing
    # -------------------------------------------------------------------------
    def test_adapter_paddle_private_header_in_src_passes(self) -> None:
        paddle_priv_hdr = self.test_root / "cpp" / "adapters" / "paddle" / "src" / "ppocr_ctc.hpp"
        paddle_priv_hdr.parent.mkdir(parents=True, exist_ok=True)
        paddle_priv_hdr.write_text(
            "#pragma once\n"
            "#include <onnxruntime_cxx_api.h>\n"
            "#include <opencv2/core.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, f"Private header in adapters/paddle/src must pass: {res.stdout}")
        self.assertIn("[CLEAN]", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 25: Public header including nlohmann JSON fails
    # -------------------------------------------------------------------------
    def test_public_header_including_nlohmann_fails(self) -> None:
        core_hdr = self.test_root / "cpp" / "include" / "sublift" / "core" / "bad_json.hpp"
        core_hdr.parent.mkdir(parents=True, exist_ok=True)
        core_hdr.write_text(
            "#pragma once\n"
            "#include <nlohmann/json.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Public header including nlohmann must fail")
        self.assertIn("RULE-4", res.stdout)
        self.assertIn("nlohmann JSON header", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 26: Adapter public header including nlohmann JSON fails
    # -------------------------------------------------------------------------
    def test_adapter_public_header_including_nlohmann_fails(self) -> None:
        adapter_hdr = (
            self.test_root
            / "cpp"
            / "adapters"
            / "ffmpeg"
            / "include"
            / "sublift"
            / "adapters"
            / "ffmpeg"
            / "bad_ffmpeg_header.hpp"
        )
        adapter_hdr.parent.mkdir(parents=True, exist_ok=True)
        adapter_hdr.write_text(
            "#pragma once\n"
            "#include <nlohmann/json_fwd.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Adapter public header including nlohmann must fail")
        self.assertIn("RULE-4", res.stdout)
        self.assertIn("nlohmann JSON header", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 27: Pure layer source including nlohmann JSON fails
    # -------------------------------------------------------------------------
    def test_pure_layer_src_including_nlohmann_fails(self) -> None:
        pure_src = self.test_root / "cpp" / "src" / "timing" / "bad_timing_json.cpp"
        pure_src.parent.mkdir(parents=True, exist_ok=True)
        pure_src.write_text(
            "#include <sublift/core/frame.hpp>\n"
            "#include <nlohmann/json.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Pure layer src including nlohmann must fail")
        self.assertIn("RULE-4", res.stdout)
        self.assertIn("nlohmann JSON header", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 28: Adapter src privately including nlohmann JSON passes
    # -------------------------------------------------------------------------
    def test_adapter_src_privately_including_nlohmann_passes(self) -> None:
        ffmpeg_src = self.test_root / "cpp" / "adapters" / "ffmpeg" / "src" / "good_ffmpeg.cpp"
        ffmpeg_src.parent.mkdir(parents=True, exist_ok=True)
        ffmpeg_src.write_text(
            "#include <sublift/core/frame.hpp>\n"
            "#include <nlohmann/json.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, f"Private nlohmann include in adapter src must pass: {res.stdout}")
        self.assertIn("[CLEAN]", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 29: Adapter opening root namespace sublift fails (RULE-9)
    # -------------------------------------------------------------------------
    def test_adapter_opening_root_sublift_namespace_fails(self) -> None:
        adapter_hdr = (
            self.test_root
            / "cpp"
            / "adapters"
            / "ffmpeg"
            / "include"
            / "sublift"
            / "adapters"
            / "ffmpeg"
            / "bad_ns.hpp"
        )
        adapter_hdr.parent.mkdir(parents=True, exist_ok=True)
        adapter_hdr.write_text(
            "#pragma once\n"
            "namespace sublift {\n"
            "struct BadLeak {};\n"
            "}\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Adapter opening root sublift must fail")
        self.assertIn("RULE-9", res.stdout)
        self.assertIn("Namespace Ownership", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 30: Adapter declaring foreign/dead namespace fails (RULE-9)
    # -------------------------------------------------------------------------
    def test_adapter_declaring_foreign_namespace_fails(self) -> None:
        adapter_hdr = (
            self.test_root
            / "cpp"
            / "adapters"
            / "ffmpeg"
            / "include"
            / "sublift"
            / "adapters"
            / "ffmpeg"
            / "bad_server_ns.hpp"
        )
        adapter_hdr.parent.mkdir(parents=True, exist_ok=True)
        adapter_hdr.write_text(
            "#pragma once\n"
            "namespace sublift::server {\n"
            "struct StaleAlias {};\n"
            "}\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Adapter declaring sublift::server must fail")
        self.assertIn("RULE-9", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 31: timing header declaring core or bare sublift fails (RULE-9)
    # -------------------------------------------------------------------------
    def test_timing_header_declaring_core_or_root_sublift_fails(self) -> None:
        timing_hdr = self.test_root / "cpp" / "include" / "sublift" / "timing" / "bad_ns.hpp"
        timing_hdr.parent.mkdir(parents=True, exist_ok=True)
        timing_hdr.write_text(
            "#pragma once\n"
            "namespace sublift {\n"
            "namespace core {}\n"
            "}\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Timing header declaring sublift/core must fail")
        self.assertIn("RULE-9", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 32: core header declaring bare detail namespace fails (RULE-9)
    # -------------------------------------------------------------------------
    def test_core_header_declaring_bare_detail_fails(self) -> None:
        core_hdr = self.test_root / "cpp" / "include" / "sublift" / "core" / "bad_detail.hpp"
        core_hdr.parent.mkdir(parents=True, exist_ok=True)
        core_hdr.write_text(
            "#pragma once\n"
            "namespace detail {\n"
            "inline int helper() { return 0; }\n"
            "}\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Core header declaring bare detail must fail")
        self.assertIn("RULE-9", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 33: adapter paddle canonical namespaces pass (RULE-9)
    # -------------------------------------------------------------------------
    def test_adapter_paddle_canonical_namespaces_pass(self) -> None:
        paddle_hdr = (
            self.test_root
            / "cpp"
            / "adapters"
            / "paddle"
            / "include"
            / "sublift"
            / "adapters"
            / "paddle"
            / "good_paddle.hpp"
        )
        paddle_hdr.parent.mkdir(parents=True, exist_ok=True)
        paddle_hdr.write_text(
            "#pragma once\n"
            "namespace sublift::adapters::paddle {\n"
            "struct GoodPaddle {};\n"
            "}\n"
        )

        paddle_src = self.test_root / "cpp" / "adapters" / "paddle" / "src" / "good_paddle.cpp"
        paddle_src.parent.mkdir(parents=True, exist_ok=True)
        paddle_src.write_text(
            "namespace sublift::adapters::paddle::detail {\n"
            "int internal_helper() { return 1; }\n"
            "}\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, f"Canonical adapter namespaces must pass: {res.stdout}")
        self.assertIn("[CLEAN]", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 34: Actual repository cpp/ passes with 0 violations
    # -------------------------------------------------------------------------
    def test_actual_project_passes(self) -> None:
        res = self.run_checker_cli(PROJECT_ROOT)
        self.assertEqual(res.returncode, 0, f"Actual project check failed:\n{res.stdout}\n{res.stderr}")
        self.assertIn("[CLEAN]", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 35: apple_vision .mm private implementation passes
    # -------------------------------------------------------------------------
    def test_apple_vision_mm_private_src_passes(self) -> None:
        mm_file = (
            self.test_root
            / "cpp"
            / "adapters"
            / "apple_vision"
            / "src"
            / "apple_vision_ocr.mm"
        )
        mm_file.parent.mkdir(parents=True, exist_ok=True)
        mm_file.write_text(
            "#import <Vision/Vision.h>\n"
            "#include <CoreGraphics/CoreGraphics.h>\n"
            "#include <Foundation/Foundation.h>\n"
            "#include <sublift/ports/ocr_engine.hpp>\n"
            "#include <sublift/core/image_view.hpp>\n"
            "@interface SubLiftVisionHelper : NSObject\n"
            "@end\n"
            "namespace sublift::adapters::apple_vision::detail {\n"
            "int helper() { return 1; }\n"
            "}\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, f"Private .mm in apple_vision/src must pass:\n{res.stdout}")
        self.assertIn("[CLEAN]", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 36: apple_vision .mm including engine or features fails
    # -------------------------------------------------------------------------
    def test_apple_vision_mm_including_engine_or_features_fails(self) -> None:
        mm_file = (
            self.test_root
            / "cpp"
            / "adapters"
            / "apple_vision"
            / "src"
            / "bad_vision.mm"
        )
        mm_file.parent.mkdir(parents=True, exist_ok=True)
        mm_file.write_text(
            "#import <sublift/engine/session.hpp>\n"
            "#include <sublift/features/mask.hpp>\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, ".mm file bypassing RULE-6/RULE-3 must fail")
        self.assertIn("RULE-6", res.stdout)
        self.assertIn("RULE-3", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 37: apple_vision public header including Apple frameworks fails
    # -------------------------------------------------------------------------
    def test_apple_vision_public_header_including_apple_frameworks_fails(self) -> None:
        hdr = (
            self.test_root
            / "cpp"
            / "adapters"
            / "apple_vision"
            / "include"
            / "sublift"
            / "adapters"
            / "apple_vision"
            / "apple_vision_ocr.hpp"
        )
        hdr.parent.mkdir(parents=True, exist_ok=True)
        for fw in (
            "<Vision/Vision.h>",
            "<CoreGraphics/CoreGraphics.h>",
            "<CoreVideo/CoreVideo.h>",
            "<Foundation/Foundation.h>",
            "<AppKit/AppKit.h>",
            "<Cocoa/Cocoa.h>",
        ):
            hdr.write_text(f"#pragma once\n#include {fw}\n")
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"Public header including {fw} must fail")
            self.assertIn("RULE-4", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 38: public headers with ObjC syntax or #import fail
    # -------------------------------------------------------------------------
    def test_public_header_with_objc_syntax_or_import_fails(self) -> None:
        hdr = (
            self.test_root
            / "cpp"
            / "adapters"
            / "apple_vision"
            / "include"
            / "sublift"
            / "adapters"
            / "apple_vision"
            / "bad_objc.hpp"
        )
        hdr.parent.mkdir(parents=True, exist_ok=True)
        for snippet in ("#import <string>\n", "@interface Foo : NSObject\n@end\n", "@protocol Bar\n@end\n"):
            hdr.write_text("#pragma once\n" + snippet)
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"Public header with {snippet!r} must fail")
            self.assertIn("RULE-4", res.stdout)

        core_hdr = self.test_root / "cpp" / "include" / "sublift" / "core" / "bad_objc.hpp"
        core_hdr.parent.mkdir(parents=True, exist_ok=True)
        core_hdr.write_text("#pragma once\n@interface CoreLeak\n@end\n")
        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("RULE-4", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 39: commented-out ObjC / Apple framework in public header passes
    # -------------------------------------------------------------------------
    def test_commented_objc_and_apple_framework_in_public_header_passes(self) -> None:
        hdr = (
            self.test_root
            / "cpp"
            / "adapters"
            / "apple_vision"
            / "include"
            / "sublift"
            / "adapters"
            / "apple_vision"
            / "clean_vision.hpp"
        )
        hdr.parent.mkdir(parents=True, exist_ok=True)
        hdr.write_text(
            "#pragma once\n"
            "// #import <Vision/Vision.h>\n"
            "/* @interface NotActive */\n"
            "#include <string>\n"
            "namespace sublift::adapters::apple_vision {\n"
            "constexpr const char* kMarker = \"@interface in string\";\n"
            "}\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, f"Commented/string ObjC tokens must pass:\n{res.stdout}")

    # -------------------------------------------------------------------------
    # Negative Test 40: non-apple_vision files including Apple frameworks fail
    # -------------------------------------------------------------------------
    def test_non_apple_vision_including_apple_framework_fails(self) -> None:
        for rel_path in (
            Path("cpp/src/engine/bad.cpp"),
            Path("cpp/adapters/paddle/src/bad.cpp"),
            Path("tools/worker/bad.cpp"),
        ):
            sub_root = Path(self.temp_dir.name) / rel_path.parts[0]
            if sub_root.exists():
                shutil.rmtree(sub_root)
            f = self.test_root / rel_path
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text("#include <Vision/Vision.h>\n")
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"{rel_path} including Vision must fail")
            self.assertIn("RULE-4", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 41: tools/worker valid includes & namespaces pass
    # -------------------------------------------------------------------------
    def test_worker_valid_includes_and_namespace_pass(self) -> None:
        proto = self.test_root / "tools" / "worker" / "protocol.hpp"
        proto.parent.mkdir(parents=True, exist_ok=True)
        proto.write_text(
            "#pragma once\n"
            "#include <nlohmann/json.hpp>\n"
            "#include <sublift/core/subtitle.hpp>\n"
            "namespace sublift::worker {\n"
            "struct Request {};\n"
            "}\n"
        )
        main_cpp = self.test_root / "tools" / "worker" / "main.cpp"
        main_cpp.write_text(
            "#include \"protocol.hpp\"\n"
            "#include <sublift/ports/ocr_engine.hpp>\n"
            "#include <sublift/timing/segment.hpp>\n"
            "#include <sublift/text/srt.hpp>\n"
            "#include <sublift/engine/session.hpp>\n"
            "#include <sublift/adapters/apple_vision/apple_vision_ocr.hpp>\n"
            "#include <sublift/adapters/paddle/paddle_ocr.hpp>\n"
            "#include <sublift/adapters/ffmpeg/ffmpeg_frame_source.hpp>\n"
            "namespace sublift::worker::detail {\n"
            "int run() { return 0; }\n"
            "}\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, f"Valid tools/worker files must pass:\n{res.stdout}")

    # -------------------------------------------------------------------------
    # Negative Test 42: tools/worker socket headers fail (RULE-10)
    # -------------------------------------------------------------------------
    def test_worker_socket_headers_fail(self) -> None:
        worker_cpp = self.test_root / "tools" / "worker" / "net.cpp"
        worker_cpp.parent.mkdir(parents=True, exist_ok=True)
        for sock_hdr in (
            "<sys/socket.h>",
            "<netinet/in.h>",
            "<arpa/inet.h>",
            "<sys/un.h>",
            "<winsock2.h>",
            "<ws2tcpip.h>",
            "<asio.hpp>",
            "<boost/asio/io_context.hpp>",
        ):
            worker_cpp.write_text(f"#include {sock_hdr}\n")
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"worker including {sock_hdr} must fail")
            self.assertIn("RULE-10", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 43: tools/worker forbidden private/3rd-party/cli headers fail
    # -------------------------------------------------------------------------
    def test_worker_forbidden_headers_fail(self) -> None:
        worker_cpp = self.test_root / "tools" / "worker" / "bad.cpp"
        worker_cpp.parent.mkdir(parents=True, exist_ok=True)

        cases = [
            ("#include <sublift/features/mask.hpp>\n", "RULE-3"),
            ("#include <opencv2/core.hpp>\n", "RULE-4"),
            ("#include <onnxruntime_cxx_api.h>\n", "RULE-4"),
            ("#include <libavcodec/avcodec.h>\n", "RULE-4"),
            ("#include <sublift/adapters/paddle/src/ppocr_ctc.hpp>\n", "RULE-10"),
            ("#include \"../cli/main.cpp\"\n", "RULE-10"),
        ]
        for content, expected_rule in cases:
            worker_cpp.write_text(content)
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"worker with {content!r} must fail")
            self.assertIn(expected_rule, res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 44: cpp/ reverse-depending on tools/ fails (RULE-10)
    # -------------------------------------------------------------------------
    def test_cpp_reverse_dependency_on_tools_fails(self) -> None:
        bad_cpp = self.test_root / "cpp" / "src" / "engine" / "bad.cpp"
        bad_cpp.parent.mkdir(parents=True, exist_ok=True)
        bad_cpp.write_text("#include \"../../../tools/worker/protocol.hpp\"\n")
        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Relative include into tools/ must fail")
        self.assertIn("RULE-10", res.stdout)

        bad_hdr = self.test_root / "cpp" / "include" / "sublift" / "core" / "bad.hpp"
        bad_hdr.parent.mkdir(parents=True, exist_ok=True)
        bad_hdr.write_text("#pragma once\n#include \"tools/worker/protocol.hpp\"\n")
        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Direct tools/ include from cpp/ must fail")
        self.assertIn("RULE-10", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 45: root with cpp/ and no desktop/src/ skips TS gracefully
    # -------------------------------------------------------------------------
    def test_missing_desktop_src_skips_gracefully(self) -> None:
        core_hdr = self.test_root / "cpp" / "include" / "sublift" / "core" / "ok.hpp"
        core_hdr.parent.mkdir(parents=True, exist_ok=True)
        core_hdr.write_text("#pragma once\nnamespace sublift { struct Ok {}; }\n")
        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, res.stdout)

    # -------------------------------------------------------------------------
    # Negative Test 46: desktop/src/core/ purity violations fail (RULE-TS-1)
    # -------------------------------------------------------------------------
    def test_desktop_core_purity_violations_fail(self) -> None:
        roi_ts = self.test_root / "desktop" / "src" / "core" / "roi.ts"
        roi_ts.parent.mkdir(parents=True, exist_ok=True)

        bad_cases = [
            "import React from 'react';\n",
            "import { create } from 'zustand';\n",
            "import { invoke } from '@tauri-apps/api/core';\n",
            "import { useStore } from '../store/workbenchStore';\n",
            "import App from '../ui/App';\n",
            "import {\n  a,\n  b,\n} from '@/store/workbenchStore';\n",
            "export const w = window.innerWidth;\n",
            "export const el = document.getElementById('x');\n",
        ]
        for code in bad_cases:
            roi_ts.write_text(code)
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"core/ with {code!r} must fail")
            self.assertIn("RULE-TS-1", res.stdout)

    # -------------------------------------------------------------------------
    # Positive Test 47: desktop/src/core/ pure TS passes
    # -------------------------------------------------------------------------
    def test_desktop_core_pure_passes(self) -> None:
        time_ts = self.test_root / "desktop" / "src" / "core" / "time.ts"
        time_ts.parent.mkdir(parents=True, exist_ok=True)
        time_ts.write_text("export function fmt(ms: number): string { return String(ms); }\n")
        roi_ts = self.test_root / "desktop" / "src" / "core" / "roi.ts"
        roi_ts.write_text(
            "// window.innerWidth in comment is ignored\n"
            "import { fmt } from './time';\n"
            "const note = 'document.getElementById in string';\n"
            "export function label(ms: number): string { return fmt(ms) + note; }\n"
        )

        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, f"Pure desktop/src/core must pass:\n{res.stdout}")

    # -------------------------------------------------------------------------
    # Negative & Positive Tests 48-50: desktop/src/{store,ui,styles}/
    # -------------------------------------------------------------------------
    def test_desktop_store_ui_styles_layering(self) -> None:
        core_ts = self.test_root / "desktop" / "src" / "core" / "roi.ts"
        core_ts.parent.mkdir(parents=True, exist_ok=True)
        core_ts.write_text("export const x = 1;\n")

        store_ts = self.test_root / "desktop" / "src" / "store" / "workbenchStore.ts"
        store_ts.parent.mkdir(parents=True, exist_ok=True)
        for bad_store in (
            "import React from 'react';\n",
            "import { createRoot } from 'react-dom/client';\n",
            "import App from '../ui/App';\n",
        ):
            store_ts.write_text(bad_store)
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"store/ with {bad_store!r} must fail")
            self.assertIn("RULE-TS-2", res.stdout)

        # Clean store + ui + css
        store_ts.write_text(
            "import { create } from 'zustand';\n"
            "import { x } from '../core/roi';\n"
            "export const useWb = () => ({ create, x });\n"
        )
        ui_tsx = self.test_root / "desktop" / "src" / "ui" / "App.tsx"
        ui_tsx.parent.mkdir(parents=True, exist_ok=True)
        ui_tsx.write_text(
            "import React from 'react';\n"
            "import { invoke } from '@tauri-apps/api/core';\n"
            "import { useWb } from '../store/workbenchStore';\n"
            "import { x } from '../core/roi';\n"
            "import '../styles/app.css';\n"
            "export const App = () => ({ React, invoke, useWb, x });\n"
        )
        css = self.test_root / "desktop" / "src" / "styles" / "app.css"
        css.parent.mkdir(parents=True, exist_ok=True)
        css.write_text("@import \"tailwindcss\";\n")

        res = self.run_checker_cli(self.test_root)
        self.assertEqual(res.returncode, 0, f"Valid desktop/src layers must pass:\n{res.stdout}")

        # Negative ui escaping desktop/src
        ui_tsx.write_text("import bad from '../../src-tauri/src/lib';\n")
        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("RULE-TS-3", res.stdout)
        ui_tsx.write_text("export const ok = 1;\n")

        # Negative non-css file in styles/
        bad_style = self.test_root / "desktop" / "src" / "styles" / "helper.ts"
        bad_style.write_text("export const c = '#000';\n")
        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("RULE-TS-4", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Tests 51-52: Syntax edge hardening (space-less TS import, 2-arg import(), C++ line splicing)
    # -------------------------------------------------------------------------
    def test_ts_spaceless_and_two_arg_dynamic_import_hardening(self) -> None:
        core_ts = self.test_root / "desktop" / "src" / "core" / "edge.ts"
        core_ts.parent.mkdir(parents=True, exist_ok=True)
        for bad_code in (
            "import{foo}from'../store/x';\n",
            "export{foo}from'../store/x';\n",
            "import*as s from'../store/x';\n",
            "export const m = import('../store/x', { with: { type: 'json' } });\n",
        ):
            core_ts.write_text(bad_code)
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"Must catch edge import syntax: {bad_code!r}")
            self.assertIn("RULE-TS-1", res.stdout)

    def test_cpp_backslash_line_continuation_include_hardening(self) -> None:
        hdr = (
            self.test_root
            / "cpp"
            / "adapters"
            / "apple_vision"
            / "include"
            / "sublift"
            / "adapters"
            / "apple_vision"
            / "bad_spliced.hpp"
        )
        hdr.parent.mkdir(parents=True, exist_ok=True)
        hdr.write_text(
            "#pragma once\n"
            "#include \\\n"
            "<Vision/Vision.h>\n"
        )
        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Backslash-continued #include <Vision/Vision.h> must fail")
        self.assertIn("RULE-4", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Tests 53-55: [High-5] TS Node built-ins, store UI packages, bare DOM/BOM
    # -------------------------------------------------------------------------
    def test_ts_node_builtins_subpaths_fail_across_layers(self) -> None:
        ui_tsx = self.test_root / "desktop" / "src" / "ui" / "BadNode.tsx"
        ui_tsx.parent.mkdir(parents=True, exist_ok=True)
        for bad_import in (
            "import fs from 'fs/promises';\n",
            "import path from 'path/posix';\n",
            "import { spawn } from 'node:child_process';\n",
        ):
            ui_tsx.write_text(bad_import)
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"ui/ importing {bad_import!r} must fail")
            self.assertIn("RULE-TS-3", res.stdout)
        ui_tsx.unlink()

        store_ts = self.test_root / "desktop" / "src" / "store" / "badNode.ts"
        store_ts.parent.mkdir(parents=True, exist_ok=True)
        for bad_import in (
            "import fs from 'node:fs';\n",
            "import fsp from 'fs/promises';\n",
        ):
            store_ts.write_text(bad_import)
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"store/ importing {bad_import!r} must fail")
            self.assertIn("RULE-TS-2", res.stdout)
        store_ts.unlink()

        core_ts = self.test_root / "desktop" / "src" / "core" / "badNode.ts"
        core_ts.parent.mkdir(parents=True, exist_ok=True)
        for bad_import in (
            "import fsp from 'fs/promises';\n",
            "import path from 'node:path';\n",
        ):
            core_ts.write_text(bad_import)
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"core/ importing {bad_import!r} must fail")
            self.assertIn("RULE-TS-1", res.stdout)

    def test_ts_store_ui_packages_and_escapes_fail(self) -> None:
        store_ts = self.test_root / "desktop" / "src" / "store" / "badUi.ts"
        store_ts.parent.mkdir(parents=True, exist_ok=True)
        for bad_code in (
            "import { Play } from 'lucide-react';\n",
            "import { useVirtualizer } from '@tanstack/react-virtual';\n",
            "import * as Dialog from '@radix-ui/react-dialog';\n",
            "import '../styles/app.css';\n",
            "import App from '@/ui/App';\n",
            "export const loadApp = () => import('../ui/App');\n",
            "import x from '../../src-tauri/x';\n",
        ):
            store_ts.write_text(bad_code)
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"store/ with {bad_code!r} must fail")
            self.assertIn("RULE-TS-2", res.stdout)

    def test_ts_core_bare_dom_and_globalthis_fail(self) -> None:
        core_ts = self.test_root / "desktop" / "src" / "core" / "badDom.ts"
        core_ts.parent.mkdir(parents=True, exist_ok=True)
        for bad_code in (
            "export const w = (window as unknown);\n",
            "export const hasDoc = typeof document !== 'undefined';\n",
            "export const nav = navigator;\n",
            "export const g = globalThis;\n",
        ):
            core_ts.write_text(bad_code)
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"core/ with {bad_code!r} must fail")
            self.assertIn("RULE-TS-1", res.stdout)

    # -------------------------------------------------------------------------
    # Negative Tests 56-59: [Low-6] Worker/Adapter namespace, ObjC tokens, relative private include
    # -------------------------------------------------------------------------
    def test_worker_foreign_namespace_fails(self) -> None:
        worker_cpp = self.test_root / "tools" / "worker" / "bad_ns.cpp"
        worker_cpp.parent.mkdir(parents=True, exist_ok=True)
        for ns in ("sublift::core", "sublift::server"):
            worker_cpp.write_text(f"namespace {ns} {{\nstruct Bad {{}};\n}}\n")
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"worker declaring {ns} must fail")
            self.assertIn("RULE-9", res.stdout)

    def test_apple_vision_public_header_detail_namespace_fails(self) -> None:
        hdr = (
            self.test_root
            / "cpp"
            / "adapters"
            / "apple_vision"
            / "include"
            / "sublift"
            / "adapters"
            / "apple_vision"
            / "bad_detail.hpp"
        )
        hdr.parent.mkdir(parents=True, exist_ok=True)
        hdr.write_text(
            "#pragma once\n"
            "namespace sublift::adapters::apple_vision::detail {\n"
            "struct Leak {};\n"
            "}\n"
        )
        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "Public header using ::detail must fail")
        self.assertIn("RULE-9", res.stdout)

    def test_adapter_public_header_objc_property_class_implementation_fail(self) -> None:
        hdr = (
            self.test_root
            / "cpp"
            / "adapters"
            / "apple_vision"
            / "include"
            / "sublift"
            / "adapters"
            / "apple_vision"
            / "bad_objc_tokens.hpp"
        )
        hdr.parent.mkdir(parents=True, exist_ok=True)
        for snippet in (
            "@class NSString;\n",
            "@property (nonatomic, copy) id item;\n",
            "@implementation Foo\n@end\n",
        ):
            hdr.write_text("#pragma once\n" + snippet)
            res = self.run_checker_cli(self.test_root)
            self.assertNotEqual(res.returncode, 0, f"Public header with {snippet!r} must fail")
            self.assertIn("RULE-4", res.stdout)

    def test_worker_relative_include_to_apple_vision_private_src_fails(self) -> None:
        worker_cpp = self.test_root / "tools" / "worker" / "bad_rel.cpp"
        worker_cpp.parent.mkdir(parents=True, exist_ok=True)
        worker_cpp.write_text(
            '#include "../../cpp/adapters/apple_vision/src/vision_ocr.mm"\n'
        )
        res = self.run_checker_cli(self.test_root)
        self.assertNotEqual(res.returncode, 0, "worker relative include into adapter src must fail")
        self.assertIn("RULE-10", res.stdout)


if __name__ == "__main__":
    unittest.main()
