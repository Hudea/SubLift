"""Environment & .env configuration resolution for tools.vlm_ocr."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Mapping

from .models import VlmEndpointConfig


DEFAULT_MINIMAX_BASE_URL = "https://api.minimax.cn/v1"
DEFAULT_MINIMAX_MODEL = "MiniMax-M3.1-Flash"
DEFAULT_NVIDIA_BASE_URL = "https://integrate.api.nvidia.com/v1"
DEFAULT_NVIDIA_MODEL = "nvidia/nemotron-3.5-super-vl-120b-a12b"
VALID_THINKING_MODES = frozenset({"auto", "none", "low", "minimal", "default"})
VALID_PROVIDERS = frozenset({"auto", "minimax", "nvidia", "openai"})

_PROVIDER_PROFILES: dict[str, tuple[tuple[str, ...], tuple[str, ...], str, tuple[str, ...], str]] = {
    "minimax": (
        ("MINIMAX_API_KEY",),
        ("MINIMAX_API_BASE_URL",),
        DEFAULT_MINIMAX_BASE_URL,
        ("MINIMAX_MODEL",),
        DEFAULT_MINIMAX_MODEL,
    ),
    "nvidia": (
        ("NVIDIA_API_KEY",),
        ("NVIDIA_API_BASE_URL",),
        DEFAULT_NVIDIA_BASE_URL,
        ("NVIDIA_MODEL",),
        DEFAULT_NVIDIA_MODEL,
    ),
    "openai": (
        ("OPENAI_API_KEY", "OPENCODE_API_KEY"),
        ("OPENAI_BASE_URL", "OPENCODE_BASE_URL"),
        DEFAULT_MINIMAX_BASE_URL,
        ("OPENAI_MODEL",),
        DEFAULT_MINIMAX_MODEL,
    ),
}


def parse_dotenv(env_path: Path) -> dict[str, str]:
    """Parse a .env file into a key-value dict, stripping quotes and comments."""
    if not env_path.is_file():
        return {}

    parsed: dict[str, str] = {}
    for raw_line in env_path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export ") :].strip()
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        key = key.strip()
        if not key:
            continue
        val = val.strip()
        if len(val) >= 2 and val[0] == val[-1] and val[0] in ('"', "'"):
            val = val[1:-1]
        else:
            # Strip inline comment when unquoted
            if " #" in val:
                val = val.split(" #", 1)[0].rstrip()
        parsed[key] = val
    return parsed


def normalize_chat_completions_url(raw_url: str) -> str:
    """Normalize base URL or full endpoint URL into a /chat/completions endpoint."""
    url = raw_url.strip().rstrip("/")
    if not url:
        raise ValueError("Empty API endpoint / base URL")
    if url.endswith("/chat/completions"):
        return url
    return f"{url}/chat/completions"


def _first_non_empty(*candidates: str | None) -> str:
    for cand in candidates:
        if cand is not None and cand.strip():
            return cand.strip()
    return ""


def _build_env_lookup(
    env_file: Path | None,
    environ: Mapping[str, str] | None,
) -> dict[str, str]:
    """Merge .env file values with process environment without overriding non-empty env vars."""
    merged = parse_dotenv(env_file) if env_file is not None else {}
    proc_env = os.environ if environ is None else environ
    for k, v in proc_env.items():
        if v.strip():
            merged[k] = v
    return merged


def _infer_auto_provider(
    env_map: Mapping[str, str],
    *,
    endpoint: str | None,
    model: str | None,
) -> str:
    model_low = (model or "").lower()
    endpoint_low = (endpoint or "").lower()
    if "nvidia" in model_low or "nvidia.com" in endpoint_low:
        return "nvidia"
    if "minimax" in model_low or "minimax" in endpoint_low:
        return "minimax"
    if (env_map.get("MINIMAX_API_KEY") or "").strip():
        return "minimax"
    if (env_map.get("NVIDIA_API_KEY") or "").strip():
        return "nvidia"
    if (env_map.get("OPENAI_API_KEY") or env_map.get("OPENCODE_API_KEY") or "").strip():
        return "openai"
    return "minimax"


def resolve_vlm_config(
    *,
    provider: str = "auto",
    endpoint: str | None = None,
    api_key: str | None = None,
    model: str | None = None,
    thinking_mode: str = "auto",
    timeout_s: float = 120.0,
    max_retries: int = 4,
    env_file: Path | None = None,
    environ: Mapping[str, str] | None = None,
) -> VlmEndpointConfig:
    """Resolve VLM config with precedence: CLI > VLM_* > selected provider profile."""
    if thinking_mode not in VALID_THINKING_MODES:
        raise ValueError(
            f"Unsupported thinking_mode {thinking_mode!r}; expected one of {sorted(VALID_THINKING_MODES)}"
        )

    env_map = _build_env_lookup(env_file, environ)
    req_provider = (provider or "auto").strip().lower()
    if req_provider == "auto":
        req_provider = (env_map.get("VLM_PROVIDER") or "").strip().lower() or "auto"
    if req_provider not in VALID_PROVIDERS:
        raise ValueError(
            f"Unsupported provider {req_provider!r}; expected one of {sorted(VALID_PROVIDERS)}"
        )

    selected_provider = (
        _infer_auto_provider(env_map, endpoint=endpoint, model=model)
        if req_provider == "auto"
        else req_provider
    )
    key_vars, url_vars, default_url, model_vars, default_model = _PROVIDER_PROFILES[selected_provider]

    resolved_key = _first_non_empty(
        api_key,
        env_map.get("VLM_API_KEY"),
        *(env_map.get(k) for k in key_vars),
    )

    raw_url = _first_non_empty(
        endpoint,
        env_map.get("VLM_ENDPOINT"),
        env_map.get("VLM_API_BASE_URL"),
        *(env_map.get(k) for k in url_vars),
        default_url,
    )
    resolved_endpoint = normalize_chat_completions_url(raw_url)

    resolved_model = _first_non_empty(
        model,
        env_map.get("VLM_MODEL"),
        *(env_map.get(k) for k in model_vars),
        default_model,
    )

    return VlmEndpointConfig(
        endpoint=resolved_endpoint,
        api_key=resolved_key,
        model=resolved_model,
        thinking_mode=thinking_mode,
        timeout_s=float(timeout_s),
        max_retries=int(max_retries),
    )
