use std::borrow::Cow;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use http::{Method, Response};
use tokio::io::AsyncReadExt;
use tokio::process::{Child, Command};
use tokio::sync::{watch, Mutex, OnceCell};

use super::native::serve_in_memory_bytes;
use super::probe::MediaClassification;
use super::MediaError;

pub const REMUX_WINDOW_SEC: f64 = 20.0;
pub const MAX_REMUX_BYTES: usize = 64 * 1024 * 1024;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RemuxKey {
    pub canonical_path: PathBuf,
    pub start_ms: u64,
}

pub struct ActiveRemuxSession {
    pub key: RemuxKey,
    pub generation: u64,
    pub cancel_tx: watch::Sender<bool>,
    pub child_slot: Arc<Mutex<Option<Child>>>,
    pub buffer: Arc<OnceCell<Result<Arc<Vec<u8>>, MediaError>>>,
}

impl ActiveRemuxSession {
    pub async fn terminate_and_reap(&self) {
        let _ = self.cancel_tx.send(true);
        kill_and_reap(&self.child_slot).await;
    }
}

pub async fn kill_and_reap(child_slot: &Mutex<Option<Child>>) {
    let mut guard = child_slot.lock().await;
    if let Some(mut child) = guard.take() {
        let _ = child.start_kill();
        let _ = child.wait().await;
    }
}

struct RemuxAbortGuard {
    armed: bool,
    cancel_tx: watch::Sender<bool>,
    child_slot: Arc<Mutex<Option<Child>>>,
}

impl RemuxAbortGuard {
    fn disarm(&mut self) {
        self.armed = false;
    }
}

impl Drop for RemuxAbortGuard {
    fn drop(&mut self) {
        if !self.armed {
            return;
        }
        let _ = self.cancel_tx.send(true);

        if let Ok(mut guard) = self.child_slot.try_lock() {
            if let Some(mut child) = guard.take() {
                let _ = child.start_kill();
                if matches!(child.try_wait(), Ok(Some(_))) {
                    return;
                }
                if let Ok(handle) = tokio::runtime::Handle::try_current() {
                    handle.spawn(async move {
                        let _ = child.wait().await;
                    });
                } else {
                    std::thread::spawn(move || {
                        for _ in 0..500 {
                            match child.try_wait() {
                                Ok(Some(_)) | Err(_) => break,
                                Ok(None) => std::thread::yield_now(),
                            }
                        }
                    });
                }
            }
            return;
        }

        let slot = Arc::clone(&self.child_slot);
        if let Ok(handle) = tokio::runtime::Handle::try_current() {
            handle.spawn(async move {
                kill_and_reap(&slot).await;
            });
        } else {
            std::thread::spawn(move || {
                if let Ok(mut guard) = slot.try_lock() {
                    if let Some(mut child) = guard.take() {
                        let _ = child.start_kill();
                        for _ in 0..500 {
                            match child.try_wait() {
                                Ok(Some(_)) | Err(_) => break,
                                Ok(None) => std::thread::yield_now(),
                            }
                        }
                    }
                }
            });
        }
    }
}

pub async fn serve_remux_request(
    active_slot: &Mutex<Option<Arc<ActiveRemuxSession>>>,
    generation_counter: &AtomicU64,
    canonical_path: &Path,
    start_ms: u64,
    classification: &MediaClassification,
    method: &Method,
    range_header: Option<&str>,
) -> Result<Response<Cow<'static, [u8]>>, MediaError> {
    let key = RemuxKey {
        canonical_path: canonical_path.to_path_buf(),
        start_ms,
    };

    let session = {
        let mut guard = active_slot.lock().await;
        if let Some(existing) = guard.as_ref() {
            let poisoned = matches!(existing.buffer.get(), Some(Err(_)));
            if existing.key == key && !*existing.cancel_tx.borrow() && !poisoned {
                Arc::clone(existing)
            } else {
                let old = guard.take();
                if let Some(old_sess) = old {
                    old_sess.terminate_and_reap().await;
                }
                let (cancel_tx, _) = watch::channel(false);
                let fresh = Arc::new(ActiveRemuxSession {
                    key: key.clone(),
                    generation: generation_counter.fetch_add(1, Ordering::Relaxed) + 1,
                    cancel_tx,
                    child_slot: Arc::new(Mutex::new(None)),
                    buffer: Arc::new(OnceCell::new()),
                });
                *guard = Some(Arc::clone(&fresh));
                fresh
            }
        } else {
            let (cancel_tx, _) = watch::channel(false);
            let fresh = Arc::new(ActiveRemuxSession {
                key: key.clone(),
                generation: generation_counter.fetch_add(1, Ordering::Relaxed) + 1,
                cancel_tx,
                child_slot: Arc::new(Mutex::new(None)),
                buffer: Arc::new(OnceCell::new()),
            });
            *guard = Some(Arc::clone(&fresh));
            fresh
        }
    };

    let classification_owned = classification.clone();
    let path_owned = canonical_path.to_path_buf();
    let result = session
        .buffer
        .get_or_init(|| async {
            run_ffmpeg_remux(
                &path_owned,
                start_ms,
                &classification_owned,
                session.cancel_tx.clone(),
                Arc::clone(&session.child_slot),
            )
            .await
        })
        .await;

    match result {
        Ok(bytes) => Ok(serve_in_memory_bytes(
            method,
            range_header,
            bytes.as_slice(),
            "video/mp4",
            "no-store",
        )),
        Err(err) => {
            let mut guard = active_slot.lock().await;
            if guard.as_ref().is_some_and(|s| Arc::ptr_eq(s, &session)) {
                *guard = None;
            }
            Err(err.clone())
        }
    }
}

async fn run_ffmpeg_remux(
    path: &Path,
    start_ms: u64,
    classification: &MediaClassification,
    cancel_tx: watch::Sender<bool>,
    child_slot: Arc<Mutex<Option<Child>>>,
) -> Result<Arc<Vec<u8>>, MediaError> {
    let mut cancel_rx = cancel_tx.subscribe();
    let mut abort_guard = RemuxAbortGuard {
        armed: true,
        cancel_tx,
        child_slot: Arc::clone(&child_slot),
    };

    if *cancel_rx.borrow() {
        abort_guard.disarm();
        return Err(MediaError::Cancelled);
    }

    let start_sec = format!("{:.3}", (start_ms as f64) / 1000.0);
    let window_sec = format!("{REMUX_WINDOW_SEC:.1}");

    let ffmpeg_bin = match super::resolve_ffmpeg_bin() {
        Ok(p) => p,
        Err(e) => {
            abort_guard.disarm();
            return Err(MediaError::RemuxFailed(e));
        }
    };
    let mut cmd = Command::new(&ffmpeg_bin);
    cmd.arg("-hide_banner")
        .arg("-loglevel")
        .arg("error")
        .arg("-ss")
        .arg(&start_sec)
        .arg("-t")
        .arg(&window_sec)
        .arg("-i")
        .arg(path)
        .arg("-map")
        .arg("0:v:0")
        .arg("-map")
        .arg("0:a:0?");

    match classification {
        MediaClassification::RemuxCopy { video_codec } => {
            cmd.arg("-c:v").arg("copy");
            if video_codec == "hevc" {
                cmd.arg("-tag:v").arg("hvc1");
            }
        }
        MediaClassification::TranscodeVideo | MediaClassification::NativeDirect { .. } => {
            cmd.arg("-c:v")
                .arg("libx264")
                .arg("-preset")
                .arg("ultrafast")
                .arg("-pix_fmt")
                .arg("yuv420p");
        }
    }

    cmd.arg("-c:a")
        .arg("aac")
        .arg("-ac")
        .arg("2")
        .arg("-f")
        .arg("mp4")
        .arg("-movflags")
        .arg("frag_keyframe+empty_moov+default_base_moof")
        .arg("pipe:1")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true);

    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => {
            abort_guard.disarm();
            return Err(MediaError::RemuxFailed(format!("failed to spawn ffmpeg: {e}")));
        }
    };

    let Some(mut stdout) = child.stdout.take() else {
        let _ = child.start_kill();
        let _ = child.wait().await;
        abort_guard.disarm();
        return Err(MediaError::RemuxFailed("failed to capture ffmpeg stdout".into()));
    };
    let mut stderr = child.stderr.take();

    {
        let mut slot = child_slot.lock().await;
        *slot = Some(child);
    }

    if *cancel_rx.borrow() {
        kill_and_reap(&child_slot).await;
        abort_guard.disarm();
        return Err(MediaError::Cancelled);
    }

    let mut out = Vec::with_capacity(1024 * 1024);
    let mut chunk = vec![0u8; 64 * 1024];
    let mut cancelled = false;
    let mut hit_cap = false;

    loop {
        tokio::select! {
            _ = cancel_rx.changed() => {
                if *cancel_rx.borrow() {
                    cancelled = true;
                    break;
                }
            }
            read_res = stdout.read(&mut chunk) => {
                match read_res {
                    Ok(0) => break,
                    Ok(n) => {
                        let remaining = MAX_REMUX_BYTES.saturating_sub(out.len());
                        if remaining == 0 {
                            hit_cap = true;
                            break;
                        }
                        let take = n.min(remaining);
                        out.extend_from_slice(&chunk[..take]);
                        if take < n {
                            hit_cap = true;
                            break;
                        }
                    }
                    Err(e) => {
                        kill_and_reap(&child_slot).await;
                        abort_guard.disarm();
                        return Err(MediaError::RemuxFailed(format!("ffmpeg stdout read error: {e}")));
                    }
                }
            }
        }
    }

    if cancelled {
        kill_and_reap(&child_slot).await;
        abort_guard.disarm();
        return Err(MediaError::Cancelled);
    }

    if hit_cap {
        kill_and_reap(&child_slot).await;
        abort_guard.disarm();
        if out.is_empty() {
            return Err(MediaError::RemuxFailed("ffmpeg remux buffer cap hit with empty output".into()));
        }
        return Ok(Arc::new(out));
    }

    let status = {
        let mut slot = child_slot.lock().await;
        if let Some(mut ch) = slot.take() {
            Some(ch.wait().await)
        } else {
            None
        }
    };
    abort_guard.disarm();

    if let Some(Ok(exit)) = status {
        if !exit.success() {
            let mut err_buf = String::new();
            if let Some(mut err_stream) = stderr.take() {
                let _ = err_stream.read_to_string(&mut err_buf).await;
            }
            return Err(MediaError::RemuxFailed(format!(
                "ffmpeg exited with {exit}: {}",
                err_buf.trim()
            )));
        }
    }

    if out.is_empty() {
        return Err(MediaError::RemuxFailed(
            "ffmpeg produced empty fMP4 stream".into(),
        ));
    }

    Ok(Arc::new(out))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn kill_and_reap_terminates_and_reaps_running_child() {
        let child = Command::new("sleep")
            .arg("60")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let pid = child.id().expect("child pid");
        let slot = Arc::new(Mutex::new(Some(child)));

        kill_and_reap(&slot).await;
        assert!(slot.lock().await.is_none());

        #[cfg(unix)]
        {
            // Verify process is actually reaped (kill(pid, 0) fails with ESRCH)
            let rc = unsafe { libc::kill(pid as libc::pid_t, 0) };
            assert_eq!(rc, -1);
        }
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn aborting_serve_remux_request_kills_and_reaps_ffmpeg_without_stop_active_stream() {
        use std::ffi::CString;
        use std::os::unix::ffi::OsStrExt;
        use std::time::Duration;

        let dir = tempfile::TempDir::new().unwrap();
        let fifo_path = dir.path().join("blocked_input.mkv");
        let c_path = CString::new(fifo_path.as_os_str().as_bytes()).unwrap();
        let mkfifo_rc = unsafe { libc::mkfifo(c_path.as_ptr(), 0o600) };
        assert_eq!(mkfifo_rc, 0, "mkfifo failed");

        // Hold O_RDWR on the FIFO so ffmpeg opens it without blocking and then blocks on read()
        let _fifo_anchor = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(&fifo_path)
            .unwrap();

        let active_slot = Arc::new(Mutex::new(None));
        let gen_counter = Arc::new(AtomicU64::new(0));

        let slot_req = Arc::clone(&active_slot);
        let gen_req = Arc::clone(&gen_counter);
        let path_req = fifo_path.clone();
        let req_handle = tokio::spawn(async move {
            serve_remux_request(
                &slot_req,
                &gen_req,
                &path_req,
                0,
                &MediaClassification::TranscodeVideo,
                &Method::GET,
                None,
            )
            .await
        });

        let ffmpeg_pid: u32 = tokio::time::timeout(Duration::from_secs(5), async {
            loop {
                let maybe_sess = active_slot.lock().await.clone();
                if let Some(sess) = maybe_sess {
                    if let Some(pid) = sess.child_slot.lock().await.as_ref().and_then(|c| c.id()) {
                        return pid;
                    }
                }
                tokio::task::yield_now().await;
            }
        })
        .await
        .expect("ffmpeg child pid should appear in child_slot");

        // Abort the in-flight request WITHOUT calling stop_active_stream()
        req_handle.abort();
        let _ = req_handle.await;

        // Wait deterministically for the spawned reaper task to waitpid() the killed ffmpeg child
        tokio::time::timeout(Duration::from_secs(5), async {
            loop {
                let rc = unsafe { libc::kill(ffmpeg_pid as libc::pid_t, 0) };
                if rc == -1 {
                    let errno = std::io::Error::last_os_error().raw_os_error();
                    if errno == Some(libc::ESRCH) {
                        break;
                    }
                }
                tokio::task::yield_now().await;
            }
        })
        .await
        .expect("aborted serve_remux_request must kill and reap ffmpeg child");

        let sess = active_slot.lock().await.clone().unwrap();
        assert!(*sess.cancel_tx.borrow());
        assert!(sess.child_slot.lock().await.is_none());
    }

    #[tokio::test]
    async fn failed_remux_does_not_poison_subsequent_requests_for_same_key() {
        let Ok(ffmpeg) = super::super::resolve_ffmpeg_bin() else {
            return;
        };
        let dir = tempfile::TempDir::new().unwrap();
        let clip_path = dir.path().join("clip.mkv");
        let active_slot = Arc::new(Mutex::new(None));
        let gen_counter = Arc::new(AtomicU64::new(0));
        let class = MediaClassification::RemuxCopy {
            video_codec: "h264".into(),
        };

        // Call 1 on missing file fails with RemuxFailed
        let r1 = serve_remux_request(
            &active_slot,
            &gen_counter,
            &clip_path,
            0,
            &class,
            &Method::GET,
            None,
        )
        .await;
        assert!(matches!(r1, Err(MediaError::RemuxFailed(_))));
        assert!(active_slot.lock().await.is_none());

        // Generate valid 0.5s synthetic MKV at the same path
        let status = std::process::Command::new(&ffmpeg)
            .args([
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "lavfi",
                "-i",
                "color=c=black:s=64x64:d=0.5",
                "-c:v",
                "libx264",
                "-y",
            ])
            .arg(&clip_path)
            .status()
            .unwrap();
        assert!(status.success());

        // Call 2 with identical (canonical_path, start_ms=0) must recover cleanly
        let r2 = serve_remux_request(
            &active_slot,
            &gen_counter,
            &clip_path,
            0,
            &class,
            &Method::GET,
            None,
        )
        .await
        .expect("subsequent request after prior failure must recover");
        assert!(matches!(
            r2.status(),
            http::StatusCode::OK | http::StatusCode::PARTIAL_CONTENT
        ));
        assert!(!r2.body().is_empty());
        assert_eq!(gen_counter.load(Ordering::Relaxed), 2);

        // Request at t >= duration returns either RemuxFailed or bounded init-only response and does not poison start_ms=0
        let r3 = serve_remux_request(
            &active_slot,
            &gen_counter,
            &clip_path,
            60_000,
            &class,
            &Method::GET,
            None,
        )
        .await;
        if let Ok(ref resp) = r3 {
            assert!(matches!(
                resp.status(),
                http::StatusCode::OK | http::StatusCode::PARTIAL_CONTENT
            ));
        }

        // Recovery at start_ms=0 still succeeds
        let r4 = serve_remux_request(
            &active_slot,
            &gen_counter,
            &clip_path,
            0,
            &class,
            &Method::GET,
            None,
        )
        .await
        .expect("start_ms=0 after t>=duration request must still succeed");
        assert!(!r4.body().is_empty());
    }
}
