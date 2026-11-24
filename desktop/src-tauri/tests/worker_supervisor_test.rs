use std::path::PathBuf;
use std::process::Command;
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use sublift_desktop_lib::dto::{
    ExtractionConfigDto, ExtractionMode, OcrEngineId, TaskEvent,
};
use sublift_desktop_lib::worker::protocol::RunExtractionParams;
use sublift_desktop_lib::worker::{
    WorkerBackend, WorkerConfig, WorkerError, WorkerEventSink, WorkerSupervisor,
};

struct MockBinArtifact {
    _dir: tempfile::TempDir,
    bin_path: PathBuf,
}

static MOCK_WORKER_BIN: OnceLock<MockBinArtifact> = OnceLock::new();

fn mock_worker_source() -> &'static str {
    r#"
use std::collections::HashMap;
use std::io::{self, BufRead, Write};
use std::sync::{Arc, Condvar, Mutex};
use std::thread;

fn json_u64(line: &str, key: &str) -> Option<u64> {
    let needle = format!("\"{}\":", key);
    let idx = line.find(&needle)? + needle.len();
    let rest = line[idx..].trim_start();
    let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
    digits.parse().ok()
}

fn json_str(line: &str, key: &str) -> Option<String> {
    let needle = format!("\"{}\":\"", key);
    let idx = line.find(&needle)? + needle.len();
    let end = line[idx..].find('"')?;
    Some(line[idx..idx + end].to_string())
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let stdout = Arc::new(Mutex::new(io::stdout()));
    let cancels: Arc<(Mutex<HashMap<u64, bool>>, Condvar)> =
        Arc::new((Mutex::new(HashMap::new()), Condvar::new()));

    let crash_flag_path = args.windows(2).find(|w| w[0] == "--crash-once").map(|w| w[1].clone());
    let eof_marker = args.windows(2).find(|w| w[0] == "--eof-marker").map(|w| w[1].clone());

    let stdin = io::stdin();
    for line_res in stdin.lock().lines() {
        let line = match line_res {
            Ok(l) => l,
            Err(_) => break,
        };
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        let id = match json_u64(trimmed, "id") {
            Some(v) => v,
            None => continue,
        };
        let op = json_str(trimmed, "op").unwrap_or_default();

        match op.as_str() {
            "capabilities" => {
                eprintln!("mock_worker capabilities pid={}", std::process::id());
                let mut out = stdout.lock().unwrap();
                writeln!(
                    out,
                    "{{\"id\":{},\"type\":\"result\",\"ok\":true,\"data\":{{\"engines\":[\"paddle\"],\"ffmpeg_available\":true,\"hwaccel_modes\":[]}}}}",
                    id
                ).unwrap();
                out.flush().unwrap();
            }
            "probe_video" => {
                if let Some(video_path) = json_str(trimmed, "video_path") {
                    if video_path == "CRASH_ONCE" {
                        if let Some(ref flag_path) = crash_flag_path {
                            if !std::path::Path::new(flag_path).exists() {
                                let _ = std::fs::write(flag_path, b"crashed");
                                eprintln!("simulated crash pid={}", std::process::id());
                                std::process::exit(42);
                            }
                        }
                    }
                }
                let mut out = stdout.lock().unwrap();
                writeln!(
                    out,
                    "{{\"id\":{},\"type\":\"result\",\"ok\":true,\"data\":{{\"width\":1280,\"height\":720,\"duration_ms\":5000,\"fps\":25.0,\"display_transform_ok\":true,\"transform_note\":null}}}}",
                    id
                ).unwrap();
                out.flush().unwrap();
            }
            "run_extraction" => {
                if let Some(video_path) = json_str(trimmed, "video_path") {
                    if video_path == "CRASH_ALWAYS" {
                        eprintln!("fatal decoder crash");
                        std::process::exit(42);
                    }
                }
                {
                    let mut map = cancels.0.lock().unwrap();
                    map.insert(id, false);
                }
                let out_arc = Arc::clone(&stdout);
                let cancels_arc = Arc::clone(&cancels);
                thread::spawn(move || {
                    {
                        let mut out = out_arc.lock().unwrap();
                        writeln!(
                            out,
                            "{{\"id\":{},\"type\":\"event\",\"event\":\"progress\",\"data\":{{\"current_ms\":500,\"total_ms\":1000,\"fps\":120.0}}}}",
                            id
                        ).unwrap();
                        out.flush().unwrap();
                    }
                    // Block deterministically on Condvar until cancel_task arrives
                    let mut guard = cancels_arc.0.lock().unwrap();
                    while !guard.get(&id).copied().unwrap_or(false) {
                        guard = cancels_arc.1.wait(guard).unwrap();
                    }
                    let mut out = out_arc.lock().unwrap();
                    writeln!(
                        out,
                        "{{\"id\":{},\"type\":\"result\",\"ok\":false,\"error\":{{\"code\":\"Aborted\",\"message\":\"cancelled by host\"}}}}",
                        id
                    ).unwrap();
                    out.flush().unwrap();
                });
            }
            "cancel_task" => {
                let target_id = json_u64(trimmed, "target_id").unwrap_or(0);
                {
                    let mut guard = cancels.0.lock().unwrap();
                    guard.insert(target_id, true);
                    cancels.1.notify_all();
                }
                let mut out = stdout.lock().unwrap();
                writeln!(out, "{{\"id\":{},\"type\":\"result\",\"ok\":true,\"data\":null}}", id).unwrap();
                out.flush().unwrap();
            }
            _ => {
                let mut out = stdout.lock().unwrap();
                writeln!(
                    out,
                    "{{\"id\":{},\"type\":\"result\",\"ok\":false,\"error\":{{\"code\":\"UnknownOp\",\"message\":\"unsupported op\"}}}}",
                    id
                ).unwrap();
                out.flush().unwrap();
            }
        }
    }

    if let Some(marker) = eof_marker {
        let _ = std::fs::write(marker, b"clean_eof_exit");
    }
}
"#
}

fn ensure_mock_worker_bin() -> &'static PathBuf {
    let artifact = MOCK_WORKER_BIN.get_or_init(|| {
        let dir = tempfile::TempDir::new().expect("create tempdir for mock worker");
        let src_path = dir.path().join("mock_worker.rs");
        let bin_name = format!("sublift_worker{}", std::env::consts::EXE_SUFFIX);
        let bin_path = dir.path().join(bin_name);
        std::fs::write(&src_path, mock_worker_source()).expect("write mock_worker.rs");

        let rustc = option_env!("RUSTC").unwrap_or("rustc");
        let output = Command::new(rustc)
            .arg("--edition")
            .arg("2021")
            .arg("-O")
            .arg(&src_path)
            .arg("-o")
            .arg(&bin_path)
            .output()
            .expect("invoke rustc for mock_worker");
        assert!(
            output.status.success(),
            "failed to compile mock_worker: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        MockBinArtifact {
            _dir: dir,
            bin_path,
        }
    });
    &artifact.bin_path
}

#[tokio::test]
async fn real_subprocess_multiplexing_cancel_crash_respawn_and_graceful_shutdown() {
    tokio::time::timeout(Duration::from_secs(20), async {
        let bin = ensure_mock_worker_bin().clone();
        let wrapper_dir = tempfile::TempDir::new().unwrap();
        let eof_marker = wrapper_dir.path().join("eof_clean.marker");
        let launch_bin = {
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                let wrapper = wrapper_dir.path().join("sublift_worker_wrapper.sh");
                let script = format!(
                    "#!/bin/sh\nexec \"{}\" --eof-marker \"{}\"\n",
                    bin.display(),
                    eof_marker.display()
                );
                std::fs::write(&wrapper, script).unwrap();
                std::fs::set_permissions(&wrapper, std::fs::Permissions::from_mode(0o755))
                    .unwrap();
                wrapper
            }
            #[cfg(not(unix))]
            {
                bin.clone()
            }
        };

        let supervisor = WorkerSupervisor::new(WorkerConfig {
            override_bin: Some(launch_bin),
            ..Default::default()
        });

        // 1. Unary call spawns resident worker
        let caps = supervisor.capabilities().await.unwrap();
        assert_eq!(caps.engines, vec![OcrEngineId::Paddle]);
        let pid1 = supervisor.current_pid().await.unwrap();

        // 2. Streaming task blocks deterministically until cancel_task arrives;
        //    concurrent probe_video interleaves on the same resident worker!
        let (progress_tx, progress_rx) = tokio::sync::oneshot::channel::<()>();
        let progress_tx = Arc::new(Mutex::new(Some(progress_tx)));
        let events = Arc::new(Mutex::new(Vec::<TaskEvent>::new()));
        let events_clone = Arc::clone(&events);
        let sink: WorkerEventSink = Arc::new(move |ev| {
            if matches!(ev, TaskEvent::Progress { .. }) {
                if let Some(tx) = progress_tx.lock().unwrap().take() {
                    let _ = tx.send(());
                }
            }
            events_clone.lock().unwrap().push(ev);
        });

        let params = RunExtractionParams {
            video_path: "/tmp/normal.mp4".into(),
            mode: ExtractionMode::LocalOcr,
            engine: Some(OcrEngineId::Paddle),
            region_box: None,
            config: ExtractionConfigDto::default(),
            strips_dir: None,
        };

        let sup_ref = &supervisor;
        let extract_fut = sup_ref.run_extraction("task-cancel-1", params, sink);
        let control_fut = async {
            progress_rx.await.unwrap();
            let probe = sup_ref.probe_video("/tmp/normal.mp4").await.unwrap();
            assert_eq!(probe.width, 1280);
            sup_ref.cancel_task("task-cancel-1").await.unwrap();
        };

        let (extract_res, ()) = tokio::join!(extract_fut, control_fut);
        assert!(matches!(extract_res, Err(WorkerError::Aborted(_))));
        assert_eq!(supervisor.current_pid().await.unwrap(), pid1);
        assert!(events
            .lock()
            .unwrap()
            .iter()
            .any(|e| matches!(e, TaskEvent::Progress { .. })));

        // 3. Simulated mid-request crash -> retryable error + stderr capture -> auto-respawn fresh PID!
        let crash_params = RunExtractionParams {
            video_path: "CRASH_ALWAYS".into(),
            mode: ExtractionMode::LocalOcr,
            engine: Some(OcrEngineId::Paddle),
            region_box: None,
            config: ExtractionConfigDto::default(),
            strips_dir: None,
        };
        let noop_sink: WorkerEventSink = Arc::new(|_| {});
        let crash_err = supervisor
            .run_extraction("task-crash-1", crash_params, noop_sink)
            .await
            .unwrap_err();
        assert!(crash_err.is_retryable());
        match crash_err {
            WorkerError::Crashed {
                status: Some(42),
                stderr_tail,
                retryable: true,
            } => {
                assert!(
                    stderr_tail.iter().any(|l| l.contains("fatal decoder crash")),
                    "expected stderr_tail to capture crash log, got: {stderr_tail:?}"
                );
            }
            other => panic!("unexpected crash error: {other:?}"),
        }

        // Auto-respawn on next call
        let caps2 = supervisor.capabilities().await.unwrap();
        assert!(caps2.ffmpeg_available);
        let pid2 = supervisor.current_pid().await.unwrap();
        assert_ne!(pid2, pid1);

        // 4. Clean shutdown closes stdin -> graceful exit (eof_marker written before exit)
        supervisor.shutdown().await;
        assert!(supervisor.current_pid().await.is_none());
        #[cfg(unix)]
        {
            let marker_text = std::fs::read_to_string(&eof_marker)
                .expect("expected mock_worker to write clean EOF exit marker");
            assert_eq!(marker_text, "clean_eof_exit");
        }
    })
    .await
    .expect("worker_supervisor_test timed out after 20s");
}

#[tokio::test]
async fn shutdown_prevents_concurrent_rpc_respawn_and_leaves_no_live_pid() {
    tokio::time::timeout(Duration::from_secs(10), async {
        let bin = ensure_mock_worker_bin().clone();
        let supervisor = WorkerSupervisor::new(WorkerConfig {
            override_bin: Some(bin),
            ..Default::default()
        });

        let caps = supervisor.capabilities().await.unwrap();
        assert_eq!(caps.engines, vec![OcrEngineId::Paddle]);
        assert!(supervisor.current_pid().await.is_some());

        let ((), concurrent_cap_res) = tokio::join!(supervisor.shutdown(), supervisor.capabilities());
        assert!(
            supervisor.current_pid().await.is_none(),
            "shutdown() must leave current_pid() == None even under concurrent capabilities() (res={concurrent_cap_res:?})"
        );
        let post_shutdown_err = supervisor.capabilities().await.unwrap_err();
        assert!(matches!(post_shutdown_err, WorkerError::Aborted(_)));
        assert!(supervisor.current_pid().await.is_none());
    })
    .await
    .expect("shutdown race regression test timed out");
}

#[tokio::test]
async fn drop_without_shutdown_closes_stdin_and_cleans_up_child() {
    tokio::time::timeout(Duration::from_secs(10), async {
        let bin = ensure_mock_worker_bin().clone();
        let wrapper_dir = tempfile::TempDir::new().unwrap();
        let eof_marker = wrapper_dir.path().join("drop_eof.marker");
        #[cfg(unix)]
        let launch_bin = {
            use std::os::unix::fs::PermissionsExt;
            let wrapper = wrapper_dir.path().join("drop_wrapper.sh");
            let script = format!(
                "#!/bin/sh\nexec \"{}\" --eof-marker \"{}\"\n",
                bin.display(),
                eof_marker.display()
            );
            std::fs::write(&wrapper, script).unwrap();
            std::fs::set_permissions(&wrapper, std::fs::Permissions::from_mode(0o755)).unwrap();
            wrapper
        };
        #[cfg(not(unix))]
        let launch_bin = bin;

        let supervisor = WorkerSupervisor::new(WorkerConfig {
            override_bin: Some(launch_bin),
            ..Default::default()
        });
        let caps = supervisor.capabilities().await.unwrap();
        assert_eq!(caps.engines, vec![OcrEngineId::Paddle]);
        let pid = supervisor.current_pid().await.unwrap();

        // Dropping supervisor without calling shutdown() must still close stdin/reap child
        drop(supervisor);

        #[cfg(unix)]
        {
            loop {
                let rc = unsafe { libc::kill(pid as libc::pid_t, 0) };
                if rc == -1 && std::io::Error::last_os_error().raw_os_error() == Some(libc::ESRCH) {
                    break;
                }
                tokio::task::yield_now().await;
            }
        }
    })
    .await
    .expect("drop cleanup timed out");
}
