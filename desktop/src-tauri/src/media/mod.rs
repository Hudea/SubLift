pub mod native;
pub mod probe;
pub mod remux;

use std::borrow::Cow;
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicU64;
use std::sync::Arc;

use http::{header, Request, Response, StatusCode};
use percent_encoding::{utf8_percent_encode, NON_ALPHANUMERIC};
use thiserror::Error;
use tokio::sync::Mutex;

use native::serve_native_file;
use probe::{MediaClassification, MediaProbeCache};
use remux::{serve_remux_request, ActiveRemuxSession};

#[derive(Clone, Debug, Error)]
pub enum MediaError {
    #[error("bad media request: {0}")]
    BadRequest(String),
    #[error("media file not found: {0}")]
    NotFound(String),
    #[error("media probe failed: {0}")]
    ProbeFailed(String),
    #[error("media remux failed: {0}")]
    RemuxFailed(String),
    #[error("media I/O error: {0}")]
    Io(String),
    #[error("media stream cancelled")]
    Cancelled,
}

pub struct MediaStreamManager {
    probe_cache: MediaProbeCache,
    active: Mutex<Option<Arc<ActiveRemuxSession>>>,
    generation: AtomicU64,
}

impl Default for MediaStreamManager {
    fn default() -> Self {
        Self::new()
    }
}

impl MediaStreamManager {
    pub fn new() -> Self {
        Self {
            probe_cache: MediaProbeCache::new(),
            active: Mutex::new(None),
            generation: AtomicU64::new(0),
        }
    }

    pub async fn stop_active_stream(&self) {
        let taken = {
            let mut guard = self.active.lock().await;
            guard.take()
        };
        if let Some(sess) = taken {
            sess.terminate_and_reap().await;
        }
    }

    pub async fn stop_all(&self) {
        self.stop_active_stream().await;
    }

    pub async fn has_active_session(&self) -> bool {
        self.active.lock().await.is_some()
    }

    pub async fn handle_http_request(&self, request: &Request<Vec<u8>>) -> Response<Cow<'static, [u8]>> {
        match self.handle_inner(request).await {
            Ok(resp) => resp,
            Err(err) => error_response(&err),
        }
    }

    async fn handle_inner(
        &self,
        request: &Request<Vec<u8>>,
    ) -> Result<Response<Cow<'static, [u8]>>, MediaError> {
        let (raw_path, start_ms) = parse_media_uri(request.uri())?;
        if !raw_path.is_absolute() {
            return Err(MediaError::BadRequest(
                "media path query parameter must be an absolute path".into(),
            ));
        }

        let canonical = tokio::fs::canonicalize(&raw_path)
            .await
            .map_err(|e| MediaError::NotFound(format!("{}: {e}", raw_path.display())))?;

        let probe_info = self.probe_cache.probe(&canonical).await?;
        let range_header = request
            .headers()
            .get(header::RANGE)
            .and_then(|v| v.to_str().ok());

        match &probe_info.classification {
            MediaClassification::NativeDirect { mime } => {
                // Switching back to native file stops any lingering remux child
                self.stop_active_stream().await;
                serve_native_file(&canonical, request.method(), range_header, mime).await
            }
            MediaClassification::RemuxCopy { .. } | MediaClassification::TranscodeVideo => {
                serve_remux_request(
                    &self.active,
                    &self.generation,
                    &canonical,
                    start_ms,
                    &probe_info.classification,
                    request.method(),
                    range_header,
                )
                .await
            }
        }
    }
}

pub(crate) fn is_executable_file(path: &Path) -> bool {
    let Ok(meta) = std::fs::metadata(path) else {
        return false;
    };
    if !meta.is_file() {
        return false;
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        meta.permissions().mode() & 0o111 != 0
    }
    #[cfg(not(unix))]
    {
        true
    }
}

fn expand_tilde(raw: &std::ffi::OsStr) -> PathBuf {
    if let Some(s) = raw.to_str() {
        if let Some(rest) = s.strip_prefix("~/") {
            if let Some(home) = std::env::var_os("HOME") {
                return PathBuf::from(home).join(rest);
            }
        } else if s == "~" {
            if let Some(home) = std::env::var_os("HOME") {
                return PathBuf::from(home);
            }
        }
    }
    PathBuf::from(raw)
}

pub(crate) fn resolve_media_binary_with(
    env_var: &str,
    bin_name: &str,
    env_override: Option<&std::ffi::OsStr>,
    path_env: Option<&std::ffi::OsStr>,
    fallbacks: &[&str],
) -> Result<PathBuf, String> {
    if let Some(raw_env) = env_override {
        if !raw_env.is_empty() {
            let candidate = expand_tilde(raw_env);
            if is_executable_file(&candidate) {
                return Ok(candidate);
            }
            return Err(format!(
                "{bin_name} executable not found at {env_var}: {}",
                candidate.display()
            ));
        }
    }

    let bin_filename = format!("{bin_name}{}", std::env::consts::EXE_SUFFIX);
    if let Some(path_val) = path_env {
        for dir in std::env::split_paths(path_val) {
            if dir.as_os_str().is_empty() {
                continue;
            }
            let cand = dir.join(&bin_filename);
            if is_executable_file(&cand) {
                return Ok(cand);
            }
        }
    }

    for fb in fallbacks {
        let cand = Path::new(fb);
        if is_executable_file(cand) {
            return Ok(cand.to_path_buf());
        }
    }

    Err(format!(
        "{bin_name} executable not found on PATH or standard Homebrew locations (set {env_var})"
    ))
}

pub(crate) fn resolve_ffmpeg_bin() -> Result<PathBuf, String> {
    let env_val = std::env::var_os("SUBLIFT_FFMPEG_PATH");
    let path_val = std::env::var_os("PATH");
    resolve_media_binary_with(
        "SUBLIFT_FFMPEG_PATH",
        "ffmpeg",
        env_val.as_deref(),
        path_val.as_deref(),
        &[
            "/opt/homebrew/opt/ffmpeg-full/bin/ffmpeg",
            "/opt/homebrew/bin/ffmpeg",
            "/usr/local/bin/ffmpeg",
            "/usr/bin/ffmpeg",
        ],
    )
}

pub(crate) fn resolve_ffprobe_bin() -> Result<PathBuf, String> {
    let env_val = std::env::var_os("SUBLIFT_FFPROBE_PATH");
    let path_val = std::env::var_os("PATH");
    resolve_media_binary_with(
        "SUBLIFT_FFPROBE_PATH",
        "ffprobe",
        env_val.as_deref(),
        path_val.as_deref(),
        &[
            "/opt/homebrew/opt/ffmpeg-full/bin/ffprobe",
            "/opt/homebrew/bin/ffprobe",
            "/usr/local/bin/ffprobe",
            "/usr/bin/ffprobe",
        ],
    )
}

pub fn build_media_url(path: &Path, start_sec: Option<f64>) -> String {
    let encoded = utf8_percent_encode(&path.to_string_lossy(), NON_ALPHANUMERIC).to_string();
    match start_sec {
        Some(t) if t.is_finite() && t > 0.0 => {
            format!("sublift://localhost/media?path={encoded}&t={t:.3}")
        }
        _ => format!("sublift://localhost/media?path={encoded}"),
    }
}

pub fn parse_media_uri(uri: &http::Uri) -> Result<(PathBuf, u64), MediaError> {
    if uri.path() != "/media" {
        return Err(MediaError::NotFound(format!(
            "unknown sublift route '{}'",
            uri.path()
        )));
    }
    let query = uri
        .query()
        .ok_or_else(|| MediaError::BadRequest("missing query parameters".into()))?;

    let mut path_opt: Option<String> = None;
    let mut start_ms: u64 = 0;

    for (k, v) in url::form_urlencoded::parse(query.as_bytes()) {
        if k == "path" {
            path_opt = Some(v.into_owned());
        } else if k == "t" {
            let secs: f64 = v
                .parse()
                .map_err(|_| MediaError::BadRequest("invalid 't' parameter".into()))?;
            if !secs.is_finite() || secs < 0.0 {
                return Err(MediaError::BadRequest(
                    "'t' parameter must be non-negative finite seconds".into(),
                ));
            }
            start_ms = (secs * 1000.0).round() as u64;
        }
    }

    let raw_path = path_opt
        .filter(|s| !s.is_empty())
        .ok_or_else(|| MediaError::BadRequest("missing 'path' query parameter".into()))?;

    Ok((PathBuf::from(raw_path), start_ms))
}

fn error_response(err: &MediaError) -> Response<Cow<'static, [u8]>> {
    let status = match err {
        MediaError::BadRequest(_) => StatusCode::BAD_REQUEST,
        MediaError::NotFound(_) => StatusCode::NOT_FOUND,
        MediaError::Cancelled => StatusCode::SERVICE_UNAVAILABLE,
        MediaError::ProbeFailed(_) | MediaError::RemuxFailed(_) | MediaError::Io(_) => {
            StatusCode::INTERNAL_SERVER_ERROR
        }
    };
    let body = serde_json::json!({ "error": err.to_string() }).to_string();
    Response::builder()
        .status(status)
        .header(header::CONTENT_TYPE, "application/json")
        .header(header::ACCESS_CONTROL_ALLOW_ORIGIN, "*")
        .body(Cow::Owned(body.into_bytes()))
        .expect("valid error response")
}

pub fn register_sublift_protocol<R: tauri::Runtime>(
    builder: tauri::Builder<R>,
    manager: Arc<MediaStreamManager>,
) -> tauri::Builder<R> {
    builder.register_asynchronous_uri_scheme_protocol("sublift", move |_ctx, request, responder| {
        let mgr = Arc::clone(&manager);
        tauri::async_runtime::spawn(async move {
            let resp = mgr.handle_http_request(&request).await;
            responder.respond(resp);
        });
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use http::Method;

    #[test]
    fn parses_media_uri_for_macos_and_windows_hosts() {
        let mac_uri: http::Uri = "sublift://localhost/media?path=%2Ftmp%2Fvideo.mp4&t=12.5"
            .parse()
            .unwrap();
        let (p1, ms1) = parse_media_uri(&mac_uri).unwrap();
        assert_eq!(p1, PathBuf::from("/tmp/video.mp4"));
        assert_eq!(ms1, 12500);

        let win_uri: http::Uri = "http://sublift.localhost/media?path=%2Ftmp%2Fclip.mkv"
            .parse()
            .unwrap();
        let (p2, ms2) = parse_media_uri(&win_uri).unwrap();
        assert_eq!(p2, PathBuf::from("/tmp/clip.mkv"));
        assert_eq!(ms2, 0);

        let bad_t: http::Uri = "sublift://localhost/media?path=%2Ftmp%2Fa.mp4&t=-1"
            .parse()
            .unwrap();
        assert!(matches!(
            parse_media_uri(&bad_t),
            Err(MediaError::BadRequest(_))
        ));
    }

    #[tokio::test]
    async fn media_stream_manager_serves_remux_and_stops_cleanly_on_mkv() {
        let repo_root = Path::new(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .unwrap()
            .parent()
            .unwrap();
        let mkv = repo_root.join("debug/smoke-test-data/Zootopia_clip_hardsub1.mkv");
        if !mkv.is_file() {
            return;
        }

        let mgr = MediaStreamManager::new();
        let url = build_media_url(&mkv, Some(0.0));
        let req = Request::builder()
            .method(Method::GET)
            .uri(url)
            .header(header::RANGE, "bytes=0-1")
            .body(Vec::new())
            .unwrap();

        let resp = mgr.handle_http_request(&req).await;
        assert_eq!(resp.status(), StatusCode::PARTIAL_CONTENT);
        assert_eq!(resp.body().len(), 2);
        assert!(mgr.has_active_session().await);

        mgr.stop_active_stream().await;
        assert!(!mgr.has_active_session().await);
    }

    #[test]
    fn resolve_media_binary_with_prefers_env_then_path_then_fallbacks() {
        let dir = tempfile::TempDir::new().unwrap();
        let make_exec = |p: &Path| {
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(p, b"#!/bin/sh\nexit 0\n").unwrap();
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                std::fs::set_permissions(p, std::fs::Permissions::from_mode(0o755)).unwrap();
            }
        };

        let env_bin = dir.path().join("env/ffmpeg");
        let path_dir = dir.path().join("path_dir");
        let path_bin = path_dir.join(format!("ffmpeg{}", std::env::consts::EXE_SUFFIX));
        let fb_bin = dir.path().join("homebrew/bin/ffmpeg");
        make_exec(&env_bin);
        make_exec(&path_bin);
        make_exec(&fb_bin);

        let fb_str = fb_bin.to_string_lossy().to_string();
        let fallbacks = [fb_str.as_str()];

        // 1. Env override wins
        let resolved_env = resolve_media_binary_with(
            "SUBLIFT_FFMPEG_PATH",
            "ffmpeg",
            Some(env_bin.as_os_str()),
            Some(path_dir.as_os_str()),
            &fallbacks,
        )
        .unwrap();
        assert_eq!(resolved_env, env_bin);

        // 2. Invalid env override fails closed
        let missing_env = dir.path().join("missing/ffmpeg");
        assert!(resolve_media_binary_with(
            "SUBLIFT_FFMPEG_PATH",
            "ffmpeg",
            Some(missing_env.as_os_str()),
            Some(path_dir.as_os_str()),
            &fallbacks,
        )
        .is_err());

        // 3. PATH wins over fallback when env unset
        let resolved_path = resolve_media_binary_with(
            "SUBLIFT_FFMPEG_PATH",
            "ffmpeg",
            None,
            Some(path_dir.as_os_str()),
            &fallbacks,
        )
        .unwrap();
        assert_eq!(resolved_path, path_bin);

        // 4. Fallback used when PATH empty
        let resolved_fb = resolve_media_binary_with(
            "SUBLIFT_FFMPEG_PATH",
            "ffmpeg",
            None,
            Some(std::ffi::OsStr::new("")),
            &fallbacks,
        )
        .unwrap();
        assert_eq!(resolved_fb, fb_bin);
    }

    #[tokio::test]
    async fn uri_routing_errors_and_switching_mkv_to_mp4_stops_remux() {
        // URI validation edge cases
        let bad_route: http::Uri = "sublift://localhost/other?path=%2Ftmp%2Fa.mp4"
            .parse()
            .unwrap();
        assert!(matches!(
            parse_media_uri(&bad_route),
            Err(MediaError::NotFound(_))
        ));

        let no_query: http::Uri = "sublift://localhost/media".parse().unwrap();
        assert!(matches!(
            parse_media_uri(&no_query),
            Err(MediaError::BadRequest(_))
        ));

        let empty_path: http::Uri = "sublift://localhost/media?path=".parse().unwrap();
        assert!(matches!(
            parse_media_uri(&empty_path),
            Err(MediaError::BadRequest(_))
        ));

        for bad_t in ["NaN", "inf", "-inf"] {
            let uri: http::Uri = format!("sublift://localhost/media?path=%2Ftmp%2Fa.mp4&t={bad_t}")
                .parse()
                .unwrap();
            assert!(matches!(
                parse_media_uri(&uri),
                Err(MediaError::BadRequest(_))
            ));
        }

        let mgr = MediaStreamManager::new();

        // Relative path -> 400
        let rel_req = Request::builder()
            .method(Method::GET)
            .uri("sublift://localhost/media?path=video.mp4")
            .body(Vec::new())
            .unwrap();
        assert_eq!(
            mgr.handle_http_request(&rel_req).await.status(),
            StatusCode::BAD_REQUEST
        );

        // Directory path -> 404
        let dir = tempfile::TempDir::new().unwrap();
        let dir_req = Request::builder()
            .method(Method::GET)
            .uri(build_media_url(dir.path(), None))
            .body(Vec::new())
            .unwrap();
        assert_eq!(
            mgr.handle_http_request(&dir_req).await.status(),
            StatusCode::NOT_FOUND
        );

        // Switching from MKV (RemuxCopy) to MP4 (NativeDirect) stops active remux session
        let Ok(ffmpeg) = resolve_ffmpeg_bin() else {
            return;
        };
        if resolve_ffprobe_bin().is_err() {
            return;
        }
        let mkv = dir.path().join("clip.mkv");
        let mp4 = dir.path().join("clip.mp4");
        assert!(std::process::Command::new(&ffmpeg)
            .args([
                "-hide_banner", "-loglevel", "error",
                "-f", "lavfi", "-i", "color=c=black:s=64x64:d=0.5",
                "-c:v", "libx264", "-pix_fmt", "yuv420p", "-y",
            ])
            .arg(&mkv)
            .status()
            .unwrap()
            .success());
        assert!(std::process::Command::new(&ffmpeg)
            .args([
                "-hide_banner", "-loglevel", "error",
                "-f", "lavfi", "-i", "color=c=black:s=64x64:d=0.5",
                "-c:v", "libx264", "-pix_fmt", "yuv420p", "-y",
            ])
            .arg(&mp4)
            .status()
            .unwrap()
            .success());

        let mkv_req = Request::builder()
            .method(Method::GET)
            .uri(build_media_url(&mkv, Some(0.0)))
            .body(Vec::new())
            .unwrap();
        let mkv_resp = mgr.handle_http_request(&mkv_req).await;
        assert!(matches!(
            mkv_resp.status(),
            StatusCode::OK | StatusCode::PARTIAL_CONTENT
        ));
        assert!(mgr.has_active_session().await);

        let mp4_req = Request::builder()
            .method(Method::GET)
            .uri(build_media_url(&mp4, None))
            .body(Vec::new())
            .unwrap();
        let mp4_resp = mgr.handle_http_request(&mp4_req).await;
        assert_eq!(mp4_resp.status(), StatusCode::OK);
        assert!(!mgr.has_active_session().await);
    }
}
