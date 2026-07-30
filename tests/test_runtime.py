"""Runtime 解析策略 (resolve_runtime) 单元测试。"""

from __future__ import annotations

from pathlib import Path
from typing import Literal

import pytest

from sublift.runtime import (
    ResolutionSource,
    RuntimePolicyError,
    WorkerChoice,
    resolve_runtime,
)
from sublift.worker_bin import resolve_worker_bin

P_DEF = ResolutionSource.PRODUCT_DEFAULT
FLAG = ResolutionSource.EXPLICIT_FLAG
ENV_VAR = ResolutionSource.ENV_VAR
PADDLE_OVR = ResolutionSource.PADDLE_OVERRIDE


def test_resolve_runtime_default_product_path() -> None:
    choice = resolve_runtime()
    assert choice.runtime == "cpp"
    assert choice.engine == "vision"
    assert choice.resolved_via == P_DEF


def test_resolve_runtime_explicit_flag_python() -> None:
    choice = resolve_runtime(requested_runtime="python", requested_engine="vision")
    assert choice.runtime == "python"
    assert choice.engine == "vision"
    assert choice.resolved_via == FLAG


def test_resolve_runtime_explicit_flag_cpp() -> None:
    choice = resolve_runtime(requested_runtime="cpp", requested_engine="vision")
    assert choice.runtime == "cpp"
    assert choice.engine == "vision"
    assert choice.resolved_via == FLAG


def test_resolve_runtime_paddle_force_python() -> None:
    choice = resolve_runtime(requested_runtime="cpp", requested_engine="paddle")
    assert choice.runtime == "python"
    assert choice.engine == "paddle"
    assert choice.resolved_via == PADDLE_OVR


def test_resolve_runtime_paddle_cpp_available() -> None:
    choice = resolve_runtime(
        requested_runtime="cpp", requested_engine="paddle", cpp_paddle_available=True
    )
    assert choice.runtime == "cpp"
    assert choice.engine == "paddle"
    assert choice.resolved_via == FLAG


def test_resolve_runtime_paddle_product_default_cpp_available() -> None:
    choice = resolve_runtime(
        requested_engine="paddle",
        default_runtime="cpp",
        cpp_paddle_available=True,
    )
    assert choice == WorkerChoice("cpp", "paddle", P_DEF)


def test_resolve_runtime_paddle_product_default_cpp_unavailable() -> None:
    choice = resolve_runtime(
        requested_engine="paddle",
        default_runtime="cpp",
        cpp_paddle_available=False,
    )
    assert choice == WorkerChoice("python", "paddle", PADDLE_OVR)


def test_probe_cpp_paddle_respects_env_off() -> None:
    from sublift.runtime import probe_cpp_paddle_available

    assert probe_cpp_paddle_available(env_override={"SUBLIFT_CPP_PADDLE": "0"}) is False



def test_resolve_runtime_env_override() -> None:
    env = {"SUBLIFT_RUNTIME": "cpp"}
    choice = resolve_runtime(env_override=env)
    assert choice.runtime == "cpp"
    assert choice.engine == "vision"
    assert choice.resolved_via == ENV_VAR


def test_resolve_runtime_flag_overrides_env() -> None:
    env = {"SUBLIFT_RUNTIME": "cpp"}
    choice = resolve_runtime(requested_runtime="python", env_override=env)
    assert choice.runtime == "python"
    assert choice.resolved_via == FLAG


def test_resolve_runtime_whitespace_and_case_trimmed() -> None:
    choice = resolve_runtime(requested_runtime="  CPP  ", requested_engine=" VISION ")
    assert choice.runtime == "cpp"
    assert choice.engine == "vision"


@pytest.mark.parametrize(
    "requested_runtime, env_val, requested_engine, default_runtime, expected_choice",
    [
        # Default python
        (None, None, "vision", "python", WorkerChoice("python", "vision", P_DEF)),
        (None, None, "mock", "python", WorkerChoice("python", "mock", P_DEF)),
        (None, None, "paddle", "python", WorkerChoice("python", "paddle", P_DEF)),
        # Default cpp; unavailable Paddle falls back explicitly.
        (None, None, "vision", "cpp", WorkerChoice("cpp", "vision", P_DEF)),
        (None, None, "mock", "cpp", WorkerChoice("cpp", "mock", P_DEF)),
        (None, None, "paddle", "cpp", WorkerChoice("python", "paddle", PADDLE_OVR)),
        # Env variable override
        (None, "cpp", "vision", "python", WorkerChoice("cpp", "vision", ENV_VAR)),
        (None, "python", "vision", "cpp", WorkerChoice("python", "vision", ENV_VAR)),
        (None, "cpp", "paddle", "python", WorkerChoice("python", "paddle", PADDLE_OVR)),
        # Explicit flag overrides everything
        ("python", "cpp", "vision", "cpp", WorkerChoice("python", "vision", FLAG)),
        ("cpp", "python", "mock", "python", WorkerChoice("cpp", "mock", FLAG)),
        ("cpp", "python", "paddle", "cpp", WorkerChoice("python", "paddle", PADDLE_OVR)),
    ],
)
def test_resolve_runtime_matrix(
    requested_runtime: str | None,
    env_val: str | None,
    requested_engine: str,
    default_runtime: Literal["python", "cpp"],
    expected_choice: WorkerChoice,
) -> None:
    env_map = {"SUBLIFT_RUNTIME": env_val} if env_val is not None else {}
    choice = resolve_runtime(
        requested_runtime=requested_runtime,
        requested_engine=requested_engine,
        env_override=env_map,
        default_runtime=default_runtime,
    )
    assert choice == expected_choice


@pytest.mark.parametrize(
    "requested_runtime, env_val, requested_engine, err_msg",
    [
        (None, None, "onnx", "Unsupported engine 'onnx'"),
        ("invalid_rt", None, "vision", "Invalid runtime 'invalid_rt'"),
        (None, "invalid_rt", "vision", "Invalid runtime 'invalid_rt'"),
    ],
)
def test_resolve_runtime_errors(
    requested_runtime: str | None,
    env_val: str | None,
    requested_engine: str,
    err_msg: str,
) -> None:
    env_map = {"SUBLIFT_RUNTIME": env_val} if env_val is not None else {}
    with pytest.raises(RuntimePolicyError) as exc_info:
        resolve_runtime(
            requested_runtime=requested_runtime,
            requested_engine=requested_engine,
            env_override=env_map,
        )
    assert err_msg in str(exc_info.value)


def test_resolve_worker_bin_prefers_release(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("SUBLIFT_WORKER_PATH", raising=False)
    rel = tmp_path / "build" / "cpp-rel" / "bin" / "sublift_worker"
    dbg = tmp_path / "build" / "cpp" / "bin" / "sublift_worker"
    rel.parent.mkdir(parents=True)
    dbg.parent.mkdir(parents=True)
    rel.write_text("x")
    dbg.write_text("x")
    rel.chmod(0o755)
    dbg.chmod(0o755)
    found = resolve_worker_bin(tmp_path)
    assert found == rel


def test_resolve_worker_bin_env_override(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    custom = tmp_path / "custom_worker"
    custom.write_text("x")
    custom.chmod(0o755)
    monkeypatch.setenv("SUBLIFT_WORKER_PATH", str(custom))
    assert resolve_worker_bin(tmp_path) == custom
