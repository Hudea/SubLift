pub mod dto;
pub mod export;

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex as StdMutex};

use base64::Engine as _;
use tauri::ipc::Channel;
use tauri::State;
use tauri_plugin_dialog::DialogExt;
use tokio::sync::watch;

use crate::dto::{
    AutoRegionDto, BatchExportConflictPolicy, BatchExportItemInput, BatchExportReport,
    CapabilitiesDto, ExportResultDto, ExtractionMode, ExtractionResultDto, FramePreviewDto,
    MergeOptionsDto, OcrEngineId, ProviderConnectionTestDto, ProviderProfileMaskedDto,
    ProviderUpsertDto, ReRecognizeItemInput, ReRecognizeResult, SessionDraftDto,
    SessionSummaryDto, SourceBox, StartExtractionParams, SubtitleEntry, SubtitleScript,
    TaskEvent, VideoInfoDto,
};
use crate::media::MediaStreamManager;
use crate::storage::{ensure_private_dir, ProviderStore, SessionStore, StorageRoot};
use crate::vlm::VlmService;
use crate::worker::protocol::RunExtractionParams;
use crate::worker::{WorkerBackend, WorkerEventSink};

use export::{batch_export_srt_impl, export_srt_impl};

static PREVIEW_TMP_SEQ: AtomicU64 = AtomicU64::new(1);
static TASK_ENTRY_SEQ: AtomicU64 = AtomicU64::new(1);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ActiveTaskKind {
    LocalWorker,
    Vlm,
}

pub struct ActiveTaskEntry {
    pub entry_id: u64,
    pub kind: ActiveTaskKind,
    pub cancelled: bool,
    pub cancel_tx: watch::Sender<bool>,
}

pub struct AppState {
    pub worker: Arc<dyn WorkerBackend>,
    pub vlm: Arc<dyn VlmService>,
    pub storage: StorageRoot,
    pub providers: Arc<ProviderStore>,
    pub sessions: Arc<SessionStore>,
    pub media: Arc<MediaStreamManager>,
    pub active_tasks: Arc<StdMutex<HashMap<String, ActiveTaskEntry>>>,
    #[cfg(test)]
    pub(crate) test_pre_backend_hook:
        StdMutex<Option<(Arc<tokio::sync::Notify>, Arc<tokio::sync::Notify>)>>,
}

impl AppState {
    pub fn new(
        worker: Arc<dyn WorkerBackend>,
        vlm: Arc<dyn VlmService>,
        storage: StorageRoot,
        media: Arc<MediaStreamManager>,
    ) -> Self {
        let providers = Arc::new(ProviderStore::new(storage.clone()));
        let sessions = Arc::new(SessionStore::new(storage.clone()));
        Self::with_stores(worker, vlm, storage, providers, sessions, media)
    }

    pub fn with_stores(
        worker: Arc<dyn WorkerBackend>,
        vlm: Arc<dyn VlmService>,
        storage: StorageRoot,
        providers: Arc<ProviderStore>,
        sessions: Arc<SessionStore>,
        media: Arc<MediaStreamManager>,
    ) -> Self {
        Self {
            worker,
            vlm,
            storage,
            providers,
            sessions,
            media,
            active_tasks: Arc::new(StdMutex::new(HashMap::new())),
            #[cfg(test)]
            test_pre_backend_hook: StdMutex::new(None),
        }
    }
}

struct ActiveTaskCleanup {
    registry: Arc<StdMutex<HashMap<String, ActiveTaskEntry>>>,
    task_id: String,
    entry_id: u64,
}

impl Drop for ActiveTaskCleanup {
    fn drop(&mut self) {
        if let Ok(mut map) = self.registry.lock() {
            if map.get(&self.task_id).map(|e| e.entry_id) == Some(self.entry_id) {
                map.remove(&self.task_id);
            }
        }
    }
}

fn take_task_cancelled(
    registry: &StdMutex<HashMap<String, ActiveTaskEntry>>,
    task_id: &str,
    entry_id: u64,
) -> bool {
    let mut map = registry.lock().expect("active_tasks lock poisoned");
    if map.get(task_id).map(|e| e.entry_id) == Some(entry_id) {
        map.remove(task_id).is_some_and(|e| e.cancelled)
    } else {
        false
    }
}

struct TempFileUnlink {
    path: PathBuf,
}

impl Drop for TempFileUnlink {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

pub(crate) async fn get_capabilities_impl(state: &AppState) -> Result<CapabilitiesDto, String> {
    state
        .worker
        .capabilities()
        .await
        .map_err(|e| format!("[{}] {e}", e.code()))
}

pub(crate) async fn probe_video_impl(
    state: &AppState,
    video_path: &str,
) -> Result<VideoInfoDto, String> {
    state
        .worker
        .probe_video(video_path)
        .await
        .map_err(|e| format!("[{}] {e}", e.code()))
}

pub(crate) async fn extract_preview_frame_impl(
    state: &AppState,
    video_path: &str,
    timestamp_ms: i64,
    crop: Option<SourceBox>,
    max_width: Option<i32>,
) -> Result<FramePreviewDto, String> {
    let preview_dir = state.storage.preview_cache_dir();
    ensure_private_dir(&preview_dir).map_err(|e| format!("[StorageError] {e}"))?;
    let seq = PREVIEW_TMP_SEQ.fetch_add(1, Ordering::Relaxed);
    let temp_path = preview_dir.join(format!("preview_{}_{seq}.png", std::process::id()));
    let _unlink = TempFileUnlink {
        path: temp_path.clone(),
    };

    let res = state
        .worker
        .extract_frame(
            video_path,
            timestamp_ms,
            crop,
            max_width,
            &temp_path.to_string_lossy(),
        )
        .await
        .map_err(|e| format!("[{}] {e}", e.code()))?;

    let read_path = res
        .out_path
        .as_deref()
        .map(PathBuf::from)
        .unwrap_or_else(|| temp_path.clone());
    let png_bytes = tokio::fs::read(&read_path)
        .await
        .map_err(|e| format!("[PreviewIo] failed to read preview frame: {e}"))?;
    let b64 = base64::engine::general_purpose::STANDARD.encode(png_bytes);

    Ok(FramePreviewDto {
        image_url: format!("data:image/png;base64,{b64}"),
        width: res.width,
        height: res.height,
    })
}

pub(crate) async fn detect_auto_region_impl(
    state: &AppState,
    video_path: &str,
    engine: OcrEngineId,
    script: SubtitleScript,
    bottom_ratio: f64,
) -> Result<AutoRegionDto, String> {
    state
        .worker
        .detect_auto_region(video_path, engine, script, bottom_ratio)
        .await
        .map_err(|e| format!("[{}] {e}", e.code()))
}

pub(crate) async fn start_extraction_impl(
    state: &AppState,
    task_id: String,
    mut params: StartExtractionParams,
    on_event: WorkerEventSink,
) -> Result<ExtractionResultDto, String> {
    if task_id.trim().is_empty() {
        return Err("[InvalidConfiguration] task_id must not be empty".to_string());
    }

    let kind = match params.mode {
        ExtractionMode::LocalOcr => {
            if params.engine.is_none() {
                return Err(
                    "[InvalidConfiguration] engine is required for local_ocr mode".to_string(),
                );
            }
            ActiveTaskKind::LocalWorker
        }
        ExtractionMode::PureTimingStrips => ActiveTaskKind::Vlm,
    };

    let entry_id = TASK_ENTRY_SEQ.fetch_add(1, Ordering::Relaxed);
    let (cancel_tx, cancel_rx) = watch::channel(false);
    {
        let mut map = state
            .active_tasks
            .lock()
            .expect("active_tasks lock poisoned");
        if map.contains_key(&task_id) {
            return Err(format!(
                "[TaskConflict] task '{task_id}' is already running"
            ));
        }
        map.insert(
            task_id.clone(),
            ActiveTaskEntry {
                entry_id,
                kind,
                cancelled: false,
                cancel_tx,
            },
        );
    }
    let _guard = ActiveTaskCleanup {
        registry: Arc::clone(&state.active_tasks),
        task_id: task_id.clone(),
        entry_id,
    };

    #[cfg(test)]
    {
        let hook = state
            .test_pre_backend_hook
            .lock()
            .expect("test_pre_backend_hook lock poisoned")
            .clone();
        if let Some((entered, release)) = hook {
            entered.notify_one();
            release.notified().await;
        }
    }

    let terminal_emitted = Arc::new(AtomicBool::new(false));
    let terminal_emitted_flag = Arc::clone(&terminal_emitted);
    let raw_on_event = Arc::clone(&on_event);
    let tracked_on_event: WorkerEventSink = Arc::new(move |ev| {
        if matches!(
            ev,
            TaskEvent::Cancelled { .. } | TaskEvent::Completed { .. } | TaskEvent::Failed { .. }
        ) && terminal_emitted_flag.swap(true, Ordering::AcqRel)
        {
            return;
        }
        raw_on_event(ev);
    });
    let emit_cancelled_once = || {
        if !terminal_emitted.swap(true, Ordering::AcqRel) {
            on_event(TaskEvent::Cancelled {
                task_id: task_id.clone(),
            });
        }
    };
    let emit_completed_once = |result: ExtractionResultDto| {
        if !terminal_emitted.swap(true, Ordering::AcqRel) {
            on_event(TaskEvent::Completed {
                task_id: task_id.clone(),
                result,
            });
        }
    };
    let emit_failed_once = |code: String, message: String, retryable: bool| {
        if !terminal_emitted.swap(true, Ordering::AcqRel) {
            on_event(TaskEvent::Failed {
                task_id: task_id.clone(),
                code,
                message,
                retryable,
            });
        }
    };

    if *cancel_rx.borrow() {
        let _ = take_task_cancelled(&state.active_tasks, &task_id, entry_id);
        emit_cancelled_once();
        return Err("[Aborted] task cancelled before start".to_string());
    }

    params.config.subtitle_script = params.script;

    match kind {
        ActiveTaskKind::LocalWorker => {
            let worker_params = RunExtractionParams {
                video_path: params.video_path,
                mode: ExtractionMode::LocalOcr,
                engine: params.engine,
                region_box: params.region_box,
                config: params.config,
                strips_dir: None,
            };
            match state
                .worker
                .run_extraction_with_cancel(
                    &task_id,
                    worker_params,
                    Arc::clone(&tracked_on_event),
                    cancel_rx,
                )
                .await
            {
                Ok(result) => {
                    if take_task_cancelled(&state.active_tasks, &task_id, entry_id) {
                        emit_cancelled_once();
                        Err("[Aborted] task cancelled".to_string())
                    } else {
                        emit_completed_once(result.clone());
                        Ok(result)
                    }
                }
                Err(crate::worker::WorkerError::Aborted(msg)) => {
                    let _ = take_task_cancelled(&state.active_tasks, &task_id, entry_id);
                    emit_cancelled_once();
                    Err(format!("[Aborted] {msg}"))
                }
                Err(err) => {
                    if take_task_cancelled(&state.active_tasks, &task_id, entry_id) {
                        emit_cancelled_once();
                        Err("[Aborted] task cancelled".to_string())
                    } else {
                        let msg = err.to_string();
                        emit_failed_once(err.code().to_string(), msg.clone(), err.is_retryable());
                        Err(format!("[{}] {msg}", err.code()))
                    }
                }
            }
        }
        ActiveTaskKind::Vlm => match state
            .vlm
            .run_vlm_extraction_with_cancel(&task_id, params, tracked_on_event, cancel_rx)
            .await
        {
            Ok(result) => {
                if take_task_cancelled(&state.active_tasks, &task_id, entry_id) {
                    emit_cancelled_once();
                    Err("[Aborted] task cancelled".to_string())
                } else {
                    emit_completed_once(result.clone());
                    Ok(result)
                }
            }
            Err(crate::vlm::VlmError::Cancelled) => {
                let _ = take_task_cancelled(&state.active_tasks, &task_id, entry_id);
                emit_cancelled_once();
                Err(format!(
                    "[{}] {}",
                    crate::vlm::VlmError::Cancelled.code(),
                    crate::vlm::VlmError::Cancelled
                ))
            }
            Err(err) => {
                if take_task_cancelled(&state.active_tasks, &task_id, entry_id) {
                    emit_cancelled_once();
                    Err("[Aborted] task cancelled".to_string())
                } else {
                    emit_failed_once(err.code().to_string(), err.to_string(), err.is_retryable());
                    Err(format!("[{}] {err}", err.code()))
                }
            }
        },
    }
}

pub(crate) async fn cancel_extraction_impl(state: &AppState, task_id: &str) -> Result<(), String> {
    let kind = {
        let mut map = state
            .active_tasks
            .lock()
            .expect("active_tasks lock poisoned");
        match map.get_mut(task_id) {
            Some(entry) => {
                if entry.cancelled {
                    None
                } else {
                    entry.cancelled = true;
                    let _ = entry.cancel_tx.send(true);
                    Some(entry.kind)
                }
            }
            None => None,
        }
    };
    match kind {
        Some(ActiveTaskKind::LocalWorker) => state
            .worker
            .cancel_task(task_id)
            .await
            .map_err(|e| format!("[{}] {e}", e.code())),
        // For VLM tasks, entry.cancel_tx.send(true) above is uniquely bound to
        // that entry_id's caller_cancel_rx; do NOT invoke state.vlm.cancel_task(task_id)
        // post-lock or a delayed cancel could hit a newer run reusing task_id (ABA).
        Some(ActiveTaskKind::Vlm) | None => Ok(()),
    }
}

pub(crate) async fn re_recognize_entries_impl(
    state: &AppState,
    video_path: &str,
    region_box: Option<SourceBox>,
    items: Vec<ReRecognizeItemInput>,
    provider_id: Option<String>,
    script: SubtitleScript,
) -> Result<Vec<ReRecognizeResult>, String> {
    state
        .vlm
        .re_recognize_entries(video_path, region_box, items, provider_id, script)
        .await
        .map_err(|e| format!("[{}] {e}", e.code()))
}

#[tauri::command(rename_all = "snake_case")]
pub async fn get_capabilities(state: State<'_, AppState>) -> Result<CapabilitiesDto, String> {
    get_capabilities_impl(&state).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn probe_video(
    state: State<'_, AppState>,
    video_path: String,
) -> Result<VideoInfoDto, String> {
    probe_video_impl(&state, &video_path).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn extract_preview_frame(
    state: State<'_, AppState>,
    video_path: String,
    timestamp_ms: i64,
    crop: Option<SourceBox>,
    max_width: Option<i32>,
) -> Result<FramePreviewDto, String> {
    extract_preview_frame_impl(&state, &video_path, timestamp_ms, crop, max_width).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn detect_auto_region(
    state: State<'_, AppState>,
    video_path: String,
    engine: OcrEngineId,
    script: SubtitleScript,
    bottom_ratio: f64,
) -> Result<AutoRegionDto, String> {
    detect_auto_region_impl(&state, &video_path, engine, script, bottom_ratio).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn start_extraction(
    state: State<'_, AppState>,
    task_id: String,
    params: StartExtractionParams,
    on_event: Channel<TaskEvent>,
) -> Result<ExtractionResultDto, String> {
    let sink: WorkerEventSink = Arc::new(move |ev| {
        let _ = on_event.send(ev);
    });
    start_extraction_impl(&state, task_id, params, sink).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn cancel_extraction(
    state: State<'_, AppState>,
    task_id: String,
) -> Result<(), String> {
    cancel_extraction_impl(&state, &task_id).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn re_recognize_entries(
    state: State<'_, AppState>,
    video_path: String,
    region_box: Option<SourceBox>,
    items: Vec<ReRecognizeItemInput>,
    provider_id: Option<String>,
    script: SubtitleScript,
) -> Result<Vec<ReRecognizeResult>, String> {
    re_recognize_entries_impl(&state, &video_path, region_box, items, provider_id, script).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn export_srt(
    state: State<'_, AppState>,
    out_path: String,
    entries: Vec<SubtitleEntry>,
    apply_merge: bool,
    merge_options: Option<MergeOptionsDto>,
) -> Result<ExportResultDto, String> {
    export_srt_impl(&*state.worker, &out_path, entries, apply_merge, merge_options).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn batch_export_srt(
    state: State<'_, AppState>,
    items: Vec<BatchExportItemInput>,
    conflict_policy: BatchExportConflictPolicy,
) -> Result<BatchExportReport, String> {
    batch_export_srt_impl(&*state.worker, items, conflict_policy).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn pick_video_files(app: tauri::AppHandle) -> Result<Vec<String>, String> {
    let (tx, rx) = tokio::sync::oneshot::channel();
    app.dialog()
        .file()
        .add_filter("Video Files", &["mp4", "mkv", "mov", "avi", "webm", "ts", "m4v"])
        .pick_files(move |files| {
            let paths = files
                .unwrap_or_default()
                .into_iter()
                .filter_map(|fp| fp.into_path().ok())
                .map(|p| p.to_string_lossy().to_string())
                .collect();
            let _ = tx.send(paths);
        });
    rx.await
        .map_err(|e| format!("[DialogError] failed to receive file picker result: {e}"))
}

#[tauri::command(rename_all = "snake_case")]
pub async fn pick_save_srt_path(
    app: tauri::AppHandle,
    default_name: String,
) -> Result<Option<String>, String> {
    let (tx, rx) = tokio::sync::oneshot::channel();
    app.dialog()
        .file()
        .add_filter("SubRip Subtitle", &["srt"])
        .set_file_name(&default_name)
        .save_file(move |file| {
            let path = file
                .and_then(|fp| fp.into_path().ok())
                .map(|p| p.to_string_lossy().to_string());
            let _ = tx.send(path);
        });
    rx.await
        .map_err(|e| format!("[DialogError] failed to receive save dialog result: {e}"))
}

#[tauri::command(rename_all = "snake_case")]
pub async fn list_providers(
    state: State<'_, AppState>,
) -> Result<Vec<ProviderProfileMaskedDto>, String> {
    state
        .providers
        .list_providers()
        .map_err(|e| format!("[StorageError] {e}"))
}

#[tauri::command(rename_all = "snake_case")]
pub async fn save_provider(
    state: State<'_, AppState>,
    profile: ProviderUpsertDto,
) -> Result<ProviderProfileMaskedDto, String> {
    state
        .providers
        .save_provider(profile)
        .map_err(|e| format!("[StorageError] {e}"))
}

#[tauri::command(rename_all = "snake_case")]
pub async fn test_provider_connection(
    state: State<'_, AppState>,
    profile: ProviderUpsertDto,
) -> Result<ProviderConnectionTestDto, String> {
    state
        .vlm
        .test_provider_connection(profile)
        .await
        .map_err(|e| format!("[{}] {e}", e.code()))
}

#[tauri::command(rename_all = "snake_case")]
pub async fn delete_provider(state: State<'_, AppState>, id: String) -> Result<(), String> {
    state
        .providers
        .delete_provider(&id)
        .map_err(|e| format!("[StorageError] {e}"))
}

#[tauri::command(rename_all = "snake_case")]
pub async fn import_providers_from_env(
    state: State<'_, AppState>,
    env_path: Option<String>,
) -> Result<Vec<ProviderProfileMaskedDto>, String> {
    state
        .providers
        .import_providers_from_env(env_path.as_deref().map(Path::new))
        .map_err(|e| format!("[StorageError] {e}"))
}

#[tauri::command(rename_all = "snake_case")]
pub async fn load_session_draft(
    state: State<'_, AppState>,
    video_path: String,
) -> Result<Option<SessionDraftDto>, String> {
    state
        .sessions
        .load_session_draft(Path::new(&video_path))
        .map_err(|e| format!("[StorageError] {e}"))
}

#[tauri::command(rename_all = "snake_case")]
pub async fn save_session_draft(
    state: State<'_, AppState>,
    draft: SessionDraftDto,
) -> Result<(), String> {
    state
        .sessions
        .save_session_draft(&draft)
        .map_err(|e| format!("[StorageError] {e}"))
}

#[tauri::command(rename_all = "snake_case")]
pub async fn list_recent_sessions(
    state: State<'_, AppState>,
) -> Result<Vec<SessionSummaryDto>, String> {
    state
        .sessions
        .list_recent_sessions()
        .map_err(|e| format!("[StorageError] {e}"))
}

#[tauri::command(rename_all = "snake_case")]
pub async fn stop_media_stream(state: State<'_, AppState>) -> Result<(), String> {
    state.media.stop_active_stream().await;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dto::{
        BatchExportItemStatus, ExtractionConfigDto, ExtractionMetricsDto, TaskStatus,
    };
    use crate::vlm::StubVlmService;
    use crate::worker::protocol::{
        ExtractFrameResult, FinalizeSrtResult, RecognizeStripsResult,
    };
    use crate::worker::{WorkerError, WorkerSupervisor};
    use async_trait::async_trait;
    use std::sync::Mutex;
    use tempfile::TempDir;

    struct MockWorkerBackend {
        fail_finalize: bool,
        cancelled_ids: Mutex<Vec<String>>,
        finalize_calls: Mutex<Vec<bool>>,
        run_calls: Mutex<Vec<String>>,
    }

    impl MockWorkerBackend {
        fn new(fail_finalize: bool) -> Self {
            Self {
                fail_finalize,
                cancelled_ids: Mutex::new(Vec::new()),
                finalize_calls: Mutex::new(Vec::new()),
                run_calls: Mutex::new(Vec::new()),
            }
        }
    }

    #[async_trait]
    impl WorkerBackend for MockWorkerBackend {
        async fn capabilities(&self) -> Result<CapabilitiesDto, WorkerError> {
            Ok(CapabilitiesDto {
                engines: vec![OcrEngineId::Paddle],
                ffmpeg_available: true,
                hwaccel_modes: vec!["videotoolbox".into()],
            })
        }

        async fn probe_video(&self, _video_path: &str) -> Result<VideoInfoDto, WorkerError> {
            Ok(VideoInfoDto {
                width: 1920,
                height: 1080,
                duration_ms: 10_000,
                fps: 24.0,
                display_transform_ok: true,
                transform_note: None,
            })
        }

        async fn extract_frame(
            &self,
            _video_path: &str,
            _timestamp_ms: i64,
            _crop: Option<SourceBox>,
            _max_width: Option<i32>,
            out_path: &str,
        ) -> Result<ExtractFrameResult, WorkerError> {
            std::fs::write(out_path, b"\x89PNG\r\n\x1a\nmock").unwrap();
            Ok(ExtractFrameResult {
                width: 640,
                height: 360,
                out_path: Some(out_path.to_string()),
            })
        }

        async fn detect_auto_region(
            &self,
            _video_path: &str,
            _engine: OcrEngineId,
            script: SubtitleScript,
            _bottom_ratio: f64,
        ) -> Result<AutoRegionDto, WorkerError> {
            Ok(AutoRegionDto {
                detected: true,
                region_box: SourceBox {
                    x: 100,
                    y: 800,
                    width: 1720,
                    height: 200,
                },
                profile: Some(crate::dto::SubtitleProfileDto {
                    script,
                    center_x: 860,
                    center_y: 100,
                    height: 44,
                    y_min: 78,
                    y_max: 122,
                }),
                sample_timestamp_ms: 1500,
                preview_text: "测试".into(),
                confidence: 0.94,
                error_message: String::new(),
            })
        }

        async fn run_extraction(
            &self,
            task_id: &str,
            _params: RunExtractionParams,
            on_event: WorkerEventSink,
        ) -> Result<ExtractionResultDto, WorkerError> {
            self.run_calls.lock().unwrap().push(task_id.to_string());
            let entry = SubtitleEntry {
                id: 1,
                start_ms: 100,
                end_ms: 900,
                text: "line 1".into(),
                confidence: 0.95,
            };
            on_event(TaskEvent::Progress {
                task_id: task_id.to_string(),
                current_ms: 500,
                total_ms: 1000,
                fps: 100.0,
            });
            on_event(TaskEvent::SubtitleEntry {
                task_id: task_id.to_string(),
                entry: entry.clone(),
            });
            Ok(ExtractionResultDto {
                entries: vec![entry],
                metrics: ExtractionMetricsDto::default(),
                failed_slot_count: None,
            })
        }

        async fn recognize_strips(
            &self,
            _engine: OcrEngineId,
            _strip_paths: Vec<String>,
            _script: SubtitleScript,
        ) -> Result<RecognizeStripsResult, WorkerError> {
            Ok(RecognizeStripsResult { items: vec![] })
        }

        async fn finalize_srt(
            &self,
            entries: Vec<SubtitleEntry>,
            apply_merge: bool,
            _merge_options: Option<MergeOptionsDto>,
        ) -> Result<FinalizeSrtResult, WorkerError> {
            self.finalize_calls.lock().unwrap().push(apply_merge);
            if self.fail_finalize {
                return Err(WorkerError::Remote {
                    code: "FinalizeFailed".into(),
                    message: "worker unavailable".into(),
                });
            }
            let srt_text = entries
                .iter()
                .enumerate()
                .map(|(i, e)| format!("{}\n00:00:00,100 --> 00:00:00,900\n{}\n\n", i + 1, e.text))
                .collect();
            Ok(FinalizeSrtResult {
                srt_text,
                cue_count: entries.len(),
            })
        }

        async fn cancel_task(&self, task_id: &str) -> Result<(), WorkerError> {
            self.cancelled_ids.lock().unwrap().push(task_id.to_string());
            Ok(())
        }

        async fn shutdown(&self) {}
    }

    fn test_state(worker: Arc<dyn WorkerBackend>, dir: &TempDir) -> AppState {
        AppState::new(
            worker,
            Arc::new(StubVlmService),
            StorageRoot::new(dir.path().join("appdata")).unwrap(),
            Arc::new(MediaStreamManager::new()),
        )
    }

    #[tokio::test]
    async fn start_extraction_validates_engine_streams_events_and_vlm_fails_closed() {
        let dir = TempDir::new().unwrap();
        let state = test_state(Arc::new(MockWorkerBackend::new(false)), &dir);
        let events = Arc::new(StdMutex::new(Vec::<TaskEvent>::new()));
        let sink_events = Arc::clone(&events);
        let sink: WorkerEventSink = Arc::new(move |ev| {
            sink_events.lock().unwrap().push(ev);
        });

        // local_ocr without engine fails closed
        let missing_engine = StartExtractionParams {
            video_path: "/tmp/v.mp4".into(),
            mode: ExtractionMode::LocalOcr,
            engine: None,
            provider_id: None,
            region_box: None,
            config: ExtractionConfigDto::default(),
            script: SubtitleScript::Auto,
        };
        let err = start_extraction_impl(&state, "t1".into(), missing_engine, Arc::clone(&sink))
            .await
            .unwrap_err();
        assert!(err.contains("InvalidConfiguration"));

        // local_ocr with engine succeeds and emits Completed
        let valid_local = StartExtractionParams {
            video_path: "/tmp/v.mp4".into(),
            mode: ExtractionMode::LocalOcr,
            engine: Some(OcrEngineId::Paddle),
            provider_id: None,
            region_box: None,
            config: ExtractionConfigDto::default(),
            script: SubtitleScript::Cjk,
        };
        let res = start_extraction_impl(&state, "t2".into(), valid_local, Arc::clone(&sink))
            .await
            .unwrap();
        assert_eq!(res.entries.len(), 1);
        assert!(events
            .lock()
            .unwrap()
            .iter()
            .any(|e| matches!(e, TaskEvent::Completed { task_id, .. } if task_id == "t2")));

        // pure_timing_strips routes to StubVlmService and fails closed
        let vlm_params = StartExtractionParams {
            video_path: "/tmp/v.mp4".into(),
            mode: ExtractionMode::PureTimingStrips,
            engine: None,
            provider_id: Some("p1".into()),
            region_box: None,
            config: ExtractionConfigDto::default(),
            script: SubtitleScript::Auto,
        };
        let vlm_err = start_extraction_impl(&state, "t3".into(), vlm_params, sink)
            .await
            .unwrap_err();
        assert!(vlm_err.contains("VlmNotImplemented"));
    }

    #[tokio::test]
    async fn extract_preview_frame_encodes_base64_and_deletes_temp_png() {
        let dir = TempDir::new().unwrap();
        let state = test_state(Arc::new(MockWorkerBackend::new(false)), &dir);
        let preview = extract_preview_frame_impl(&state, "/tmp/v.mp4", 1200, None, Some(960))
            .await
            .unwrap();
        assert!(preview.image_url.starts_with("data:image/png;base64,"));
        assert_eq!(preview.width, 640);
        // Temp preview PNG must be unlinked immediately
        let leftover: Vec<_> = std::fs::read_dir(state.storage.preview_cache_dir())
            .unwrap()
            .filter_map(Result::ok)
            .collect();
        assert!(leftover.is_empty());
    }

    #[tokio::test]
    async fn export_and_batch_export_enforce_policies_and_fail_closed() {
        let dir = TempDir::new().unwrap();
        let video_dir = dir.path().join("videos");
        std::fs::create_dir_all(&video_dir).unwrap();

        let entries = vec![SubtitleEntry {
            id: 1,
            start_ms: 100,
            end_ms: 900,
            text: "Hello".into(),
            confidence: 0.9,
        }];

        // Fail closed when worker fails
        let failing_worker = MockWorkerBackend::new(true);
        let fail_target = video_dir.join("fail.srt");
        let err = export_srt_impl(
            &failing_worker,
            fail_target.to_str().unwrap(),
            entries.clone(),
            false,
            None,
        )
        .await
        .unwrap_err();
        assert!(err.contains("FinalizeFailed"));
        assert!(!fail_target.exists());

        // Batch export conflict policies
        let worker = MockWorkerBackend::new(false);
        let existing_srt = video_dir.join("ep01.srt");
        std::fs::write(&existing_srt, "OLD").unwrap();
        let fresh_srt = video_dir.join("ep02.srt");

        let report_rename = batch_export_srt_impl(
            &worker,
            vec![
                BatchExportItemInput {
                    video_path: video_dir.join("ep01.mp4").to_string_lossy().into(),
                    out_path: existing_srt.to_string_lossy().into(),
                    entries: entries.clone(),
                },
                BatchExportItemInput {
                    video_path: video_dir.join("ep02.mp4").to_string_lossy().into(),
                    out_path: fresh_srt.to_string_lossy().into(),
                    entries: entries.clone(),
                },
            ],
            BatchExportConflictPolicy::Rename,
        )
        .await
        .unwrap();

        assert_eq!(report_rename.exported_count, 2);
        assert_eq!(report_rename.skipped_count, 0);
        assert_eq!(report_rename.failed_count, 0);
        assert_eq!(report_rename.results[0].status, BatchExportItemStatus::Renamed);
        assert!(video_dir.join("ep01 (1).srt").is_file());
        assert_eq!(std::fs::read_to_string(&existing_srt).unwrap(), "OLD");
        // Batch export always passes apply_merge = false
        assert!(worker.finalize_calls.lock().unwrap().iter().all(|&m| !m));

        // Skip policy
        let report_skip = batch_export_srt_impl(
            &worker,
            vec![BatchExportItemInput {
                video_path: video_dir.join("ep01.mp4").to_string_lossy().into(),
                out_path: existing_srt.to_string_lossy().into(),
                entries: entries.clone(),
            }],
            BatchExportConflictPolicy::Skip,
        )
        .await
        .unwrap();
        assert_eq!(report_skip.skipped_count, 1);
        assert_eq!(report_skip.exported_count, 0);

        // Single export_srt with apply_merge=true and Overwrite policy
        let single_target = video_dir.join("single.srt");
        let single_res = export_srt_impl(
            &worker,
            single_target.to_str().unwrap(),
            entries.clone(),
            true,
            Some(MergeOptionsDto {
                min_duration_ms: 500,
                merge_gap_ms: 1000,
                drop_empty_text: false,
            }),
        )
        .await
        .unwrap();
        assert_eq!(single_res.cue_count, 1);
        assert!(single_target.is_file());

        let report_overwrite = batch_export_srt_impl(
            &worker,
            vec![BatchExportItemInput {
                video_path: video_dir.join("ep01.mp4").to_string_lossy().into(),
                out_path: existing_srt.to_string_lossy().into(),
                entries,
            }],
            BatchExportConflictPolicy::Overwrite,
        )
        .await
        .unwrap();
        assert_eq!(report_overwrite.exported_count, 1);
        assert_ne!(std::fs::read_to_string(&existing_srt).unwrap(), "OLD");
    }

    #[tokio::test]
    async fn re_recognize_and_session_draft_guard_work() {
        let dir = TempDir::new().unwrap();
        let state = test_state(Arc::new(MockWorkerBackend::new(false)), &dir);

        let err = re_recognize_entries_impl(
            &state,
            "/tmp/v.mp4",
            None,
            vec![ReRecognizeItemInput {
                cue_id: 1,
                start_ms: 100,
                end_ms: 500,
            }],
            None,
            SubtitleScript::Auto,
        )
        .await
        .unwrap_err();
        assert!(err.contains("VlmNotImplemented"));

        let draft = SessionDraftDto {
            video_path: "/tmp/nonexistent_video_for_test.mp4".into(),
            engine: crate::dto::WorkbenchEngine::Paddle,
            provider_id: None,
            script: SubtitleScript::Auto,
            sample_fps: 5.0,
            roi_mode: crate::dto::RoiMode::Auto,
            region_box: None,
            status: TaskStatus::Extracting,
            entries: vec![],
            updated_at_ms: 1,
        };
        assert!(state.sessions.save_session_draft(&draft).is_err());
    }

    struct MockVlmService {
        run_calls: Mutex<Vec<String>>,
        cancelled_ids: Mutex<Vec<String>>,
        unblock_rx: Option<watch::Receiver<u64>>,
    }

    impl MockVlmService {
        fn new(unblock_rx: Option<watch::Receiver<u64>>) -> Self {
            Self {
                run_calls: Mutex::new(Vec::new()),
                cancelled_ids: Mutex::new(Vec::new()),
                unblock_rx,
            }
        }
    }

    #[async_trait]
    impl VlmService for MockVlmService {
        async fn run_vlm_extraction(
            &self,
            task_id: &str,
            params: StartExtractionParams,
            _on_event: WorkerEventSink,
        ) -> Result<ExtractionResultDto, crate::vlm::VlmError> {
            self.run_calls.lock().unwrap().push(task_id.to_string());
            if let Some(rx) = &self.unblock_rx {
                let iter_target = params
                    .video_path
                    .rsplit(':')
                    .next()
                    .and_then(|n| n.parse::<u64>().ok())
                    .unwrap_or(0);
                let mut rx = rx.clone();
                let _ = rx.wait_for(|&v| v >= iter_target).await;
            }
            Ok(ExtractionResultDto {
                entries: vec![SubtitleEntry {
                    id: 1,
                    start_ms: 100,
                    end_ms: 900,
                    text: "vlm uncancelled".into(),
                    confidence: 0.99,
                }],
                metrics: ExtractionMetricsDto::default(),
                failed_slot_count: Some(0),
            })
        }

        async fn re_recognize_entries(
            &self,
            _video_path: &str,
            _region_box: Option<SourceBox>,
            _items: Vec<ReRecognizeItemInput>,
            _provider_id: Option<String>,
            _script: SubtitleScript,
        ) -> Result<Vec<ReRecognizeResult>, crate::vlm::VlmError> {
            Ok(vec![])
        }

        async fn test_provider_connection(
            &self,
            profile: ProviderUpsertDto,
        ) -> Result<ProviderConnectionTestDto, crate::vlm::VlmError> {
            Ok(ProviderConnectionTestDto {
                latency_ms: 12,
                endpoint: profile.base_url,
                model: profile.model,
                response_preview: "OK".into(),
            })
        }

        async fn cancel_task(&self, task_id: &str) -> Result<(), crate::vlm::VlmError> {
            self.cancelled_ids.lock().unwrap().push(task_id.to_string());
            Ok(())
        }
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 4)]
    async fn pre_entry_cancel_aborts_local_worker_deterministically() {
        let dir = TempDir::new().unwrap();
        let backend = Arc::new(MockWorkerBackend::new(false));
        let state = Arc::new(test_state(Arc::clone(&backend) as Arc<dyn WorkerBackend>, &dir));

        let entered = Arc::new(tokio::sync::Notify::new());
        let release = Arc::new(tokio::sync::Notify::new());
        *state.test_pre_backend_hook.lock().unwrap() =
            Some((Arc::clone(&entered), Arc::clone(&release)));

        let events = Arc::new(StdMutex::new(Vec::<TaskEvent>::new()));
        let sink_events = Arc::clone(&events);
        let sink: WorkerEventSink = Arc::new(move |ev| {
            sink_events.lock().unwrap().push(ev);
        });

        let params = StartExtractionParams {
            video_path: "/tmp/v.mp4".into(),
            mode: ExtractionMode::LocalOcr,
            engine: Some(OcrEngineId::Paddle),
            provider_id: None,
            region_box: None,
            config: ExtractionConfigDto::default(),
            script: SubtitleScript::Auto,
        };

        let st_run = Arc::clone(&state);
        let run_handle = tokio::spawn(async move {
            start_extraction_impl(&st_run, "pre-local".into(), params, sink).await
        });

        entered.notified().await;
        assert!(state.active_tasks.lock().unwrap().contains_key("pre-local"));
        cancel_extraction_impl(&state, "pre-local").await.unwrap();
        // Duplicate cancel call must not double-emit TaskEvent::Cancelled
        cancel_extraction_impl(&state, "pre-local").await.unwrap();
        release.notify_one();

        let res = run_handle.await.unwrap();
        assert!(
            matches!(&res, Err(msg) if msg.starts_with("[Aborted]")),
            "expected [Aborted] error, got: {res:?}"
        );
        assert!(
            backend.run_calls.lock().unwrap().is_empty(),
            "pre-entry cancel must abort before WorkerBackend::run_extraction runs"
        );
        assert!(!state.active_tasks.lock().unwrap().contains_key("pre-local"));

        let evs = events.lock().unwrap();
        let cancelled_count = evs
            .iter()
            .filter(|e| matches!(e, TaskEvent::Cancelled { task_id } if task_id == "pre-local"))
            .count();
        assert_eq!(cancelled_count, 1);
        assert!(!evs
            .iter()
            .any(|e| matches!(e, TaskEvent::Completed { .. } | TaskEvent::Failed { .. })));
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 4)]
    async fn pre_entry_cancel_aborts_vlm_deterministically() {
        for use_stub in [true, false] {
            let dir = TempDir::new().unwrap();
            let mock_vlm = Arc::new(MockVlmService::new(None));
            let vlm_service: Arc<dyn VlmService> = if use_stub {
                Arc::new(StubVlmService)
            } else {
                Arc::clone(&mock_vlm) as Arc<dyn VlmService>
            };
            let state = Arc::new(AppState::new(
                Arc::new(MockWorkerBackend::new(false)),
                vlm_service,
                StorageRoot::new(dir.path().join("appdata")).unwrap(),
                Arc::new(MediaStreamManager::new()),
            ));

            let entered = Arc::new(tokio::sync::Notify::new());
            let release = Arc::new(tokio::sync::Notify::new());
            *state.test_pre_backend_hook.lock().unwrap() =
                Some((Arc::clone(&entered), Arc::clone(&release)));

            let events = Arc::new(StdMutex::new(Vec::<TaskEvent>::new()));
            let sink_events = Arc::clone(&events);
            let sink: WorkerEventSink = Arc::new(move |ev| {
                sink_events.lock().unwrap().push(ev);
            });

            let params = StartExtractionParams {
                video_path: "/tmp/v.mp4".into(),
                mode: ExtractionMode::PureTimingStrips,
                engine: None,
                provider_id: Some("p1".into()),
                region_box: None,
                config: ExtractionConfigDto::default(),
                script: SubtitleScript::Auto,
            };

            let st_run = Arc::clone(&state);
            let run_handle = tokio::spawn(async move {
                start_extraction_impl(&st_run, "pre-vlm".into(), params, sink).await
            });

            entered.notified().await;
            assert!(state.active_tasks.lock().unwrap().contains_key("pre-vlm"));
            cancel_extraction_impl(&state, "pre-vlm").await.unwrap();
            cancel_extraction_impl(&state, "pre-vlm").await.unwrap();
            release.notify_one();

            let res = run_handle.await.unwrap();
            assert!(
                matches!(&res, Err(msg) if msg.starts_with("[Aborted]")),
                "use_stub={use_stub}: expected [Aborted] error, got: {res:?}"
            );
            assert!(mock_vlm.run_calls.lock().unwrap().is_empty());
            assert!(!state.active_tasks.lock().unwrap().contains_key("pre-vlm"));

            let evs = events.lock().unwrap();
            let cancelled_count = evs
                .iter()
                .filter(|e| matches!(e, TaskEvent::Cancelled { task_id } if task_id == "pre-vlm"))
                .count();
            assert_eq!(cancelled_count, 1);
            assert!(!evs
                .iter()
                .any(|e| matches!(e, TaskEvent::Completed { .. } | TaskEvent::Failed { .. })));
        }
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 4)]
    async fn multi_thread_start_cancel_race_stress_for_local_worker_and_vlm() {
        const ITERATIONS: u64 = 200;

        // 1. Real WorkerSupervisor (duplex in-memory worker) + AppState.active_tasks race (200 iters)
        let (unblock_tx, unblock_rx) = watch::channel(0u64);
        let (sup, worker_task) = WorkerSupervisor::new_duplex_for_test(unblock_rx.clone());
        let dir = TempDir::new().unwrap();
        let mock_vlm = Arc::new(MockVlmService::new(Some(unblock_rx)));
        let state = Arc::new(AppState::new(
            Arc::clone(&sup) as Arc<dyn WorkerBackend>,
            Arc::clone(&mock_vlm) as Arc<dyn VlmService>,
            StorageRoot::new(dir.path().join("appdata")).unwrap(),
            Arc::new(MediaStreamManager::new()),
        ));

        tokio::time::timeout(std::time::Duration::from_secs(15), async {
            for i in 1..=ITERATIONS {
                let task_id = format!("stress-local-{i}");
                let events = Arc::new(StdMutex::new(Vec::<TaskEvent>::new()));
                let sink_events = Arc::clone(&events);
                let sink: WorkerEventSink = Arc::new(move |ev| {
                    sink_events.lock().unwrap().push(ev);
                });
                let params = StartExtractionParams {
                    video_path: format!("race:{i}"),
                    mode: ExtractionMode::LocalOcr,
                    engine: Some(OcrEngineId::Paddle),
                    provider_id: None,
                    region_box: None,
                    config: ExtractionConfigDto::default(),
                    script: SubtitleScript::Auto,
                };

                let st_start = Arc::clone(&state);
                let tid_start = task_id.clone();
                let start_handle = tokio::spawn(async move {
                    start_extraction_impl(&st_start, tid_start, params, sink).await
                });

                let st_cancel = Arc::clone(&state);
                let tid_cancel = task_id.clone();
                let cancel_handle = tokio::spawn(async move {
                    while !st_cancel
                        .active_tasks
                        .lock()
                        .unwrap()
                        .contains_key(&tid_cancel)
                    {
                        tokio::task::yield_now().await;
                    }
                    cancel_extraction_impl(&st_cancel, &tid_cancel).await
                });

                cancel_handle.await.unwrap().unwrap();
                let _ = unblock_tx.send(i);
                let start_res = start_handle.await.unwrap();
                assert!(
                    matches!(&start_res, Err(msg) if msg.starts_with("[Aborted]")),
                    "local iter {i}: expected [Aborted], got: {start_res:?}"
                );
                let evs = events.lock().unwrap();
                assert_eq!(
                    evs.iter()
                        .filter(|e| matches!(e, TaskEvent::Cancelled { task_id: id } if id == &task_id))
                        .count(),
                    1,
                    "local iter {i}: expected exactly 1 TaskEvent::Cancelled"
                );
                assert!(!evs
                    .iter()
                    .any(|e| matches!(e, TaskEvent::Completed { .. })));
            }

            // 2. VLM pre-entry & in-flight race across AppState.active_tasks (200 iters)
            for j in 1..=ITERATIONS {
                let seq = ITERATIONS + j;
                let task_id = format!("stress-vlm-{j}");
                let events = Arc::new(StdMutex::new(Vec::<TaskEvent>::new()));
                let sink_events = Arc::clone(&events);
                let sink: WorkerEventSink = Arc::new(move |ev| {
                    sink_events.lock().unwrap().push(ev);
                });
                let params = StartExtractionParams {
                    video_path: format!("race:{seq}"),
                    mode: ExtractionMode::PureTimingStrips,
                    engine: None,
                    provider_id: Some("p1".into()),
                    region_box: None,
                    config: ExtractionConfigDto::default(),
                    script: SubtitleScript::Auto,
                };

                let st_start = Arc::clone(&state);
                let tid_start = task_id.clone();
                let start_handle = tokio::spawn(async move {
                    start_extraction_impl(&st_start, tid_start, params, sink).await
                });

                let st_cancel = Arc::clone(&state);
                let tid_cancel = task_id.clone();
                let cancel_handle = tokio::spawn(async move {
                    while !st_cancel
                        .active_tasks
                        .lock()
                        .unwrap()
                        .contains_key(&tid_cancel)
                    {
                        tokio::task::yield_now().await;
                    }
                    cancel_extraction_impl(&st_cancel, &tid_cancel).await
                });

                cancel_handle.await.unwrap().unwrap();
                let _ = unblock_tx.send(seq);
                let start_res = start_handle.await.unwrap();
                assert!(
                    matches!(&start_res, Err(msg) if msg.starts_with("[Aborted]")),
                    "vlm iter {j}: expected [Aborted], got: {start_res:?}"
                );
                let evs = events.lock().unwrap();
                assert_eq!(
                    evs.iter()
                        .filter(|e| matches!(e, TaskEvent::Cancelled { task_id: id } if id == &task_id))
                        .count(),
                    1,
                    "vlm iter {j}: expected exactly 1 TaskEvent::Cancelled"
                );
                assert!(!evs
                    .iter()
                    .any(|e| matches!(e, TaskEvent::Completed { .. })));
            }
        })
        .await
        .expect("multi-thread start/cancel stress test timed out");

        assert!(state.active_tasks.lock().unwrap().is_empty());
        sup.shutdown().await;
        worker_task.await.unwrap();
    }

    #[tokio::test]
    async fn re_recognize_entries_with_native_vlm_maps_categories_and_isolates_partial_failures() {
        use crate::dto::{ApiKeyUpdate, ProviderType, ProviderUpsertDto, ReRecognizeCategory, ThinkingMode};
        use crate::storage::CheckpointStore;
        use crate::vlm::image::{write_ppm_atomic, RgbImage};
        use crate::vlm::NativeVlmService;
        use tokio::io::{AsyncReadExt, AsyncWriteExt};
        use tokio::net::TcpListener;

        struct ReOcrMockWorker {
            seeks: StdMutex<Vec<(i64, Option<SourceBox>)>>,
        }

        #[async_trait]
        impl WorkerBackend for ReOcrMockWorker {
            async fn capabilities(&self) -> Result<CapabilitiesDto, WorkerError> {
                Ok(CapabilitiesDto {
                    engines: vec![OcrEngineId::Paddle],
                    ffmpeg_available: true,
                    hwaccel_modes: vec![],
                })
            }

            async fn probe_video(&self, _video_path: &str) -> Result<VideoInfoDto, WorkerError> {
                Ok(VideoInfoDto {
                    width: 1920,
                    height: 1080,
                    duration_ms: 10_000,
                    fps: 24.0,
                    display_transform_ok: true,
                    transform_note: None,
                })
            }

            async fn extract_frame(
                &self,
                _video_path: &str,
                timestamp_ms: i64,
                crop: Option<SourceBox>,
                _max_width: Option<i32>,
                out_path: &str,
            ) -> Result<ExtractFrameResult, WorkerError> {
                self.seeks.lock().unwrap().push((timestamp_ms, crop));
                // Simulate midpoint failure at 1500ms, succeeding on fallback start_ms=1000ms
                if timestamp_ms == 1500 {
                    return Err(WorkerError::Remote {
                        code: "DecoderFailed".into(),
                        message: "seek miss".into(),
                    });
                }
                let img = RgbImage::new_filled(320, 40, [30, 60, 90]);
                write_ppm_atomic(Path::new(out_path), &img).unwrap();
                Ok(ExtractFrameResult {
                    width: 320,
                    height: 40,
                    out_path: Some(out_path.to_string()),
                })
            }

            async fn detect_auto_region(
                &self,
                _video_path: &str,
                _engine: OcrEngineId,
                _script: SubtitleScript,
                _bottom_ratio: f64,
            ) -> Result<AutoRegionDto, WorkerError> {
                unreachable!()
            }

            async fn run_extraction(
                &self,
                _task_id: &str,
                _params: RunExtractionParams,
                _on_event: WorkerEventSink,
            ) -> Result<ExtractionResultDto, WorkerError> {
                unreachable!()
            }

            async fn recognize_strips(
                &self,
                _engine: OcrEngineId,
                _strip_paths: Vec<String>,
                _script: SubtitleScript,
            ) -> Result<RecognizeStripsResult, WorkerError> {
                unreachable!()
            }

            async fn finalize_srt(
                &self,
                _entries: Vec<SubtitleEntry>,
                _apply_merge: bool,
                _merge_options: Option<MergeOptionsDto>,
            ) -> Result<FinalizeSrtResult, WorkerError> {
                unreachable!()
            }

            async fn cancel_task(&self, _task_id: &str) -> Result<(), WorkerError> {
                Ok(())
            }

            async fn shutdown(&self) {}
        }

        let listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let addr = listener.local_addr().unwrap();
        let srv = tokio::spawn(async move {
            loop {
                let Ok((mut stream, _)) = listener.accept().await else {
                    break;
                };
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
                                let parsed: serde_json::Value = serde_json::from_str(&body).unwrap();
                                let prompt =
                                    parsed["messages"][1]["content"][1]["text"].as_str().unwrap();
                                let resp_json = if prompt.contains("[1, 2, 3]")
                                    || prompt.contains("[2, 3]")
                                    || prompt.contains("[3]")
                                {
                                    // Slot 3 triggers content_filter -> partial failure for cue 30
                                    r#"{"choices":[{"finish_reason":"content_filter","message":{"content":""}}]}"#.to_string()
                                } else if prompt.contains("[1]") {
                                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":1,\"category\":\"dialogue\",\"lines\":[\"哦  对?\"]}]}"}}]}"#.to_string()
                                } else {
                                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":2,\"category\":\"watermark\",\"lines\":[\"TV\"]}]}"}}]}"#.to_string()
                                };
                                let resp = format!(
                                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{resp_json}",
                                    resp_json.len()
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

        let dir = TempDir::new().unwrap();
        let storage = StorageRoot::new(dir.path().join("appdata")).unwrap();
        let providers = Arc::new(ProviderStore::new(storage.clone()));
        providers
            .save_provider(ProviderUpsertDto {
                id: Some("p-reocr".into()),
                name: "Local".into(),
                provider_type: ProviderType::OpenAi,
                base_url: format!("http://127.0.0.1:{}/v1", addr.port()),
                model: "test-vl".into(),
                thinking_mode: ThinkingMode::Default,
                concurrency: 2,
                is_default: true,
                api_key_update: ApiKeyUpdate::Clear,
                custom_headers: Vec::new(),
            })
            .unwrap();
        let worker = Arc::new(ReOcrMockWorker {
            seeks: StdMutex::new(Vec::new()),
        });
        let checkpoints = Arc::new(CheckpointStore::new(storage.clone()));
        let vlm = Arc::new(NativeVlmService::new(
            Arc::clone(&worker) as Arc<dyn WorkerBackend>,
            Arc::clone(&providers),
            checkpoints,
            storage.clone(),
        ));
        let sessions = Arc::new(SessionStore::new(storage.clone()));
        let state = AppState::with_stores(
            Arc::clone(&worker) as Arc<dyn WorkerBackend>,
            vlm,
            storage.clone(),
            providers,
            sessions,
            Arc::new(MediaStreamManager::new()),
        );

        let res = re_recognize_entries_impl(
            &state,
            "/tmp/video.mp4",
            Some(SourceBox {
                x: 0,
                y: 778,
                width: 1920,
                height: 121,
            }),
            vec![
                ReRecognizeItemInput {
                    cue_id: 10,
                    start_ms: 1000,
                    end_ms: 2000, // mid=1500 fails, falls back to 1000
                },
                ReRecognizeItemInput {
                    cue_id: 20,
                    start_ms: 2000,
                    end_ms: 3000, // mid=2500 -> watermark -> Empty("")
                },
                ReRecognizeItemInput {
                    cue_id: 30,
                    start_ms: 3000,
                    end_ms: 4000, // mid=3500 -> content_filter -> ok: false
                },
            ],
            Some("p-reocr".into()),
            SubtitleScript::Auto,
        )
        .await
        .unwrap();

        assert_eq!(res.len(), 3);
        assert!(res[0].ok);
        assert_eq!(res[0].cue_id, 10);
        assert_eq!(res[0].text.as_deref(), Some("哦 对？"));
        assert_eq!(res[0].category, Some(ReRecognizeCategory::Dialogue));

        assert!(res[1].ok);
        assert_eq!(res[1].cue_id, 20);
        assert_eq!(res[1].text.as_deref(), Some(""));
        assert_eq!(res[1].category, Some(ReRecognizeCategory::Empty));

        assert!(!res[2].ok);
        assert_eq!(res[2].cue_id, 30);
        assert!(res[2].error_message.is_some());

        // Verify crop_box forwarded with 10% horizontal trim + 12px vertical pad
        let seeks = worker.seeks.lock().unwrap();
        assert_eq!(seeks[0].0, 1500);
        assert_eq!(seeks[1].0, 1000);
        assert_eq!(
            seeks[0].1,
            Some(SourceBox {
                x: 192,
                y: 766,
                width: 1536,
                height: 145
            })
        );
        assert_eq!(
            std::fs::read_dir(storage.preview_cache_dir()).unwrap().count(),
            0
        );
        srv.abort();
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 4)]
    async fn aba_delayed_cancel_never_aborts_reused_vlm_task_id() {
        struct AbaVlmService {
            task1_entered: Arc<tokio::sync::Notify>,
            task2_entered: Arc<tokio::sync::Notify>,
            task2_release: Arc<tokio::sync::Notify>,
            vlm_cancel_calls: Mutex<Vec<String>>,
        }

        #[async_trait]
        impl VlmService for AbaVlmService {
            async fn run_vlm_extraction(
                &self,
                _task_id: &str,
                _params: StartExtractionParams,
                _on_event: WorkerEventSink,
            ) -> Result<ExtractionResultDto, crate::vlm::VlmError> {
                unreachable!()
            }

            async fn run_vlm_extraction_with_cancel(
                &self,
                _task_id: &str,
                params: StartExtractionParams,
                _on_event: WorkerEventSink,
                mut cancel_rx: watch::Receiver<bool>,
            ) -> Result<ExtractionResultDto, crate::vlm::VlmError> {
                if params.video_path == "task1" {
                    self.task1_entered.notify_one();
                    if !*cancel_rx.borrow() {
                        let _ = cancel_rx.wait_for(|&v| v).await;
                    }
                    return Err(crate::vlm::VlmError::Cancelled);
                }

                self.task2_entered.notify_one();
                tokio::select! {
                    biased;
                    _ = cancel_rx.wait_for(|&v| v) => Err(crate::vlm::VlmError::Cancelled),
                    _ = self.task2_release.notified() => Ok(ExtractionResultDto {
                        entries: vec![SubtitleEntry {
                            id: 1,
                            start_ms: 100,
                            end_ms: 900,
                            text: "task2 ok".into(),
                            confidence: 0.99,
                        }],
                        metrics: ExtractionMetricsDto::default(),
                        failed_slot_count: Some(0),
                    }),
                }
            }

            async fn re_recognize_entries(
                &self,
                _video_path: &str,
                _region_box: Option<SourceBox>,
                _items: Vec<ReRecognizeItemInput>,
                _provider_id: Option<String>,
                _script: SubtitleScript,
            ) -> Result<Vec<ReRecognizeResult>, crate::vlm::VlmError> {
                Ok(vec![])
            }

            async fn test_provider_connection(
                &self,
                _profile: ProviderUpsertDto,
            ) -> Result<ProviderConnectionTestDto, crate::vlm::VlmError> {
                Err(crate::vlm::VlmError::NotImplemented("mock".into()))
            }

            async fn cancel_task(&self, task_id: &str) -> Result<(), crate::vlm::VlmError> {
                self.vlm_cancel_calls.lock().unwrap().push(task_id.to_string());
                Ok(())
            }
        }

        let dir = TempDir::new().unwrap();
        let task1_entered = Arc::new(tokio::sync::Notify::new());
        let task2_entered = Arc::new(tokio::sync::Notify::new());
        let task2_release = Arc::new(tokio::sync::Notify::new());
        let vlm = Arc::new(AbaVlmService {
            task1_entered: Arc::clone(&task1_entered),
            task2_entered: Arc::clone(&task2_entered),
            task2_release: Arc::clone(&task2_release),
            vlm_cancel_calls: Mutex::new(Vec::new()),
        });
        let state = Arc::new(AppState::new(
            Arc::new(MockWorkerBackend::new(false)),
            Arc::clone(&vlm) as Arc<dyn VlmService>,
            StorageRoot::new(dir.path().join("appdata")).unwrap(),
            Arc::new(MediaStreamManager::new()),
        ));

        for iter in 0..50 {
            *state.test_pre_backend_hook.lock().unwrap() = None;
            let params1 = StartExtractionParams {
                video_path: "task1".into(),
                mode: ExtractionMode::PureTimingStrips,
                engine: None,
                provider_id: Some("p1".into()),
                region_box: None,
                config: ExtractionConfigDto::default(),
                script: SubtitleScript::Auto,
            };
            let st1 = Arc::clone(&state);
            let sink1: WorkerEventSink = Arc::new(|_| {});
            let t1 = tokio::spawn(async move {
                start_extraction_impl(&st1, "aba-same".into(), params1, sink1).await
            });

            task1_entered.notified().await;

            // Fire cancel for Task 1, wait for Task 1 to exit, and immediately reuse "aba-same"
            // for Task 2 before/while Task 1's cancel future completes.
            let st_cancel = Arc::clone(&state);
            let cancel_handle =
                tokio::spawn(async move { cancel_extraction_impl(&st_cancel, "aba-same").await });

            let t1_res = t1.await.unwrap();
            assert!(
                matches!(&t1_res, Err(msg) if msg.starts_with("[Aborted]")),
                "iter {iter}: Task 1 must abort, got {t1_res:?}"
            );

            let events2 = Arc::new(StdMutex::new(Vec::<TaskEvent>::new()));
            let ev2 = Arc::clone(&events2);
            let sink2: WorkerEventSink = Arc::new(move |ev| {
                ev2.lock().unwrap().push(ev);
            });
            let params2 = StartExtractionParams {
                video_path: "task2".into(),
                mode: ExtractionMode::PureTimingStrips,
                engine: None,
                provider_id: Some("p1".into()),
                region_box: None,
                config: ExtractionConfigDto::default(),
                script: SubtitleScript::Auto,
            };
            let hook_entered = Arc::new(tokio::sync::Notify::new());
            let hook_release = Arc::new(tokio::sync::Notify::new());
            *state.test_pre_backend_hook.lock().unwrap() =
                Some((Arc::clone(&hook_entered), Arc::clone(&hook_release)));

            let st2 = Arc::clone(&state);
            let t2 = tokio::spawn(async move {
                start_extraction_impl(&st2, "aba-same".into(), params2, sink2).await
            });

            hook_entered.notified().await;
            *state.test_pre_backend_hook.lock().unwrap() = None;
            cancel_handle.await.unwrap().unwrap();
            hook_release.notify_one();

            task2_entered.notified().await;
            task2_release.notify_one();

            let t2_res = t2
                .await
                .unwrap()
                .unwrap_or_else(|e| panic!("iter {iter}: ABA bug killed Task 2: {e}"));
            assert_eq!(t2_res.entries[0].text, "task2 ok");
            assert!(events2
                .lock()
                .unwrap()
                .iter()
                .any(|e| matches!(e, TaskEvent::Completed { task_id, .. } if task_id == "aba-same")));
        }

        assert!(
            vlm.vlm_cancel_calls.lock().unwrap().is_empty(),
            "cancel_extraction_impl must not call vlm.cancel_task post-lock"
        );
        assert!(state.active_tasks.lock().unwrap().is_empty());
    }

    #[tokio::test]
    async fn start_extraction_rejects_empty_and_duplicate_task_id_and_emits_retryable_crash() {
        struct CrashingWorker;

        #[async_trait]
        impl WorkerBackend for CrashingWorker {
            async fn capabilities(&self) -> Result<CapabilitiesDto, WorkerError> {
                unreachable!()
            }
            async fn probe_video(&self, _: &str) -> Result<VideoInfoDto, WorkerError> {
                unreachable!()
            }
            async fn extract_frame(
                &self,
                _: &str,
                _: i64,
                _: Option<SourceBox>,
                _: Option<i32>,
                _: &str,
            ) -> Result<ExtractFrameResult, WorkerError> {
                unreachable!()
            }
            async fn detect_auto_region(
                &self,
                _: &str,
                _: OcrEngineId,
                _: SubtitleScript,
                _: f64,
            ) -> Result<AutoRegionDto, WorkerError> {
                unreachable!()
            }
            async fn run_extraction(
                &self,
                _: &str,
                _: RunExtractionParams,
                _: WorkerEventSink,
            ) -> Result<ExtractionResultDto, WorkerError> {
                Err(WorkerError::Crashed {
                    status: Some(139),
                    stderr_tail: vec!["segfault".into()],
                    retryable: true,
                })
            }
            async fn recognize_strips(
                &self,
                _: OcrEngineId,
                _: Vec<String>,
                _: SubtitleScript,
            ) -> Result<RecognizeStripsResult, WorkerError> {
                unreachable!()
            }
            async fn finalize_srt(
                &self,
                _: Vec<SubtitleEntry>,
                _: bool,
                _: Option<MergeOptionsDto>,
            ) -> Result<FinalizeSrtResult, WorkerError> {
                unreachable!()
            }
            async fn cancel_task(&self, _: &str) -> Result<(), WorkerError> {
                Ok(())
            }
            async fn shutdown(&self) {}
        }

        let dir = TempDir::new().unwrap();
        let state = Arc::new(AppState::new(
            Arc::new(CrashingWorker),
            Arc::new(StubVlmService),
            StorageRoot::new(dir.path().join("appdata")).unwrap(),
            Arc::new(MediaStreamManager::new()),
        ));

        let noop_sink: WorkerEventSink = Arc::new(|_| {});
        let valid_params = StartExtractionParams {
            video_path: "/tmp/a.mp4".into(),
            mode: ExtractionMode::LocalOcr,
            engine: Some(OcrEngineId::Paddle),
            provider_id: None,
            region_box: None,
            config: ExtractionConfigDto::default(),
            script: SubtitleScript::Auto,
        };

        // 1. Empty / whitespace task_id -> InvalidConfiguration
        for bad_id in ["", "   \t\n"] {
            let err = start_extraction_impl(
                &state,
                bad_id.to_string(),
                valid_params.clone(),
                Arc::clone(&noop_sink),
            )
            .await
            .unwrap_err();
            assert!(err.starts_with("[InvalidConfiguration]"), "got {err}");
        }

        // 2. LocalOcr missing engine -> InvalidConfiguration
        let mut missing_engine = valid_params.clone();
        missing_engine.engine = None;
        let err_eng = start_extraction_impl(&state, "t-no-eng".into(), missing_engine, Arc::clone(&noop_sink))
            .await
            .unwrap_err();
        assert!(err_eng.starts_with("[InvalidConfiguration]"), "got {err_eng}");

        // 3. cancel_extraction on unknown task_id returns Ok(())
        cancel_extraction_impl(&state, "never-existed").await.unwrap();

        // 4. Duplicate running task_id -> TaskConflict, and crashing worker emits Failed{retryable:true}
        let entered = Arc::new(tokio::sync::Notify::new());
        let release = Arc::new(tokio::sync::Notify::new());
        *state.test_pre_backend_hook.lock().unwrap() =
            Some((Arc::clone(&entered), Arc::clone(&release)));

        let events = Arc::new(StdMutex::new(Vec::<TaskEvent>::new()));
        let ev_ref = Arc::clone(&events);
        let crash_sink: WorkerEventSink = Arc::new(move |ev| {
            ev_ref.lock().unwrap().push(ev);
        });
        let st1 = Arc::clone(&state);
        let p1 = valid_params.clone();
        let h1 = tokio::spawn(async move {
            start_extraction_impl(&st1, "dup-task".into(), p1, crash_sink).await
        });

        entered.notified().await;
        *state.test_pre_backend_hook.lock().unwrap() = None;

        let dup_err = start_extraction_impl(
            &state,
            "dup-task".into(),
            valid_params,
            noop_sink,
        )
        .await
        .unwrap_err();
        assert!(dup_err.starts_with("[TaskConflict]"), "got {dup_err}");

        release.notify_one();
        let crash_err = h1.await.unwrap().unwrap_err();
        assert!(crash_err.starts_with("[WorkerCrashed]"), "got {crash_err}");
        let recorded = events.lock().unwrap();
        assert!(recorded.iter().any(|e| matches!(
            e,
            TaskEvent::Failed {
                task_id,
                code,
                retryable: true,
                ..
            } if task_id == "dup-task" && code == "WorkerCrashed"
        )));
    }

    #[tokio::test]
    async fn export_rejects_symlinks_missing_dirs_and_isolates_batch_failures() {
        let dir = TempDir::new().unwrap();
        let worker = MockWorkerBackend::new(false);
        let sample_entries = vec![SubtitleEntry {
            id: 1,
            start_ms: 0,
            end_ms: 1000,
            text: "Hi".into(),
            confidence: 0.9,
        }];

        // 1. Empty out_path -> [InvalidPath]
        let empty_err = export_srt_impl(&worker, "   ", sample_entries.clone(), false, None)
            .await
            .unwrap_err();
        assert!(empty_err.starts_with("[InvalidPath]"), "got {empty_err}");

        // 2. Missing parent directory -> [InvalidPath]
        let missing_dir_out = dir.path().join("no_such_dir").join("out.srt");
        let missing_err = export_srt_impl(
            &worker,
            &missing_dir_out.to_string_lossy(),
            sample_entries.clone(),
            false,
            None,
        )
        .await
        .unwrap_err();
        assert!(missing_err.starts_with("[InvalidPath]"), "got {missing_err}");

        // 3. Symlink target & Rename candidate symlink rejection + batch failure isolation
        #[cfg(unix)]
        {
            let real_target = dir.path().join("real.srt");
            std::fs::write(&real_target, b"real").unwrap();
            let sym_target = dir.path().join("sym.srt");
            std::os::unix::fs::symlink(&real_target, &sym_target).unwrap();

            let sym_err = export_srt_impl(
                &worker,
                &sym_target.to_string_lossy(),
                sample_entries.clone(),
                false,
                None,
            )
            .await
            .unwrap_err();
            assert!(sym_err.starts_with("[SymlinkRejected]"), "got {sym_err}");

            let ep01 = dir.path().join("ep01.srt");
            std::fs::write(&ep01, b"existing").unwrap();
            let ep01_rename_sym = dir.path().join("ep01 (1).srt");
            std::os::unix::fs::symlink(&real_target, &ep01_rename_sym).unwrap();
            let ep02 = dir.path().join("ep02.srt");

            let report = batch_export_srt_impl(
                &worker,
                vec![
                    BatchExportItemInput {
                        video_path: "/v/ep01.mp4".into(),
                        out_path: ep01.to_string_lossy().to_string(),
                        entries: sample_entries.clone(),
                    },
                    BatchExportItemInput {
                        video_path: "/v/ep02.mp4".into(),
                        out_path: ep02.to_string_lossy().to_string(),
                        entries: sample_entries.clone(),
                    },
                ],
                BatchExportConflictPolicy::Rename,
            )
            .await
            .unwrap();

            assert_eq!(report.failed_count, 1);
            assert_eq!(report.exported_count, 1);
            assert_eq!(report.results[0].status, BatchExportItemStatus::Failed);
            assert!(report.results[0]
                .error_message
                .as_deref()
                .unwrap()
                .starts_with("[SymlinkRejected]"));
            assert_eq!(report.results[1].status, BatchExportItemStatus::Exported);
            assert!(ep02.is_file());
        }
    }
}
