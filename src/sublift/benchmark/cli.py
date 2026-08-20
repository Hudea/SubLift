"""Unified command-line interface for SubLift benchmark workflows."""

from __future__ import annotations

import argparse
import dataclasses
import json
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from sublift.benchmark.config import (
    ManifestError,
    RunConfig,
    config_to_dict,
    load_manifest,
    resolve_run_config,
)
from sublift.benchmark.git_utils import (
    clean_commit_message,
    get_latest_commit_message,
    resolve_auto_increment_label,
)
from sublift.benchmark.matrix import expand_matrix, parse_set_args, parse_vary_args
from sublift.benchmark.matrix_report import (
    MatrixRecord,
    completed_record,
    failed_record,
    write_matrix_reports,
)
from sublift.benchmark.report import write_reports
from sublift.benchmark.score import align_existing_srt

_EXPECTED_ERRORS = (FileNotFoundError, ManifestError, RuntimeError, ValueError)


def build_parser() -> argparse.ArgumentParser:
    """Build the unified benchmark parser."""
    parser = argparse.ArgumentParser(
        prog="sublift-benchmark",
        description="SubLift benchmark：统一运行、参数矩阵、已有 SRT 评分与配置检查。",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    run_parser = subparsers.add_parser("run", help="执行一个 benchmark config")
    _add_config_options(run_parser)
    run_parser.set_defaults(handler=_command_run)

    matrix_parser = subparsers.add_parser("matrix", help="执行参数笛卡尔积矩阵")
    _add_config_options(matrix_parser)
    matrix_parser.add_argument(
        "--vary",
        action="append",
        default=[],
        metavar="KEY=VALUES",
        help='矩阵轴，可重复；推荐写法：--vary \'fps=[5,8,12]\'',
    )
    matrix_parser.add_argument(
        "--max-cases",
        type=int,
        default=64,
        help="矩阵最大组合数，防止意外爆炸（默认 64）",
    )
    matrix_parser.add_argument(
        "--dry-run",
        action="store_true",
        help="只解析并打印矩阵，不执行提取、不写报告",
    )
    matrix_parser.add_argument(
        "--keep-going",
        action="store_true",
        help="单组失败后继续剩余组合",
    )
    matrix_parser.set_defaults(handler=_command_matrix)

    score_parser = subparsers.add_parser("score", help="用同一诊断口径评分已有 SRT")
    _add_config_options(score_parser)
    score_parser.add_argument("detected", type=Path, help="待评分 SRT")
    score_parser.add_argument(
        "--elapsed",
        type=float,
        default=0.0,
        help="已有运行的 wall 秒数（默认 0，不计算有效 speed factor）",
    )
    score_parser.add_argument(
        "--duration",
        type=float,
        default=None,
        help="视频秒数覆盖值；缺省时从 config 视频探测",
    )
    score_parser.set_defaults(handler=_command_score)

    compare_parser = subparsers.add_parser(
        "compare-roi",
        help="比较 full/ROI 两份 agent JSON 的 feat-039 硬门",
    )
    compare_parser.add_argument("full_agent_json", type=Path)
    compare_parser.add_argument("roi_agent_json", type=Path)
    compare_parser.add_argument("--out", type=Path, default=None)
    compare_parser.set_defaults(handler=_command_compare_roi)

    overhead_parser = subparsers.add_parser(
        "overhead",
        help="交错测量 performance summary 相对 off 的扰动",
    )
    _add_config_options(overhead_parser)
    overhead_parser.add_argument(
        "--runs",
        type=int,
        default=3,
        help="每种模式的配对运行次数（默认 3）",
    )
    overhead_parser.add_argument(
        "--target-pct",
        type=float,
        default=5.0,
        help="允许的 summary 中位扰动百分比（默认 5）",
    )
    overhead_parser.set_defaults(handler=_command_overhead)

    show_parser = subparsers.add_parser("show", help="解析并输出最终配置，不执行")
    show_parser.add_argument("config", type=Path, help="benchmark JSON config")
    show_parser.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="通用参数覆盖，可重复",
    )
    show_parser.add_argument(
        "--backend",
        choices=("native", "oracle"),
        default=None,
        help="提取后端：native（默认）或 oracle",
    )
    show_parser.set_defaults(handler=_command_show)
    return parser


def _add_config_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("config", type=Path, help="benchmark JSON config")
    parser.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="通用参数覆盖，可重复，例如 --set fps=8",
    )
    parser.add_argument(
        "--label",
        default="manifest",
        help="manifest=使用配置标签；auto=按提交递增；其它值=显式标签",
    )
    parser.add_argument(
        "--backend",
        choices=("native", "oracle"),
        default=None,
        help="提取后端：native（默认，产品 CLI）或 oracle（冻结 Python Pipeline）",
    )


def main(argv: Sequence[str] | None = None) -> int:
    """Run the selected benchmark command."""
    parser = build_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)
    handler = args.handler
    try:
        return int(handler(args))
    except _EXPECTED_ERRORS as exc:
        print(f"benchmark 失败: {exc}", file=sys.stderr)
        return 1


def _command_run(args: argparse.Namespace) -> int:
    from sublift.benchmark.runner import run_benchmark

    document = load_manifest(args.config)
    config = resolve_run_config(document, overrides=_config_overrides(args))
    config = _prepare_single_output(config, label_mode=args.label)
    paths = write_reports(run_benchmark(config))
    _print_paths("benchmark 完成", paths)
    return 0


def _command_score(args: argparse.Namespace) -> int:
    if args.elapsed < 0:
        raise ManifestError("--elapsed 不能为负")
    if args.duration is not None and args.duration <= 0:
        raise ManifestError("--duration 必须大于 0")
    document = load_manifest(args.config)
    config = resolve_run_config(document, overrides=_config_overrides(args))
    config = _prepare_single_output(config, label_mode=args.label)
    result = align_existing_srt(
        config,
        args.detected,
        elapsed_seconds=args.elapsed,
        video_duration_seconds=args.duration,
    )
    paths = write_reports(result)
    _print_paths("SRT 评分完成", paths)
    return 0


def _command_show(args: argparse.Namespace) -> int:
    document = load_manifest(args.config)
    config = resolve_run_config(document, overrides=_config_overrides(args))
    payload = {
        "schema_version": 2,
        "config": config_to_dict(config),
        "matrix": document.matrix,
    }
    print(json.dumps(payload, ensure_ascii=False, indent=2))
    return 0


def _command_compare_roi(args: argparse.Namespace) -> int:
    from sublift.benchmark.roi_compare import main as compare_main

    forwarded = [str(args.full_agent_json), str(args.roi_agent_json)]
    if args.out is not None:
        forwarded.extend(["--out", str(args.out)])
    return compare_main(forwarded)


def _command_overhead(args: argparse.Namespace) -> int:
    from sublift.benchmark.overhead import run_overhead

    document = load_manifest(args.config)
    config = resolve_run_config(document, overrides=_config_overrides(args))
    config = _prepare_single_output(config, label_mode=args.label)
    report, path = run_overhead(
        config,
        runs=args.runs,
        target_pct=args.target_pct,
    )
    print(
        f"overhead={float(report['overhead_pct']):.2f}% "
        f"consistent={report['detection_hashes']['consistent']}"
    )
    print(f"  overhead_json: {path}")
    return 0 if report["pass"] else 2


def _command_matrix(args: argparse.Namespace) -> int:
    from sublift.benchmark.runner import run_benchmark

    document = load_manifest(args.config)
    fixed_overrides = _config_overrides(args)
    cli_axes = parse_vary_args(args.vary)
    base_config = resolve_run_config(document, overrides=fixed_overrides)
    matrix_label, matrix_output = _prepare_matrix_output(base_config, label_mode=args.label)
    cells = expand_matrix(
        document,
        fixed_overrides=fixed_overrides,
        cli_axes=cli_axes,
        base_label=matrix_label,
        max_cases=args.max_cases,
    )

    plan = {
        "schema_version": 1,
        "label": matrix_label,
        "case_count": len(cells),
        "output_dir": str(matrix_output),
        "cells": [
            {
                "index": cell.index,
                "label": cell.label,
                "overrides": cell.overrides,
                "config": config_to_dict(cell.config),
            }
            for cell in cells
        ],
    }
    if args.dry_run:
        print(json.dumps(plan, ensure_ascii=False, indent=2))
        return 0

    matrix_output.mkdir(parents=True, exist_ok=True)
    (matrix_output / "matrix.plan.json").write_text(
        json.dumps(plan, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    records: list[MatrixRecord] = []
    stop_after_failure = False
    for cell in cells:
        cell_config = dataclasses.replace(
            cell.config,
            label=cell.label,
            output_dir=matrix_output / cell.label,
        )
        print(f"[{cell.index}/{len(cells)}] {cell.label}", file=sys.stderr)
        try:
            result = run_benchmark(cell_config)
            artifacts = write_reports(result)
            records.append(
                completed_record(
                    label=cell.label,
                    overrides=cell.overrides,
                    result=result,
                    artifacts=artifacts,
                )
            )
        except _EXPECTED_ERRORS as exc:
            records.append(
                failed_record(
                    label=cell.label,
                    overrides=cell.overrides,
                    error=str(exc),
                )
            )
            print(f"  failed: {exc}", file=sys.stderr)
            if not args.keep_going:
                stop_after_failure = True
                break

    paths = write_matrix_reports(records, matrix_output)
    _print_paths("matrix 完成", paths)
    return 1 if stop_after_failure or any(record.status != "pass" for record in records) else 0


def _config_overrides(args: argparse.Namespace) -> dict[str, Any]:
    overrides: dict[str, Any] = dict(parse_set_args(args.set))
    backend = getattr(args, "backend", None)
    if backend:
        overrides["backend"] = backend
    return overrides


def _prepare_single_output(config: RunConfig, *, label_mode: str) -> RunConfig:
    if label_mode == "auto":
        base_label = clean_commit_message(get_latest_commit_message())
        label, output_dir = resolve_auto_increment_label(config.output_dir, base_label)
        return dataclasses.replace(config, label=label, output_dir=output_dir)
    resolved_label = config.label if label_mode == "manifest" else label_mode
    resolved_label = resolved_label or "run"
    return dataclasses.replace(
        config,
        label=resolved_label,
        output_dir=config.output_dir / resolved_label,
    )


def _prepare_matrix_output(config: RunConfig, *, label_mode: str) -> tuple[str, Path]:
    if label_mode == "auto":
        base_label = clean_commit_message(get_latest_commit_message())
        return resolve_auto_increment_label(config.output_dir, base_label)
    label = config.label if label_mode == "manifest" else label_mode
    label = label or "matrix"
    return label, config.output_dir / label


def _print_paths(title: str, paths: dict[str, Path]) -> None:
    print(f"{title}:")
    for kind, path in paths.items():
        print(f"  {kind}: {path}")


if __name__ == "__main__":
    raise SystemExit(main())
