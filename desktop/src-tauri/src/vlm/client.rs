use std::collections::{HashMap, HashSet};
use std::future::Future;
use std::pin::Pin;
use std::sync::{Arc, Mutex as StdMutex};
use std::time::{Duration, Instant};

use async_trait::async_trait;
use base64::Engine as _;
use reqwest::header::{HeaderName, HeaderValue};
use serde_json::{json, Value};
use tokio::sync::watch;

use crate::dto::{CustomHeaderDto, ProviderConnectionTestDto, SubtitleScript, ThinkingMode};

use super::checkpoint::{FailedSlotRecord, SheetBatchRecord, SheetSlotJson};
use super::image::{encode_png_fast, RgbImage};
use super::rate_limit::{
    is_content_filter_error, is_fatal_http, is_rate_limit_error, parse_retry_after,
    AdaptivePolicyState, RateLimitGate,
};
use super::schema::{
    extract_json_object, validate_and_align_sheet_slots, TranscribedSlot, PLACEHOLDER_CATEGORY,
    PLACEHOLDER_TEXT,
};
use super::sheet_builder::{
    render_subsheet, PreparedSheet, SheetSpec, TimingSlot, DEFAULT_STRIP_WIDTH,
};
use super::{wait_until_cancelled, VlmError};

pub const DEFAULT_TAIL_HEDGE_MIN_S: f64 = 12.0;
pub const DEFAULT_TAIL_HEDGE_FACTOR: f64 = 3.5;
pub const MAX_HEDGED_REQUESTS: u32 = 2;
pub const MAX_SPLIT_DEPTH: usize = 4;

pub const SYSTEM_PROMPT: &str = "\
你是一个影视硬字幕视觉 OCR 结构化转录专家。\n\
输入是一张单列从上到下排列的字幕抽帧拼图（Contact Sheet）：\n\
- 左侧是浅色标签栏，印有槽位编号 `#NNN`（如 `#001`）与秒级时间范围，右侧由蓝色竖线分隔的是视频字幕画面。\n\
- **严禁将左侧标签栏的 `#NNN` 编号或时间文字抄入字幕 `lines` 中**！\n\
\n\
请按编号顺序逐行读取右侧视频画面中的**前景硬字幕**，并严格遵守以下规则：\n\
1. **槽位编号严格一致 (`id`)**：每个条目的 `id` 必须等于左侧印制的 `#NNN` 整数编号（例如 `#041` 对应 `\"id\": 41`），严禁局部重新从 1 编号。\n\
2. **双行拆分 (`lines`)**：\n\
   - 单行字幕输出含 1 个字符串的数组，如 `[\"你好\"]`。\n\
   - 上下两行字幕必须按从上到下顺序拆为 2 个字符串，如 `[\"-什么？\", \"-怎么回事？\"]`。\n\
   - 双人对话行首连字符 `-` 紧跟文字（如 `\"-姐姐！\"`）。\n\
3. **忠实保留句内空格与标点**：\n\
   - 画面中分句之间以空格分隔（无逗号）时，必须原样保留单个半角空格 ` `（例如 `\"哦 对\"`、`\"你想起来了 对吗？\"`），严禁擅自删除或改成逗号。\n\
   - 原样保留画面中的繁体/简体字形、引号 `“` `”`、单字符省略号 `…` 及括号说明 `（...）`。\n\
4. **忽略背景干扰与 5 类分类 (`category`)**：\n\
   - 严禁转录背景中的非字幕文字，包括：韩文/日文/英文演职员表与制作字幕（如 `기획`、`제작`、`연출`、`촬영`、`극본`、导演/编剧名）、新闻背景滚屏、台标水印或道具招牌字。\n\
   - 若当前帧**既有前景中文字幕、又有背景演职员表/道具字**，只转录前景中文字幕，严禁将演职员表文字混入 `lines`。\n\
   - 若当前帧**只有**演职员表/道具招牌字而无前景字幕，标为 `\"scene_text\"` 且 `lines: []`（绝对不可标为 `\"dialogue\"`）。\n\
   - 常规前景台词标为 `\"dialogue\"`；括号人物/地点/前情提要/歌词注释标为 `\"bracket_note\"`；\n\
     仅有角标水印标为 `\"watermark\"`；无任何文字或仅有模糊背景标为 `\"empty\"` 且 `lines: []`。\n\
5. **严格输出 JSON 对象**：\n\
   仅输出合法 JSON，格式为 `{\"slots\": [{\"id\": 41, \"category\": \"dialogue\", \"lines\": [\"你好\"]}]}`。";

#[derive(Clone, Debug)]
pub struct VlmEndpointConfig {
    pub endpoint: String,
    pub api_key: String,
    pub model: String,
    pub thinking_mode: ThinkingMode,
    pub timeout_s: f64,
    pub max_retries: u32,
    pub custom_headers: Vec<CustomHeaderDto>,
    pub session_id: String,
}

impl Default for VlmEndpointConfig {
    fn default() -> Self {
        Self {
            endpoint: String::new(),
            api_key: String::new(),
            model: String::new(),
            thinking_mode: ThinkingMode::Auto,
            timeout_s: 120.0,
            max_retries: 4,
            custom_headers: Vec::new(),
            session_id: String::new(),
        }
    }
}

pub fn has_custom_auth_header(headers: &[CustomHeaderDto]) -> bool {
    headers.iter().any(|h| {
        if h.value.trim().is_empty() {
            return false;
        }
        let lower = h.name.trim().to_ascii_lowercase();
        matches!(
            lower.as_str(),
            "authorization" | "api-key" | "x-api-key" | "proxy-authorization"
        ) || lower.ends_with("-authorization")
    })
}

pub fn normalize_chat_completions_url(raw_url: &str) -> Result<String, VlmError> {
    let trimmed = raw_url.trim().trim_end_matches('/');
    if trimmed.is_empty() {
        return Err(VlmError::InvalidConfiguration(
            "VLM base_url must not be empty".into(),
        ));
    }
    if trimmed.ends_with("/chat/completions") {
        Ok(trimmed.to_string())
    } else {
        Ok(format!("{trimmed}/chat/completions"))
    }
}

pub fn validate_endpoint_and_key_with_headers(
    endpoint: &str,
    api_key: &str,
    custom_headers: &[CustomHeaderDto],
) -> Result<(), VlmError> {
    if api_key.trim().is_empty()
        && !has_custom_auth_header(custom_headers)
        && !endpoint.contains("127.0.0.1")
        && !endpoint.contains("localhost")
    {
        return Err(VlmError::InvalidConfiguration(
            "VLM API key is required for non-local endpoints".into(),
        ));
    }
    Ok(())
}

pub fn validate_endpoint_and_key(endpoint: &str, api_key: &str) -> Result<(), VlmError> {
    validate_endpoint_and_key_with_headers(endpoint, api_key, &[])
}

pub fn build_user_prompt(sheet_name: &str, expected_ids: &[u64]) -> String {
    let ids_list = format!(
        "[{}]",
        expected_ids
            .iter()
            .map(u64::to_string)
            .collect::<Vec<_>>()
            .join(", ")
    );
    format!(
        "请按顺序提取本张拼图（{sheet_name}）中全部 {} 个槽位的前景字幕，期望槽位 id 列表依次为 {ids_list}。严禁抄入左侧 `#NNN` 编号。仅输出 JSON 对象 {{\"slots\": [...]}}。",
        expected_ids.len()
    )
}

#[derive(Debug, Clone)]
pub enum VlmCallError {
    Fatal(String),
    ContentFilter(String),
    ImmediateSplit(String),
    Splittable(String),
    NonSplittable(String),
    Cancelled,
}

impl VlmCallError {
    pub fn is_fatal(&self) -> bool {
        matches!(self, Self::Fatal(_))
    }

    pub fn is_splittable(&self) -> bool {
        matches!(
            self,
            Self::ContentFilter(_) | Self::ImmediateSplit(_) | Self::Splittable(_)
        )
    }

    pub fn message(&self) -> &str {
        match self {
            Self::Fatal(m)
            | Self::ContentFilter(m)
            | Self::ImmediateSplit(m)
            | Self::Splittable(m)
            | Self::NonSplittable(m) => m.as_str(),
            Self::Cancelled => "VLM task cancelled",
        }
    }

    pub fn into_vlm_error(self) -> VlmError {
        match self {
            Self::Cancelled => VlmError::Cancelled,
            Self::Fatal(m) => VlmError::Failed {
                code: "VlmFatal".into(),
                message: m,
                retryable: false,
            },
            Self::ContentFilter(m) => VlmError::Failed {
                code: "VlmContentFilter".into(),
                message: m,
                retryable: false,
            },
            Self::ImmediateSplit(m) | Self::Splittable(m) | Self::NonSplittable(m) => {
                VlmError::Failed {
                    code: "VlmRequestFailed".into(),
                    message: m,
                    retryable: true,
                }
            }
        }
    }
}

#[async_trait]
pub trait LeafFallbackResolver: Send + Sync {
    async fn resolve_failed_slot(
        &self,
        slot: &TimingSlot,
        strip_img: Option<&RgbImage>,
        reason: &str,
    ) -> Result<(TranscribedSlot, FailedSlotRecord), VlmError>;
}

pub fn extract_error_body(raw_bytes: &[u8]) -> String {
    let text = String::from_utf8_lossy(raw_bytes).trim().to_string();
    if let Ok(obj) = serde_json::from_str::<Value>(&text) {
        if let Some(err) = obj.get("error") {
            if let Some(msg) = err.get("message").and_then(Value::as_str) {
                if !msg.is_empty() {
                    return msg.to_string();
                }
            }
            if let Some(msg) = err.as_str() {
                if !msg.is_empty() {
                    return msg.to_string();
                }
            }
        }
        if let Some(msg) = obj.get("message").and_then(Value::as_str) {
            if !msg.is_empty() {
                return msg.to_string();
            }
        }
    }
    text.chars().take(300).collect()
}

#[derive(Clone)]
pub struct VlmSessionContext {
    pub http: reqwest::Client,
    pub config: VlmEndpointConfig,
    pub policy: Arc<AdaptivePolicyState>,
    pub rate_gate: Arc<RateLimitGate>,
    pub script: SubtitleScript,
}

fn apply_endpoint_headers(
    mut req: reqwest::RequestBuilder,
    config: &VlmEndpointConfig,
) -> Result<reqwest::RequestBuilder, VlmCallError> {
    if !config.api_key.trim().is_empty() {
        req = req.header("Authorization", format!("Bearer {}", config.api_key));
    }

    let fallback_session_id = if config.session_id.trim().is_empty() {
        uuid::Uuid::new_v4().to_string()
    } else {
        String::new()
    };
    let effective_session_id = if config.session_id.trim().is_empty() {
        fallback_session_id.as_str()
    } else {
        config.session_id.as_str()
    };
    for custom in &config.custom_headers {
        let name = custom.name.trim();
        if name.is_empty() {
            continue;
        }
        let raw_val = custom.value.trim();
        let resolved_val = if raw_val.contains("{{session_id}}") {
            raw_val.replace("{{session_id}}", effective_session_id)
        } else {
            raw_val.to_string()
        };
        let header_name = HeaderName::from_bytes(name.as_bytes()).map_err(|e| {
            VlmCallError::Fatal(format!("invalid custom header name '{name}': {e}"))
        })?;
        let header_value = HeaderValue::from_str(&resolved_val).map_err(|e| {
            VlmCallError::Fatal(format!("invalid custom header value for '{name}': {e}"))
        })?;
        req = req.header(header_name, header_value);
    }
    Ok(req)
}

fn extract_choice_text_preview(choice0: &Value) -> String {
    let raw = match choice0.pointer("/message/content") {
        Some(Value::String(s)) => s.clone(),
        Some(Value::Array(parts)) => parts
            .iter()
            .filter_map(|p| p.get("text").and_then(Value::as_str))
            .collect::<Vec<_>>()
            .join(" "),
        _ => String::new(),
    };
    let source = if raw.trim().is_empty() {
        choice0
            .pointer("/message/reasoning_content")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string()
    } else {
        raw
    };
    let collapsed = source.split_whitespace().collect::<Vec<_>>().join(" ");
    if collapsed.is_empty() {
        "OK".to_string()
    } else {
        collapsed.chars().take(60).collect()
    }
}

pub async fn probe_vlm_connectivity(
    http: &reqwest::Client,
    config: &VlmEndpointConfig,
) -> Result<ProviderConnectionTestDto, VlmError> {
    let probe_img = RgbImage::new_filled(32, 16, [24, 24, 28]);
    let png_bytes = encode_png_fast(&probe_img).map_err(|e| VlmError::Failed {
        code: "VlmImageError".into(),
        message: format!("生成连通性测试图像失败: {e}"),
        retryable: false,
    })?;
    let image_data_uri = format!(
        "data:image/png;base64,{}",
        base64::engine::general_purpose::STANDARD.encode(&png_bytes)
    );

    let policy = AdaptivePolicyState::new(config.thinking_mode, &config.model, &config.endpoint);
    let timeout = Duration::from_secs_f64(config.timeout_s.clamp(1.0, 20.0));
    let mut last_err: Option<VlmError> = None;

    for _attempt in 0..3 {
        let (effort, _use_json_fmt) = policy.current_params();
        let mut payload = json!({
            "model": config.model,
            "temperature": 0.0,
            "max_tokens": 16,
            "messages": [
                {
                    "role": "user",
                    "content": [
                        { "type": "image_url", "image_url": { "url": image_data_uri } },
                        { "type": "text", "text": "Connectivity check. Reply with OK." }
                    ]
                }
            ]
        });
        if let Some(ref eff) = effort {
            payload["reasoning_effort"] = Value::String(eff.clone());
        }

        let req = http
            .post(&config.endpoint)
            .header("Content-Type", "application/json")
            .timeout(timeout);
        let req = apply_endpoint_headers(req, config).map_err(VlmCallError::into_vlm_error)?;

        let t0 = Instant::now();
        let resp = req.json(&payload).send().await.map_err(|e| VlmError::Failed {
            code: "VlmNetworkError".into(),
            message: format!("网络连接或超时失败: {e}"),
            retryable: true,
        })?;
        let status = resp.status().as_u16();
        let body_bytes = resp.bytes().await.map_err(|e| VlmError::Failed {
            code: "VlmNetworkError".into(),
            message: format!("读取响应体失败: {e}"),
            retryable: true,
        })?;
        let latency_ms = (t0.elapsed().as_millis() as u64).max(1);

        if !(200..300).contains(&status) {
            let err_msg = extract_error_body(&body_bytes);
            let lower = err_msg.to_ascii_lowercase();
            if (status == 400 || status == 422)
                && (lower.contains("reasoning") || lower.contains("2013"))
                && policy.downgrade_effort(effort.as_deref())
            {
                last_err = Some(VlmError::Failed {
                    code: "VlmHttpError".into(),
                    message: format!("HTTP {status}: {err_msg}"),
                    retryable: false,
                });
                continue;
            }
            let code = match status {
                401 | 403 => "VlmAuthError",
                429 => "VlmRateLimited",
                _ => "VlmHttpError",
            };
            return Err(VlmError::Failed {
                code: code.into(),
                message: format!("HTTP {status}: {err_msg}"),
                retryable: status == 429 || status >= 500,
            });
        }

        let raw_data = serde_json::from_slice::<Value>(&body_bytes).map_err(|e| VlmError::Failed {
            code: "VlmInvalidResponse".into(),
            message: format!("响应不是合法 JSON: {e}"),
            retryable: false,
        })?;

        if let Some(base_resp) = raw_data.get("base_resp").and_then(Value::as_object) {
            let code = base_resp
                .get("status_code")
                .and_then(Value::as_i64)
                .unwrap_or(0);
            if code != 0 {
                let b_msg = base_resp
                    .get("status_msg")
                    .and_then(Value::as_str)
                    .unwrap_or("upstream error");
                let err_code = if is_fatal_http(Some(200), b_msg, Some(code)) {
                    "VlmAuthError"
                } else {
                    "VlmHttpError"
                };
                return Err(VlmError::Failed {
                    code: err_code.into(),
                    message: format!("API base_resp ({code}): {b_msg}"),
                    retryable: false,
                });
            }
        }

        if raw_data.get("error").is_some_and(|v| !v.is_null()) {
            return Err(VlmError::Failed {
                code: "VlmHttpError".into(),
                message: extract_error_body(&body_bytes),
                retryable: false,
            });
        }

        let Some(choice0) = raw_data
            .get("choices")
            .and_then(Value::as_array)
            .and_then(|arr| arr.first())
        else {
            return Err(VlmError::Failed {
                code: "VlmInvalidResponse".into(),
                message: "响应缺少 choices 字段".into(),
                retryable: false,
            });
        };

        let response_preview = extract_choice_text_preview(choice0);
        return Ok(ProviderConnectionTestDto {
            latency_ms,
            endpoint: config.endpoint.clone(),
            model: config.model.clone(),
            response_preview,
        });
    }

    Err(last_err.unwrap_or_else(|| VlmError::Failed {
        code: "VlmHttpError".into(),
        message: "端点拒绝了推理参数降级尝试".into(),
        retryable: false,
    }))
}

pub async fn call_chat_completions(
    session: &VlmSessionContext,
    sheet: &SheetSpec,
    sheet_png_bytes: &[u8],
    cancel_rx: &mut watch::Receiver<bool>,
) -> Result<SheetBatchRecord, VlmCallError> {
    let http = &session.http;
    let config = &session.config;
    let policy = &session.policy;
    let rate_gate = &session.rate_gate;
    let script = session.script;
    let expected_ids = sheet.expected_slot_ids.clone();
    let b64 = base64::engine::general_purpose::STANDARD.encode(sheet_png_bytes);
    let image_data_uri = format!("data:image/png;base64,{b64}");
    let user_prompt = build_user_prompt(&sheet.sheet_name, &expected_ids);

    let max_attempts = config.max_retries.max(1);
    let max_rate_limit_retries = max_attempts.max(6);
    let mut transient_failures: u32 = 0;
    let mut rate_limit_failures: u32 = 0;
    let mut policy_adjustments: u32 = 0;

    loop {
        rate_gate
            .wait_if_needed(cancel_rx)
            .await
            .map_err(|_| VlmCallError::Cancelled)?;

        let (effort, use_json_fmt) = policy.current_params();
        let mut payload = json!({
            "model": config.model,
            "temperature": 0.01,
            "messages": [
                { "role": "system", "content": SYSTEM_PROMPT },
                {
                    "role": "user",
                    "content": [
                        { "type": "image_url", "image_url": { "url": image_data_uri } },
                        { "type": "text", "text": user_prompt }
                    ]
                }
            ],
            "max_tokens": 4096
        });
        if use_json_fmt {
            payload["response_format"] = json!({ "type": "json_object" });
        }
        if let Some(ref eff) = effort {
            payload["reasoning_effort"] = Value::String(eff.clone());
        }

        let req = http
            .post(&config.endpoint)
            .header("Content-Type", "application/json")
            .timeout(Duration::from_secs_f64(config.timeout_s.max(1.0)));
        let req = apply_endpoint_headers(req, config)?;

        let t0 = Instant::now();
        let resp_res = tokio::select! {
            biased;
            _ = wait_until_cancelled(cancel_rx) => return Err(VlmCallError::Cancelled),
            res = req.json(&payload).send() => res,
        };

        let attempt_err: VlmCallError = match resp_res {
            Err(net_err) => {
                transient_failures += 1;
                VlmCallError::NonSplittable(format!("network/timeout error: {net_err}"))
            }
            Ok(resp) => {
                let status = resp.status().as_u16();
                let headers = resp.headers().clone();
                let body_bytes = tokio::select! {
                    biased;
                    _ = wait_until_cancelled(cancel_rx) => return Err(VlmCallError::Cancelled),
                    b = resp.bytes() => b.unwrap_or_default(),
                };
                let elapsed_s = (t0.elapsed().as_secs_f64() * 1000.0).round() / 1000.0;

                if !(200..300).contains(&status) {
                    let retry_after = parse_retry_after(&headers);
                    let err_msg = extract_error_body(&body_bytes);
                    if is_fatal_http(Some(status), &err_msg, None) {
                        return Err(VlmCallError::Fatal(format!("HTTP {status}: {err_msg}")));
                    }
                    if is_rate_limit_error(Some(status), &err_msg, None) {
                        rate_limit_failures += 1;
                        if rate_limit_failures > max_rate_limit_retries {
                            return Err(VlmCallError::NonSplittable(format!(
                                "HTTP {status} rate-limit exhausted ({rate_limit_failures} retries): {err_msg}"
                            )));
                        }
                        rate_gate.record_429(retry_after, rate_limit_failures - 1);
                        continue;
                    }
                    if status == 400 || status == 422 {
                        if is_content_filter_error(&err_msg, None, None, None) {
                            return Err(VlmCallError::ContentFilter(format!(
                                "HTTP {status} content filter: {err_msg}"
                            )));
                        }
                        let lower = err_msg.to_ascii_lowercase();
                        if policy_adjustments < 4 {
                            if (lower.contains("reasoning") || lower.contains("2013"))
                                && policy.downgrade_effort(effort.as_deref())
                            {
                                policy_adjustments += 1;
                                continue;
                            }
                            if lower.contains("response_format")
                                && policy.disable_json_format(use_json_fmt)
                            {
                                policy_adjustments += 1;
                                continue;
                            }
                        }
                    }

                    transient_failures += 1;
                    if status >= 500 {
                        VlmCallError::NonSplittable(format!("HTTP {status}: {err_msg}"))
                    } else {
                        VlmCallError::Splittable(format!("HTTP {status}: {err_msg}"))
                    }
                } else {
                    match serde_json::from_slice::<Value>(&body_bytes) {
                        Err(json_err) => {
                            transient_failures += 1;
                            VlmCallError::Splittable(format!(
                                "invalid JSON response body: {json_err}"
                            ))
                        }
                        Ok(raw_data) => {
                            if let Some(base_resp) =
                                raw_data.get("base_resp").and_then(Value::as_object)
                            {
                                let code = base_resp
                                    .get("status_code")
                                    .and_then(Value::as_i64)
                                    .unwrap_or(0);
                                if code != 0 {
                                    let b_msg = base_resp
                                        .get("status_msg")
                                        .and_then(Value::as_str)
                                        .unwrap_or("upstream error");
                                    if is_fatal_http(Some(200), b_msg, Some(code)) {
                                        return Err(VlmCallError::Fatal(format!(
                                            "API base_resp fatal ({code}): {b_msg}"
                                        )));
                                    }
                                    if is_content_filter_error(b_msg, Some(code), None, None) {
                                        return Err(VlmCallError::ContentFilter(format!(
                                            "API base_resp content filter ({code}): {b_msg}"
                                        )));
                                    }
                                    if is_rate_limit_error(None, b_msg, Some(code)) {
                                        rate_limit_failures += 1;
                                        if rate_limit_failures > max_rate_limit_retries {
                                            return Err(VlmCallError::NonSplittable(format!(
                                                "API rate limit base_resp ({code}): {b_msg}"
                                            )));
                                        }
                                        rate_gate.record_429(None, rate_limit_failures - 1);
                                        continue;
                                    }
                                    transient_failures += 1;
                                    let err = VlmCallError::Splittable(format!(
                                        "API base_resp error ({code}): {b_msg}"
                                    ));
                                    if transient_failures >= max_attempts {
                                        return Err(err);
                                    }
                                    let wait_s = (1.0 * transient_failures as f64).min(4.0);
                                    tokio::select! {
                                        biased;
                                        _ = wait_until_cancelled(cancel_rx) => return Err(VlmCallError::Cancelled),
                                        _ = tokio::time::sleep(Duration::from_secs_f64(wait_s)) => {}
                                    }
                                    continue;
                                }
                            }

                            let choice0 = raw_data
                                .get("choices")
                                .and_then(Value::as_array)
                                .and_then(|arr| arr.first());
                            let Some(choice0) = choice0 else {
                                transient_failures += 1;
                                let err = VlmCallError::Splittable(
                                    "API response missing 'choices'".into(),
                                );
                                if transient_failures >= max_attempts {
                                    return Err(err);
                                }
                                let wait_s = (1.0 * transient_failures as f64).min(4.0);
                                tokio::select! {
                                    biased;
                                    _ = wait_until_cancelled(cancel_rx) => return Err(VlmCallError::Cancelled),
                                    _ = tokio::time::sleep(Duration::from_secs_f64(wait_s)) => {}
                                }
                                continue;
                            };

                            let finish_reason = choice0
                                .get("finish_reason")
                                .and_then(Value::as_str)
                                .unwrap_or("")
                                .trim()
                                .to_ascii_lowercase();
                            let msg = choice0.get("message").unwrap_or(&Value::Null);
                            let refusal = msg.get("refusal").and_then(Value::as_str);
                            if is_content_filter_error(
                                "",
                                None,
                                Some(finish_reason.as_str()),
                                refusal,
                            ) {
                                return Err(VlmCallError::ContentFilter(format!(
                                    "VLM refused/filtered (finish_reason={finish_reason:?}, refusal={refusal:?})"
                                )));
                            }

                            let content =
                                msg.get("content").and_then(Value::as_str).unwrap_or("");
                            let parsed_result = extract_json_object(content).and_then(|obj| {
                                validate_and_align_sheet_slots(&obj, &expected_ids, script, false)
                            });

                            match parsed_result {
                                Ok((validated, remapped)) => {
                                    rate_gate.record_success();
                                    return Ok(SheetBatchRecord {
                                        model: Some(config.model.clone()),
                                        sheet: sheet.sheet_name.clone(),
                                        expected_slot_ids: expected_ids,
                                        remapped_local_ids: remapped,
                                        split_rescued: false,
                                        hedged_win: false,
                                        reasoning_effort_used: effort
                                            .unwrap_or_else(|| "omitted".to_string()),
                                        elapsed_s,
                                        usage: raw_data
                                            .get("usage")
                                            .cloned()
                                            .unwrap_or_else(|| json!({})),
                                        degraded_slot_ids: Vec::new(),
                                        failed_slots: Vec::new(),
                                        slots: validated
                                            .into_iter()
                                            .map(|s| SheetSlotJson {
                                                id: s.slot_id,
                                                category: s.category,
                                                lines: s.lines,
                                            })
                                            .collect(),
                                    });
                                }
                                Err(parse_err) => {
                                    if finish_reason == "length" {
                                        return Err(VlmCallError::ImmediateSplit(format!(
                                            "truncated output (finish_reason='length'): {parse_err}"
                                        )));
                                    }
                                    transient_failures += 1;
                                    VlmCallError::Splittable(parse_err)
                                }
                            }
                        }
                    }
                }
            }
        };

        if transient_failures >= max_attempts {
            return Err(attempt_err);
        }
        let wait_s = (1.0 * transient_failures as f64).min(4.0);
        tokio::select! {
            biased;
            _ = wait_until_cancelled(cancel_rx) => return Err(VlmCallError::Cancelled),
            _ = tokio::time::sleep(Duration::from_secs_f64(wait_s)) => {}
        }
    }
}

#[derive(Clone)]
pub struct SplitRescueContext {
    pub session: VlmSessionContext,
    pub slots_by_id: Arc<HashMap<u64, TimingSlot>>,
    pub strip_images_by_id: Arc<HashMap<u64, Arc<RgbImage>>>,
    pub strip_w: u32,
    pub strip_h: u32,
    pub fallback_resolver: Option<Arc<dyn LeafFallbackResolver>>,
    pub allow_placeholder_leaf: bool,
}

#[derive(Debug)]
pub struct SplitRescueOutcome {
    pub batch: SheetBatchRecord,
    pub split_count: u32,
}

pub fn transcribe_sheet_with_split_rescue<'a>(
    ctx: &'a SplitRescueContext,
    sheet: SheetSpec,
    sheet_png_bytes: Arc<Vec<u8>>,
    depth: usize,
    cancel_rx: &'a mut watch::Receiver<bool>,
) -> Pin<Box<dyn Future<Output = Result<SplitRescueOutcome, VlmCallError>> + Send + 'a>> {
    Box::pin(async move {
        if *cancel_rx.borrow() {
            return Err(VlmCallError::Cancelled);
        }
        match call_chat_completions(&ctx.session, &sheet, &sheet_png_bytes, cancel_rx).await {
            Ok(batch) => Ok(SplitRescueOutcome {
                batch,
                split_count: 0,
            }),
            Err(err @ VlmCallError::Fatal(_)) | Err(err @ VlmCallError::Cancelled) => Err(err),
            Err(err) => {
                let expected_ids = &sheet.expected_slot_ids;
                let has_ram_strips = expected_ids
                    .iter()
                    .all(|id| ctx.strip_images_by_id.contains_key(id) && ctx.slots_by_id.contains_key(id));
                let can_split = err.is_splittable()
                    && depth < MAX_SPLIT_DEPTH
                    && expected_ids.len() > 1
                    && has_ram_strips;

                if !can_split {
                    if expected_ids.len() == 1 {
                        if let Some(slot) = ctx.slots_by_id.get(&expected_ids[0]) {
                            if let Some(resolver) = &ctx.fallback_resolver {
                                let strip_img = ctx.strip_images_by_id.get(&slot.slot_id).map(|a| a.as_ref());
                                let (t_slot, record) = resolver
                                    .resolve_failed_slot(slot, strip_img, err.message())
                                    .await
                                    .map_err(|ve| match ve {
                                        VlmError::Cancelled => VlmCallError::Cancelled,
                                        other => VlmCallError::NonSplittable(other.to_string()),
                                    })?;
                                return Ok(SplitRescueOutcome {
                                    batch: SheetBatchRecord {
                                        model: Some(ctx.session.config.model.clone()),
                                        sheet: sheet.sheet_name,
                                        expected_slot_ids: vec![slot.slot_id],
                                        remapped_local_ids: false,
                                        split_rescued: depth > 0,
                                        hedged_win: false,
                                        reasoning_effort_used: "fallback".into(),
                                        elapsed_s: 0.0,
                                        usage: json!({
                                            "prompt_tokens": 0,
                                            "completion_tokens": 0,
                                            "total_tokens": 0
                                        }),
                                        degraded_slot_ids: vec![slot.slot_id],
                                        failed_slots: vec![record],
                                        slots: vec![SheetSlotJson {
                                            id: t_slot.slot_id,
                                            category: t_slot.category,
                                            lines: t_slot.lines,
                                        }],
                                    },
                                    split_count: 0,
                                });
                            } else if ctx.allow_placeholder_leaf {
                                let record = FailedSlotRecord {
                                    slot_id: slot.slot_id,
                                    start_ms: slot.start_ms,
                                    end_ms: slot.end_ms,
                                    reason: err.message().to_string(),
                                    resolution: "placeholder".into(),
                                    fallback_text: PLACEHOLDER_TEXT.into(),
                                };
                                return Ok(SplitRescueOutcome {
                                    batch: SheetBatchRecord {
                                        model: Some(ctx.session.config.model.clone()),
                                        sheet: sheet.sheet_name,
                                        expected_slot_ids: vec![slot.slot_id],
                                        remapped_local_ids: false,
                                        split_rescued: depth > 0,
                                        hedged_win: false,
                                        reasoning_effort_used: "fallback".into(),
                                        elapsed_s: 0.0,
                                        usage: json!({
                                            "prompt_tokens": 0,
                                            "completion_tokens": 0,
                                            "total_tokens": 0
                                        }),
                                        degraded_slot_ids: vec![slot.slot_id],
                                        failed_slots: vec![record],
                                        slots: vec![SheetSlotJson {
                                            id: slot.slot_id,
                                            category: PLACEHOLDER_CATEGORY.into(),
                                            lines: vec![PLACEHOLDER_TEXT.into()],
                                        }],
                                    },
                                    split_count: 0,
                                });
                            }
                        }
                    }
                    return Err(err);
                }

                let mid = expected_ids.len() / 2;
                let halves = [&expected_ids[..mid], &expected_ids[mid..]];
                let mut combined_slots = Vec::with_capacity(expected_ids.len());
                let mut combined_degraded_ids = Vec::new();
                let mut combined_failed_slots = Vec::new();
                let mut total_prompt = 0u64;
                let mut total_completion = 0u64;
                let mut total_tokens = 0u64;
                let mut total_elapsed = 0.0f64;
                let mut any_remapped = false;
                let mut total_splits = 1u32;

                for (part_idx, sub_ids) in halves.iter().enumerate() {
                    let sub_slots: Vec<TimingSlot> =
                        sub_ids.iter().map(|id| ctx.slots_by_id[id].clone()).collect();
                    let (sub_spec, sub_png) = render_subsheet(
                        &sheet,
                        &sub_slots,
                        &ctx.strip_images_by_id,
                        ctx.strip_w.clamp(1, DEFAULT_STRIP_WIDTH),
                        ctx.strip_h,
                        depth,
                        part_idx + 1,
                    )
                    .map_err(VlmCallError::NonSplittable)?;

                    let sub_out = transcribe_sheet_with_split_rescue(
                        ctx,
                        sub_spec,
                        sub_png,
                        depth + 1,
                        cancel_rx,
                    )
                    .await?;

                    total_splits += sub_out.split_count;
                    combined_slots.extend(sub_out.batch.slots);
                    combined_degraded_ids.extend(sub_out.batch.degraded_slot_ids);
                    combined_failed_slots.extend(sub_out.batch.failed_slots);
                    total_elapsed += sub_out.batch.elapsed_s;
                    any_remapped = any_remapped || sub_out.batch.remapped_local_ids;

                    total_prompt += sub_out
                        .batch
                        .usage
                        .get("prompt_tokens")
                        .and_then(Value::as_u64)
                        .unwrap_or(0);
                    total_completion += sub_out
                        .batch
                        .usage
                        .get("completion_tokens")
                        .and_then(Value::as_u64)
                        .unwrap_or(0);
                    total_tokens += sub_out
                        .batch
                        .usage
                        .get("total_tokens")
                        .and_then(Value::as_u64)
                        .unwrap_or(0);
                }

                let dedup_degraded: Vec<u64> = {
                    let set: HashSet<u64> = combined_degraded_ids.into_iter().collect();
                    let mut v: Vec<u64> = set.into_iter().collect();
                    v.sort_unstable();
                    v
                };

                Ok(SplitRescueOutcome {
                    batch: SheetBatchRecord {
                        model: Some(ctx.session.config.model.clone()),
                        sheet: sheet.sheet_name,
                        expected_slot_ids: expected_ids.clone(),
                        remapped_local_ids: any_remapped,
                        split_rescued: true,
                        hedged_win: false,
                        reasoning_effort_used: "split".into(),
                        elapsed_s: (total_elapsed * 1000.0).round() / 1000.0,
                        usage: json!({
                            "prompt_tokens": total_prompt,
                            "completion_tokens": total_completion,
                            "total_tokens": total_tokens
                        }),
                        degraded_slot_ids: dedup_degraded,
                        failed_slots: combined_failed_slots,
                        slots: combined_slots,
                    },
                    split_count: total_splits,
                })
            }
        }
    })
}

#[derive(Debug, Default)]
pub struct HedgeCoordinatorState {
    pub all_batches_dispatched: bool,
    pub total_dispatched: usize,
    pub completed_count: usize,
    pub completed_latencies_s: Vec<f64>,
    pub hedged_requests: u32,
    pub hedged_wins: u32,
}

pub struct HedgeCoordinator {
    pub state: StdMutex<HedgeCoordinatorState>,
    pub eff_vlm_workers: usize,
    pub tail_hedge_min_s: f64,
    pub tail_hedge_factor: f64,
}

impl HedgeCoordinator {
    pub fn new(eff_vlm_workers: usize, tail_hedge_min_s: f64, tail_hedge_factor: f64) -> Self {
        Self {
            state: StdMutex::new(HedgeCoordinatorState::default()),
            eff_vlm_workers: eff_vlm_workers.max(1),
            tail_hedge_min_s,
            tail_hedge_factor,
        }
    }

    pub fn try_claim_hedge(&self, elapsed_s: f64, rate_gate: &RateLimitGate) -> bool {
        if rate_gate.is_rate_limited() {
            return false;
        }
        let mut guard = self.state.lock().expect("hedge coordinator lock poisoned");
        if !guard.all_batches_dispatched || guard.hedged_requests >= MAX_HEDGED_REQUESTS {
            return false;
        }
        let pending = guard.total_dispatched.saturating_sub(guard.completed_count);
        let max_pending = 3.min(self.eff_vlm_workers);
        if !(1 <= pending && pending <= max_pending) {
            return false;
        }
        let median_s = if guard.completed_latencies_s.len() >= 3 {
            let mut sorted = guard.completed_latencies_s.clone();
            sorted.sort_by(|a, b| a.total_cmp(b));
            sorted[sorted.len() / 2]
        } else {
            0.0
        };
        let threshold_s = self
            .tail_hedge_min_s
            .max(self.tail_hedge_factor * median_s);
        if elapsed_s >= threshold_s {
            guard.hedged_requests += 1;
            true
        } else {
            false
        }
    }

    pub fn record_completion(&self, elapsed_s: f64, hedged_win: bool) {
        let mut guard = self.state.lock().expect("hedge coordinator lock poisoned");
        guard.completed_count += 1;
        guard.completed_latencies_s.push(elapsed_s);
        if hedged_win {
            guard.hedged_wins += 1;
        }
    }

    pub fn record_cached_hit(&self) {
        let mut guard = self.state.lock().expect("hedge coordinator lock poisoned");
        guard.completed_count += 1;
    }
}

pub async fn transcribe_sheet_with_tail_hedging(
    prepared: PreparedSheet,
    session: VlmSessionContext,
    fallback_resolver: Option<Arc<dyn LeafFallbackResolver>>,
    allow_placeholder_leaf: bool,
    hedge_coord: Arc<HedgeCoordinator>,
    mut cancel_rx: watch::Receiver<bool>,
) -> Result<SplitRescueOutcome, VlmCallError> {
    if *cancel_rx.borrow() {
        return Err(VlmCallError::Cancelled);
    }

    let slots_by_id: Arc<HashMap<u64, TimingSlot>> = Arc::new(
        prepared
            .batch_slots
            .iter()
            .map(|s| (s.slot_id, s.clone()))
            .collect(),
    );
    let ctx = SplitRescueContext {
        session: session.clone(),
        slots_by_id,
        strip_images_by_id: Arc::clone(&prepared.strip_images),
        strip_w: DEFAULT_STRIP_WIDTH,
        strip_h: prepared
            .strip_images
            .values()
            .next()
            .map(|i| i.height)
            .unwrap_or(90),
        fallback_resolver,
        allow_placeholder_leaf,
    };

    let spec = prepared.spec.clone();
    let png_bytes = Arc::clone(&prepared.png_bytes);
    let mut primary_cancel = cancel_rx.clone();
    let hedge_cancel_template = cancel_rx.clone();
    let primary_spec = spec.clone();
    let primary_png = Arc::clone(&png_bytes);
    let mut primary_handle = tokio::spawn(async move {
        transcribe_sheet_with_split_rescue(&ctx, primary_spec, primary_png, 0, &mut primary_cancel)
            .await
    });

    let attempt_t0 = Instant::now();
    let mut hedge_handle: Option<tokio::task::JoinHandle<Result<SheetBatchRecord, VlmCallError>>> =
        None;
    let mut primary_err: Option<VlmCallError> = None;
    let mut hedge_err: Option<VlmCallError> = None;
    let mut primary_done = false;
    let mut hedge_done = false;
    let mut ticker = tokio::time::interval(Duration::from_millis(50));
    ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);

    loop {
        tokio::select! {
            biased;
            _ = wait_until_cancelled(&mut cancel_rx) => {
                primary_handle.abort();
                if let Some(h) = hedge_handle {
                    h.abort();
                }
                return Err(VlmCallError::Cancelled);
            }
            res = &mut primary_handle, if !primary_done => {
                primary_done = true;
                match res {
                    Ok(Ok(outcome)) => {
                        if let Some(h) = hedge_handle {
                            h.abort();
                        }
                        hedge_coord.record_completion(outcome.batch.elapsed_s, false);
                        return Ok(outcome);
                    }
                    Ok(Err(err)) => {
                        if err.is_fatal() || matches!(err, VlmCallError::Cancelled) {
                            if let Some(h) = hedge_handle {
                                h.abort();
                            }
                            return Err(err);
                        }
                        primary_err = Some(err);
                    }
                    Err(join_err) => {
                        primary_err = Some(VlmCallError::NonSplittable(format!(
                            "primary VLM task panicked: {join_err}"
                        )));
                    }
                }
                if hedge_handle.is_none() || hedge_done {
                    return Err(primary_err.or(hedge_err).unwrap_or_else(|| {
                        VlmCallError::Splittable("primary sheet task failed".into())
                    }));
                }
            }
            res = async {
                match hedge_handle.as_mut() {
                    Some(h) => h.await,
                    None => std::future::pending().await,
                }
            }, if hedge_handle.is_some() && !hedge_done => {
                hedge_done = true;
                match res {
                    Ok(Ok(mut batch)) => {
                        primary_handle.abort();
                        batch.hedged_win = true;
                        hedge_coord.record_completion(batch.elapsed_s, true);
                        return Ok(SplitRescueOutcome {
                            batch,
                            split_count: 0,
                        });
                    }
                    Ok(Err(err)) => {
                        if err.is_fatal() || matches!(err, VlmCallError::Cancelled) {
                            primary_handle.abort();
                            return Err(err);
                        }
                        hedge_err = Some(err);
                    }
                    Err(join_err) => {
                        hedge_err = Some(VlmCallError::NonSplittable(format!(
                            "hedge VLM task panicked: {join_err}"
                        )));
                    }
                }
                if primary_done {
                    return Err(primary_err.or(hedge_err).unwrap_or_else(|| {
                        VlmCallError::Splittable("hedged sheet task failed".into())
                    }));
                }
            }
            _ = ticker.tick(), if hedge_handle.is_none() => {
                let elapsed_s = attempt_t0.elapsed().as_secs_f64();
                if hedge_coord.try_claim_hedge(elapsed_s, &session.rate_gate) {
                    let h_session = session.clone();
                    let h_spec = spec.clone();
                    let h_png = Arc::clone(&png_bytes);
                    let mut h_cancel = hedge_cancel_template.clone();
                    hedge_handle = Some(tokio::spawn(async move {
                        call_chat_completions(&h_session, &h_spec, &h_png, &mut h_cancel).await
                    }));
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::vlm::sheet_builder::build_prepared_sheet;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::TcpListener;

    async fn spawn_mock_http_server<F>(handler: F) -> (String, tokio::task::JoinHandle<()>)
    where
        F: Fn(usize, String) -> (u16, String, String) + Send + Sync + 'static,
    {
        let listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let addr = listener.local_addr().unwrap();
        let seq = Arc::new(AtomicUsize::new(0));
        let handler = Arc::new(handler);

        let handle = tokio::spawn(async move {
            loop {
                let Ok((mut stream, _)) = listener.accept().await else {
                    break;
                };
                let seq = Arc::clone(&seq);
                let handler = Arc::clone(&handler);
                tokio::spawn(async move {
                    let mut buf = Vec::new();
                    let mut chunk = [0u8; 4096];
                    let mut header_end = None;
                    let mut content_len = 0usize;

                    loop {
                        let n = match stream.read(&mut chunk).await {
                            Ok(0) | Err(_) => return,
                            Ok(n) => n,
                        };
                        buf.extend_from_slice(&chunk[..n]);
                        if header_end.is_none() {
                            if let Some(pos) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
                                header_end = Some(pos + 4);
                                let hdr_str = String::from_utf8_lossy(&buf[..pos]);
                                for line in hdr_str.lines() {
                                    if let Some((k, v)) = line.split_once(':') {
                                        if k.trim().eq_ignore_ascii_case("content-length") {
                                            content_len = v.trim().parse().unwrap_or(0);
                                        }
                                    }
                                }
                            }
                        }
                        if let Some(end) = header_end {
                            if buf.len() >= end + content_len {
                                let body = String::from_utf8_lossy(&buf[end..end + content_len])
                                    .to_string();
                                let call_idx = seq.fetch_add(1, Ordering::SeqCst);
                                let (status, extra_headers, resp_body) = handler(call_idx, body);
                                let reason = match status {
                                    200 => "OK",
                                    400 => "Bad Request",
                                    401 => "Unauthorized",
                                    429 => "Too Many Requests",
                                    500 => "Internal Server Error",
                                    _ => "Error",
                                };
                                let resp = format!(
                                    "HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n{extra_headers}Connection: close\r\n\r\n{resp_body}",
                                    resp_body.len()
                                );
                                let _ = stream.write_all(resp.as_bytes()).await;
                                let _ = stream.flush().await;
                                break;
                            }
                        }
                    }
                });
            }
        });

        (format!("http://127.0.0.1:{}/v1", addr.port()), handle)
    }

    fn make_test_prepared(slot_ids: &[u64]) -> PreparedSheet {
        let slots: Vec<TimingSlot> = slot_ids
            .iter()
            .enumerate()
            .map(|(i, &id)| TimingSlot::new(id, (i as i64) * 1000, ((i + 1) as i64) * 1000))
            .collect();
        let mut strips = HashMap::new();
        for &id in slot_ids {
            strips.insert(id, Arc::new(RgbImage::new_filled(960, 90, [10, 20, 30])));
        }
        build_prepared_sheet(1, slots, strips, 960, 90, 0.0).unwrap()
    }

    #[test]
    fn url_normalization_and_local_key_optionality() {
        assert_eq!(
            normalize_chat_completions_url("https://api.minimax.cn/v1/").unwrap(),
            "https://api.minimax.cn/v1/chat/completions"
        );
        assert_eq!(
            normalize_chat_completions_url("https://api.minimax.cn/v1/chat/completions").unwrap(),
            "https://api.minimax.cn/v1/chat/completions"
        );
        assert!(validate_endpoint_and_key("http://127.0.0.1:8080/v1/chat/completions", "").is_ok());
        assert!(validate_endpoint_and_key("http://localhost:11434/v1/chat/completions", "").is_ok());
        assert!(validate_endpoint_and_key("https://api.minimax.cn/v1/chat/completions", "").is_err());
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 4)]
    async fn concurrent_barrier_4_minimax_auto_downgrade_low_to_none_never_splits() {
        let listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let addr = listener.local_addr().unwrap();
        let barrier = Arc::new(tokio::sync::Barrier::new(4));
        let srv = tokio::spawn(async move {
            loop {
                let Ok((mut stream, _)) = listener.accept().await else {
                    break;
                };
                let barrier = Arc::clone(&barrier);
                tokio::spawn(async move {
                    let mut buf = Vec::new();
                    let mut chunk = [0u8; 4096];
                    let mut header_end = None;
                    let mut content_len = 0usize;
                    loop {
                        let n = match stream.read(&mut chunk).await {
                            Ok(0) | Err(_) => return,
                            Ok(n) => n,
                        };
                        buf.extend_from_slice(&chunk[..n]);
                        if header_end.is_none() {
                            if let Some(pos) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
                                header_end = Some(pos + 4);
                                let hdr_str = String::from_utf8_lossy(&buf[..pos]);
                                for line in hdr_str.lines() {
                                    if let Some((k, v)) = line.split_once(':') {
                                        if k.trim().eq_ignore_ascii_case("content-length") {
                                            content_len = v.trim().parse().unwrap_or(0);
                                        }
                                    }
                                }
                            }
                        }
                        if let Some(end) = header_end {
                            if buf.len() >= end + content_len {
                                let body = String::from_utf8_lossy(&buf[end..end + content_len]);
                                let parsed: Value = serde_json::from_str(&body).unwrap();
                                let (status, resp_body) =
                                    if parsed.get("reasoning_effort").and_then(Value::as_str)
                                        == Some("low")
                                    {
                                        barrier.wait().await;
                                        (
                                            400,
                                            json!({"error": {"message": "invalid params 2013: unsupported reasoning_effort"}}).to_string(),
                                        )
                                    } else {
                                        assert!(parsed.get("reasoning_effort").is_none());
                                        let content = json!({"slots": [
                                            {"id": 1, "category": "dialogue", "lines": ["OK1"]},
                                            {"id": 2, "category": "dialogue", "lines": ["OK2"]}
                                        ]})
                                        .to_string();
                                        (
                                            200,
                                            json!({"choices": [{"finish_reason": "stop", "message": {"content": content}}]}).to_string(),
                                        )
                                    };
                                let reason = if status == 200 { "OK" } else { "Bad Request" };
                                let resp = format!(
                                    "HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{resp_body}",
                                    resp_body.len()
                                );
                                let _ = stream.write_all(resp.as_bytes()).await;
                                let _ = stream.flush().await;
                                break;
                            }
                        }
                    }
                });
            }
        });

        let cfg = VlmEndpointConfig {
            endpoint: format!("http://127.0.0.1:{}/v1/chat/completions", addr.port()),
            api_key: String::new(),
            model: "MiniMax-M3.1-Flash".into(),
            thinking_mode: ThinkingMode::Auto,
            timeout_s: 10.0,
            max_retries: 1,
            ..Default::default()
        };
        let session = VlmSessionContext {
            http: reqwest::Client::new(),
            config: cfg.clone(),
            policy: Arc::new(AdaptivePolicyState::new(
                cfg.thinking_mode,
                &cfg.model,
                &cfg.endpoint,
            )),
            rate_gate: Arc::new(RateLimitGate::new()),
            script: SubtitleScript::Auto,
        };
        assert_eq!(session.policy.current_params(), (Some("low".into()), true));

        let mut handles = Vec::new();
        for _ in 0..4 {
            let prepared = make_test_prepared(&[1, 2]);
            let slots_by_id: Arc<HashMap<u64, TimingSlot>> = Arc::new(
                prepared
                    .batch_slots
                    .iter()
                    .map(|s| (s.slot_id, s.clone()))
                    .collect(),
            );
            let ctx = SplitRescueContext {
                session: session.clone(),
                slots_by_id,
                strip_images_by_id: Arc::clone(&prepared.strip_images),
                strip_w: 960,
                strip_h: 90,
                fallback_resolver: None,
                allow_placeholder_leaf: false,
            };
            handles.push(tokio::spawn(async move {
                let mut cancel_rx = watch::channel(false).1;
                transcribe_sheet_with_split_rescue(
                    &ctx,
                    prepared.spec,
                    prepared.png_bytes,
                    0,
                    &mut cancel_rx,
                )
                .await
            }));
        }

        for h in handles {
            let out = h.await.unwrap().unwrap();
            assert_eq!(out.split_count, 0);
            assert!(!out.batch.split_rescued);
            assert_eq!(out.batch.reasoning_effort_used, "omitted");
        }
        assert_eq!(session.policy.current_params(), (None, true));
        srv.abort();
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 4)]
    async fn concurrent_barrier_4_disable_json_format_never_splits() {
        let listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let addr = listener.local_addr().unwrap();
        let barrier = Arc::new(tokio::sync::Barrier::new(4));
        let srv = tokio::spawn(async move {
            loop {
                let Ok((mut stream, _)) = listener.accept().await else {
                    break;
                };
                let barrier = Arc::clone(&barrier);
                tokio::spawn(async move {
                    let mut buf = Vec::new();
                    let mut chunk = [0u8; 4096];
                    let mut header_end = None;
                    let mut content_len = 0usize;
                    loop {
                        let n = match stream.read(&mut chunk).await {
                            Ok(0) | Err(_) => return,
                            Ok(n) => n,
                        };
                        buf.extend_from_slice(&chunk[..n]);
                        if header_end.is_none() {
                            if let Some(pos) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
                                header_end = Some(pos + 4);
                                let hdr_str = String::from_utf8_lossy(&buf[..pos]);
                                for line in hdr_str.lines() {
                                    if let Some((k, v)) = line.split_once(':') {
                                        if k.trim().eq_ignore_ascii_case("content-length") {
                                            content_len = v.trim().parse().unwrap_or(0);
                                        }
                                    }
                                }
                            }
                        }
                        if let Some(end) = header_end {
                            if buf.len() >= end + content_len {
                                let body = String::from_utf8_lossy(&buf[end..end + content_len]);
                                let parsed: Value = serde_json::from_str(&body).unwrap();
                                let (status, resp_body) = if parsed.get("response_format").is_some()
                                {
                                    barrier.wait().await;
                                    (
                                        400,
                                        json!({"error": {"message": "unsupported response_format"}})
                                            .to_string(),
                                    )
                                } else {
                                    let content = json!({"slots": [
                                        {"id": 1, "category": "dialogue", "lines": ["OK1"]},
                                        {"id": 2, "category": "dialogue", "lines": ["OK2"]}
                                    ]})
                                    .to_string();
                                    (
                                        200,
                                        json!({"choices": [{"finish_reason": "stop", "message": {"content": content}}]}).to_string(),
                                    )
                                };
                                let reason = if status == 200 { "OK" } else { "Bad Request" };
                                let resp = format!(
                                    "HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{resp_body}",
                                    resp_body.len()
                                );
                                let _ = stream.write_all(resp.as_bytes()).await;
                                let _ = stream.flush().await;
                                break;
                            }
                        }
                    }
                });
            }
        });

        let cfg = VlmEndpointConfig {
            endpoint: format!("http://127.0.0.1:{}/v1/chat/completions", addr.port()),
            api_key: String::new(),
            model: "custom-vl".into(),
            thinking_mode: ThinkingMode::Default,
            timeout_s: 10.0,
            max_retries: 1,
            ..Default::default()
        };
        let session = VlmSessionContext {
            http: reqwest::Client::new(),
            config: cfg.clone(),
            policy: Arc::new(AdaptivePolicyState::new(
                cfg.thinking_mode,
                &cfg.model,
                &cfg.endpoint,
            )),
            rate_gate: Arc::new(RateLimitGate::new()),
            script: SubtitleScript::Auto,
        };
        assert_eq!(session.policy.current_params(), (None, true));

        let mut handles = Vec::new();
        for _ in 0..4 {
            let prepared = make_test_prepared(&[1, 2]);
            let slots_by_id: Arc<HashMap<u64, TimingSlot>> = Arc::new(
                prepared
                    .batch_slots
                    .iter()
                    .map(|s| (s.slot_id, s.clone()))
                    .collect(),
            );
            let ctx = SplitRescueContext {
                session: session.clone(),
                slots_by_id,
                strip_images_by_id: Arc::clone(&prepared.strip_images),
                strip_w: 960,
                strip_h: 90,
                fallback_resolver: None,
                allow_placeholder_leaf: false,
            };
            handles.push(tokio::spawn(async move {
                let mut cancel_rx = watch::channel(false).1;
                transcribe_sheet_with_split_rescue(
                    &ctx,
                    prepared.spec,
                    prepared.png_bytes,
                    0,
                    &mut cancel_rx,
                )
                .await
            }));
        }

        for h in handles {
            let out = h.await.unwrap().unwrap();
            assert_eq!(out.split_count, 0);
            assert!(!out.batch.split_rescued);
        }
        assert_eq!(session.policy.current_params(), (None, false));
        srv.abort();
    }

    #[tokio::test]
    async fn fatal_401_aborts_immediately_without_retries_or_split() {
        let calls = Arc::new(AtomicUsize::new(0));
        let calls_srv = Arc::clone(&calls);
        let (base_url, srv) = spawn_mock_http_server(move |_, _| {
            calls_srv.fetch_add(1, Ordering::SeqCst);
            (
                401,
                String::new(),
                r#"{"error":{"message":"invalid_api_key"}}"#.into(),
            )
        })
        .await;

        let prepared = make_test_prepared(&[1, 2, 3, 4]);
        let cfg = VlmEndpointConfig {
            endpoint: normalize_chat_completions_url(&base_url).unwrap(),
            api_key: "bad-key".into(),
            model: "test-model".into(),
            thinking_mode: ThinkingMode::Auto,
            timeout_s: 10.0,
            max_retries: 4,
            ..Default::default()
        };
        let mut cancel_rx = watch::channel(false).1;
        let policy = Arc::new(AdaptivePolicyState::new(cfg.thinking_mode, &cfg.model, &cfg.endpoint));
        let gate = Arc::new(RateLimitGate::new());
        let slots_by_id: Arc<HashMap<u64, TimingSlot>> = Arc::new(
            prepared.batch_slots.iter().map(|s| (s.slot_id, s.clone())).collect(),
        );
        let ctx = SplitRescueContext {
            session: VlmSessionContext {
                http: reqwest::Client::new(),
                config: cfg,
                policy,
                rate_gate: gate,
                script: SubtitleScript::Auto,
            },
            slots_by_id,
            strip_images_by_id: Arc::clone(&prepared.strip_images),
            strip_w: 960,
            strip_h: 90,
            fallback_resolver: None,
            allow_placeholder_leaf: true,
        };
        let err = transcribe_sheet_with_split_rescue(
            &ctx,
            prepared.spec,
            prepared.png_bytes,
            0,
            &mut cancel_rx,
        )
        .await
        .unwrap_err();
        assert!(err.is_fatal());
        assert_eq!(calls.load(Ordering::SeqCst), 1);
        srv.abort();
    }

    #[tokio::test]
    async fn adaptive_reasoning_downgrade_400_2013_and_429_no_split_retry() {
        let calls = Arc::new(AtomicUsize::new(0));
        let calls_srv = Arc::clone(&calls);
        let (base_url, srv) = spawn_mock_http_server(move |idx, body| {
            let n = calls_srv.fetch_add(1, Ordering::SeqCst);
            if n == 0 {
                assert!(body.contains(r#""reasoning_effort":"none""#));
                return (
                    400,
                    String::new(),
                    r#"{"error":{"message":"invalid params 2013: unsupported reasoning_effort"}}"#
                        .into(),
                );
            }
            if n == 1 {
                assert!(body.contains(r#""reasoning_effort":"low""#));
                return (
                    429,
                    "Retry-After: 0.01\r\n".into(),
                    r#"{"error":{"message":"rate limit exceeded"}}"#.into(),
                );
            }
            assert_eq!(idx, 2);
            (
                200,
                String::new(),
                r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":1,\"category\":\"dialogue\",\"lines\":[\"OK1\"]},{\"id\":2,\"category\":\"dialogue\",\"lines\":[\"OK2\"]}]}"}}]}"#.into(),
            )
        })
        .await;

        let prepared = make_test_prepared(&[1, 2]);
        let cfg = VlmEndpointConfig {
            endpoint: normalize_chat_completions_url(&base_url).unwrap(),
            api_key: String::new(),
            model: "custom-vl".into(),
            thinking_mode: ThinkingMode::Auto,
            timeout_s: 10.0,
            max_retries: 2,
            ..Default::default()
        };
        let policy = Arc::new(AdaptivePolicyState::new(cfg.thinking_mode, &cfg.model, &cfg.endpoint));
        let gate = Arc::new(RateLimitGate::new());
        let mut cancel_rx = watch::channel(false).1;
        let session = VlmSessionContext {
            http: reqwest::Client::new(),
            config: cfg,
            policy,
            rate_gate: Arc::clone(&gate),
            script: SubtitleScript::Auto,
        };
        let res = call_chat_completions(
            &session,
            &prepared.spec,
            &prepared.png_bytes,
            &mut cancel_rx,
        )
        .await
        .unwrap();
        assert_eq!(res.reasoning_effort_used, "low");
        assert_eq!(res.slots.len(), 2);
        assert_eq!(calls.load(Ordering::SeqCst), 3);
        assert!(!gate.is_rate_limited());
        srv.abort();
    }

    #[tokio::test]
    async fn content_filter_triggers_immediate_bisect_and_leaf_placeholder_fallback() {
        let calls = Arc::new(AtomicUsize::new(0));
        let calls_srv = Arc::clone(&calls);
        let (base_url, srv) = spawn_mock_http_server(move |_, body| {
            calls_srv.fetch_add(1, Ordering::SeqCst);
            let parsed: Value = serde_json::from_str(&body).unwrap();
            let prompt = parsed["messages"][1]["content"][1]["text"].as_str().unwrap();
            // Reject any batch containing slot 3 with content_filter
            if prompt.contains("[1, 2, 3, 4]") || prompt.contains("[3, 4]") || prompt.contains("[3]") {
                return (
                    200,
                    String::new(),
                    r#"{"choices":[{"finish_reason":"content_filter","message":{"content":""}}]}"#.into(),
                );
            }
            if prompt.contains("[1, 2]") {
                return (
                    200,
                    String::new(),
                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":1,\"category\":\"dialogue\",\"lines\":[\"One\"]},{\"id\":2,\"category\":\"dialogue\",\"lines\":[\"Two\"]}]}"}}]}"#.into(),
                );
            }
            (
                200,
                String::new(),
                r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":4,\"category\":\"dialogue\",\"lines\":[\"Four\"]}]}"}}]}"#.into(),
            )
        })
        .await;

        let prepared = make_test_prepared(&[1, 2, 3, 4]);
        let cfg = VlmEndpointConfig {
            endpoint: normalize_chat_completions_url(&base_url).unwrap(),
            api_key: String::new(),
            model: "test-vl".into(),
            thinking_mode: ThinkingMode::Default,
            timeout_s: 10.0,
            max_retries: 4,
            ..Default::default()
        };
        let policy = Arc::new(AdaptivePolicyState::new(cfg.thinking_mode, &cfg.model, &cfg.endpoint));
        let gate = Arc::new(RateLimitGate::new());
        let slots_by_id: Arc<HashMap<u64, TimingSlot>> = Arc::new(
            prepared.batch_slots.iter().map(|s| (s.slot_id, s.clone())).collect(),
        );
        let ctx = SplitRescueContext {
            session: VlmSessionContext {
                http: reqwest::Client::new(),
                config: cfg,
                policy,
                rate_gate: gate,
                script: SubtitleScript::Auto,
            },
            slots_by_id,
            strip_images_by_id: Arc::clone(&prepared.strip_images),
            strip_w: 960,
            strip_h: 90,
            fallback_resolver: None,
            allow_placeholder_leaf: true,
        };
        let mut cancel_rx = watch::channel(false).1;
        let out = transcribe_sheet_with_split_rescue(
            &ctx,
            prepared.spec,
            prepared.png_bytes,
            0,
            &mut cancel_rx,
        )
        .await
        .unwrap();

        assert!(out.batch.split_rescued);
        assert_eq!(out.batch.degraded_slot_ids, vec![3]);
        assert_eq!(out.batch.slots[2].category, PLACEHOLDER_CATEGORY);
        assert_eq!(out.batch.slots[2].lines, vec![PLACEHOLDER_TEXT]);
        assert_eq!(out.batch.slots[3].lines, vec!["Four"]);
        // Zero retry sleeps on content_filter: exact 5 calls ([1,2,3,4], [1,2], [3,4], [3], [4])
        assert_eq!(calls.load(Ordering::SeqCst), 5);
        srv.abort();
    }

    #[tokio::test]
    async fn tail_hedging_wins_without_waiting_for_slow_primary() {
        let listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let addr = listener.local_addr().unwrap();
        let req_num = Arc::new(AtomicUsize::new(0));
        let req_srv = Arc::clone(&req_num);
        let slow_release = Arc::new(tokio::sync::Notify::new());
        let slow_release_srv = Arc::clone(&slow_release);

        let srv = tokio::spawn(async move {
            loop {
                let Ok((mut stream, _)) = listener.accept().await else {
                    break;
                };
                let n = req_srv.fetch_add(1, Ordering::SeqCst);
                let rel = Arc::clone(&slow_release_srv);
                tokio::spawn(async move {
                    let mut buf = [0u8; 4096];
                    let _ = stream.read(&mut buf).await;
                    if n == 0 {
                        // Primary blocks until test finishes
                        rel.notified().await;
                    }
                    let body = r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":1,\"category\":\"dialogue\",\"lines\":[\"Hedged\"]}]}"}}]}"#;
                    let resp = format!(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                        body.len(),
                        body
                    );
                    let _ = stream.write_all(resp.as_bytes()).await;
                });
            }
        });

        let prepared = make_test_prepared(&[1]);
        let cfg = VlmEndpointConfig {
            endpoint: format!("http://127.0.0.1:{}/v1/chat/completions", addr.port()),
            api_key: String::new(),
            model: "test-vl".into(),
            thinking_mode: ThinkingMode::Default,
            timeout_s: 30.0,
            max_retries: 1,
            ..Default::default()
        };
        let coord = Arc::new(HedgeCoordinator::new(4, 0.0, 0.0));
        {
            let mut g = coord.state.lock().unwrap();
            g.all_batches_dispatched = true;
            g.total_dispatched = 1;
        }

        let outcome = transcribe_sheet_with_tail_hedging(
            prepared,
            VlmSessionContext {
                http: reqwest::Client::new(),
                config: cfg,
                policy: Arc::new(AdaptivePolicyState::new(ThinkingMode::Default, "test-vl", "")),
                rate_gate: Arc::new(RateLimitGate::new()),
                script: SubtitleScript::Auto,
            },
            None,
            true,
            Arc::clone(&coord),
            watch::channel(false).1,
        )
        .await
        .unwrap();

        assert!(outcome.batch.hedged_win);
        assert_eq!(outcome.batch.slots[0].lines, vec!["Hedged"]);
        let g = coord.state.lock().unwrap();
        assert_eq!(g.hedged_requests, 1);
        assert_eq!(g.hedged_wins, 1);

        slow_release.notify_waiters();
        srv.abort();
    }

    #[tokio::test]
    async fn length_truncation_splits_immediately_and_http_500_never_splits() {
        // 1. finish_reason == "length" triggers immediate binary split (0 retry sleeps)
        let len_calls = Arc::new(AtomicUsize::new(0));
        let len_calls_srv = Arc::clone(&len_calls);
        let (len_url, len_srv) = spawn_mock_http_server(move |_, body| {
            len_calls_srv.fetch_add(1, Ordering::SeqCst);
            let v: Value = serde_json::from_str(&body).unwrap();
            let prompt = v["messages"][1]["content"][1]["text"].as_str().unwrap();
            if prompt.contains("[1, 2]") {
                (
                    200,
                    String::new(),
                    r#"{"choices":[{"finish_reason":"length","message":{"content":"{\"slots\":[{\"id\":1"}}]}"#.into(),
                )
            } else if prompt.contains("[1]") {
                (
                    200,
                    String::new(),
                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":1,\"category\":\"dialogue\",\"lines\":[\"L1\"]}]}"}}]}"#.into(),
                )
            } else {
                (
                    200,
                    String::new(),
                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":2,\"category\":\"dialogue\",\"lines\":[\"L2\"]}]}"}}]}"#.into(),
                )
            }
        })
        .await;

        let prepared = make_test_prepared(&[1, 2]);
        let cfg = VlmEndpointConfig {
            endpoint: normalize_chat_completions_url(&len_url).unwrap(),
            api_key: String::new(),
            model: "test-vl".into(),
            thinking_mode: ThinkingMode::Default,
            timeout_s: 10.0,
            max_retries: 4,
            ..Default::default()
        };
        let slots_by_id: Arc<HashMap<u64, TimingSlot>> = Arc::new(
            prepared.batch_slots.iter().map(|s| (s.slot_id, s.clone())).collect(),
        );
        let ctx = SplitRescueContext {
            session: VlmSessionContext {
                http: reqwest::Client::new(),
                config: cfg.clone(),
                policy: Arc::new(AdaptivePolicyState::new(cfg.thinking_mode, &cfg.model, &cfg.endpoint)),
                rate_gate: Arc::new(RateLimitGate::new()),
                script: SubtitleScript::Auto,
            },
            slots_by_id,
            strip_images_by_id: Arc::clone(&prepared.strip_images),
            strip_w: 960,
            strip_h: 90,
            fallback_resolver: None,
            allow_placeholder_leaf: true,
        };
        let mut cancel_rx = watch::channel(false).1;
        let out = transcribe_sheet_with_split_rescue(
            &ctx,
            prepared.spec.clone(),
            prepared.png_bytes.clone(),
            0,
            &mut cancel_rx,
        )
        .await
        .unwrap();
        assert!(out.batch.split_rescued);
        assert_eq!(out.batch.slots.len(), 2);
        // Immediate split: exactly 3 calls ([1,2] length -> [1] -> [2]), zero retries on [1,2]
        assert_eq!(len_calls.load(Ordering::SeqCst), 3);
        len_srv.abort();

        // 2. HTTP 500 retries max_retries=2 times and fails NonSplittable without splitting
        let err_calls = Arc::new(AtomicUsize::new(0));
        let err_calls_srv = Arc::clone(&err_calls);
        let (err_url, err_srv) = spawn_mock_http_server(move |_, _| {
            err_calls_srv.fetch_add(1, Ordering::SeqCst);
            (500, String::new(), r#"{"error":{"message":"server error"}}"#.into())
        })
        .await;
        let cfg500 = VlmEndpointConfig {
            endpoint: normalize_chat_completions_url(&err_url).unwrap(),
            api_key: String::new(),
            model: "test-vl".into(),
            thinking_mode: ThinkingMode::Default,
            timeout_s: 10.0,
            max_retries: 2,
            ..Default::default()
        };
        let slots_by_id_500: Arc<HashMap<u64, TimingSlot>> = Arc::new(
            prepared.batch_slots.iter().map(|s| (s.slot_id, s.clone())).collect(),
        );
        let ctx500 = SplitRescueContext {
            session: VlmSessionContext {
                http: reqwest::Client::new(),
                config: cfg500.clone(),
                policy: Arc::new(AdaptivePolicyState::new(cfg500.thinking_mode, &cfg500.model, &cfg500.endpoint)),
                rate_gate: Arc::new(RateLimitGate::new()),
                script: SubtitleScript::Auto,
            },
            slots_by_id: slots_by_id_500,
            strip_images_by_id: Arc::clone(&prepared.strip_images),
            strip_w: 960,
            strip_h: 90,
            fallback_resolver: None,
            allow_placeholder_leaf: false,
        };
        let err = transcribe_sheet_with_split_rescue(
            &ctx500,
            prepared.spec,
            prepared.png_bytes,
            0,
            &mut cancel_rx,
        )
        .await
        .unwrap_err();
        assert!(matches!(err, VlmCallError::NonSplittable(_)));
        assert_eq!(err_calls.load(Ordering::SeqCst), 2);
        err_srv.abort();
    }

    #[tokio::test]
    async fn ten_slot_sheet_reaches_depth_4_bisect_down_to_single_slot() {
        let (url, srv) = spawn_mock_http_server(move |_, body| {
            let v: Value = serde_json::from_str(&body).unwrap();
            let prompt = v["messages"][1]["content"][1]["text"].as_str().unwrap();
            let ids_part = prompt
                .split("依次为 [")
                .nth(1)
                .and_then(|s| s.split(']').next())
                .unwrap();
            let ids: Vec<u64> = ids_part
                .split(',')
                .filter_map(|x| x.trim().parse::<u64>().ok())
                .collect();
            if ids.contains(&10) && ids.len() > 1 {
                return (
                    200,
                    String::new(),
                    r#"{"choices":[{"finish_reason":"content_filter","message":{"content":""}}]}"#.into(),
                );
            }
            if ids == [10] {
                return (
                    200,
                    String::new(),
                    r#"{"choices":[{"finish_reason":"content_filter","message":{"content":""}}]}"#.into(),
                );
            }
            let slots_json: Vec<Value> = ids
                .iter()
                .map(|id| json!({"id": id, "category": "dialogue", "lines": [format!("S{id}")]}))
                .collect();
            (
                200,
                String::new(),
                json!({"choices":[{"finish_reason":"stop","message":{"content": json!({"slots": slots_json}).to_string()}}]}).to_string(),
            )
        })
        .await;

        let prepared = make_test_prepared(&[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
        let cfg = VlmEndpointConfig {
            endpoint: normalize_chat_completions_url(&url).unwrap(),
            api_key: String::new(),
            model: "test-vl".into(),
            thinking_mode: ThinkingMode::Default,
            timeout_s: 10.0,
            max_retries: 1,
            ..Default::default()
        };
        let slots_by_id: Arc<HashMap<u64, TimingSlot>> = Arc::new(
            prepared.batch_slots.iter().map(|s| (s.slot_id, s.clone())).collect(),
        );
        let ctx = SplitRescueContext {
            session: VlmSessionContext {
                http: reqwest::Client::new(),
                config: cfg.clone(),
                policy: Arc::new(AdaptivePolicyState::new(cfg.thinking_mode, &cfg.model, &cfg.endpoint)),
                rate_gate: Arc::new(RateLimitGate::new()),
                script: SubtitleScript::Auto,
            },
            slots_by_id,
            strip_images_by_id: Arc::clone(&prepared.strip_images),
            strip_w: 960,
            strip_h: 90,
            fallback_resolver: None,
            allow_placeholder_leaf: true,
        };
        let mut cancel_rx = watch::channel(false).1;
        let out = transcribe_sheet_with_split_rescue(
            &ctx,
            prepared.spec,
            prepared.png_bytes,
            0,
            &mut cancel_rx,
        )
        .await
        .unwrap();
        assert!(out.batch.split_rescued);
        assert_eq!(out.batch.degraded_slot_ids, vec![10]);
        assert_eq!(out.batch.slots.len(), 10);
        assert_eq!(out.batch.slots[9].category, PLACEHOLDER_CATEGORY);
        assert_eq!(out.batch.slots[9].lines, vec![PLACEHOLDER_TEXT]);
        srv.abort();
    }

    #[tokio::test]
    async fn base_resp_1002_1004_1027_and_413_and_429_exhaustion() {
        // 1. base_resp 1002 retries via RateLimitGate without splitting, then succeeds
        let calls_1002 = Arc::new(AtomicUsize::new(0));
        let calls_1002_srv = Arc::clone(&calls_1002);
        let (u1, s1) = spawn_mock_http_server(move |_, _| {
            let n = calls_1002_srv.fetch_add(1, Ordering::SeqCst);
            if n == 0 {
                (
                    200,
                    String::new(),
                    r#"{"base_resp":{"status_code":1002,"status_msg":"rate limit"}}"#.into(),
                )
            } else {
                (
                    200,
                    String::new(),
                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":1,\"category\":\"dialogue\",\"lines\":[\"Recovered\"]}]}"}}]}"#.into(),
                )
            }
        })
        .await;
        let prepared = make_test_prepared(&[1, 2]);
        let slots_by_id: Arc<HashMap<u64, TimingSlot>> = Arc::new(
            prepared.batch_slots.iter().map(|s| (s.slot_id, s.clone())).collect(),
        );
        let make_ctx = |url: &str, max_retries: u32| SplitRescueContext {
            session: VlmSessionContext {
                http: reqwest::Client::new(),
                config: VlmEndpointConfig {
                    endpoint: normalize_chat_completions_url(url).unwrap(),
                    api_key: String::new(),
                    model: "test-vl".into(),
                    thinking_mode: ThinkingMode::Default,
                    timeout_s: 10.0,
                    max_retries,
                    ..Default::default()
                },
                policy: Arc::new(AdaptivePolicyState::new(ThinkingMode::Default, "test-vl", "")),
                rate_gate: Arc::new(RateLimitGate::new()),
                script: SubtitleScript::Auto,
            },
            slots_by_id: Arc::clone(&slots_by_id),
            strip_images_by_id: Arc::clone(&prepared.strip_images),
            strip_w: 960,
            strip_h: 90,
            fallback_resolver: None,
            allow_placeholder_leaf: true,
        };
        let mut cancel_rx = watch::channel(false).1;
        let prepared1 = make_test_prepared(&[1]);
        let r_1002 = call_chat_completions(
            &make_ctx(&u1, 1).session,
            &prepared1.spec,
            &prepared1.png_bytes,
            &mut cancel_rx,
        )
        .await
        .unwrap();
        assert_eq!(r_1002.slots[0].lines, vec!["Recovered"]);
        assert_eq!(calls_1002.load(Ordering::SeqCst), 2);
        s1.abort();

        // 2. base_resp 1004 -> Fatal immediately
        let (u2, s2) = spawn_mock_http_server(move |_, _| {
            (
                200,
                String::new(),
                r#"{"base_resp":{"status_code":1004,"status_msg":"auth failed"}}"#.into(),
            )
        })
        .await;
        let err_1004 = call_chat_completions(
            &make_ctx(&u2, 3).session,
            &prepared1.spec,
            &prepared1.png_bytes,
            &mut cancel_rx,
        )
        .await
        .unwrap_err();
        assert!(err_1004.is_fatal());
        s2.abort();

        // 3. base_resp 1027 -> ContentFilter triggers bisect
        let calls_1027 = Arc::new(AtomicUsize::new(0));
        let calls_1027_srv = Arc::clone(&calls_1027);
        let (u3, s3) = spawn_mock_http_server(move |_, body| {
            calls_1027_srv.fetch_add(1, Ordering::SeqCst);
            let v: Value = serde_json::from_str(&body).unwrap();
            let prompt = v["messages"][1]["content"][1]["text"].as_str().unwrap();
            if prompt.contains("[1, 2]") {
                (
                    200,
                    String::new(),
                    r#"{"base_resp":{"status_code":1027,"status_msg":"sensitive content"}}"#.into(),
                )
            } else if prompt.contains("[1]") {
                (
                    200,
                    String::new(),
                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":1,\"category\":\"dialogue\",\"lines\":[\"A\"]}]}"}}]}"#.into(),
                )
            } else {
                (
                    200,
                    String::new(),
                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":2,\"category\":\"dialogue\",\"lines\":[\"B\"]}]}"}}]}"#.into(),
                )
            }
        })
        .await;
        let out_1027 = transcribe_sheet_with_split_rescue(
            &make_ctx(&u3, 1),
            prepared.spec.clone(),
            prepared.png_bytes.clone(),
            0,
            &mut cancel_rx,
        )
        .await
        .unwrap();
        assert!(out_1027.batch.split_rescued);
        assert_eq!(calls_1027.load(Ordering::SeqCst), 3);
        s3.abort();

        // 4. 7 consecutive HTTP 429 with Retry-After: 0 -> exhausts max_rate_limit_retries as NonSplittable without split
        let calls_429 = Arc::new(AtomicUsize::new(0));
        let calls_429_srv = Arc::clone(&calls_429);
        let (u4, s4) = spawn_mock_http_server(move |_, _| {
            calls_429_srv.fetch_add(1, Ordering::SeqCst);
            (
                429,
                "Retry-After: 0\r\n".into(),
                r#"{"error":{"message":"rate limit"}}"#.into(),
            )
        })
        .await;
        let err_429 = transcribe_sheet_with_split_rescue(
            &make_ctx(&u4, 1),
            prepared.spec.clone(),
            prepared.png_bytes.clone(),
            0,
            &mut cancel_rx,
        )
        .await
        .unwrap_err();
        assert!(matches!(err_429, VlmCallError::NonSplittable(_)));
        assert_eq!(calls_429.load(Ordering::SeqCst), 7);
        s4.abort();

        // 5. HTTP 413 Payload Too Large -> Splittable -> bisects [1,2] -> [1], [2]
        let (u5, s5) = spawn_mock_http_server(move |_, body| {
            let v: Value = serde_json::from_str(&body).unwrap();
            let prompt = v["messages"][1]["content"][1]["text"].as_str().unwrap();
            if prompt.contains("[1, 2]") {
                (413, String::new(), r#"{"error":{"message":"payload too large"}}"#.into())
            } else if prompt.contains("[1]") {
                (200, String::new(), r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":1,\"category\":\"dialogue\",\"lines\":[\"One\"]}]}"}}]}"#.into())
            } else {
                (200, String::new(), r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":2,\"category\":\"dialogue\",\"lines\":[\"Two\"]}]}"}}]}"#.into())
            }
        })
        .await;
        let out_413 = transcribe_sheet_with_split_rescue(
            &make_ctx(&u5, 1),
            prepared.spec.clone(),
            prepared.png_bytes.clone(),
            0,
            &mut cancel_rx,
        )
        .await
        .unwrap();
        assert!(out_413.batch.split_rescued);
        assert_eq!(out_413.batch.slots[0].lines, vec!["One"]);
        s5.abort();
    }

    #[tokio::test]
    async fn dual_400_downgrade_exhausted_400_fallback_network_err_and_hedge_401_abort() {
        // 1. Dual 400 downgrade: first 2013 reasoning -> second response_format -> 200 OK
        let calls = Arc::new(AtomicUsize::new(0));
        let calls_srv = Arc::clone(&calls);
        let (u1, s1) = spawn_mock_http_server(move |_, body| {
            let n = calls_srv.fetch_add(1, Ordering::SeqCst);
            let parsed: Value = serde_json::from_str(&body).unwrap();
            if n == 0 {
                assert_eq!(parsed["reasoning_effort"], "none");
                assert!(parsed.get("response_format").is_some());
                (400, String::new(), r#"{"error":{"message":"invalid params 2013: reasoning"}}"#.into())
            } else if n == 1 {
                assert_eq!(parsed["reasoning_effort"], "low");
                assert!(parsed.get("response_format").is_some());
                (400, String::new(), r#"{"error":{"message":"unsupported response_format"}}"#.into())
            } else {
                assert!(parsed.get("response_format").is_none());
                (200, String::new(), r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":1,\"category\":\"dialogue\",\"lines\":[\"DualOk\"]}]}"}}]}"#.into())
            }
        })
        .await;
        let prepared = make_test_prepared(&[1]);
        let mut cancel_rx = watch::channel(false).1;
        let session = VlmSessionContext {
            http: reqwest::Client::new(),
            config: VlmEndpointConfig {
                endpoint: normalize_chat_completions_url(&u1).unwrap(),
                api_key: String::new(),
                model: "custom-vl".into(),
                thinking_mode: ThinkingMode::Auto,
                timeout_s: 10.0,
                max_retries: 1,
                ..Default::default()
            },
            policy: Arc::new(AdaptivePolicyState::new(ThinkingMode::Auto, "custom-vl", "")),
            rate_gate: Arc::new(RateLimitGate::new()),
            script: SubtitleScript::Auto,
        };
        let res = call_chat_completions(&session, &prepared.spec, &prepared.png_bytes, &mut cancel_rx)
            .await
            .unwrap();
        assert_eq!(res.slots[0].lines, vec!["DualOk"]);
        assert_eq!(calls.load(Ordering::SeqCst), 3);
        s1.abort();

        // 2. Exhausted effort chain + 400 -> Splittable
        let (u2, s2) = spawn_mock_http_server(move |_, _| {
            (400, String::new(), r#"{"error":{"message":"invalid params 2013: reasoning"}}"#.into())
        })
        .await;
        let exhausted_policy = Arc::new(AdaptivePolicyState::new(ThinkingMode::None, "custom-vl", ""));
        exhausted_policy.downgrade_effort(None);
        let session2 = VlmSessionContext {
            http: reqwest::Client::new(),
            config: VlmEndpointConfig {
                endpoint: normalize_chat_completions_url(&u2).unwrap(),
                api_key: String::new(),
                model: "custom-vl".into(),
                thinking_mode: ThinkingMode::None,
                timeout_s: 10.0,
                max_retries: 1,
                ..Default::default()
            },
            policy: exhausted_policy,
            rate_gate: Arc::new(RateLimitGate::new()),
            script: SubtitleScript::Auto,
        };
        let err_400 = call_chat_completions(&session2, &prepared.spec, &prepared.png_bytes, &mut cancel_rx)
            .await
            .unwrap_err();
        assert!(matches!(err_400, VlmCallError::Splittable(_)));
        s2.abort();

        // 3. Network connection refused -> NonSplittable
        let dead_listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let dead_port = dead_listener.local_addr().unwrap().port();
        drop(dead_listener);
        let net_session = VlmSessionContext {
            http: reqwest::Client::new(),
            config: VlmEndpointConfig {
                endpoint: format!("http://127.0.0.1:{dead_port}/v1/chat/completions"),
                api_key: String::new(),
                model: "test".into(),
                thinking_mode: ThinkingMode::Default,
                timeout_s: 2.0,
                max_retries: 1,
                ..Default::default()
            },
            policy: Arc::new(AdaptivePolicyState::new(ThinkingMode::Default, "test", "")),
            rate_gate: Arc::new(RateLimitGate::new()),
            script: SubtitleScript::Auto,
        };
        let net_err = call_chat_completions(&net_session, &prepared.spec, &prepared.png_bytes, &mut cancel_rx)
            .await
            .unwrap_err();
        assert!(matches!(net_err, VlmCallError::NonSplittable(_)));

        // 4. Tail-hedging: hedge hits 401 Fatal while primary is blocked -> aborts primary immediately;
        //    also test both primary and hedge failing with 500
        for hedge_status in [401u16, 500u16] {
            let listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
            let addr = listener.local_addr().unwrap();
            let req_seq = Arc::new(AtomicUsize::new(0));
            let req_seq_srv = Arc::clone(&req_seq);
            let release = Arc::new(tokio::sync::Notify::new());
            let release_srv = Arc::clone(&release);

            let srv = tokio::spawn(async move {
                loop {
                    let Ok((mut stream, _)) = listener.accept().await else {
                        break;
                    };
                    let n = req_seq_srv.fetch_add(1, Ordering::SeqCst);
                    let rel = Arc::clone(&release_srv);
                    tokio::spawn(async move {
                        let mut buf = [0u8; 4096];
                        let _ = stream.read(&mut buf).await;
                        if n == 0 && hedge_status == 401 {
                            rel.notified().await;
                        }
                        let st = if n == 0 { 500 } else { hedge_status };
                        let body = r#"{"error":{"message":"err"}}"#;
                        let resp = format!(
                            "HTTP/1.1 {st} ERR\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
                            body.len()
                        );
                        let _ = stream.write_all(resp.as_bytes()).await;
                    });
                }
            });

            let coord = Arc::new(HedgeCoordinator::new(4, 0.0, 0.0));
            {
                let mut g = coord.state.lock().unwrap();
                g.all_batches_dispatched = true;
                g.total_dispatched = 1;
            }
            let res = transcribe_sheet_with_tail_hedging(
                make_test_prepared(&[1]),
                VlmSessionContext {
                    http: reqwest::Client::new(),
                    config: VlmEndpointConfig {
                        endpoint: format!("http://127.0.0.1:{}/v1/chat/completions", addr.port()),
                        api_key: String::new(),
                        model: "test-vl".into(),
                        thinking_mode: ThinkingMode::Default,
                        timeout_s: 10.0,
                        max_retries: 1,
                        ..Default::default()
                    },
                    policy: Arc::new(AdaptivePolicyState::new(ThinkingMode::Default, "test-vl", "")),
                    rate_gate: Arc::new(RateLimitGate::new()),
                    script: SubtitleScript::Auto,
                },
                None,
                false,
                coord,
                watch::channel(false).1,
            )
            .await;

            let err = res.unwrap_err();
            if hedge_status == 401 {
                assert!(err.is_fatal());
            } else {
                assert!(matches!(err, VlmCallError::NonSplittable(_)));
            }
            release.notify_waiters();
            srv.abort();
        }
    }

    #[tokio::test]
    async fn custom_headers_inject_session_id_and_override_defaults() {
        let custom_headers = vec![
            CustomHeaderDto {
                name: "x-opencode-session".into(),
                value: "{{session_id}}".into(),
            },
            CustomHeaderDto {
                name: "User-Agent".into(),
                value: "X-Custom-Client/1.0".into(),
            },
            CustomHeaderDto {
                name: "X-Title".into(),
                value: "SubLift".into(),
            },
            CustomHeaderDto {
                name: "Authorization".into(),
                value: "Bearer custom-override".into(),
            },
        ];
        assert!(validate_endpoint_and_key_with_headers(
            "https://api.example.com/v1",
            "",
            &[]
        )
        .is_err());
        assert!(validate_endpoint_and_key_with_headers(
            "https://api.example.com/v1",
            "",
            &custom_headers
        )
        .is_ok());

        let captured = Arc::new(std::sync::Mutex::new(Vec::<Vec<(String, String)>>::new()));
        let listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let addr = listener.local_addr().unwrap();
        let captured_srv = Arc::clone(&captured);
        let srv = tokio::spawn(async move {
            loop {
                let Ok((mut stream, _)) = listener.accept().await else {
                    break;
                };
                let captured_conn = Arc::clone(&captured_srv);
                tokio::spawn(async move {
                    let mut buf = Vec::new();
                    let mut chunk = [0u8; 4096];
                    let mut header_end = None;
                    let mut content_len = 0usize;
                    let mut parsed_headers = Vec::new();
                    loop {
                        let n = match stream.read(&mut chunk).await {
                            Ok(0) | Err(_) => return,
                            Ok(n) => n,
                        };
                        buf.extend_from_slice(&chunk[..n]);
                        if header_end.is_none() {
                            if let Some(pos) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
                                header_end = Some(pos + 4);
                                let hdr_str = String::from_utf8_lossy(&buf[..pos]);
                                for line in hdr_str.lines().skip(1) {
                                    if let Some((k, v)) = line.split_once(':') {
                                        let key = k.trim().to_ascii_lowercase();
                                        let val = v.trim().to_string();
                                        if key == "content-length" {
                                            content_len = val.parse().unwrap_or(0);
                                        }
                                        parsed_headers.push((key, val));
                                    }
                                }
                            }
                        }
                        if let Some(end) = header_end {
                            if buf.len() >= end + content_len {
                                captured_conn.lock().unwrap().push(parsed_headers);
                                let content = json!({"slots": [
                                    {"id": 1, "category": "dialogue", "lines": ["OK"]}
                                ]})
                                .to_string();
                                let body = json!({"choices": [{"finish_reason": "stop", "message": {"content": content}}]})
                                    .to_string();
                                let resp = format!(
                                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
                                    body.len()
                                );
                                let _ = stream.write_all(resp.as_bytes()).await;
                                let _ = stream.flush().await;
                                break;
                            }
                        }
                    }
                });
            }
        });

        let prepared = make_test_prepared(&[1]);
        let endpoint = format!("http://127.0.0.1:{}/v1/chat/completions", addr.port());
        let mut cancel_rx = watch::channel(false).1;

        // 1. Task-scoped session_id stays consistent across calls in the same session.
        let session = VlmSessionContext {
            http: reqwest::Client::new(),
            config: VlmEndpointConfig {
                endpoint: endpoint.clone(),
                api_key: String::new(),
                model: "custom-vl".into(),
                thinking_mode: ThinkingMode::Default,
                timeout_s: 10.0,
                max_retries: 1,
                custom_headers: custom_headers.clone(),
                session_id: "task-uuid-123".into(),
            },
            policy: Arc::new(AdaptivePolicyState::new(ThinkingMode::Default, "custom-vl", "")),
            rate_gate: Arc::new(RateLimitGate::new()),
            script: SubtitleScript::Auto,
        };
        for _ in 0..2 {
            let out = call_chat_completions(&session, &prepared.spec, &prepared.png_bytes, &mut cancel_rx)
                .await
                .unwrap();
            assert_eq!(out.slots[0].lines, vec!["OK"]);
        }

        // 2. Empty session_id falls back to per-call fresh UUID v4 values.
        let fallback_session = VlmSessionContext {
            config: VlmEndpointConfig {
                endpoint,
                api_key: String::new(),
                model: "custom-vl".into(),
                thinking_mode: ThinkingMode::Default,
                timeout_s: 10.0,
                max_retries: 1,
                custom_headers,
                session_id: String::new(),
            },
            ..session
        };
        for _ in 0..2 {
            let out = call_chat_completions(
                &fallback_session,
                &prepared.spec,
                &prepared.png_bytes,
                &mut cancel_rx,
            )
            .await
            .unwrap();
            assert_eq!(out.slots[0].lines, vec!["OK"]);
        }

        srv.abort();
        let reqs = captured.lock().unwrap();
        assert_eq!(reqs.len(), 4);

        for req in &reqs[0..2] {
            let find = |name: &str| {
                req.iter()
                    .filter(|(k, _)| k == name)
                    .map(|(_, v)| v.as_str())
                    .collect::<Vec<_>>()
            };
            assert_eq!(find("x-opencode-session"), vec!["task-uuid-123"]);
            assert_eq!(find("user-agent"), vec!["X-Custom-Client/1.0"]);
            assert_eq!(find("x-title"), vec!["SubLift"]);
            assert_eq!(find("authorization"), vec!["Bearer custom-override"]);
        }

        let fb1 = reqs[2]
            .iter()
            .find(|(k, _)| k == "x-opencode-session")
            .unwrap()
            .1
            .clone();
        let fb2 = reqs[3]
            .iter()
            .find(|(k, _)| k == "x-opencode-session")
            .unwrap()
            .1
            .clone();
        assert!(uuid::Uuid::parse_str(&fb1).is_ok());
        assert!(uuid::Uuid::parse_str(&fb2).is_ok());
        assert_ne!(fb1, fb2);
    }

    #[tokio::test]
    async fn probe_vlm_connectivity_verifies_vision_headers_downgrade_and_errors() {
        let calls = Arc::new(AtomicUsize::new(0));
        let seen_headers = Arc::new(std::sync::Mutex::new(Vec::<Vec<(String, String)>>::new()));
        let listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let addr = listener.local_addr().unwrap();
        let calls_srv = Arc::clone(&calls);
        let hdrs_srv = Arc::clone(&seen_headers);
        let srv = tokio::spawn(async move {
            loop {
                let Ok((mut stream, _)) = listener.accept().await else {
                    break;
                };
                let calls_conn = Arc::clone(&calls_srv);
                let hdrs_conn = Arc::clone(&hdrs_srv);
                tokio::spawn(async move {
                    let mut buf = Vec::new();
                    let mut chunk = [0u8; 4096];
                    let mut header_end = None;
                    let mut content_len = 0usize;
                    let mut parsed_headers = Vec::new();
                    loop {
                        let n = match stream.read(&mut chunk).await {
                            Ok(0) | Err(_) => return,
                            Ok(n) => n,
                        };
                        buf.extend_from_slice(&chunk[..n]);
                        if header_end.is_none() {
                            if let Some(pos) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
                                header_end = Some(pos + 4);
                                let hdr_str = String::from_utf8_lossy(&buf[..pos]);
                                for line in hdr_str.lines().skip(1) {
                                    if let Some((k, v)) = line.split_once(':') {
                                        let key = k.trim().to_ascii_lowercase();
                                        let val = v.trim().to_string();
                                        if key == "content-length" {
                                            content_len = val.parse().unwrap_or(0);
                                        }
                                        parsed_headers.push((key, val));
                                    }
                                }
                            }
                        }
                        if let Some(end) = header_end {
                            if buf.len() >= end + content_len {
                                let idx = calls_conn.fetch_add(1, Ordering::SeqCst);
                                let body_str =
                                    String::from_utf8_lossy(&buf[end..end + content_len]);
                                let req_json: Value = serde_json::from_str(&body_str).unwrap();
                                assert_eq!(req_json["max_tokens"], 16);
                                assert!(req_json["messages"][0]["content"][0]["image_url"]["url"]
                                    .as_str()
                                    .unwrap()
                                    .starts_with("data:image/png;base64,"));
                                hdrs_conn.lock().unwrap().push(parsed_headers);

                                let (status, resp_body) = match idx {
                                    0 => (
                                        400,
                                        json!({"error": {"message": "invalid params 2013: unsupported reasoning_effort"}}).to_string(),
                                    ),
                                    1 => (
                                        200,
                                        json!({"choices": [{"finish_reason": "stop", "message": {"content": "  OK\nconnected  "}}]}).to_string(),
                                    ),
                                    2 => (
                                        401,
                                        json!({"error": {"message": "invalid api key"}}).to_string(),
                                    ),
                                    _ => (
                                        200,
                                        json!({"base_resp": {"status_code": 1004, "status_msg": "cookie missing"}}).to_string(),
                                    ),
                                };
                                let reason = if status == 200 { "OK" } else { "Error" };
                                let resp = format!(
                                    "HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{resp_body}",
                                    resp_body.len()
                                );
                                let _ = stream.write_all(resp.as_bytes()).await;
                                let _ = stream.flush().await;
                                break;
                            }
                        }
                    }
                });
            }
        });

        let endpoint = format!("http://127.0.0.1:{}/v1/chat/completions", addr.port());
        let http = reqwest::Client::new();
        let custom_headers = vec![
            CustomHeaderDto {
                name: "x-opencode-session".into(),
                value: "{{session_id}}".into(),
            },
            CustomHeaderDto {
                name: "User-Agent".into(),
                value: "X-Probe-Client/1.0".into(),
            },
        ];

        // 1. Auto reasoning_effort 400 -> auto-downgrades and succeeds with custom headers & preview
        let cfg = VlmEndpointConfig {
            endpoint: endpoint.clone(),
            api_key: "sk-probe-123".into(),
            model: "MiniMax-M3.1-Flash".into(),
            thinking_mode: ThinkingMode::Auto,
            timeout_s: 10.0,
            max_retries: 1,
            custom_headers: custom_headers.clone(),
            session_id: "probe-sess-42".into(),
        };
        let res = probe_vlm_connectivity(&http, &cfg).await.unwrap();
        assert_eq!(res.model, "MiniMax-M3.1-Flash");
        assert_eq!(res.endpoint, endpoint);
        assert_eq!(res.response_preview, "OK connected");
        assert!(res.latency_ms >= 1);

        // 2. HTTP 401 -> VlmAuthError
        let err_401 = probe_vlm_connectivity(&http, &cfg).await.unwrap_err();
        assert_eq!(err_401.code(), "VlmAuthError");

        // 3. HTTP 200 with base_resp error -> VlmAuthError (1004 maps to fatal auth)
        let err_base = probe_vlm_connectivity(&http, &cfg).await.unwrap_err();
        assert_eq!(err_base.code(), "VlmAuthError");

        srv.abort();
        let hdrs = seen_headers.lock().unwrap();
        assert_eq!(hdrs.len(), 4);
        for req_hdrs in hdrs.iter() {
            let find = |k: &str| {
                req_hdrs
                    .iter()
                    .find(|(name, _)| name == k)
                    .map(|(_, v)| v.as_str())
            };
            assert_eq!(find("x-opencode-session"), Some("probe-sess-42"));
            assert_eq!(find("user-agent"), Some("X-Probe-Client/1.0"));
            assert_eq!(find("authorization"), Some("Bearer sk-probe-123"));
        }
    }
}
