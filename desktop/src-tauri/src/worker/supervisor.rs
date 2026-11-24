use std::collections::{HashMap, VecDeque};
use std::process::Stdio;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex as StdMutex};
use std::time::Duration;

use tokio::io::{AsyncBufReadExt, AsyncRead, AsyncWrite, AsyncWriteExt, BufReader};
use tokio::process::{Child, Command};
use tokio::sync::{mpsc, oneshot, watch, Mutex as AsyncMutex};

use crate::dto::{ExtractionSubPhase, TaskEvent};

use super::discovery::resolve_worker_binary;
use super::protocol::{CancelTaskParams, WorkerInbound, WorkerRequest, WorkerStreamEvent};
use super::{WorkerConfig, WorkerError, WorkerEventSink};

const STDERR_RING_CAP: usize = 32;
const SHUTDOWN_GRACE_TIMEOUT: Duration = Duration::from_secs(2);

pub(crate) struct PendingCall {
    pub result_tx: oneshot::Sender<Result<serde_json::Value, WorkerError>>,
    pub event_tx: Option<mpsc::UnboundedSender<WorkerStreamEvent>>,
}

pub(crate) type PendingMap = Arc<StdMutex<HashMap<u64, PendingCall>>>;

struct ResidentWorker {
    stdin_tx: Option<mpsc::Sender<String>>,
    pending: PendingMap,
    alive: Arc<AtomicBool>,
    shutdown_tx: Option<oneshot::Sender<()>>,
    io_handle: tokio::task::JoinHandle<()>,
    pid: Option<u32>,
}

enum ActiveTaskState {
    Starting {
        req_id: u64,
        cancelled: bool,
        cancel_tx: watch::Sender<bool>,
    },
    Dispatched {
        req_id: u64,
    },
}

impl ActiveTaskState {
    fn req_id(&self) -> u64 {
        match self {
            Self::Starting { req_id, .. } | Self::Dispatched { req_id } => *req_id,
        }
    }
}

struct ActiveTaskGuard {
    active_tasks: Arc<StdMutex<HashMap<String, ActiveTaskState>>>,
    task_id: String,
    req_id: u64,
}

impl Drop for ActiveTaskGuard {
    fn drop(&mut self) {
        if let Ok(mut map) = self.active_tasks.lock() {
            if map.get(&self.task_id).map(ActiveTaskState::req_id) == Some(self.req_id) {
                map.remove(&self.task_id);
            }
        }
    }
}

pub struct WorkerSupervisor {
    config: WorkerConfig,
    next_id: AtomicU64,
    shut_down: AtomicBool,
    runtime: AsyncMutex<Option<ResidentWorker>>,
    active_tasks: Arc<StdMutex<HashMap<String, ActiveTaskState>>>,
    #[cfg(test)]
    test_pre_dispatch_hook: StdMutex<Option<(Arc<tokio::sync::Notify>, Arc<tokio::sync::Notify>)>>,
}

impl WorkerSupervisor {
    pub fn new(config: WorkerConfig) -> Self {
        Self {
            config,
            next_id: AtomicU64::new(1),
            shut_down: AtomicBool::new(false),
            runtime: AsyncMutex::new(None),
            active_tasks: Arc::new(StdMutex::new(HashMap::new())),
            #[cfg(test)]
            test_pre_dispatch_hook: StdMutex::new(None),
        }
    }

    pub async fn current_pid(&self) -> Option<u32> {
        let guard = self.runtime.lock().await;
        guard
            .as_ref()
            .filter(|w| w.alive.load(Ordering::Acquire))
            .and_then(|w| w.pid)
    }

    async fn acquire_handles(
        &self,
    ) -> Result<(mpsc::Sender<String>, PendingMap, Arc<AtomicBool>), WorkerError> {
        if self.shut_down.load(Ordering::Acquire) {
            return Err(WorkerError::Aborted(
                "worker supervisor is shutting down".into(),
            ));
        }

        let mut guard = self.runtime.lock().await;
        if self.shut_down.load(Ordering::Acquire) {
            return Err(WorkerError::Aborted(
                "worker supervisor is shutting down".into(),
            ));
        }

        let need_spawn = match guard.as_ref() {
            Some(worker) => !worker.alive.load(Ordering::Acquire),
            None => true,
        };

        if need_spawn {
            if let Some(old) = guard.take() {
                drop(old.stdin_tx);
                let _ = tokio::time::timeout(Duration::from_millis(200), old.io_handle).await;
            }
            let spawned = Self::spawn_resident(&self.config)?;
            *guard = Some(spawned);
        }

        let worker = guard.as_ref().expect("resident worker initialized");
        let stdin_tx = worker
            .stdin_tx
            .as_ref()
            .cloned()
            .ok_or_else(|| WorkerError::Io("worker stdin closed".into()))?;
        Ok((stdin_tx, Arc::clone(&worker.pending), Arc::clone(&worker.alive)))
    }

    fn spawn_resident(config: &WorkerConfig) -> Result<ResidentWorker, WorkerError> {
        let bin_path = resolve_worker_binary(config)?;
        let mut cmd = Command::new(&bin_path);
        cmd.stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .kill_on_drop(true);

        let mut child = cmd
            .spawn()
            .map_err(|e| WorkerError::SpawnFailed(format!("{}: {e}", bin_path.display())))?;

        let pid = child.id();
        let stdin = child
            .stdin
            .take()
            .ok_or_else(|| WorkerError::SpawnFailed("failed to capture child stdin".into()))?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| WorkerError::SpawnFailed("failed to capture child stdout".into()))?;
        let stderr = child.stderr.take();

        let (stdin_tx, stdin_rx) = mpsc::channel::<String>(256);
        let (shutdown_tx, shutdown_rx) = oneshot::channel::<()>();
        let pending: PendingMap = Arc::new(StdMutex::new(HashMap::new()));
        let alive = Arc::new(AtomicBool::new(true));

        let io_handle = spawn_process_io_loop(
            stdout,
            stdin,
            stderr,
            Some(child),
            stdin_rx,
            shutdown_rx,
            Arc::clone(&pending),
            Arc::clone(&alive),
        );

        Ok(ResidentWorker {
            stdin_tx: Some(stdin_tx),
            pending,
            alive,
            shutdown_tx: Some(shutdown_tx),
            io_handle,
            pid,
        })
    }

    pub async fn call_unary<P: serde::Serialize>(
        &self,
        op: &str,
        params: P,
    ) -> Result<serde_json::Value, WorkerError> {
        let req_id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let payload = serde_json::to_string(&WorkerRequest {
            id: req_id,
            op,
            params,
        })
        .map_err(|e| WorkerError::Serialization(e.to_string()))?;

        let (stdin_tx, pending, alive) = self.acquire_handles().await?;
        let (result_tx, result_rx) = oneshot::channel();

        {
            let mut map = pending.lock().expect("pending lock poisoned");
            if !alive.load(Ordering::Acquire) {
                return Err(WorkerError::Crashed {
                    status: None,
                    stderr_tail: Vec::new(),
                    retryable: true,
                });
            }
            map.insert(
                req_id,
                PendingCall {
                    result_tx,
                    event_tx: None,
                },
            );
        }

        if stdin_tx.send(payload).await.is_err() {
            if let Ok(mut map) = pending.lock() {
                map.remove(&req_id);
            }
            return Err(WorkerError::Crashed {
                status: None,
                stderr_tail: Vec::new(),
                retryable: true,
            });
        }

        result_rx.await.unwrap_or_else(|_| Err(WorkerError::Crashed {
            status: None,
            stderr_tail: Vec::new(),
            retryable: true,
        }))
    }

    pub async fn call_streaming<P: serde::Serialize>(
        &self,
        task_id: &str,
        op: &str,
        params: P,
        on_event: WorkerEventSink,
    ) -> Result<serde_json::Value, WorkerError> {
        self.call_streaming_with_cancel(task_id, op, params, on_event, None)
            .await
    }

    pub async fn call_streaming_with_cancel<P: serde::Serialize>(
        &self,
        task_id: &str,
        op: &str,
        params: P,
        on_event: WorkerEventSink,
        mut external_cancel_rx: Option<watch::Receiver<bool>>,
    ) -> Result<serde_json::Value, WorkerError> {
        if external_cancel_rx.as_ref().is_some_and(|rx| *rx.borrow()) {
            return Err(WorkerError::Aborted(
                "task cancelled before worker start".into(),
            ));
        }

        let req_id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let (cancel_tx, mut cancel_rx) = watch::channel(false);
        {
            let mut active = self.active_tasks.lock().expect("active_tasks lock poisoned");
            if active.contains_key(task_id) {
                return Err(WorkerError::Remote {
                    code: "TaskConflict".to_string(),
                    message: format!("task '{task_id}' is already active in worker supervisor"),
                });
            }
            active.insert(
                task_id.to_string(),
                ActiveTaskState::Starting {
                    req_id,
                    cancelled: false,
                    cancel_tx,
                },
            );
        }
        let _guard = ActiveTaskGuard {
            active_tasks: Arc::clone(&self.active_tasks),
            task_id: task_id.to_string(),
            req_id,
        };

        if external_cancel_rx.as_ref().is_some_and(|rx| *rx.borrow()) {
            return Err(WorkerError::Aborted(
                "task cancelled before worker start".into(),
            ));
        }

        let payload = serde_json::to_string(&WorkerRequest {
            id: req_id,
            op,
            params,
        })
        .map_err(|e| WorkerError::Serialization(e.to_string()))?;

        let (stdin_tx, pending, alive) = tokio::select! {
            biased;
            _ = cancel_rx.wait_for(|&v| v) => {
                return Err(WorkerError::Aborted(
                    "task cancelled before worker initialized".into(),
                ));
            }
            _ = wait_external_cancel(&mut external_cancel_rx) => {
                return Err(WorkerError::Aborted(
                    "task cancelled before worker initialized".into(),
                ));
            }
            res = self.acquire_handles() => res?,
        };

        #[cfg(test)]
        {
            let hook = self
                .test_pre_dispatch_hook
                .lock()
                .expect("test_pre_dispatch_hook lock poisoned")
                .clone();
            if let Some((entered, release)) = hook {
                entered.notify_one();
                release.notified().await;
            }
        }

        let permit = tokio::select! {
            biased;
            _ = cancel_rx.wait_for(|&v| v) => {
                return Err(WorkerError::Aborted(
                    "task cancelled before dispatch".into(),
                ));
            }
            _ = wait_external_cancel(&mut external_cancel_rx) => {
                return Err(WorkerError::Aborted(
                    "task cancelled before dispatch".into(),
                ));
            }
            res = stdin_tx.reserve() => {
                res.map_err(|_| WorkerError::Crashed {
                    status: None,
                    stderr_tail: Vec::new(),
                    retryable: true,
                })?
            }
        };

        let (result_tx, mut result_rx) = oneshot::channel();
        let (event_tx, mut event_rx) = mpsc::unbounded_channel();

        {
            let mut active = self.active_tasks.lock().expect("active_tasks lock poisoned");
            let internally_cancelled = matches!(
                active.get(task_id),
                Some(ActiveTaskState::Starting {
                    req_id: cur_id,
                    cancelled: true,
                    ..
                }) if *cur_id == req_id
            );
            let externally_cancelled = external_cancel_rx.as_ref().is_some_and(|rx| *rx.borrow());
            if internally_cancelled || externally_cancelled {
                return Err(WorkerError::Aborted(
                    "task cancelled before dispatch".into(),
                ));
            }

            {
                let mut map = pending.lock().expect("pending lock poisoned");
                if !alive.load(Ordering::Acquire) {
                    return Err(WorkerError::Crashed {
                        status: None,
                        stderr_tail: Vec::new(),
                        retryable: true,
                    });
                }
                map.insert(
                    req_id,
                    PendingCall {
                        result_tx,
                        event_tx: Some(event_tx),
                    },
                );
            }

            active.insert(task_id.to_string(), ActiveTaskState::Dispatched { req_id });
            permit.send(payload);
        }

        let mut phase_emitted = false;
        let mut event_open = true;
        let final_res = loop {
            tokio::select! {
                biased;
                maybe_ev = event_rx.recv(), if event_open => {
                    match maybe_ev {
                        Some(ev) => {
                            forward_worker_event(task_id, ev, &mut phase_emitted, &on_event);
                        }
                        None => {
                            event_open = false;
                        }
                    }
                }
                res = &mut result_rx => {
                    while let Ok(ev) = event_rx.try_recv() {
                        forward_worker_event(task_id, ev, &mut phase_emitted, &on_event);
                    }
                    break res.unwrap_or_else(|_| Err(WorkerError::Crashed {
                        status: None,
                        stderr_tail: Vec::new(),
                        retryable: true,
                    }));
                }
            }
        };

        final_res
    }

    pub async fn cancel_task_by_id(&self, task_id: &str) -> Result<(), WorkerError> {
        let target_id = {
            let mut active = self.active_tasks.lock().expect("active_tasks lock poisoned");
            match active.get_mut(task_id) {
                Some(ActiveTaskState::Starting {
                    cancelled,
                    cancel_tx,
                    ..
                }) => {
                    *cancelled = true;
                    let _ = cancel_tx.send(true);
                    return Ok(());
                }
                Some(ActiveTaskState::Dispatched { req_id }) => Some(*req_id),
                None => None,
            }
        };
        let Some(target_id) = target_id else {
            return Ok(());
        };
        let _ = self
            .call_unary("cancel_task", CancelTaskParams { target_id })
            .await?;
        Ok(())
    }

    pub async fn shutdown(&self) {
        self.shut_down.store(true, Ordering::Release);
        let mut guard = self.runtime.lock().await;
        let Some(mut worker) = guard.take() else {
            return;
        };

        if let Some(tx) = worker.shutdown_tx.take() {
            let _ = tx.send(());
        }
        drop(worker.stdin_tx.take());
        let _ = worker.io_handle.await;
    }
}

async fn wait_external_cancel(rx: &mut Option<watch::Receiver<bool>>) {
    match rx {
        Some(r) => {
            let _ = r.wait_for(|&v| v).await;
        }
        None => std::future::pending::<()>().await,
    }
}

fn forward_worker_event(
    task_id: &str,
    ev: WorkerStreamEvent,
    phase_emitted: &mut bool,
    on_event: &WorkerEventSink,
) {
    match ev {
        WorkerStreamEvent::Progress {
            current_ms,
            total_ms,
            fps,
        } => {
            if !*phase_emitted {
                *phase_emitted = true;
                on_event(TaskEvent::Phase {
                    task_id: task_id.to_string(),
                    sub_phase: ExtractionSubPhase::Timing,
                });
            }
            on_event(TaskEvent::Progress {
                task_id: task_id.to_string(),
                current_ms,
                total_ms,
                fps,
            });
        }
        WorkerStreamEvent::SubtitleEntry(entry) => {
            on_event(TaskEvent::SubtitleEntry {
                task_id: task_id.to_string(),
                entry,
            });
        }
        WorkerStreamEvent::TimingSlot {
            index,
            start_ms,
            end_ms,
            strip_path,
        } => {
            on_event(TaskEvent::TimingSlot {
                task_id: task_id.to_string(),
                index,
                start_ms,
                end_ms,
                strip_path,
            });
        }
        WorkerStreamEvent::Unknown(name) => {
            eprintln!("[worker_supervisor] ignoring unknown worker event: {name}");
        }
    }
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn spawn_process_io_loop<R, W, S>(
    stdout: R,
    mut stdin: W,
    stderr: Option<S>,
    mut child: Option<Child>,
    mut stdin_rx: mpsc::Receiver<String>,
    mut shutdown_rx: oneshot::Receiver<()>,
    pending: PendingMap,
    alive: Arc<AtomicBool>,
) -> tokio::task::JoinHandle<()>
where
    R: AsyncRead + Unpin + Send + 'static,
    W: AsyncWrite + Unpin + Send + 'static,
    S: AsyncRead + Unpin + Send + 'static,
{
    let stderr_ring = Arc::new(StdMutex::new(VecDeque::<String>::with_capacity(
        STDERR_RING_CAP,
    )));

    let stderr_ring_clone = Arc::clone(&stderr_ring);
    let stderr_handle = stderr.map(|err_stream| {
        tokio::spawn(async move {
            let mut lines = BufReader::new(err_stream).lines();
            while let Ok(Some(line)) = lines.next_line().await {
                eprintln!("[sublift_worker] {line}");
                if let Ok(mut ring) = stderr_ring_clone.lock() {
                    if ring.len() >= STDERR_RING_CAP {
                        ring.pop_front();
                    }
                    ring.push_back(line);
                }
            }
        })
    });

    let writer_handle = tokio::spawn(async move {
        while let Some(line) = stdin_rx.recv().await {
            if stdin.write_all(line.as_bytes()).await.is_err() {
                break;
            }
            if stdin.write_all(b"\n").await.is_err() {
                break;
            }
            if stdin.flush().await.is_err() {
                break;
            }
        }
        let _ = stdin.shutdown().await;
    });

    tokio::spawn(async move {
        let mut lines = BufReader::new(stdout).lines();
        let mut shutting_down = false;

        loop {
            tokio::select! {
                _ = &mut shutdown_rx, if !shutting_down => {
                    shutting_down = true;
                    break;
                }
                line_res = lines.next_line() => {
                    match line_res {
                        Ok(Some(line)) => {
                            let trimmed = line.trim();
                            if trimmed.is_empty() {
                                continue;
                            }
                            match serde_json::from_str::<WorkerInbound>(trimmed) {
                                Ok(WorkerInbound::Event { id, event, data }) => {
                                    match WorkerStreamEvent::parse(&event, data) {
                                        Ok(parsed) => {
                                            let map = pending.lock().expect("pending lock poisoned");
                                            if let Some(call) = map.get(&id) {
                                                if let Some(tx) = &call.event_tx {
                                                    let _ = tx.send(parsed);
                                                }
                                            }
                                        }
                                        Err(e) => {
                                            eprintln!(
                                                "[worker_supervisor] malformed worker event '{event}' for id={id}: {e}"
                                            );
                                        }
                                    }
                                }
                                Ok(WorkerInbound::Result { id, ok, data, error }) => {
                                    let call = {
                                        let mut map = pending.lock().expect("pending lock poisoned");
                                        map.remove(&id)
                                    };
                                    if let Some(call) = call {
                                        let res = if ok {
                                            Ok(data.unwrap_or(serde_json::Value::Null))
                                        } else {
                                            let err = error.unwrap_or(super::protocol::WorkerRemoteError {
                                                code: "WorkerError".to_string(),
                                                message: "worker returned error without details".to_string(),
                                            });
                                            if err.code.eq_ignore_ascii_case("Aborted")
                                                || err.code.eq_ignore_ascii_case("Cancelled")
                                            {
                                                Err(WorkerError::Aborted(err.message))
                                            } else {
                                                Err(WorkerError::Remote {
                                                    code: err.code,
                                                    message: err.message,
                                                })
                                            }
                                        };
                                        let _ = call.result_tx.send(res);
                                    }
                                }
                                Err(e) => {
                                    eprintln!("[worker_supervisor] ignoring non-JSON stdout line: {e}");
                                }
                            }
                        }
                        Ok(None) | Err(_) => {
                            break;
                        }
                    }
                }
            }
        }

        alive.store(false, Ordering::Release);
        writer_handle.abort();
        let _ = writer_handle.await;

        let exit_code = if let Some(mut ch) = child.take() {
            if shutting_down {
                match tokio::time::timeout(SHUTDOWN_GRACE_TIMEOUT, ch.wait()).await {
                    Ok(Ok(status)) => status.code(),
                    _ => {
                        let _ = ch.start_kill();
                        ch.wait().await.ok().and_then(|s| s.code())
                    }
                }
            } else {
                ch.wait().await.ok().and_then(|s| s.code())
            }
        } else {
            None
        };

        if let Some(h) = stderr_handle {
            let _ = tokio::time::timeout(Duration::from_millis(200), h).await;
        }

        let stderr_tail: Vec<String> = stderr_ring
            .lock()
            .map(|r| r.iter().cloned().collect())
            .unwrap_or_default();

        let drained: Vec<PendingCall> = {
            let mut map = pending.lock().expect("pending lock poisoned");
            map.drain().map(|(_, v)| v).collect()
        };

        for call in drained {
            let err = if shutting_down {
                WorkerError::Aborted("worker shutting down".to_string())
            } else {
                WorkerError::Crashed {
                    status: exit_code,
                    stderr_tail: stderr_tail.clone(),
                    retryable: true,
                }
            };
            let _ = call.result_tx.send(Err(err));
        }
    })
}

#[cfg(test)]
impl WorkerSupervisor {
    pub(crate) fn new_duplex_for_test(
        unblock_rx: watch::Receiver<u64>,
    ) -> (Arc<Self>, tokio::task::JoinHandle<()>) {
        let (host_read, worker_write) = tokio::io::duplex(8192);
        let (mut worker_read, host_write) = tokio::io::duplex(8192);
        let (stdin_tx, stdin_rx) = mpsc::channel::<String>(64);
        let (shutdown_tx, shutdown_rx) = oneshot::channel::<()>();
        let pending: PendingMap = Arc::new(StdMutex::new(HashMap::new()));
        let alive = Arc::new(AtomicBool::new(true));

        let io_handle = spawn_process_io_loop(
            host_read,
            host_write,
            Option::<tokio::io::DuplexStream>::None,
            None,
            stdin_rx,
            shutdown_rx,
            Arc::clone(&pending),
            Arc::clone(&alive),
        );

        let worker_task = tokio::spawn(async move {
            let mut lines = BufReader::new(&mut worker_read).lines();
            let cancels = Arc::new(StdMutex::new(HashMap::<u64, oneshot::Sender<()>>::new()));
            let out_lock = Arc::new(AsyncMutex::new(()));
            let worker_write = Arc::new(AsyncMutex::new(worker_write));

            while let Ok(Some(line)) = lines.next_line().await {
                let req: serde_json::Value = match serde_json::from_str(&line) {
                    Ok(v) => v,
                    Err(_) => continue,
                };
                let id = req["id"].as_u64().unwrap_or(0);
                let op = req["op"].as_str().unwrap_or("");
                match op {
                    "run_extraction" => {
                        let iter_target = req["params"]["video_path"]
                            .as_str()
                            .and_then(|s| s.rsplit(':').next())
                            .and_then(|n| n.parse::<u64>().ok())
                            .unwrap_or(0);
                        let (c_tx, mut c_rx) = oneshot::channel::<()>();
                        cancels.lock().unwrap().insert(id, c_tx);
                        let mut rx = unblock_rx.clone();
                        let writer = Arc::clone(&worker_write);
                        let cancels_ref = Arc::clone(&cancels);
                        let lock_ref = Arc::clone(&out_lock);
                        tokio::spawn(async move {
                            let cancelled = tokio::select! {
                                biased;
                                _ = &mut c_rx => true,
                                _ = rx.wait_for(|&v| v >= iter_target) => false,
                            };
                            cancels_ref.lock().unwrap().remove(&id);
                            let resp = if cancelled {
                                format!(
                                    r#"{{"id":{id},"type":"result","ok":false,"error":{{"code":"Aborted","message":"cancelled"}}}}"#
                                )
                            } else {
                                format!(
                                    r#"{{"id":{id},"type":"result","ok":true,"data":{{"entries":[{{"id":1,"start_ms":100,"end_ms":900,"text":"uncancelled","confidence":0.99}}],"metrics":{{"frames_decoded":1,"frames_ocr":1,"elapsed_ms":1}}}}}}"#
                                )
                            };
                            let _g = lock_ref.lock().await;
                            let mut w = writer.lock().await;
                            let _ = w.write_all(resp.as_bytes()).await;
                            let _ = w.write_all(b"\n").await;
                            let _ = w.flush().await;
                        });
                    }
                    "cancel_task" => {
                        let target_id = req["params"]["target_id"].as_u64().unwrap_or(0);
                        if let Some(tx) = cancels.lock().unwrap().remove(&target_id) {
                            let _ = tx.send(());
                        }
                        let ack = format!(r#"{{"id":{id},"type":"result","ok":true,"data":null}}"#);
                        let _g = out_lock.lock().await;
                        let mut w = worker_write.lock().await;
                        let _ = w.write_all(ack.as_bytes()).await;
                        let _ = w.write_all(b"\n").await;
                        let _ = w.flush().await;
                    }
                    _ => {
                        let err = format!(
                            r#"{{"id":{id},"type":"result","ok":false,"error":{{"code":"UnknownOp","message":"unsupported"}}}}"#
                        );
                        let _g = out_lock.lock().await;
                        let mut w = worker_write.lock().await;
                        let _ = w.write_all(err.as_bytes()).await;
                        let _ = w.write_all(b"\n").await;
                        let _ = w.flush().await;
                    }
                }
            }
        });

        let sup = Arc::new(Self::new(WorkerConfig::default()));
        {
            // Synchronously attach ResidentWorker so callers start warm without spawning an OS child.
            let mut rt = sup.runtime.try_lock().expect("fresh runtime lock");
            *rt = Some(ResidentWorker {
                stdin_tx: Some(stdin_tx),
                pending,
                alive,
                shutdown_tx: Some(shutdown_tx),
                io_handle,
                pid: Some(4242),
            });
        }
        (sup, worker_task)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};

    #[tokio::test]
    async fn duplex_multiplexing_allows_fast_unary_to_finish_before_slow_stream() {
        let (host_read, mut worker_write) = tokio::io::duplex(8192);
        let (mut worker_read, host_write) = tokio::io::duplex(8192);
        let (stdin_tx, stdin_rx) = mpsc::channel::<String>(32);
        let (shutdown_tx, shutdown_rx) = oneshot::channel::<()>();
        let pending: PendingMap = Arc::new(StdMutex::new(HashMap::new()));
        let alive = Arc::new(AtomicBool::new(true));

        let io_handle = spawn_process_io_loop(
            host_read,
            host_write,
            Option::<tokio::io::DuplexStream>::None,
            None,
            stdin_rx,
            shutdown_rx,
            Arc::clone(&pending),
            Arc::clone(&alive),
        );

        let worker_task = tokio::spawn(async move {
            let mut lines = BufReader::new(&mut worker_read).lines();
            let req1: serde_json::Value =
                serde_json::from_str(&lines.next_line().await.unwrap().unwrap()).unwrap();
            let req2: serde_json::Value =
                serde_json::from_str(&lines.next_line().await.unwrap().unwrap()).unwrap();
            let id1 = req1["id"].as_u64().unwrap();
            let id2 = req2["id"].as_u64().unwrap();

            // Respond to req2 BEFORE req1 finishes
            let resp2 = format!(
                r#"{{"id":{id2},"type":"result","ok":true,"data":{{"pong":2}}}}"#
            );
            worker_write.write_all(resp2.as_bytes()).await.unwrap();
            worker_write.write_all(b"\n").await.unwrap();

            // Send streaming events + malformed line + final result for req1
            let ev1 = format!(
                r#"{{"id":{id1},"type":"event","event":"progress","data":{{"current_ms":1000,"total_ms":5000,"fps":60.0}}}}"#
            );
            let ev2 = format!(
                r#"{{"id":{id1},"type":"event","event":"subtitle_entry","data":{{"id":1,"start_ms":100,"end_ms":900,"text":"hello","confidence":0.95}}}}"#
            );
            let ev3 = format!(
                r#"{{"id":{id1},"type":"event","event":"timing_slot","data":{{"index":0,"start_ms":100,"end_ms":900,"strip_path":"/tmp/s0.ppm"}}}}"#
            );
            let resp1 = format!(
                r#"{{"id":{id1},"type":"result","ok":true,"data":{{"done":true}}}}"#
            );
            for line in [ev1, "NOT_JSON_GARBAGE".to_string(), ev2, ev3, resp1] {
                worker_write.write_all(line.as_bytes()).await.unwrap();
                worker_write.write_all(b"\n").await.unwrap();
            }
            worker_write.flush().await.unwrap();
        });

        let (tx1, rx1) = oneshot::channel();
        let (ev_tx1, mut ev_rx1) = mpsc::unbounded_channel();
        pending.lock().unwrap().insert(
            1,
            PendingCall {
                result_tx: tx1,
                event_tx: Some(ev_tx1),
            },
        );
        let (tx2, rx2) = oneshot::channel();
        pending.lock().unwrap().insert(
            2,
            PendingCall {
                result_tx: tx2,
                event_tx: None,
            },
        );

        stdin_tx
            .send(r#"{"id":1,"op":"run_extraction","params":{}}"#.to_string())
            .await
            .unwrap();
        stdin_tx
            .send(r#"{"id":2,"op":"capabilities","params":{}}"#.to_string())
            .await
            .unwrap();

        let res2 = rx2.await.unwrap().unwrap();
        assert_eq!(res2["pong"], 2);

        let res1 = rx1.await.unwrap().unwrap();
        assert_eq!(res1["done"], true);

        let mut events = Vec::new();
        while let Ok(ev) = ev_rx1.try_recv() {
            events.push(ev);
        }
        assert_eq!(events.len(), 3);
        assert!(matches!(events[0], WorkerStreamEvent::Progress { current_ms: 1000, .. }));
        assert!(matches!(events[1], WorkerStreamEvent::SubtitleEntry(ref e) if e.text == "hello"));
        assert!(matches!(events[2], WorkerStreamEvent::TimingSlot { index: 0, .. }));

        let _ = shutdown_tx.send(());
        drop(stdin_tx);
        worker_task.await.unwrap();
        io_handle.await.unwrap();
    }

    #[tokio::test]
    async fn duplex_eof_fails_all_pending_callers_with_retryable_true() {
        let (host_read, worker_write) = tokio::io::duplex(4096);
        let (_worker_read, host_write) = tokio::io::duplex(4096);
        let (_stdin_tx, stdin_rx) = mpsc::channel::<String>(8);
        let (_shutdown_tx, shutdown_rx) = oneshot::channel::<()>();
        let pending: PendingMap = Arc::new(StdMutex::new(HashMap::new()));
        let alive = Arc::new(AtomicBool::new(true));

        let io_handle = spawn_process_io_loop(
            host_read,
            host_write,
            Option::<tokio::io::DuplexStream>::None,
            None,
            stdin_rx,
            shutdown_rx,
            Arc::clone(&pending),
            Arc::clone(&alive),
        );

        let (tx1, rx1) = oneshot::channel();
        let (tx2, rx2) = oneshot::channel();
        {
            let mut map = pending.lock().unwrap();
            map.insert(
                10,
                PendingCall {
                    result_tx: tx1,
                    event_tx: None,
                },
            );
            map.insert(
                11,
                PendingCall {
                    result_tx: tx2,
                    event_tx: None,
                },
            );
        }

        // Simulate abrupt worker crash by dropping stdout write half
        drop(worker_write);

        let err1 = rx1.await.unwrap().unwrap_err();
        let err2 = rx2.await.unwrap().unwrap_err();
        assert!(err1.is_retryable());
        assert!(err2.is_retryable());
        assert!(!alive.load(Ordering::Acquire));

        io_handle.await.unwrap();
    }

    #[tokio::test]
    async fn cold_start_cancel_during_acquire_handles_aborts_without_spawning() {
        let sup = Arc::new(WorkerSupervisor::new(WorkerConfig {
            override_bin: Some(std::path::PathBuf::from("/nonexistent/sublift_worker_never_spawned")),
            ..Default::default()
        }));
        let rt_guard = sup.runtime.lock().await;

        let sup_call = Arc::clone(&sup);
        let sink: WorkerEventSink = Arc::new(|_| {});
        let call_handle = tokio::spawn(async move {
            sup_call
                .call_streaming("cold-cancel", "run_extraction", serde_json::json!({}), sink)
                .await
        });

        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                if sup.active_tasks.lock().unwrap().contains_key("cold-cancel") {
                    break;
                }
                tokio::task::yield_now().await;
            }
        })
        .await
        .expect("task should register Starting before acquire_handles");

        sup.cancel_task_by_id("cold-cancel").await.unwrap();
        let res = tokio::time::timeout(Duration::from_secs(2), call_handle)
            .await
            .expect("call_streaming should abort without waiting for runtime lock")
            .unwrap();
        assert!(matches!(res, Err(WorkerError::Aborted(_))));
        drop(rt_guard);
        assert!(sup.current_pid().await.is_none());
        assert!(!sup.active_tasks.lock().unwrap().contains_key("cold-cancel"));
    }

    #[tokio::test]
    async fn warm_worker_pre_dispatch_cancel_prevents_stdin_send_and_races() {
        let (host_read, mut worker_write) = tokio::io::duplex(8192);
        let (mut worker_read, host_write) = tokio::io::duplex(8192);
        let (stdin_tx, stdin_rx) = mpsc::channel::<String>(32);
        let (shutdown_tx, shutdown_rx) = oneshot::channel::<()>();
        let pending: PendingMap = Arc::new(StdMutex::new(HashMap::new()));
        let alive = Arc::new(AtomicBool::new(true));

        let io_handle = spawn_process_io_loop(
            host_read,
            host_write,
            Option::<tokio::io::DuplexStream>::None,
            None,
            stdin_rx,
            shutdown_rx,
            Arc::clone(&pending),
            Arc::clone(&alive),
        );

        let received_ops = Arc::new(StdMutex::new(Vec::<String>::new()));
        let received_ops_worker = Arc::clone(&received_ops);
        let worker_task = tokio::spawn(async move {
            let mut lines = BufReader::new(&mut worker_read).lines();
            let mut active_extractions = std::collections::HashSet::<u64>::new();
            while let Ok(Some(line)) = lines.next_line().await {
                let req: serde_json::Value = serde_json::from_str(&line).unwrap();
                let id = req["id"].as_u64().unwrap();
                let op = req["op"].as_str().unwrap().to_string();
                received_ops_worker.lock().unwrap().push(op.clone());
                match op.as_str() {
                    "run_extraction" => {
                        active_extractions.insert(id);
                    }
                    "cancel_task" => {
                        let target_id = req["params"]["target_id"].as_u64().unwrap();
                        assert!(
                            active_extractions.remove(&target_id),
                            "cancel_task arrived before run_extraction for target_id={target_id}"
                        );
                        let ack = format!(r#"{{"id":{id},"type":"result","ok":true,"data":null}}"#);
                        let aborted = format!(
                            r#"{{"id":{target_id},"type":"result","ok":false,"error":{{"code":"Aborted","message":"cancelled"}}}}"#
                        );
                        worker_write.write_all(ack.as_bytes()).await.unwrap();
                        worker_write.write_all(b"\n").await.unwrap();
                        worker_write.write_all(aborted.as_bytes()).await.unwrap();
                        worker_write.write_all(b"\n").await.unwrap();
                        worker_write.flush().await.unwrap();
                    }
                    other => panic!("unexpected op: {other}"),
                }
            }
        });

        let sup = Arc::new(WorkerSupervisor::new(WorkerConfig::default()));
        {
            let mut rt = sup.runtime.lock().await;
            *rt = Some(ResidentWorker {
                stdin_tx: Some(stdin_tx),
                pending,
                alive,
                shutdown_tx: Some(shutdown_tx),
                io_handle,
                pid: Some(4242),
            });
        }

        // Case 1: Deterministic pre-dispatch pause right after acquire_handles()
        let entered = Arc::new(tokio::sync::Notify::new());
        let release = Arc::new(tokio::sync::Notify::new());
        *sup.test_pre_dispatch_hook.lock().unwrap() =
            Some((Arc::clone(&entered), Arc::clone(&release)));

        let sup_call = Arc::clone(&sup);
        let sink: WorkerEventSink = Arc::new(|_| {});
        let paused_call = tokio::spawn(async move {
            sup_call
                .call_streaming("warm-cancel", "run_extraction", serde_json::json!({}), sink)
                .await
        });

        entered.notified().await;
        sup.cancel_task_by_id("warm-cancel").await.unwrap();
        release.notify_one();

        let res = paused_call.await.unwrap();
        assert!(matches!(res, Err(WorkerError::Aborted(_))));
        assert!(
            received_ops.lock().unwrap().is_empty(),
            "pre-dispatch cancel must not send run_extraction or cancel_task"
        );

        // Case 2: Clear hook and race call_streaming vs cancel_task_by_id 30 times
        *sup.test_pre_dispatch_hook.lock().unwrap() = None;
        tokio::time::timeout(Duration::from_secs(5), async {
            for i in 0..30 {
                let task_id = format!("race-{i}");
                let sink: WorkerEventSink = Arc::new(|_| {});
                let (run_res, cancel_res) = tokio::join!(
                    sup.call_streaming(&task_id, "run_extraction", serde_json::json!({}), sink),
                    async {
                        tokio::task::yield_now().await;
                        sup.cancel_task_by_id(&task_id).await
                    }
                );
                cancel_res.unwrap();
                assert!(
                    matches!(run_res, Err(WorkerError::Aborted(_))),
                    "iter {i}: expected Aborted, got {run_res:?}"
                );
            }
        })
        .await
        .expect("concurrent cancel race loop should never deadlock");

        sup.shutdown().await;
        worker_task.await.unwrap();
        assert!(sup.current_pid().await.is_none());
    }

    #[tokio::test]
    async fn call_streaming_with_cancel_aborts_on_external_cancel_before_dispatch() {
        let sup = Arc::new(WorkerSupervisor::new(WorkerConfig {
            override_bin: Some(std::path::PathBuf::from("/nonexistent/never_spawned")),
            ..Default::default()
        }));

        // 1. Pre-cancelled external watch channel aborts before registering or spawning
        let (tx1, rx1) = watch::channel(true);
        let sink: WorkerEventSink = Arc::new(|_| {});
        let res1 = sup
            .call_streaming_with_cancel("ext-pre", "run_extraction", serde_json::json!({}), sink, Some(rx1))
            .await;
        assert!(matches!(res1, Err(WorkerError::Aborted(_))));
        assert!(sup.current_pid().await.is_none());
        drop(tx1);

        // 2. External cancel triggered while acquire_handles() is blocked aborts immediately
        let rt_guard = sup.runtime.lock().await;
        let (tx2, rx2) = watch::channel(false);
        let sup_call = Arc::clone(&sup);
        let sink2: WorkerEventSink = Arc::new(|_| {});
        let handle = tokio::spawn(async move {
            sup_call
                .call_streaming_with_cancel(
                    "ext-inflight",
                    "run_extraction",
                    serde_json::json!({}),
                    sink2,
                    Some(rx2),
                )
                .await
        });

        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                if sup.active_tasks.lock().unwrap().contains_key("ext-inflight") {
                    break;
                }
                tokio::task::yield_now().await;
            }
        })
        .await
        .expect("task should register Starting before acquire_handles");

        let _ = tx2.send(true);
        let res2 = tokio::time::timeout(Duration::from_secs(2), handle)
            .await
            .expect("external cancel should abort without waiting for runtime lock")
            .unwrap();
        assert!(matches!(res2, Err(WorkerError::Aborted(_))));
        drop(rt_guard);
        assert!(sup.current_pid().await.is_none());
        assert!(!sup.active_tasks.lock().unwrap().contains_key("ext-inflight"));
    }

    #[tokio::test]
    async fn malformed_and_unknown_events_do_not_break_result_and_duplicate_task_id_rejected() {
        let (host_read, mut worker_write) = tokio::io::duplex(8192);
        let (mut worker_read, host_write) = tokio::io::duplex(8192);
        let (stdin_tx, stdin_rx) = mpsc::channel::<String>(32);
        let (shutdown_tx, shutdown_rx) = oneshot::channel::<()>();
        let pending: PendingMap = Arc::new(StdMutex::new(HashMap::new()));
        let alive = Arc::new(AtomicBool::new(true));

        let io_handle = spawn_process_io_loop(
            host_read,
            host_write,
            Option::<tokio::io::DuplexStream>::None,
            None,
            stdin_rx,
            shutdown_rx,
            Arc::clone(&pending),
            Arc::clone(&alive),
        );

        let worker_task = tokio::spawn(async move {
            let mut lines = BufReader::new(&mut worker_read).lines();
            let req: serde_json::Value =
                serde_json::from_str(&lines.next_line().await.unwrap().unwrap()).unwrap();
            let id = req["id"].as_u64().unwrap();

            let malformed_ev =
                format!(r#"{{"id":{id},"type":"event","event":"progress","data":{{}}}}"#);
            let unknown_ev =
                format!(r#"{{"id":{id},"type":"event","event":"custom_ev","data":{{"k":1}}}}"#);
            let ok_ev = format!(
                r#"{{"id":{id},"type":"event","event":"progress","data":{{"current_ms":10,"total_ms":100,"fps":25.0}}}}"#
            );
            let result = format!(r#"{{"id":{id},"type":"result","ok":true,"data":{{"ok":true}}}}"#);
            for line in [malformed_ev, unknown_ev, ok_ev, result] {
                worker_write.write_all(line.as_bytes()).await.unwrap();
                worker_write.write_all(b"\n").await.unwrap();
            }
            worker_write.flush().await.unwrap();
        });

        let sup = Arc::new(WorkerSupervisor::new(WorkerConfig::default()));
        // cancel_task_by_id on unknown id returns Ok(()) without spawning
        sup.cancel_task_by_id("unknown-task").await.unwrap();

        {
            let mut rt = sup.runtime.lock().await;
            *rt = Some(ResidentWorker {
                stdin_tx: Some(stdin_tx),
                pending,
                alive,
                shutdown_tx: Some(shutdown_tx),
                io_handle,
                pid: Some(9001),
            });
        }

        let events = Arc::new(StdMutex::new(Vec::<TaskEvent>::new()));
        let events_ref = Arc::clone(&events);
        let sink: WorkerEventSink = Arc::new(move |ev| {
            events_ref.lock().unwrap().push(ev);
        });
        let res = sup
            .call_streaming("ev-task", "run_extraction", serde_json::json!({}), sink)
            .await
            .unwrap();
        assert_eq!(res["ok"], true);
        assert!(events
            .lock()
            .unwrap()
            .iter()
            .any(|e| matches!(e, TaskEvent::Progress { current_ms: 10, .. })));

        sup.shutdown().await;
        worker_task.await.unwrap();

        // Duplicate active task_id rejection
        let sup_dup = Arc::new(WorkerSupervisor::new(WorkerConfig {
            override_bin: Some(std::path::PathBuf::from("/nonexistent/never_spawned")),
            ..Default::default()
        }));
        let _rt_guard = sup_dup.runtime.lock().await;
        let sup_ref = Arc::clone(&sup_dup);
        let sink1: WorkerEventSink = Arc::new(|_| {});
        let first = tokio::spawn(async move {
            sup_ref
                .call_streaming("dup-id", "run_extraction", serde_json::json!({}), sink1)
                .await
        });

        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                if sup_dup.active_tasks.lock().unwrap().contains_key("dup-id") {
                    break;
                }
                tokio::task::yield_now().await;
            }
        })
        .await
        .unwrap();

        let sink2: WorkerEventSink = Arc::new(|_| {});
        let dup_err = sup_dup
            .call_streaming("dup-id", "run_extraction", serde_json::json!({}), sink2)
            .await
            .unwrap_err();
        assert!(matches!(dup_err, WorkerError::Remote { ref code, .. } if code == "TaskConflict"));

        sup_dup.cancel_task_by_id("dup-id").await.unwrap();
        assert!(matches!(first.await.unwrap(), Err(WorkerError::Aborted(_))));
    }
}
