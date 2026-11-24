use std::sync::Mutex as StdMutex;
use std::time::Duration;

use reqwest::header::HeaderMap;
use tokio::sync::watch;
use tokio::time::Instant;

use crate::dto::ThinkingMode;

use super::{wait_until_cancelled, VlmError};

#[derive(Debug)]
struct GateState {
    blocked_until: Option<Instant>,
    consecutive_429: u32,
}

pub struct RateLimitGate {
    state: StdMutex<GateState>,
}

impl Default for RateLimitGate {
    fn default() -> Self {
        Self::new()
    }
}

impl RateLimitGate {
    pub fn new() -> Self {
        Self {
            state: StdMutex::new(GateState {
                blocked_until: None,
                consecutive_429: 0,
            }),
        }
    }

    pub fn record_429(&self, retry_after_s: Option<f64>, attempt: u32) -> Duration {
        let jitter = rand::random_range(0.0_f64..0.5_f64);
        self.record_429_with_jitter(retry_after_s, attempt, jitter)
    }

    pub fn record_429_with_jitter(
        &self,
        retry_after_s: Option<f64>,
        attempt: u32,
        jitter_s: f64,
    ) -> Duration {
        let delay = compute_rate_limit_delay(retry_after_s, attempt, jitter_s);
        let deadline = Instant::now() + delay;
        let mut guard = self.state.lock().expect("rate limit gate lock poisoned");
        guard.consecutive_429 = guard.consecutive_429.saturating_add(1);
        guard.blocked_until = Some(match guard.blocked_until {
            Some(prev) if prev > deadline => prev,
            _ => deadline,
        });
        delay
    }

    pub fn record_success(&self) {
        let mut guard = self.state.lock().expect("rate limit gate lock poisoned");
        guard.consecutive_429 = 0;
    }

    pub fn is_rate_limited(&self) -> bool {
        let guard = self.state.lock().expect("rate limit gate lock poisoned");
        guard.consecutive_429 > 0
    }

    pub async fn wait_if_needed(&self, cancel_rx: &mut watch::Receiver<bool>) -> Result<(), VlmError> {
        loop {
            if *cancel_rx.borrow() {
                return Err(VlmError::Cancelled);
            }
            let maybe_deadline = {
                let guard = self.state.lock().expect("rate limit gate lock poisoned");
                guard.blocked_until
            };
            match maybe_deadline {
                None => return Ok(()),
                Some(deadline) => {
                    if Instant::now() >= deadline {
                        return Ok(());
                    }
                    tokio::select! {
                        biased;
                        _ = wait_until_cancelled(cancel_rx) => {
                            return Err(VlmError::Cancelled);
                        }
                        _ = tokio::time::sleep_until(deadline) => {}
                    }
                }
            }
        }
    }
}

pub fn compute_rate_limit_delay(
    retry_after_s: Option<f64>,
    attempt: u32,
    jitter_s: f64,
) -> Duration {
    if let Some(ra) = retry_after_s {
        if ra.is_finite() && ra >= 0.0 {
            let capped = Duration::from_secs_f64(ra).min(Duration::from_secs(60));
            return capped;
        }
    }
    let exp = attempt.min(5) as i32;
    let base_s = 1.5_f64 * 2.0_f64.powi(exp) + jitter_s.clamp(0.0, 0.5);
    Duration::from_secs_f64(base_s).min(Duration::from_secs(45))
}

pub fn parse_retry_after(headers: &HeaderMap) -> Option<f64> {
    let raw = headers.get("retry-after")?;
    let text = raw.to_str().ok()?.trim();
    let parsed = text.parse::<f64>().ok()?;
    if parsed.is_finite() {
        Some(parsed.max(0.0))
    } else {
        None
    }
}

const FATAL_HTTP_STATUSES: [u16; 4] = [401, 402, 403, 404];

pub fn is_fatal_http(status: Option<u16>, message: &str, base_code: Option<i64>) -> bool {
    if let Some(code) = status {
        if FATAL_HTTP_STATUSES.contains(&code) {
            return true;
        }
    }
    if let Some(bc) = base_code {
        if matches!(bc, 1004 | 1008 | 2049) {
            return true;
        }
    }
    let lower = message.to_ascii_lowercase();
    [
        "unauthorized",
        "invalid_api_key",
        "invalid api key",
        "authentication_error",
        "insufficient_quota",
        "insufficient balance",
        "model_not_found",
    ]
    .iter()
    .any(|tok| lower.contains(tok))
}

pub fn is_rate_limit_error(status: Option<u16>, message: &str, base_code: Option<i64>) -> bool {
    if status == Some(429) {
        return true;
    }
    if let Some(bc) = base_code {
        if matches!(bc, 429 | 1002) {
            return true;
        }
    }
    let lower = message.to_ascii_lowercase();
    ["rate limit", "too many requests", "rpm", "tpm"]
        .iter()
        .any(|tok| lower.contains(tok))
}

pub fn is_content_filter_error(
    message: &str,
    base_code: Option<i64>,
    finish_reason: Option<&str>,
    refusal: Option<&str>,
) -> bool {
    if finish_reason
        .map(|s| s.trim().eq_ignore_ascii_case("content_filter"))
        .unwrap_or(false)
    {
        return true;
    }
    if refusal.map(|s| !s.trim().is_empty()).unwrap_or(false) {
        return true;
    }
    if base_code == Some(1027) {
        return true;
    }
    let lower = message.to_ascii_lowercase();
    [
        "content_filter",
        "content_policy",
        "content safety",
        "safety filter",
        "sensitive",
        "moderation",
        "inappropriate",
        "violates",
        "prohibited",
    ]
    .iter()
    .any(|tok| lower.contains(tok))
}

pub fn initial_effort_candidates(
    thinking_mode: ThinkingMode,
    model: &str,
    endpoint: &str,
) -> Vec<Option<String>> {
    match thinking_mode {
        ThinkingMode::Default => vec![None],
        ThinkingMode::Auto => {
            let hint = format!("{model} {endpoint}").to_ascii_lowercase();
            if hint.contains("minimax") {
                vec![Some("low".to_string()), None]
            } else {
                vec![Some("none".to_string()), Some("low".to_string()), None]
            }
        }
        ThinkingMode::None => vec![Some("none".to_string()), Some("low".to_string()), None],
        ThinkingMode::Minimal => vec![Some("minimal".to_string()), Some("low".to_string()), None],
        ThinkingMode::Low => vec![Some("low".to_string()), None],
    }
}

#[derive(Debug)]
struct PolicyInner {
    effort_chain: Vec<Option<String>>,
    effort_idx: usize,
    use_json_format: bool,
}

pub struct AdaptivePolicyState {
    inner: StdMutex<PolicyInner>,
}

impl AdaptivePolicyState {
    pub fn new(thinking_mode: ThinkingMode, model: &str, endpoint: &str) -> Self {
        Self {
            inner: StdMutex::new(PolicyInner {
                effort_chain: initial_effort_candidates(thinking_mode, model, endpoint),
                effort_idx: 0,
                use_json_format: true,
            }),
        }
    }

    pub fn current_params(&self) -> (Option<String>, bool) {
        let guard = self.inner.lock().expect("adaptive policy lock poisoned");
        let idx = guard.effort_idx.min(guard.effort_chain.len() - 1);
        (guard.effort_chain[idx].clone(), guard.use_json_format)
    }

    pub fn downgrade_effort(&self, failed_effort: Option<&str>) -> bool {
        let mut guard = self.inner.lock().expect("adaptive policy lock poisoned");
        let cur_idx = guard.effort_idx.min(guard.effort_chain.len() - 1);
        let cur = guard.effort_chain[cur_idx].as_deref();
        if cur != failed_effort {
            // Another worker already advanced the shared policy past `failed_effort`.
            return true;
        }
        if guard.effort_idx + 1 < guard.effort_chain.len() {
            guard.effort_idx += 1;
            return true;
        }
        false
    }

    pub fn disable_json_format(&self, attempted_json_format: bool) -> bool {
        if !attempted_json_format {
            return false;
        }
        let mut guard = self.inner.lock().expect("adaptive policy lock poisoned");
        guard.use_json_format = false;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use reqwest::header::HeaderValue;

    #[test]
    fn rate_limit_gate_backoff_math_and_retry_after_cap() {
        assert_eq!(
            compute_rate_limit_delay(None, 0, 0.25),
            Duration::from_secs_f64(1.75)
        );
        assert_eq!(
            compute_rate_limit_delay(Some(3.0), 0, 0.25),
            Duration::from_secs_f64(3.0)
        );
        assert_eq!(
            compute_rate_limit_delay(Some(120.0), 0, 0.25),
            Duration::from_secs(60)
        );
        assert_eq!(
            compute_rate_limit_delay(None, 10, 0.5),
            Duration::from_secs(45)
        );

        let mut headers = HeaderMap::new();
        headers.insert("Retry-After", HeaderValue::from_static("4.5"));
        assert_eq!(parse_retry_after(&headers), Some(4.5));

        let gate = RateLimitGate::new();
        assert!(!gate.is_rate_limited());
        let d = gate.record_429_with_jitter(Some(0.01), 0, 0.0);
        assert_eq!(d, Duration::from_secs_f64(0.01));
        assert!(gate.is_rate_limited());
        gate.record_success();
        assert!(!gate.is_rate_limited());
    }

    #[tokio::test]
    async fn rate_limit_gate_wakes_immediately_on_cancel() {
        let gate = RateLimitGate::new();
        gate.record_429_with_jitter(Some(30.0), 0, 0.0);
        let (tx, mut rx) = watch::channel(false);

        let handle = tokio::spawn(async move { gate.wait_if_needed(&mut rx).await });
        tx.send(true).unwrap();
        let res = tokio::time::timeout(Duration::from_secs(2), handle)
            .await
            .expect("wait_if_needed did not wake on cancel")
            .unwrap();
        assert!(matches!(res, Err(VlmError::Cancelled)));
    }

    #[test]
    fn adaptive_policy_compare_and_advance_prevents_double_skip() {
        let policy = AdaptivePolicyState::new(
            ThinkingMode::Auto,
            "nvidia/nemotron-3.5-super-vl",
            "https://integrate.api.nvidia.com/v1/chat/completions",
        );
        assert_eq!(policy.current_params(), (Some("none".into()), true));

        // Two workers fail concurrently with "none": first advances to "low", second sees cur != "none" and does not skip to None!
        assert!(policy.downgrade_effort(Some("none")));
        assert_eq!(policy.current_params(), (Some("low".into()), true));
        assert!(policy.downgrade_effort(Some("none")));
        assert_eq!(policy.current_params(), (Some("low".into()), true));

        // Now "low" fails -> advances to None; concurrent "low" failure still returns true so peers retry
        assert!(policy.downgrade_effort(Some("low")));
        assert_eq!(policy.current_params(), (None, true));
        assert!(policy.downgrade_effort(Some("low")));
        assert_eq!(policy.current_params(), (None, true));
        assert!(!policy.downgrade_effort(None));

        // disable_json_format returns true whenever the attempt used json_format (even if a peer already flipped it)
        assert!(policy.disable_json_format(true));
        assert!(policy.disable_json_format(true));
        assert!(!policy.disable_json_format(false));
        assert_eq!(policy.current_params(), (None, false));

        // MiniMax auto chain starts at "low" -> None
        let mm = AdaptivePolicyState::new(
            ThinkingMode::Auto,
            "MiniMax-M3.1-Flash",
            "https://api.minimax.cn/v1/chat/completions",
        );
        assert_eq!(mm.current_params(), (Some("low".into()), true));
        assert!(mm.downgrade_effort(Some("low")));
        assert!(mm.downgrade_effort(Some("low")));
        assert_eq!(mm.current_params(), (None, true));
        assert!(!mm.downgrade_effort(None));
    }

    #[test]
    fn error_classification_matches_taxonomy() {
        assert!(is_fatal_http(Some(401), "", None));
        assert!(is_fatal_http(Some(402), "", None));
        assert!(is_fatal_http(Some(403), "", None));
        assert!(is_fatal_http(Some(404), "", None));
        assert!(is_fatal_http(Some(200), "", Some(1004)));
        assert!(is_fatal_http(Some(200), "", Some(1008)));
        assert!(is_fatal_http(Some(200), "", Some(2049)));
        assert!(is_fatal_http(Some(400), "insufficient_quota", None));

        assert!(is_rate_limit_error(Some(429), "", None));
        assert!(is_rate_limit_error(Some(200), "", Some(1002)));
        assert!(is_rate_limit_error(Some(503), "TPM exceeded", None));

        assert!(is_content_filter_error("", None, Some("content_filter"), None));
        assert!(is_content_filter_error("", None, None, Some("Cannot comply")));
        assert!(is_content_filter_error("", Some(1027), None, None));
        assert!(is_content_filter_error("Safety Filter triggered", None, None, None));
    }
}
