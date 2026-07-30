"""Freeze or verify real Python/C++ Paddle stage traces.

Default ``--check`` is offline and validates the committed envelope plus
fixture/model fingerprints without loading OCR models. ``--runtime`` performs
the explicit live dual-runtime replay required by Phase 6.8.
"""

from __future__ import annotations

import argparse
import importlib.metadata
import json
import subprocess
import sys
from pathlib import Path
from typing import Any, cast

from PIL import Image

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.parity.paddle_stage_trace import (  # noqa: E402
    capture_cpp_trace,
    capture_python_trace,
    normalize_trace_for_golden,
    sha256_file,
)

SCHEMA_VERSION = 2
FIXTURES_DIR = REPO_ROOT / "benchmark" / "parity" / "fixtures" / "paddle"
FIXTURE_MANIFEST = FIXTURES_DIR / "manifest.json"
FREEZE_MANIFEST = REPO_ROOT / "scripts" / "parity" / "freeze_paddle_manifest.json"
GOLDEN_PATH = (
    REPO_ROOT
    / "benchmark"
    / "parity"
    / "goldens"
    / "paddle"
    / "paddle_stages.v2.json"
)


def _git_head() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def _runtime_versions() -> dict[str, str]:
    import cv2
    import onnxruntime  # type: ignore[import-untyped]

    capi_dir = Path(onnxruntime.__file__).resolve().parent / "capi"
    runtime_libraries = sorted(
        {
            *capi_dir.glob("libonnxruntime.*.dylib"),
            *capi_dir.glob("libonnxruntime.so*"),
            *capi_dir.glob("onnxruntime.dll"),
        }
    )
    runtime_library_sha256 = (
        sha256_file(runtime_libraries[0]) if runtime_libraries else "unavailable"
    )

    return {
        "python": sys.version.split()[0],
        "rapidocr": importlib.metadata.version("rapidocr"),
        "onnxruntime": importlib.metadata.version("onnxruntime"),
        "opencv": cv2.__version__,
        "pillow": importlib.metadata.version("pillow"),
        "numpy": importlib.metadata.version("numpy"),
        "onnxruntime_library_sha256": runtime_library_sha256,
    }


def _load_fixture_manifest() -> dict[str, Any]:
    data = cast(
        dict[str, Any],
        json.loads(FIXTURE_MANIFEST.read_text(encoding="utf-8")),
    )
    fixtures = data.get("fixtures")
    if not isinstance(fixtures, list) or not fixtures:
        raise RuntimeError("Paddle fixture manifest must contain a non-empty fixtures list")
    for fixture in fixtures:
        asset = FIXTURES_DIR / str(fixture["name"])
        if not asset.is_file():
            raise RuntimeError(f"missing Paddle fixture: {asset}")
        actual = sha256_file(asset)
        expected = fixture.get("sha256")
        if expected is not None and expected != actual:
            raise RuntimeError(
                f"fixture hash mismatch for {asset.name}: {actual} != {expected}"
            )
        fixture["sha256"] = actual
    return data


def _validate_freeze_manifest(data: dict[str, Any]) -> None:
    required_top = {
        "schema_version",
        "oracle_commit",
        "dependencies",
        "model",
        "parameters",
    }
    missing = sorted(required_top.difference(data))
    if missing:
        raise RuntimeError(f"freeze manifest missing keys: {missing}")
    model = data["model"]
    for key in (
        "type",
        "det_file",
        "det_sha256",
        "cls_file",
        "cls_sha256",
        "rec_file",
        "rec_sha256",
        "dictionary_file",
        "dictionary_sha256",
    ):
        if key not in model:
            raise RuntimeError(f"freeze manifest model missing {key}")


def _validate_live_model_files(
    freeze: dict[str, Any],
    model_root: Path,
) -> None:
    model = freeze["model"]
    pairs = (
        ("det_file", "det_sha256"),
        ("cls_file", "cls_sha256"),
        ("rec_file", "rec_sha256"),
        ("dictionary_file", "dictionary_sha256"),
    )
    for file_key, hash_key in pairs:
        path = model_root / str(model[file_key])
        if not path.is_file():
            raise RuntimeError(f"frozen Paddle model file is missing: {path}")
        actual = sha256_file(path)
        expected = str(model[hash_key])
        if actual != expected:
            raise RuntimeError(
                f"frozen Paddle model hash mismatch for {path.name}: "
                f"{actual} != {expected}"
            )


def build_live_report(
    *,
    raw_dir: Path | None = None,
    report_with_timings: bool = False,
) -> dict[str, Any]:
    freeze = json.loads(FREEZE_MANIFEST.read_text(encoding="utf-8"))
    _validate_freeze_manifest(freeze)
    fixtures = _load_fixture_manifest()
    model_root = Path(str(freeze["model"]["root"])).expanduser()
    model_type = str(freeze["model"]["type"])
    _validate_live_model_files(freeze, model_root)

    cases: list[dict[str, Any]] = []
    for fixture in fixtures["fixtures"]:
        asset = FIXTURES_DIR / str(fixture["name"])
        case_id = asset.stem
        with Image.open(asset) as opened:
            image = opened.convert("RGB")
            case_raw = raw_dir / case_id if raw_dir is not None else None
            oracle_raw = case_raw / "python" if case_raw is not None else None
            candidate_raw = case_raw / "cpp" if case_raw is not None else None
            oracle = capture_python_trace(
                image,
                model_type=model_type,
                model_root=model_root,
                raw_dir=oracle_raw,
            )
            candidate = capture_cpp_trace(
                image,
                repo_root=REPO_ROOT,
                model_type=model_type,
                model_root=model_root,
                raw_dir=candidate_raw,
            )
        if not report_with_timings:
            oracle = normalize_trace_for_golden(oracle)
            candidate = normalize_trace_for_golden(candidate)
        cases.append(
            {
                "case_id": case_id,
                "asset": asset.name,
                "asset_sha256": fixture["sha256"],
                "expected_text": fixture.get("expected_text", []),
                "oracle": oracle,
                "candidate": candidate,
            }
        )

    report = {
        "schema_version": SCHEMA_VERSION,
        "kind": "paddle_stage_parity",
        "oracle_commit": freeze["oracle_commit"],
        "fingerprint": freeze,
        "fixture_manifest_sha256": sha256_file(FIXTURE_MANIFEST),
        "runtime_versions": _runtime_versions(),
        "cases": cases,
    }
    if report_with_timings:
        report["observed_candidate_commit"] = _git_head()
    return report


def validate_offline_golden() -> dict[str, Any]:
    if not GOLDEN_PATH.is_file():
        raise RuntimeError(f"missing Paddle stage golden: {GOLDEN_PATH}")
    golden = cast(
        dict[str, Any],
        json.loads(GOLDEN_PATH.read_text(encoding="utf-8")),
    )
    if golden.get("schema_version") != SCHEMA_VERSION:
        raise RuntimeError(
            f"unsupported Paddle stage schema: {golden.get('schema_version')}"
        )
    if golden.get("kind") != "paddle_stage_parity":
        raise RuntimeError("invalid Paddle stage golden kind")
    freeze = json.loads(FREEZE_MANIFEST.read_text(encoding="utf-8"))
    _validate_freeze_manifest(freeze)
    fixtures = _load_fixture_manifest()
    if golden.get("fingerprint") != freeze:
        raise RuntimeError("Paddle stage golden fingerprint is stale")
    if golden.get("fixture_manifest_sha256") != sha256_file(FIXTURE_MANIFEST):
        raise RuntimeError("Paddle stage golden fixture manifest hash is stale")
    expected_ids = [Path(str(item["name"])).stem for item in fixtures["fixtures"]]
    actual_ids = [str(case.get("case_id")) for case in golden.get("cases", [])]
    if actual_ids != expected_ids:
        raise RuntimeError(
            f"Paddle stage cases mismatch: expected {expected_ids}, got {actual_ids}"
        )
    for case in golden["cases"]:
        for runtime in ("oracle", "candidate"):
            trace = case.get(runtime)
            if not isinstance(trace, dict):
                raise RuntimeError(f"{case['case_id']} missing {runtime} trace")
            stages = trace.get("stages", {})
            expected_stages = {
                "1_global_preprocess",
                "2_det_preprocess",
                "3_det_infer",
                "4_det_postprocess",
                "5_perspective_crop",
                "6_cls",
                "7_rec_preprocess",
                "8_rec_infer",
                "9_rec_decode",
                "10_output",
            }
            if set(stages) != expected_stages:
                raise RuntimeError(
                    f"{case['case_id']} {runtime} stage schema mismatch"
                )
    return golden


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Freeze/check real Python and C++ Paddle stage traces"
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify committed golden; offline unless --runtime is also supplied",
    )
    parser.add_argument(
        "--runtime",
        action="store_true",
        help="run live Python and C++ inference (explicit model-dependent gate)",
    )
    parser.add_argument(
        "--report-out",
        type=Path,
        default=None,
        help="write an observational live report including timings",
    )
    parser.add_argument(
        "--raw-dir",
        type=Path,
        default=None,
        help="write canonical tensor/image binaries for numeric diagnosis",
    )
    args = parser.parse_args()

    try:
        if args.check and not args.runtime:
            validate_offline_golden()
            print(f"[OK] Offline Paddle stage golden valid: {GOLDEN_PATH}")
            return

        observed = build_live_report(
            raw_dir=args.raw_dir,
            report_with_timings=args.report_out is not None,
        )
        deterministic = normalize_trace_for_golden(observed)
        if args.report_out is not None:
            args.report_out.parent.mkdir(parents=True, exist_ok=True)
            args.report_out.write_text(
                json.dumps(observed, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )

        formatted = json.dumps(deterministic, indent=2, ensure_ascii=False) + "\n"
        if args.check:
            existing = GOLDEN_PATH.read_text(encoding="utf-8")
            if json.loads(existing) != deterministic:
                raise RuntimeError(
                    "live Python/C++ Paddle traces do not match committed golden"
                )
            print(f"[OK] Live Paddle stage traces match: {GOLDEN_PATH}")
            return

        GOLDEN_PATH.parent.mkdir(parents=True, exist_ok=True)
        GOLDEN_PATH.write_text(formatted, encoding="utf-8")
        print(f"[OK] Wrote real Paddle stage golden: {GOLDEN_PATH}")
    except Exception as error:
        print(f"[FAIL] {error}", file=sys.stderr)
        raise SystemExit(1) from error


if __name__ == "__main__":
    main()
