"""OpenAI-compatible VLM client with adaptive thinking, Sheet-Split Rescue, and checkpoints."""

from __future__ import annotations

import base64
import json
import queue
import random
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import Future, ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping

from PIL import Image

from .assembler import extract_json_object, validate_and_align_sheet_slots
from .models import (
    FailedSlotRecord,
    SheetSpec,
    SlotFallbackResolver,
    TimingSlot,
    TranscribedSlot,
    VlmEndpointConfig,
)
from .sheet_builder import (
    PreparedSheet,
    compose_contact_sheet_image,
    encode_image_png_bytes,
    render_single_contact_sheet,
)


SYSTEM_PROMPT = "\n".join(
    [
        "你是一个影视硬字幕视觉 OCR 结构化转录专家。",
        "输入是一张单列从上到下排列的字幕抽帧拼图（Contact Sheet）：",
        "- 左侧是浅色标签栏，印有槽位编号 `#NNN`（如 `#001`）与秒级时间范围，右侧由蓝色竖线分隔的是视频字幕画面。",
        "- **严禁将左侧标签栏的 `#NNN` 编号或时间文字抄入字幕 `lines` 中**！",
        "",
        "请按编号顺序逐行读取右侧视频画面中的**前景硬字幕**，并严格遵守以下规则：",
        "1. **槽位编号严格一致 (`id`)**：每个条目的 `id` 必须等于左侧印制的 `#NNN` 整数编号（例如 `#041` 对应 `\"id\": 41`），严禁局部重新从 1 编号。",
        "2. **双行拆分 (`lines`)**：",
        "   - 单行字幕输出含 1 个字符串的数组，如 `[\"你好\"]`。",
        "   - 上下两行字幕必须按从上到下顺序拆为 2 个字符串，如 `[\"-什么？\", \"-怎么回事？\"]`。",
        "   - 双人对话行首连字符 `-` 紧跟文字（如 `\"-姐姐！\"`）。",
        "3. **忠实保留句内空格与标点**：",
        "   - 画面中分句之间以空格分隔（无逗号）时，必须原样保留单个半角空格 ` `（例如 `\"哦 对\"`、`\"你想起来了 对吗？\"`），严禁擅自删除或改成逗号。",
        "   - 原样保留画面中的繁体/简体字形、引号 `“` `”`、单字符省略号 `…` 及括号说明 `（...）`。",
        "4. **忽略背景干扰与 5 类分类 (`category`)**：",
        "   - 严禁转录背景中的非字幕文字，包括：韩文/日文/英文演职员表与制作字幕（如 `기획`、`제작`、`연출`、`촬영`、`극본`、导演/编剧名）、新闻背景滚屏、台标水印或道具招牌字。",
        "   - 若当前帧**既有前景中文字幕、又有背景演职员表/道具字**，只转录前景中文字幕，严禁将演职员表文字混入 `lines`。",
        "   - 若当前帧**只有**演职员表/道具招牌字而无前景字幕，标为 `\"scene_text\"` 且 `lines: []`（绝对不可标为 `\"dialogue\"`）。",
        "   - 常规前景台词标为 `\"dialogue\"`；括号人物/地点/前情提要/歌词注释标为 `\"bracket_note\"`；",
        "     仅有角标水印标为 `\"watermark\"`；无任何文字或仅有模糊背景标为 `\"empty\"` 且 `lines: []`。",
        "5. **严格输出 JSON 对象**：",
        "   仅输出合法 JSON，格式为 `{\"slots\": [{\"id\": 41, \"category\": \"dialogue\", \"lines\": [\"你好\"]}]}`。",
    ]
)


class VlmApiError(RuntimeError):
    """Structured error from VLM HTTP API call."""

    def __init__(
        self,
        message: str,
        *,
        http_status: int | None = None,
        fatal: bool = False,
        splittable: bool = True,
        immediate_split: bool = False,
    ) -> None:
        super().__init__(message)
        self.http_status = http_status
        self.fatal = fatal
        self.splittable = splittable
        self.immediate_split = immediate_split


class FatalVlmError(VlmApiError):
    """Auth, quota, or endpoint configuration error that should not be retried or split."""

    def __init__(self, message: str, *, http_status: int | None = None) -> None:
        super().__init__(message, http_status=http_status, fatal=True, splittable=False)


class ContentFilterVlmError(VlmApiError):
    """Content safety/moderation refusal that should bypass retries and bisect immediately."""

    def __init__(self, message: str, *, http_status: int | None = None) -> None:
        super().__init__(message, http_status=http_status, fatal=False, splittable=True)


class NonSplittableVlmError(VlmApiError):
    """Rate-limit, server, or transport failure that must not trigger a split storm."""

    def __init__(self, message: str, *, http_status: int | None = None) -> None:
        super().__init__(message, http_status=http_status, fatal=False, splittable=False)


def _is_fatal_http(status: int, message: str, *, base_code: int | None = None) -> bool:
    if status in (401, 402, 403, 404):
        return True
    if base_code in (1004, 1008, 2049):
        return True
    lower = message.lower()
    return any(
        tok in lower
        for tok in (
            "unauthorized",
            "invalid_api_key",
            "invalid api key",
            "authentication_error",
            "insufficient_quota",
            "insufficient balance",
            "model_not_found",
        )
    )


def _is_rate_limit_error(status: int | None, message: str, *, base_code: int | None = None) -> bool:
    if status == 429 or base_code in (429, 1002):
        return True
    lower = message.lower()
    return any(tok in lower for tok in ("rate limit", "too many requests", "rpm", "tpm"))


def _is_content_filter_error(
    message: str,
    *,
    base_code: int | None = None,
    finish_reason: str | None = None,
    refusal: Any = None,
) -> bool:
    if (finish_reason or "").strip().lower() == "content_filter":
        return True
    if refusal is not None and str(refusal).strip():
        return True
    if base_code in (1027,):
        return True
    lower = message.lower()
    return any(
        tok in lower
        for tok in (
            "content_filter",
            "content_policy",
            "content safety",
            "safety filter",
            "sensitive",
            "moderation",
            "inappropriate",
            "violates",
            "prohibited",
        )
    )


def _parse_retry_after(headers: Any) -> float | None:
    if headers is None:
        return None
    try:
        raw = headers.get("Retry-After")
    except Exception:
        return None
    if raw is None:
        return None
    try:
        return max(0.0, float(str(raw).strip()))
    except ValueError:
        return None


class _RateLimitGate:
    """Thread-safe shared backoff gate synchronizing concurrent workers on HTTP 429."""

    def __init__(
        self,
        *,
        monotonic_fn: Callable[[], float] = time.monotonic,
        jitter_fn: Callable[[], float] | None = None,
    ) -> None:
        self._cond = threading.Condition()
        self._blocked_until = 0.0
        self._consecutive_429 = 0
        self._monotonic = monotonic_fn
        self._jitter = jitter_fn or (lambda: random.uniform(0.0, 0.5))

    def wait_if_needed(self) -> None:
        while True:
            with self._cond:
                remaining = self._blocked_until - self._monotonic()
                if remaining <= 0.0:
                    return
                self._cond.wait(timeout=remaining)

    def record_429(self, retry_after_s: float | None = None, attempt: int = 0) -> float:
        with self._cond:
            self._consecutive_429 += 1
            if retry_after_s is not None and retry_after_s >= 0.0:
                delay = min(60.0, float(retry_after_s))
            else:
                exp = min(max(0, attempt), 5)
                delay = min(45.0, (1.5 * (2.0 ** exp)) + float(self._jitter()))
            deadline = self._monotonic() + delay
            if deadline > self._blocked_until:
                self._blocked_until = deadline
            return delay

    def record_success(self) -> None:
        with self._cond:
            self._consecutive_429 = 0

    def is_rate_limited(self) -> bool:
        with self._cond:
            return self._consecutive_429 > 0


def _initial_effort_candidates(config: VlmEndpointConfig) -> list[str | None]:
    mode = config.thinking_mode
    if mode == "default":
        return [None]
    if mode == "auto":
        hint = f"{config.model} {config.endpoint}".lower()
        if "minimax" in hint:
            return ["low", None]
        return ["none", "low", None]
    if mode == "none":
        return ["none", "low", None]
    if mode == "minimal":
        return ["minimal", "low", None]
    return [mode, None]


def _extract_error_body(raw_bytes: bytes) -> str:
    text = raw_bytes.decode("utf-8", errors="replace").strip()
    try:
        obj = json.loads(text)
        if isinstance(obj, dict):
            err = obj.get("error")
            if isinstance(err, dict) and err.get("message"):
                return str(err["message"])
            if isinstance(err, str) and err:
                return err
            if obj.get("message"):
                return str(obj["message"])
    except Exception:
        pass
    return text[:300]


def _log(msg: str, *, quiet: bool) -> None:
    if not quiet:
        print(msg, file=sys.stderr, flush=True)


class _AdaptivePolicyState:
    """Thread-safe session cache for working reasoning_effort & response_format params."""

    def __init__(self, config: VlmEndpointConfig) -> None:
        self._lock = threading.Lock()
        self._effort_chain = _initial_effort_candidates(config)
        self._effort_idx = 0
        self._use_json_format = True

    def current_params(self) -> tuple[str | None, bool]:
        with self._lock:
            eff = self._effort_chain[min(self._effort_idx, len(self._effort_chain) - 1)]
            return eff, self._use_json_format

    def downgrade_effort(self, failed_effort: str | None) -> bool:
        with self._lock:
            cur = self._effort_chain[min(self._effort_idx, len(self._effort_chain) - 1)]
            if cur == failed_effort and self._effort_idx + 1 < len(self._effort_chain):
                self._effort_idx += 1
                return True
            return self._effort_idx + 1 < len(self._effort_chain)

    def disable_json_format(self) -> bool:
        with self._lock:
            if self._use_json_format:
                self._use_json_format = False
                return True
            return False


def _encode_image_data_uri(image_source: Path | bytes) -> str:
    raw = (
        bytes(image_source)
        if isinstance(image_source, (bytes, bytearray, memoryview))
        else Path(image_source).read_bytes()
    )
    b64 = base64.b64encode(raw).decode("ascii")
    return f"data:image/png;base64,{b64}"


def _call_chat_completions(
    *,
    config: VlmEndpointConfig,
    sheet: SheetSpec,
    policy: _AdaptivePolicyState,
    script: str,
    sheet_png_bytes: bytes | None = None,
    rate_gate: _RateLimitGate | None = None,
) -> dict[str, Any]:
    expected_ids = [int(x) for x in sheet.expected_slot_ids]
    image_data_uri = _encode_image_data_uri(
        sheet_png_bytes if sheet_png_bytes is not None else Path(sheet.sheet_path)
    )

    user_prompt = (
        f"请按顺序提取本张拼图（{sheet.sheet_name}）中全部 {len(expected_ids)} 个槽位的前景字幕，"
        f"期望槽位 id 列表依次为 {expected_ids}。"
        f'严禁抄入左侧 `#NNN` 编号。仅输出 JSON 对象 {{"slots": [...]}}。'
    )

    active_gate = rate_gate or _RateLimitGate()
    last_err: Exception | None = None
    last_splittable = True
    max_attempts = max(1, config.max_retries)
    max_rate_limit_retries = max(6, max_attempts)
    transient_failures = 0
    rate_limit_failures = 0
    policy_adjustments = 0

    while True:
        active_gate.wait_if_needed()
        effort, use_json_fmt = policy.current_params()
        payload: dict[str, Any] = {
            "model": config.model,
            "temperature": 0.01,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "image_url",
                            "image_url": {"url": image_data_uri},
                        },
                        {"type": "text", "text": user_prompt},
                    ],
                },
            ],
            "max_tokens": 4096,
        }
        if use_json_fmt:
            payload["response_format"] = {"type": "json_object"}
        if effort is not None:
            payload["reasoning_effort"] = effort

        headers = {"Content-Type": "application/json"}
        if config.api_key:
            headers["Authorization"] = f"Bearer {config.api_key}"

        req = urllib.request.Request(
            config.endpoint,
            data=json.dumps(payload).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        t0 = time.perf_counter()
        try:
            with urllib.request.urlopen(req, timeout=config.timeout_s) as resp:
                raw_data = json.loads(resp.read().decode("utf-8"))
            elapsed_s = time.perf_counter() - t0

            base_resp = raw_data.get("base_resp")
            if isinstance(base_resp, dict) and int(base_resp.get("status_code", 0) or 0) != 0:
                code = int(base_resp.get("status_code", -1))
                b_msg = str(base_resp.get("status_msg") or "upstream error")
                if _is_fatal_http(200, b_msg, base_code=code):
                    raise FatalVlmError(f"API base_resp fatal ({code}): {b_msg}", http_status=200)
                if _is_content_filter_error(b_msg, base_code=code):
                    raise ContentFilterVlmError(
                        f"API base_resp content filter ({code}): {b_msg}", http_status=200
                    )
                if _is_rate_limit_error(None, b_msg, base_code=code):
                    rate_limit_failures += 1
                    msg_err = f"API rate limit base_resp ({code}): {b_msg}"
                    if rate_limit_failures > max_rate_limit_retries:
                        raise NonSplittableVlmError(msg_err, http_status=429)
                    active_gate.record_429(retry_after_s=None, attempt=rate_limit_failures - 1)
                    continue
                raise VlmApiError(f"API base_resp error ({code}): {b_msg}", http_status=200)

            choices = raw_data.get("choices") or []
            if not choices:
                raise VlmApiError("API response missing 'choices'")
            choice0 = choices[0]
            finish_reason = str(choice0.get("finish_reason") or "").strip().lower()
            msg = choice0.get("message") or {}
            refusal = msg.get("refusal")
            if _is_content_filter_error("", finish_reason=finish_reason, refusal=refusal):
                raise ContentFilterVlmError(
                    f"VLM refused/filtered (finish_reason={finish_reason!r}, refusal={refusal!r})",
                    http_status=200,
                )
            content = msg.get("content") or ""
            try:
                parsed_obj = extract_json_object(content)
                validated, remapped = validate_and_align_sheet_slots(
                    parsed_obj, expected_ids, script=script, allow_placeholder=False
                )
            except Exception as parse_exc:
                if finish_reason == "length":
                    raise VlmApiError(
                        f"Truncated output (finish_reason='length'): {parse_exc}",
                        http_status=200,
                        splittable=True,
                        immediate_split=True,
                    ) from parse_exc
                raise
            active_gate.record_success()
            return {
                "sheet": sheet.sheet_name,
                "expected_slot_ids": expected_ids,
                "remapped_local_ids": remapped,
                "reasoning_effort_used": effort or "omitted",
                "elapsed_s": round(elapsed_s, 3),
                "usage": raw_data.get("usage", {}),
                "degraded_slot_ids": [],
                "failed_slots": [],
                "slots": [
                    {"id": s.slot_id, "category": s.category, "lines": s.lines}
                    for s in validated
                ],
            }
        except (FatalVlmError, ContentFilterVlmError):
            raise
        except urllib.error.HTTPError as http_err:
            retry_after = _parse_retry_after(getattr(http_err, "headers", None))
            err_msg = _extract_error_body(http_err.read())
            http_err.close()
            if _is_fatal_http(http_err.code, err_msg):
                raise FatalVlmError(
                    f"HTTP {http_err.code}: {err_msg}", http_status=http_err.code
                ) from http_err

            if _is_rate_limit_error(http_err.code, err_msg):
                rate_limit_failures += 1
                if rate_limit_failures > max_rate_limit_retries:
                    raise NonSplittableVlmError(
                        f"HTTP {http_err.code} rate-limit exhausted ({rate_limit_failures} retries): {err_msg}",
                        http_status=http_err.code,
                    ) from http_err
                active_gate.record_429(retry_after_s=retry_after, attempt=rate_limit_failures - 1)
                continue

            if http_err.code in (400, 422):
                if _is_content_filter_error(err_msg):
                    raise ContentFilterVlmError(
                        f"HTTP {http_err.code} content filter: {err_msg}", http_status=http_err.code
                    ) from http_err
                lower = err_msg.lower()
                if policy_adjustments < 4:
                    if ("reasoning" in lower or "2013" in lower) and policy.downgrade_effort(effort):
                        policy_adjustments += 1
                        last_err = VlmApiError(
                            f"HTTP {http_err.code} ({err_msg}); retrying with downgraded reasoning_effort",
                            http_status=http_err.code,
                        )
                        continue
                    if "response_format" in lower and policy.disable_json_format():
                        policy_adjustments += 1
                        last_err = VlmApiError(
                            f"HTTP {http_err.code} ({err_msg}); retrying without response_format",
                            http_status=http_err.code,
                        )
                        continue

            transient_failures += 1
            last_splittable = http_err.code < 500
            last_err = (
                NonSplittableVlmError(f"HTTP {http_err.code}: {err_msg}", http_status=http_err.code)
                if http_err.code >= 500
                else VlmApiError(f"HTTP {http_err.code}: {err_msg}", http_status=http_err.code)
            )
        except NonSplittableVlmError:
            raise
        except (urllib.error.URLError, TimeoutError, OSError) as net_exc:
            transient_failures += 1
            last_splittable = False
            last_err = NonSplittableVlmError(f"Network/timeout error: {net_exc}")
        except Exception as exc:
            if getattr(exc, "immediate_split", False):
                raise
            transient_failures += 1
            last_splittable = getattr(exc, "splittable", True)
            last_err = exc

        if transient_failures >= max_attempts:
            if not last_splittable:
                raise NonSplittableVlmError(
                    f"Sheet {sheet.sheet_name} failed after {max_attempts} attempts: {last_err}"
                ) from last_err
            raise VlmApiError(
                f"Sheet {sheet.sheet_name} failed after {max_attempts} attempts: {last_err}",
                splittable=True,
            ) from last_err
        time.sleep(min(4.0, 1.0 * transient_failures))


def _resolve_leaf_fallback(
    *,
    slot: TimingSlot,
    strip_images_by_id: Mapping[int, Image.Image] | None,
    strip_paths_by_id: Mapping[int, Path] | None,
    fallback_resolver: SlotFallbackResolver,
    reason: str,
    sheet_name: str,
    depth: int,
) -> dict[str, Any]:
    opened: Image.Image | None = None
    strip_img: Any = None
    try:
        if strip_images_by_id is not None and slot.slot_id in strip_images_by_id:
            strip_img = strip_images_by_id[slot.slot_id]
        elif strip_paths_by_id is not None and slot.slot_id in strip_paths_by_id:
            opened = Image.open(strip_paths_by_id[slot.slot_id])
            strip_img = opened.convert("RGB")
        t_slot, record = fallback_resolver.resolve_failed_slot(slot, strip_img, reason)
    finally:
        if opened is not None:
            opened.close()

    return {
        "sheet": sheet_name,
        "expected_slot_ids": [slot.slot_id],
        "remapped_local_ids": False,
        "split_rescued": depth > 0,
        "elapsed_s": 0.0,
        "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0},
        "degraded_slot_ids": [slot.slot_id],
        "failed_slots": [record.to_dict()],
        "slots": [{"id": t_slot.slot_id, "category": t_slot.category, "lines": t_slot.lines}],
    }


def _transcribe_sheet_with_split_rescue(
    *,
    config: VlmEndpointConfig,
    sheet: SheetSpec,
    policy: _AdaptivePolicyState,
    script: str,
    slots_by_id: dict[int, TimingSlot] | None,
    strip_paths_by_id: dict[int, Path] | None,
    rescue_dir: Path,
    strip_w: int,
    strip_h: int,
    sheet_png_bytes: bytes | None = None,
    strip_images_by_id: Mapping[int, Image.Image] | None = None,
    save_rescue_to_disk: bool = False,
    depth: int = 0,
    max_depth: int = 4,
    rate_gate: _RateLimitGate | None = None,
    fallback_resolver: SlotFallbackResolver | None = None,
) -> dict[str, Any]:
    """Transcribe sheet, falling back to bisecting sheet into halves and leaf fallback."""
    try:
        return _call_chat_completions(
            config=config,
            sheet=sheet,
            policy=policy,
            script=script,
            sheet_png_bytes=sheet_png_bytes,
            rate_gate=rate_gate,
        )
    except FatalVlmError:
        raise
    except Exception as exc:
        expected_ids = sheet.expected_slot_ids
        is_splittable = getattr(exc, "splittable", True)
        has_ram_strips = (
            strip_images_by_id is not None
            and all(sid in strip_images_by_id for sid in expected_ids)
        )
        has_disk_strips = (
            strip_paths_by_id is not None
            and all(sid in strip_paths_by_id for sid in expected_ids)
        )
        can_split = (
            is_splittable
            and depth < max_depth
            and len(expected_ids) > 1
            and slots_by_id is not None
            and all(sid in slots_by_id for sid in expected_ids)
            and (has_ram_strips or has_disk_strips)
        )
        if not can_split:
            if (
                len(expected_ids) == 1
                and fallback_resolver is not None
                and slots_by_id is not None
                and expected_ids[0] in slots_by_id
            ):
                return _resolve_leaf_fallback(
                    slot=slots_by_id[expected_ids[0]],
                    strip_images_by_id=strip_images_by_id,
                    strip_paths_by_id=strip_paths_by_id,
                    fallback_resolver=fallback_resolver,
                    reason=str(exc),
                    sheet_name=sheet.sheet_name,
                    depth=depth,
                )
            raise exc

        mid = len(expected_ids) // 2
        halves = [expected_ids[:mid], expected_ids[mid:]]
        combined_slots: list[dict[str, Any]] = []
        combined_degraded_ids: list[int] = []
        combined_failed_slots: list[dict[str, Any]] = []
        total_prompt = 0
        total_completion = 0
        total_tokens = 0
        total_elapsed = 0.0
        any_remapped = False

        for part_idx, sub_ids in enumerate(halves, 1):
            sub_slots = [slots_by_id[sid] for sid in sub_ids]  # type: ignore[index]
            sub_name = f"{Path(sheet.sheet_name).stem}_d{depth + 1}_p{part_idx}.png"
            sub_bytes: bytes | None = None

            if has_ram_strips:
                sub_canvas = compose_contact_sheet_image(
                    sub_slots,
                    strip_images_by_id,  # type: ignore[arg-type]
                    sheet_index=sheet.sheet_index,
                    strip_w=strip_w,
                    strip_h=strip_h,
                )
                sub_bytes = encode_image_png_bytes(sub_canvas, compress_level=1)
                if save_rescue_to_disk:
                    rescue_dir.mkdir(parents=True, exist_ok=True)
                    sub_disk_path = rescue_dir / sub_name
                    sub_disk_path.write_bytes(sub_bytes)
                    sub_sheet_path = str(sub_disk_path)
                else:
                    sub_sheet_path = f"memory://{sub_name}"
                sub_spec = SheetSpec(
                    sheet_index=sheet.sheet_index,
                    sheet_name=sub_name,
                    sheet_path=sub_sheet_path,
                    expected_slot_ids=[s.slot_id for s in sub_slots],
                )
            else:
                sub_path = rescue_dir / sub_name
                sub_spec = render_single_contact_sheet(
                    sub_slots,
                    strip_paths_by_id,  # type: ignore[arg-type]
                    sub_path,
                    sheet_index=sheet.sheet_index,
                    strip_w=strip_w,
                    strip_h=strip_h,
                )

            sub_res = _transcribe_sheet_with_split_rescue(
                config=config,
                sheet=sub_spec,
                policy=policy,
                script=script,
                slots_by_id=slots_by_id,
                strip_paths_by_id=strip_paths_by_id,
                rescue_dir=rescue_dir,
                strip_w=strip_w,
                strip_h=strip_h,
                sheet_png_bytes=sub_bytes,
                strip_images_by_id=strip_images_by_id,
                save_rescue_to_disk=save_rescue_to_disk,
                depth=depth + 1,
                max_depth=max_depth,
                rate_gate=rate_gate,
                fallback_resolver=fallback_resolver,
            )
            combined_slots.extend(sub_res["slots"])
            combined_degraded_ids.extend(int(x) for x in (sub_res.get("degraded_slot_ids") or []))
            combined_failed_slots.extend(sub_res.get("failed_slots") or [])
            total_elapsed += float(sub_res.get("elapsed_s", 0.0))
            any_remapped = any_remapped or bool(sub_res.get("remapped_local_ids"))
            u = sub_res.get("usage") or {}
            total_prompt += int(u.get("prompt_tokens", 0) or 0)
            total_completion += int(u.get("completion_tokens", 0) or 0)
            total_tokens += int(u.get("total_tokens", 0) or 0)

        return {
            "sheet": sheet.sheet_name,
            "expected_slot_ids": expected_ids,
            "remapped_local_ids": any_remapped,
            "split_rescued": True,
            "elapsed_s": round(total_elapsed, 3),
            "usage": {
                "prompt_tokens": total_prompt,
                "completion_tokens": total_completion,
                "total_tokens": total_tokens,
            },
            "degraded_slot_ids": sorted(set(combined_degraded_ids)),
            "failed_slots": combined_failed_slots,
            "slots": combined_slots,
        }


def _write_sheet_raw_response(
    work_dir: Path,
    batch_res: dict[str, Any],
    *,
    model: str | None = None,
) -> None:
    sheet_name = str(batch_res.get("sheet") or "").strip()
    if not sheet_name:
        return
    raw_dir = work_dir / "raw_responses"
    raw_dir.mkdir(parents=True, exist_ok=True)
    stem = Path(sheet_name).stem
    payload = batch_res if model is None or "model" in batch_res else {"model": model, **batch_res}
    out_path = raw_dir / f"{stem}.json"
    tmp_path = raw_dir / f".{stem}.tmp.json"
    tmp_path.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    tmp_path.replace(out_path)


def _load_cached_by_sheet(work_dir: Path, model: str) -> dict[str, dict[str, Any]]:
    cached_by_sheet: dict[str, dict[str, Any]] = {}
    for cand_file in (work_dir / "vlm_responses.json", work_dir / "vlm_responses.partial.json"):
        if cand_file.is_file() and cand_file.stat().st_size > 0:
            try:
                loaded = json.loads(cand_file.read_text(encoding="utf-8"))
                if loaded.get("model") and loaded.get("model") != model:
                    continue
                for b in loaded.get("batches", []):
                    if isinstance(b, dict) and b.get("sheet") and not b.get("degraded_slot_ids"):
                        cached_by_sheet.setdefault(str(b["sheet"]), b)
            except Exception:
                continue

    raw_dir = work_dir / "raw_responses"
    if raw_dir.is_dir():
        for raw_file in sorted(raw_dir.glob("sheet_*.json")):
            if raw_file.name.startswith("."):
                continue
            try:
                b = json.loads(raw_file.read_text(encoding="utf-8"))
                if not isinstance(b, dict) or not b.get("sheet"):
                    continue
                if b.get("model") and b.get("model") != model:
                    continue
                if not b.get("degraded_slot_ids"):
                    cached_by_sheet.setdefault(str(b["sheet"]), b)
            except Exception:
                continue
    return cached_by_sheet


def _finalize_failed_slots(work_dir: Path, batches: list[dict[str, Any]]) -> list[dict[str, Any]]:
    all_failed: list[dict[str, Any]] = []
    for batch in batches:
        for rec in batch.get("failed_slots") or []:
            if isinstance(rec, dict):
                all_failed.append(rec)
    all_failed.sort(key=lambda r: int(r.get("slot_id", 0)))
    failed_path = work_dir / "failed_slots.json"
    if all_failed:
        payload = {"failed_count": len(all_failed), "slots": all_failed}
        failed_path.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    elif failed_path.is_file():
        failed_path.unlink()
    return all_failed


def transcribe_contact_sheets(
    manifest: list[SheetSpec],
    out_dir: Path,
    *,
    config: VlmEndpointConfig,
    workers: int = 4,
    script: str = "auto",
    slots_by_id: dict[int, TimingSlot] | None = None,
    strip_paths_by_id: dict[int, Path] | None = None,
    strip_w: int = 960,
    strip_h: int = 90,
    fallback_resolver: SlotFallbackResolver | None = None,
    reuse_cache: bool = True,
    quiet: bool = False,
) -> tuple[dict[str, Any], list[TranscribedSlot]]:
    """Transcribe all Contact Sheets concurrently with incremental .partial.json checkpoints."""
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / "vlm_responses.json"
    partial_file = out_dir / "vlm_responses.partial.json"

    if reuse_cache and out_file.is_file() and out_file.stat().st_size > 0:
        try:
            cached = json.loads(out_file.read_text(encoding="utf-8"))
            batches = cached.get("batches", [])
            has_degraded = any(bool(b.get("degraded_slot_ids")) for b in batches if isinstance(b, dict))
            if (
                len(batches) == len(manifest)
                and not has_degraded
                and (not cached.get("model") or cached.get("model") == config.model)
            ):
                all_slots: list[TranscribedSlot] = []
                for batch, spec in zip(batches, manifest, strict=True):
                    validated, _ = validate_and_align_sheet_slots(
                        {"slots": batch["slots"]},
                        spec.expected_slot_ids,
                        script=script,
                        allow_placeholder=True,
                    )
                    all_slots.extend(validated)
                _log(
                    f"[Stage 3] Reused cached VLM responses ({len(manifest)} sheets) from {out_file.name}",
                    quiet=quiet,
                )
                return cached, all_slots
        except Exception:
            pass

    if not config.api_key and "127.0.0.1" not in config.endpoint and "localhost" not in config.endpoint:
        raise ValueError(
            "VLM API key is required. Configure MINIMAX_API_KEY in .env or pass --api-key."
        )

    total_sheets = len(manifest)
    if total_sheets == 0:
        empty_doc = {
            "model": config.model,
            "endpoint": config.endpoint,
            "thinking_mode": config.thinking_mode,
            "wall_s": 0.0,
            "sum_s": 0.0,
            "batches": [],
        }
        out_file.write_text(json.dumps(empty_doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        _finalize_failed_slots(out_dir, [])
        return empty_doc, []

    batches_by_idx: list[dict[str, Any] | None] = [None] * total_sheets
    if reuse_cache:
        cached_by_sheet = _load_cached_by_sheet(out_dir, config.model)
        for idx, spec in enumerate(manifest):
            cand = cached_by_sheet.get(spec.sheet_name)
            if cand is not None and cand.get("expected_slot_ids") == spec.expected_slot_ids:
                try:
                    validate_and_align_sheet_slots(
                        {"slots": cand.get("slots", [])}, spec.expected_slot_ids, script=script
                    )
                    batches_by_idx[idx] = cand
                except Exception:
                    pass

    completed_count = sum(1 for b in batches_by_idx if b is not None)
    pending_indices = [idx for idx, b in enumerate(batches_by_idx) if b is None]
    if completed_count > 0 and pending_indices:
        _log(
            f"[Stage 3] Resumed {completed_count}/{total_sheets} sheets from {partial_file.name}; "
            f"{len(pending_indices)} remaining...",
            quiet=quiet,
        )

    _log(
        f"[Stage 3] Transcribing {len(pending_indices)}/{total_sheets} sheets "
        f"(model={config.model}, thinking={config.thinking_mode}, workers={workers})...",
        quiet=quiet,
    )

    policy = _AdaptivePolicyState(config)
    rate_gate = _RateLimitGate()
    checkpoint_lock = threading.Lock()
    rescue_dir = out_dir / "sheets" / "rescue"
    t0 = time.perf_counter()

    if pending_indices:
        first_err: Exception | None = None
        with ThreadPoolExecutor(max_workers=max(1, min(workers, len(pending_indices)))) as pool:
            futs = {
                pool.submit(
                    _transcribe_sheet_with_split_rescue,
                    config=config,
                    sheet=manifest[idx],
                    policy=policy,
                    script=script,
                    slots_by_id=slots_by_id,
                    strip_paths_by_id=strip_paths_by_id,
                    rescue_dir=rescue_dir,
                    strip_w=strip_w,
                    strip_h=strip_h,
                    rate_gate=rate_gate,
                    fallback_resolver=fallback_resolver,
                ): idx
                for idx in pending_indices
            }
            for fut in as_completed(futs):
                idx = futs[fut]
                try:
                    batch_res = fut.result()
                except Exception as exc:
                    if first_err is None:
                        first_err = exc
                    continue

                with checkpoint_lock:
                    batches_by_idx[idx] = batch_res
                    _write_sheet_raw_response(out_dir, batch_res, model=config.model)
                    done_now = sum(1 for b in batches_by_idx if b is not None)
                    ready = [b for b in batches_by_idx if b is not None]
                    partial_payload = {
                        "model": config.model,
                        "endpoint": config.endpoint,
                        "thinking_mode": config.thinking_mode,
                        "completed_sheets": done_now,
                        "total_sheets": total_sheets,
                        "batches": ready,
                    }
                    partial_file.write_text(
                        json.dumps(partial_payload, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8",
                    )

                exp = batch_res.get("expected_slot_ids") or []
                span = f"#{exp[0]:03d}..#{exp[-1]:03d}" if exp else ""
                rescue_note = " [split-rescued]" if batch_res.get("split_rescued") else ""
                degraded_ids = batch_res.get("degraded_slot_ids") or []
                degraded_note = f" [degraded:{len(degraded_ids)}]" if degraded_ids else ""
                _log(
                    f"[VLM] {done_now}/{total_sheets} {batch_res['sheet']} ({span}, "
                    f"{batch_res['elapsed_s']:.2f}s{rescue_note}{degraded_note})",
                    quiet=quiet,
                )

        if first_err is not None:
            raise first_err

    batches = [b for b in batches_by_idx if b is not None]
    wall_s = time.perf_counter() - t0
    sum_s = sum(float(b.get("elapsed_s", 0.0)) for b in batches)
    doc: dict[str, Any] = {
        "model": config.model,
        "endpoint": config.endpoint,
        "thinking_mode": config.thinking_mode,
        "wall_s": round(wall_s, 3),
        "sum_s": round(sum_s, 3),
        "batches": batches,
    }
    out_file.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    _finalize_failed_slots(out_dir, batches)
    if partial_file.is_file():
        partial_file.unlink()

    all_transcribed: list[TranscribedSlot] = []
    for batch, spec in zip(batches, manifest, strict=True):
        validated, _ = validate_and_align_sheet_slots(
            {"slots": batch["slots"]},
            spec.expected_slot_ids,
            script=script,
            allow_placeholder=True,
        )
        all_transcribed.extend(validated)

    _log(
        f"[Stage 3] Completed {total_sheets} sheets in {wall_s:.2f}s (api_sum={sum_s:.2f}s)",
        quiet=quiet,
    )
    return doc, all_transcribed


def transcribe_streaming_sheets(
    slot_stream: Iterable[TimingSlot],
    prepare_sheet_fn: Callable[[int, list[TimingSlot]], PreparedSheet],
    work_dir: Path,
    *,
    config: VlmEndpointConfig,
    slots_per_sheet: int = 10,
    script: str = "auto",
    strip_w: int = 960,
    strip_h: int = 90,
    extract_workers: int = 8,
    vlm_workers: int = 10,
    max_in_flight_sheets: int | None = None,
    fallback_resolver: SlotFallbackResolver | None = None,
    save_debug_sheets: bool = False,
    reuse_cache: bool = True,
    tail_hedge_min_s: float = 12.0,
    tail_hedge_factor: float = 3.5,
    quiet: bool = False,
) -> tuple[dict[str, Any], list[TranscribedSlot], list[TimingSlot], dict[str, Any]]:
    """Stream TimingSlots -> bounded in-memory Contact Sheets -> concurrent VLM OCR."""
    if slots_per_sheet <= 0:
        raise ValueError(f"slots_per_sheet must be positive, got {slots_per_sheet}")
    if (
        not config.api_key
        and "127.0.0.1" not in config.endpoint
        and "localhost" not in config.endpoint
    ):
        raise ValueError(
            "VLM API key is required. Configure MINIMAX_API_KEY in .env or pass --api-key."
        )

    work_dir.mkdir(parents=True, exist_ok=True)
    out_file = work_dir / "vlm_responses.json"
    partial_file = work_dir / "vlm_responses.partial.json"

    cached_by_sheet = _load_cached_by_sheet(work_dir, config.model) if reuse_cache else {}

    eff_extract_workers = max(1, extract_workers)
    eff_vlm_workers = max(1, vlm_workers)
    eff_max_in_flight = max(
        1,
        max_in_flight_sheets
        if max_in_flight_sheets is not None
        else max(eff_vlm_workers * 2, eff_extract_workers + 4),
    )

    in_flight_sem = threading.BoundedSemaphore(eff_max_in_flight)
    cancel_event = threading.Event()
    state_lock = threading.Lock()
    checkpoint_lock = threading.Lock()
    policy = _AdaptivePolicyState(config)
    rate_gate = _RateLimitGate()
    rescue_dir = work_dir / "sheets" / "rescue"

    t_pipeline0 = time.perf_counter()
    results_by_idx: dict[int, dict[str, Any]] = {}
    specs_by_idx: dict[int, SheetSpec] = {}
    first_err: Exception | None = None
    first_sheet_dispatched_at_s: float | None = None
    vlm_started_at: float | None = None
    decode_sum_s = 0.0
    encode_sum_s = 0.0
    total_sheet_png_bytes = 0
    in_memory_sheets_encoded = 0
    total_dispatched = 0
    cached_hits = 0
    timing_stream_done = False
    all_batches_dispatched = False
    known_total_sheets = 0
    completed_sheet_latencies: list[float] = []
    hedged_requests = 0
    hedged_wins = 0

    def _write_partial_checkpoint(streaming_complete: bool) -> None:
        with state_lock:
            ordered = [results_by_idx[idx] for idx in sorted(results_by_idx)]
            total_known = max(known_total_sheets, total_dispatched, len(ordered))
        payload = {
            "model": config.model,
            "endpoint": config.endpoint,
            "thinking_mode": config.thinking_mode,
            "completed_sheets": len(ordered),
            "total_sheets": total_known,
            "streaming_timing_complete": streaming_complete,
            "batches": ordered,
        }
        tmp_partial = partial_file.with_suffix(".json.tmp")
        with checkpoint_lock:
            tmp_partial.write_text(
                json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
            tmp_partial.replace(partial_file)

    def _run_vlm_and_release(prepared: PreparedSheet) -> None:
        nonlocal first_err, decode_sum_s, encode_sum_s
        nonlocal total_sheet_png_bytes, in_memory_sheets_encoded
        nonlocal hedged_requests, hedged_wins
        png_bytes_ref = prepared.png_bytes
        strip_images_ref = dict(prepared.strip_images)
        batch_slots_map = {s.slot_id: s for s in prepared.batch_slots}
        spec_ref = prepared.spec
        try:
            outcome_q: queue.Queue[tuple[str, dict[str, Any] | None, Exception | None]] = queue.Queue()

            def _exec_primary() -> None:
                try:
                    out = _transcribe_sheet_with_split_rescue(
                        config=config,
                        sheet=spec_ref,
                        policy=policy,
                        script=script,
                        slots_by_id=batch_slots_map,
                        strip_paths_by_id=None,
                        rescue_dir=rescue_dir,
                        strip_w=strip_w,
                        strip_h=strip_h,
                        sheet_png_bytes=png_bytes_ref,
                        strip_images_by_id=strip_images_ref,
                        save_rescue_to_disk=save_debug_sheets,
                        rate_gate=rate_gate,
                        fallback_resolver=fallback_resolver,
                    )
                    outcome_q.put(("primary", out, None))
                except Exception as exc:
                    outcome_q.put(("primary", None, exc))

            def _exec_hedge() -> None:
                try:
                    out = _call_chat_completions(
                        config=config,
                        sheet=spec_ref,
                        policy=policy,
                        script=script,
                        sheet_png_bytes=png_bytes_ref,
                        rate_gate=rate_gate,
                    )
                    outcome_q.put(("hedge", out, None))
                except Exception as exc:
                    outcome_q.put(("hedge", None, exc))

            attempt_t0 = time.perf_counter()
            threading.Thread(target=_exec_primary, daemon=True).start()

            expected_outcomes = 1
            received_outcomes = 0
            hedge_launched = False
            primary_err: Exception | None = None
            res: dict[str, Any] | None = None
            winner_tag = "primary"

            while received_outcomes < expected_outcomes:
                try:
                    tag, item_res, item_err = outcome_q.get(timeout=0.05)
                except queue.Empty:
                    if cancel_event.is_set():
                        break
                    if not hedge_launched and not rate_gate.is_rate_limited():
                        elapsed_attempt = time.perf_counter() - attempt_t0
                        should_launch = False
                        with state_lock:
                            pending_sheets = total_dispatched - len(results_by_idx)
                            lat_cnt = len(completed_sheet_latencies)
                            median_s = (
                                sorted(completed_sheet_latencies)[lat_cnt // 2]
                                if lat_cnt >= 3
                                else 0.0
                            )
                            threshold_s = max(tail_hedge_min_s, tail_hedge_factor * median_s)
                            if (
                                all_batches_dispatched
                                and 1 <= pending_sheets <= min(3, eff_vlm_workers)
                                and hedged_requests < 2
                                and elapsed_attempt >= threshold_s
                            ):
                                hedged_requests += 1
                                hedge_launched = True
                                should_launch = True
                        if should_launch:
                            expected_outcomes = 2
                            _log(
                                f"[VLM] Speculative tail hedge launched for {spec_ref.sheet_name} at +{elapsed_attempt:.1f}s",
                                quiet=quiet,
                            )
                            threading.Thread(target=_exec_hedge, daemon=True).start()
                    continue

                received_outcomes += 1
                if item_err is None and item_res is not None:
                    res = item_res
                    winner_tag = tag
                    break
                if isinstance(item_err, FatalVlmError):
                    raise item_err
                if tag == "primary" or primary_err is None:
                    primary_err = item_err

            if res is None:
                if primary_err is not None:
                    raise primary_err
                if cancel_event.is_set():
                    return
                raise VlmApiError(f"Sheet {spec_ref.sheet_name} produced no result")

            if winner_tag == "hedge":
                res = dict(res)
                res["hedged_win"] = True

            _write_sheet_raw_response(work_dir, res, model=config.model)
            with state_lock:
                if winner_tag == "hedge":
                    hedged_wins += 1
                results_by_idx[spec_ref.sheet_index] = res
                specs_by_idx[spec_ref.sheet_index] = spec_ref
                completed_sheet_latencies.append(float(res.get("elapsed_s", 0.0)))
                decode_sum_s += prepared.decode_s
                encode_sum_s += prepared.encode_s
                in_memory_sheets_encoded += 1
                total_sheet_png_bytes += len(png_bytes_ref)
                done_now = len(results_by_idx)
                total_display = max(known_total_sheets, total_dispatched, done_now)
            _write_partial_checkpoint(False)
            exp = res.get("expected_slot_ids") or []
            span = f"#{exp[0]:03d}..#{exp[-1]:03d}" if exp else ""
            rescue_note = " [split-rescued]" if res.get("split_rescued") else ""
            hedge_note = " [hedged-win]" if winner_tag == "hedge" else ""
            degraded_ids = res.get("degraded_slot_ids") or []
            degraded_note = f" [degraded:{len(degraded_ids)}]" if degraded_ids else ""
            _log(
                f"[VLM] {done_now}/{total_display} {res['sheet']} ({span}, "
                f"{res['elapsed_s']:.2f}s{rescue_note}{hedge_note}{degraded_note})",
                quiet=quiet,
            )
        except Exception as exc:
            with state_lock:
                if first_err is None:
                    first_err = exc
            cancel_event.set()
        finally:
            # Rebind rather than clearing in place so a losing hedged thread still holds valid refs.
            prepared.strip_images = {}
            prepared.png_bytes = b""
            in_flight_sem.release()

    def _decode_and_dispatch(
        sheet_idx: int,
        batch_slots: list[TimingSlot],
        vlm_pool: ThreadPoolExecutor,
    ) -> Future[None] | None:
        nonlocal first_err, first_sheet_dispatched_at_s, vlm_started_at
        if cancel_event.is_set():
            in_flight_sem.release()
            return None
        try:
            prepared = prepare_sheet_fn(sheet_idx, batch_slots)
        except Exception as exc:
            with state_lock:
                if first_err is None:
                    first_err = exc
            cancel_event.set()
            in_flight_sem.release()
            return None

        now = time.perf_counter()
        with state_lock:
            if first_sheet_dispatched_at_s is None:
                first_sheet_dispatched_at_s = now - t_pipeline0
                vlm_started_at = now
                _log(
                    f"[Stream] First sheet {prepared.spec.sheet_name} dispatched to VLM at +{first_sheet_dispatched_at_s:.2f}s",
                    quiet=quiet,
                )

        try:
            return vlm_pool.submit(_run_vlm_and_release, prepared)
        except Exception as exc:
            prepared.strip_images = {}
            prepared.png_bytes = b""
            with state_lock:
                if first_err is None:
                    first_err = exc
            cancel_event.set()
            in_flight_sem.release()
            return None

    all_slots: list[TimingSlot] = []
    decode_futs: list[Future[Future[None] | None]] = []
    vlm_futs: list[Future[None]] = []
    sheet_idx = 0
    batch_buf: list[TimingSlot] = []

    def _submit_batch(
        batch_slots: list[TimingSlot],
        decode_pool: ThreadPoolExecutor,
        vlm_pool: ThreadPoolExecutor,
    ) -> None:
        nonlocal sheet_idx, total_dispatched, cached_hits
        sheet_idx += 1
        cur_idx = sheet_idx
        expected_ids = [s.slot_id for s in batch_slots]
        sheet_name = f"sheet_{cur_idx:02d}.png"
        sheet_path = str(work_dir / "sheets" / sheet_name) if save_debug_sheets else f"memory://{sheet_name}"
        spec = SheetSpec(
            sheet_index=cur_idx,
            sheet_name=sheet_name,
            sheet_path=sheet_path,
            expected_slot_ids=expected_ids,
        )

        with state_lock:
            total_dispatched += 1

        cached_cand = cached_by_sheet.get(sheet_name)
        if cached_cand is not None and cached_cand.get("expected_slot_ids") == expected_ids:
            try:
                validate_and_align_sheet_slots(
                    {"slots": cached_cand.get("slots", [])}, expected_ids, script=script
                )
                with state_lock:
                    results_by_idx[cur_idx] = cached_cand
                    specs_by_idx[cur_idx] = spec
                    cached_hits += 1
                return
            except Exception:
                pass

        acquired = False
        while not cancel_event.is_set():
            if in_flight_sem.acquire(timeout=0.05):
                acquired = True
                break
        if not acquired:
            return

        try:
            fut = decode_pool.submit(_decode_and_dispatch, cur_idx, batch_slots, vlm_pool)
        except Exception:
            in_flight_sem.release()
            raise
        decode_futs.append(fut)

    t_timing_done = t_pipeline0
    slot_q: queue.Queue[tuple[str, Any]] = queue.Queue()

    def _pump_slots() -> None:
        nonlocal t_timing_done, timing_stream_done, known_total_sheets
        pumped_count = 0
        pump_err = False
        try:
            for slot in slot_stream:
                if cancel_event.is_set():
                    break
                pumped_count += 1
                slot_q.put(("slot", slot))
        except Exception as exc:
            pump_err = True
            slot_q.put(("err", exc))
        finally:
            close_fn = getattr(slot_stream, "close", None)
            if callable(close_fn):
                try:
                    close_fn()
                except Exception:
                    pass
            t_timing_done = time.perf_counter()
            with state_lock:
                timing_stream_done = True
                if not cancel_event.is_set() and not pump_err:
                    known_total_sheets = (pumped_count + slots_per_sheet - 1) // slots_per_sheet
            slot_q.put(("eof", None))

    decode_pool = ThreadPoolExecutor(max_workers=eff_extract_workers)
    vlm_pool = ThreadPoolExecutor(max_workers=eff_vlm_workers)
    pump_thread = threading.Thread(target=_pump_slots, daemon=True)
    pump_thread.start()
    try:
        while not cancel_event.is_set():
            try:
                kind, payload = slot_q.get(timeout=0.05)
            except queue.Empty:
                continue
            if kind == "err":
                cancel_event.set()
                with state_lock:
                    has_prior_err = first_err is not None
                if not has_prior_err:
                    raise payload
                break
            if kind == "eof":
                break
            slot = payload
            all_slots.append(slot)
            batch_buf.append(slot)
            if len(batch_buf) >= slots_per_sheet:
                _submit_batch(batch_buf, decode_pool, vlm_pool)
                batch_buf = []

        if batch_buf and not cancel_event.is_set():
            _submit_batch(batch_buf, decode_pool, vlm_pool)
            batch_buf = []

        with state_lock:
            all_batches_dispatched = True

        for d_fut in decode_futs:
            v_fut = d_fut.result()
            if v_fut is not None:
                vlm_futs.append(v_fut)

        for v_fut in vlm_futs:
            v_fut.result()
    finally:
        cancel_event.set()
        pump_thread.join(timeout=5.0)
        vlm_pool.shutdown(wait=False, cancel_futures=True)
        decode_pool.shutdown(wait=True, cancel_futures=True)

    if first_err is not None:
        raise first_err

    t_end = time.perf_counter()
    ordered_indices = sorted(results_by_idx.keys())
    batches = [results_by_idx[idx] for idx in ordered_indices]
    manifest = [specs_by_idx[idx] for idx in ordered_indices]

    if save_debug_sheets:
        sheet_dir = work_dir / "sheets"
        sheet_dir.mkdir(parents=True, exist_ok=True)
        manifest_doc = [
            {
                "sheet_index": s.sheet_index,
                "sheet_name": s.sheet_name,
                "sheet_path": s.sheet_path,
                "expected_slot_ids": s.expected_slot_ids,
            }
            for s in manifest
        ]
        (sheet_dir / "sheets_manifest.json").write_text(
            json.dumps(manifest_doc, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )

    stage3_vlm_wall_s = (t_end - vlm_started_at) if vlm_started_at is not None else 0.0
    sum_s = sum(float(b.get("elapsed_s", 0.0)) for b in batches)
    doc: dict[str, Any] = {
        "model": config.model,
        "endpoint": config.endpoint,
        "thinking_mode": config.thinking_mode,
        "wall_s": round(stage3_vlm_wall_s, 3),
        "sum_s": round(sum_s, 3),
        "batches": batches,
    }
    out_file.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    all_failed_slots = _finalize_failed_slots(work_dir, batches)
    if partial_file.is_file():
        partial_file.unlink()

    all_transcribed: list[TranscribedSlot] = []
    for batch, spec in zip(batches, manifest, strict=True):
        validated, _ = validate_and_align_sheet_slots(
            {"slots": batch["slots"]},
            spec.expected_slot_ids,
            script=script,
            allow_placeholder=True,
        )
        all_transcribed.extend(validated)

    stats: dict[str, Any] = {
        "pipeline_wall_s": round(t_end - t_pipeline0, 3),
        "stage1_timing_wall_s": round(t_timing_done - t_pipeline0, 3),
        "first_sheet_dispatched_at_s": (
            round(first_sheet_dispatched_at_s, 3) if first_sheet_dispatched_at_s is not None else None
        ),
        "stage2_decode_sum_s": round(decode_sum_s, 3),
        "stage2_sheet_encode_sum_s": round(encode_sum_s, 3),
        "stage3_vlm_wall_s": round(stage3_vlm_wall_s, 3),
        "stage3_tail_after_timing_s": round(max(0.0, t_end - t_timing_done), 3),
        "extract_workers": eff_extract_workers,
        "vlm_workers": eff_vlm_workers,
        "max_in_flight_sheets": eff_max_in_flight,
        "total_sheets": len(manifest),
        "in_memory_sheets_encoded": in_memory_sheets_encoded,
        "cached_sheet_hits": cached_hits,
        "total_sheet_png_bytes": total_sheet_png_bytes,
        "degraded_slots": len(all_failed_slots),
        "failed_slots": all_failed_slots,
        "hedged_requests": hedged_requests,
        "hedged_wins": hedged_wins,
    }
    _log(
        f"[Stream] Completed {len(manifest)} sheets ({len(all_slots)} slots) in {t_end - t_pipeline0:.2f}s "
        f"(first_sheet=+{stats['first_sheet_dispatched_at_s'] or 0:.2f}s, "
        f"tail_after_timing={stats['stage3_tail_after_timing_s']:.2f}s, api_sum={sum_s:.2f}s)",
        quiet=quiet,
    )
    return doc, all_transcribed, all_slots, stats


def summarize_vlm_usage(vlm_doc: dict[str, Any]) -> dict[str, Any]:
    """Summarize token usage, latency, and recovery stats from vlm_responses.json."""
    batches = vlm_doc.get("batches", [])
    prompt_tok = sum(int((b.get("usage") or {}).get("prompt_tokens", 0) or 0) for b in batches)
    compl_tok = sum(int((b.get("usage") or {}).get("completion_tokens", 0) or 0) for b in batches)
    total_tok = sum(int((b.get("usage") or {}).get("total_tokens", 0) or 0) for b in batches)
    reasoning_tok = sum(
        int(((b.get("usage") or {}).get("completion_tokens_details") or {}).get("reasoning_tokens", 0) or 0)
        for b in batches
    )
    return {
        "wall_s": vlm_doc.get("wall_s", 0.0),
        "sum_s": vlm_doc.get("sum_s", 0.0),
        "prompt_tokens": prompt_tok,
        "completion_tokens": compl_tok,
        "reasoning_tokens": reasoning_tok,
        "total_tokens": total_tok or (prompt_tok + compl_tok),
        "remapped_sheets": sum(1 for b in batches if b.get("remapped_local_ids")),
        "split_rescued_sheets": sum(1 for b in batches if b.get("split_rescued")),
        "degraded_slots": sum(len(b.get("degraded_slot_ids") or []) for b in batches),
    }
