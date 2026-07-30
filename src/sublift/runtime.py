"""
src/sublift/runtime.py
----------------------
SubLift Runtime Policy and Worker Selection Resolver.
Implements the engine x runtime resolution matrix for SubLift CLI and host launchers.
"""

from __future__ import annotations

import os
from collections.abc import Mapping
from dataclasses import dataclass
from enum import StrEnum, unique
from pathlib import Path
from typing import Literal, cast


class RuntimePolicyError(ValueError):
    """Raised when runtime or engine input is invalid."""

    pass


@unique
class ResolutionSource(StrEnum):
    EXPLICIT_FLAG = "explicit_flag"
    ENV_VAR = "env_var"
    PRODUCT_DEFAULT = "product_default"



@dataclass(frozen=True)
class WorkerChoice:
    runtime: Literal["python", "cpp"]
    engine: Literal["vision", "mock", "paddle"]
    resolved_via: ResolutionSource


SUPPORTED_ENGINES = {"vision", "mock", "paddle"}
SUPPORTED_RUNTIMES = {"python", "cpp"}


DEFAULT_RUNTIME: Literal["python", "cpp"] = "cpp"


def probe_cpp_paddle_available(
    *,
    env_override: Mapping[str, str] | None = None,
    repo_root: Path | None = None,
) -> bool:
    """Heuristic: C++ paddle is product-usable when worker exists and PP-OCRv6 models exist.

    Does not start the worker. Matches C++ ``is_paddle_available()`` model-path check
    for the default ``small`` layout under ``SUBLIFT_PADDLE_MODEL_DIR`` / cache.
    Set ``SUBLIFT_CPP_PADDLE=0`` to force unavailable; ``=1`` still requires models.
    """
    env = env_override if env_override is not None else os.environ
    flag = (env.get("SUBLIFT_CPP_PADDLE") or "").strip().lower()
    if flag in {"0", "false", "no", "off"}:
        return False

    from sublift.worker_bin import resolve_worker_bin

    worker_bin = resolve_worker_bin(repo_root)
    if worker_bin is None or not worker_bin.is_file():
        return False

    import subprocess

    try:
        res = subprocess.run(
            [str(worker_bin), "--probe-engine", "paddle"],
            capture_output=True,
            text=True,
            timeout=2.0,
            env=dict(env),
        )
        return res.returncode == 0
    except Exception:
        return False


def resolve_runtime(
    requested_runtime: str | None = None,
    requested_engine: str = "vision",
    env_override: Mapping[str, str] | None = None,
    default_runtime: str = DEFAULT_RUNTIME,
    cpp_paddle_available: bool = False,
) -> WorkerChoice:
    """
    Resolves the final WorkerChoice (runtime, engine, resolved_via).

    Priority hierarchy:
    1. Explicit flag (requested_runtime)
    2. Environment variable SUBLIFT_RUNTIME
    3. Product default (default_runtime)

    Cross-matrix rules:
    - engine == 'paddle' routes to runtime='cpp' when the resolved runtime is
      'cpp' and C++ paddle is available.
    - engine == 'paddle' and runtime == 'cpp' raises RuntimePolicyError if C++
      paddle is unavailable.
    - Explicit/env runtime='python' remains the rollback path.
    - engine in ('vision', 'mock') routes to resolved runtime.
    - Unsupported engines or invalid runtimes raise RuntimePolicyError.
    """
    engine_norm = (requested_engine or "").strip().lower()
    if engine_norm not in SUPPORTED_ENGINES:
        raise RuntimePolicyError(
            f"Unsupported engine '{requested_engine}'. Supported: {sorted(SUPPORTED_ENGINES)}"
        )

    source = ResolutionSource.PRODUCT_DEFAULT
    raw_runtime: str | None = None

    if requested_runtime is not None and requested_runtime.strip():
        raw_runtime = requested_runtime.strip().lower()
        source = ResolutionSource.EXPLICIT_FLAG
    else:
        env_map = env_override if env_override is not None else os.environ
        env_val = env_map.get("SUBLIFT_RUNTIME")
        if env_val is not None and env_val.strip():
            raw_runtime = env_val.strip().lower()
            source = ResolutionSource.ENV_VAR
        else:
            raw_runtime = (default_runtime or "").strip().lower()

    if raw_runtime not in SUPPORTED_RUNTIMES:
        raise RuntimePolicyError(
            f"Invalid runtime '{raw_runtime}'. Supported: {sorted(SUPPORTED_RUNTIMES)}"
        )

    resolved_engine = cast(Literal["vision", "mock", "paddle"], engine_norm)

    if resolved_engine == "paddle":
        if raw_runtime == "cpp" and cpp_paddle_available:
            return WorkerChoice(runtime="cpp", engine="paddle", resolved_via=source)
        if raw_runtime == "cpp":
            raise RuntimePolicyError(
                "Paddle C++ engine is unavailable on this system. "
                "Use '--runtime python' to explicitly request Python Paddle fallback."
            )
        return WorkerChoice(runtime="python", engine="paddle", resolved_via=source)

    resolved_runtime = cast(Literal["python", "cpp"], raw_runtime)
    return WorkerChoice(runtime=resolved_runtime, engine=resolved_engine, resolved_via=source)
