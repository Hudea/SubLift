use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex as StdMutex};
use std::time::Instant;

use async_trait::async_trait;
use serde_json::json;
use tokio::sync::{mpsc, watch, OnceCell, Semaphore};

use crate::dto::{
    ExtractionMode, ExtractionResultDto, ExtractionSubPhase, OcrEngineId,
    ProviderConnectionTestDto, ProviderUpsertDto, ReRecognizeCategory, ReRecognizeItemInput,
    ReRecognizeResult, SourceBox, StartExtractionParams, SubtitleScript, TaskEvent,
};
use crate::storage::{ensure_private_dir, CheckpointStore, ProviderStore, StorageError, StorageRoot};
use crate::worker::protocol::RunExtractionParams;
use crate::worker::{WorkerBackend, WorkerEventSink};

use super::assembler::{
    assemble_cues_from_slots, assembled_cues_to_subtitle_entries, DEFAULT_MAX_MERGE_GAP_MS,
};
use super::checkpoint::{
    compute_vlm_checkpoint_key, finalize_checkpoint_files, load_cached_by_sheet,
    write_partial_checkpoint, write_sheet_raw_response, FailedSlotRecord, FinalResponsesDoc,
    PartialCheckpointDoc, SheetBatchRecord,
};
use super::client::{
    normalize_chat_completions_url, transcribe_sheet_with_split_rescue,
    transcribe_sheet_with_tail_hedging, validate_endpoint_and_key_with_headers, HedgeCoordinator,
    LeafFallbackResolver, SplitRescueContext, VlmCallError, VlmEndpointConfig, VlmSessionContext,
};
use super::image::{
    fit_strip_with_black_pad, read_ppm_and_unlink, write_ppm_atomic, RgbImage, TempFileUnlink,
};
use super::rate_limit::{AdaptivePolicyState, RateLimitGate};
use super::schema::{
    validate_and_align_sheet_slots, TranscribedSlot, PLACEHOLDER_CATEGORY, PLACEHOLDER_TEXT,
};
use super::sheet_builder::{
    apply_full_width_horizontal_trim, build_prepared_sheet, compute_strip_crop_geometry,
    compute_target_strip_height, resolve_default_bottom_region, PreparedSheet, SheetSpec,
    TimingSlot, DEFAULT_MAX_STRIP_HEIGHT, DEFAULT_MIN_STRIP_HEIGHT, DEFAULT_SLOTS_PER_SHEET,
    DEFAULT_STRIP_WIDTH,
};
use super::{wait_until_cancelled, VlmError, VlmService};

static FALLBACK_PPM_SEQ: AtomicU64 = AtomicU64::new(1);
static VLM_TASK_GEN_SEQ: AtomicU64 = AtomicU64::new(1);

const RE_OCR_FAILED_CATEGORY: &str = "__re_ocr_failed__";

struct VlmActiveEntry {
    generation: u64,
    cancel_tx: watch::Sender<bool>,
}

pub struct NativeVlmService {
    worker: Arc<dyn WorkerBackend>,
    providers: Arc<ProviderStore>,
    checkpoints: Arc<CheckpointStore>,
    storage: StorageRoot,
    http_client: reqwest::Client,
    active_tasks: Arc<StdMutex<HashMap<String, VlmActiveEntry>>>,
    slots_per_sheet: usize,
    tail_hedge_min_s: f64,
    tail_hedge_factor: f64,
    reuse_cache: bool,
    max_retries: u32,
}

impl NativeVlmService {
    pub fn new(
        worker: Arc<dyn WorkerBackend>,
        providers: Arc<ProviderStore>,
        checkpoints: Arc<CheckpointStore>,
        storage: StorageRoot,
    ) -> Self {
        let http_client = reqwest::Client::builder()
            .build()
            .unwrap_or_else(|_| reqwest::Client::new());
        Self {
            worker,
            providers,
            checkpoints,
            storage,
            http_client,
            active_tasks: Arc::new(StdMutex::new(HashMap::new())),
            slots_per_sheet: DEFAULT_SLOTS_PER_SHEET,
            tail_hedge_min_s: super::client::DEFAULT_TAIL_HEDGE_MIN_S,
            tail_hedge_factor: super::client::DEFAULT_TAIL_HEDGE_FACTOR,
            reuse_cache: true,
            max_retries: 4,
        }
    }

    #[cfg(test)]
    pub(crate) fn with_test_tuning(
        mut self,
        slots_per_sheet: usize,
        tail_hedge_min_s: f64,
        tail_hedge_factor: f64,
        reuse_cache: bool,
        max_retries: u32,
    ) -> Self {
        self.slots_per_sheet = slots_per_sheet.max(1);
        self.tail_hedge_min_s = tail_hedge_min_s;
        self.tail_hedge_factor = tail_hedge_factor;
        self.reuse_cache = reuse_cache;
        self.max_retries = max_retries.max(1);
        self
    }

    fn resolve_endpoint_config(
        &self,
        provider_id: Option<&str>,
    ) -> Result<(VlmEndpointConfig, usize), VlmError> {
        let profile = self
            .providers
            .resolve_provider_secret(provider_id)
            .map_err(|e| match e {
                StorageError::NotFound(msg) | StorageError::InvalidInput(msg) => {
                    VlmError::InvalidConfiguration(msg)
                }
                other => VlmError::Failed {
                    code: "StorageError".into(),
                    message: other.to_string(),
                    retryable: false,
                },
            })?;

        let endpoint = normalize_chat_completions_url(&profile.base_url)?;
        validate_endpoint_and_key_with_headers(
            &endpoint,
            &profile.api_key,
            &profile.custom_headers,
        )?;
        let concurrency = (profile.concurrency as usize).clamp(1, 16);
        let session_id = uuid::Uuid::new_v4().to_string();

        Ok((
            VlmEndpointConfig {
                endpoint,
                api_key: profile.api_key,
                model: profile.model,
                thinking_mode: profile.thinking_mode,
                timeout_s: 120.0,
                max_retries: self.max_retries,
                custom_headers: profile.custom_headers,
                session_id,
            },
            concurrency,
        ))
    }
}

struct ActiveVlmTaskGuard {
    registry: Arc<StdMutex<HashMap<String, VlmActiveEntry>>>,
    task_id: String,
    generation: u64,
}

impl Drop for ActiveVlmTaskGuard {
    fn drop(&mut self) {
        if let Ok(mut map) = self.registry.lock() {
            if map.get(&self.task_id).map(|e| e.generation) == Some(self.generation) {
                map.remove(&self.task_id);
            }
        }
    }
}

struct WorkerLeafFallbackResolver {
    worker: Arc<dyn WorkerBackend>,
    temp_dir: PathBuf,
    script: SubtitleScript,
    preferred_engine: OnceCell<Option<OcrEngineId>>,
}

impl WorkerLeafFallbackResolver {
    fn new(worker: Arc<dyn WorkerBackend>, temp_dir: PathBuf, script: SubtitleScript) -> Self {
        Self {
            worker,
            temp_dir,
            script,
            preferred_engine: OnceCell::new(),
        }
    }

    async fn resolve_local_engine(&self) -> Option<OcrEngineId> {
        *self
            .preferred_engine
            .get_or_init(|| async {
                let Ok(caps) = self.worker.capabilities().await else {
                    return None;
                };
                if caps.engines.contains(&OcrEngineId::Paddle) {
                    Some(OcrEngineId::Paddle)
                } else if caps.engines.contains(&OcrEngineId::AppleVision) {
                    Some(OcrEngineId::AppleVision)
                } else {
                    None
                }
            })
            .await
    }
}

#[async_trait]
impl LeafFallbackResolver for WorkerLeafFallbackResolver {
    async fn resolve_failed_slot(
        &self,
        slot: &TimingSlot,
        strip_img: Option<&RgbImage>,
        reason: &str,
    ) -> Result<(TranscribedSlot, FailedSlotRecord), VlmError> {
        let Some(engine) = self.resolve_local_engine().await else {
            return Ok((
                TranscribedSlot {
                    slot_id: slot.slot_id,
                    category: PLACEHOLDER_CATEGORY.into(),
                    lines: vec![PLACEHOLDER_TEXT.into()],
                },
                FailedSlotRecord {
                    slot_id: slot.slot_id,
                    start_ms: slot.start_ms,
                    end_ms: slot.end_ms,
                    reason: reason.to_string(),
                    resolution: "placeholder".into(),
                    fallback_text: PLACEHOLDER_TEXT.into(),
                },
            ));
        };

        let Some(img) = strip_img else {
            return Ok((
                TranscribedSlot {
                    slot_id: slot.slot_id,
                    category: PLACEHOLDER_CATEGORY.into(),
                    lines: vec![PLACEHOLDER_TEXT.into()],
                },
                FailedSlotRecord {
                    slot_id: slot.slot_id,
                    start_ms: slot.start_ms,
                    end_ms: slot.end_ms,
                    reason: reason.to_string(),
                    resolution: "placeholder".into(),
                    fallback_text: PLACEHOLDER_TEXT.into(),
                },
            ));
        };

        let seq = FALLBACK_PPM_SEQ.fetch_add(1, Ordering::Relaxed);
        let ppm_path = self
            .temp_dir
            .join(format!("leaf_{:04}_{seq}.ppm", slot.slot_id));
        let _unlink = TempFileUnlink::new(ppm_path.clone());

        if write_ppm_atomic(&ppm_path, img).is_err() {
            return Ok((
                TranscribedSlot {
                    slot_id: slot.slot_id,
                    category: PLACEHOLDER_CATEGORY.into(),
                    lines: vec![PLACEHOLDER_TEXT.into()],
                },
                FailedSlotRecord {
                    slot_id: slot.slot_id,
                    start_ms: slot.start_ms,
                    end_ms: slot.end_ms,
                    reason: reason.to_string(),
                    resolution: "placeholder_after_local_error".into(),
                    fallback_text: PLACEHOLDER_TEXT.into(),
                },
            ));
        }

        let path_str = ppm_path.to_string_lossy().to_string();
        let engine_tag = match engine {
            OcrEngineId::Paddle => "paddle",
            OcrEngineId::AppleVision => "apple_vision",
        };
        match self
            .worker
            .recognize_strips(engine, vec![path_str], self.script)
            .await
        {
            Ok(res) => {
                if let Some(item) = res.items.into_iter().next() {
                    if item.ok {
                        let lines: Vec<String> = item
                            .text
                            .lines()
                            .map(str::trim)
                            .filter(|s| !s.is_empty())
                            .map(ToOwned::to_owned)
                            .collect();
                        if !lines.is_empty() {
                            let joined = lines.join("\n");
                            return Ok((
                                TranscribedSlot {
                                    slot_id: slot.slot_id,
                                    category: "dialogue".into(),
                                    lines,
                                },
                                FailedSlotRecord {
                                    slot_id: slot.slot_id,
                                    start_ms: slot.start_ms,
                                    end_ms: slot.end_ms,
                                    reason: reason.to_string(),
                                    resolution: engine_tag.into(),
                                    fallback_text: joined,
                                },
                            ));
                        }
                    }
                }
                Ok((
                    TranscribedSlot {
                        slot_id: slot.slot_id,
                        category: PLACEHOLDER_CATEGORY.into(),
                        lines: vec![PLACEHOLDER_TEXT.into()],
                    },
                    FailedSlotRecord {
                        slot_id: slot.slot_id,
                        start_ms: slot.start_ms,
                        end_ms: slot.end_ms,
                        reason: reason.to_string(),
                        resolution: "placeholder_after_local_empty".into(),
                        fallback_text: PLACEHOLDER_TEXT.into(),
                    },
                ))
            }
            Err(_) => Ok((
                TranscribedSlot {
                    slot_id: slot.slot_id,
                    category: PLACEHOLDER_CATEGORY.into(),
                    lines: vec![PLACEHOLDER_TEXT.into()],
                },
                FailedSlotRecord {
                    slot_id: slot.slot_id,
                    start_ms: slot.start_ms,
                    end_ms: slot.end_ms,
                    reason: reason.to_string(),
                    resolution: "placeholder_after_local_error".into(),
                    fallback_text: PLACEHOLDER_TEXT.into(),
                },
            )),
        }
    }
}

struct ReRecognizeFailureRecorder;

#[async_trait]
impl LeafFallbackResolver for ReRecognizeFailureRecorder {
    async fn resolve_failed_slot(
        &self,
        slot: &TimingSlot,
        _strip_img: Option<&RgbImage>,
        reason: &str,
    ) -> Result<(TranscribedSlot, FailedSlotRecord), VlmError> {
        Ok((
            TranscribedSlot {
                slot_id: slot.slot_id,
                category: RE_OCR_FAILED_CATEGORY.into(),
                lines: vec![reason.to_string()],
            },
            FailedSlotRecord {
                slot_id: slot.slot_id,
                start_ms: slot.start_ms,
                end_ms: slot.end_ms,
                reason: reason.to_string(),
                resolution: "re_ocr_failed".into(),
                fallback_text: String::new(),
            },
        ))
    }
}

struct SharedSheetState {
    results_by_idx: BTreeMap<u32, SheetBatchRecord>,
    specs_by_idx: BTreeMap<u32, SheetSpec>,
    completed_sheets: u32,
    retried_splits: u32,
    total_dispatched: u32,
    first_err: Option<VlmError>,
}

async fn extract_fallback_strip(
    worker: &dyn WorkerBackend,
    video_path: &str,
    slot: &TimingSlot,
    crop_box: SourceBox,
    temp_dir: &Path,
) -> Result<RgbImage, String> {
    let candidates = [
        slot.mid_ms.max(0),
        slot.start_ms.max(0),
        (slot.start_ms - 100).max(0),
    ];
    let mut last_err = "no frame extracted".to_string();
    for seek_ms in candidates {
        let seq = FALLBACK_PPM_SEQ.fetch_add(1, Ordering::Relaxed);
        let fallback_path = temp_dir.join(format!("fb_{:04}_{seq}.ppm", slot.slot_id));
        let path_str = fallback_path.to_string_lossy().to_string();
        match worker
            .extract_frame(
                video_path,
                seek_ms,
                Some(crop_box),
                Some(DEFAULT_STRIP_WIDTH as i32),
                &path_str,
            )
            .await
        {
            Ok(_) => match read_ppm_and_unlink(&fallback_path) {
                Ok(img) => return Ok(img),
                Err(e) => last_err = e,
            },
            Err(e) => {
                let _ = std::fs::remove_file(&fallback_path);
                last_err = e.to_string();
            }
        }
    }
    Err(last_err)
}

struct SheetDecodeConfig {
    worker: Arc<dyn WorkerBackend>,
    video_path: String,
    crop_box: SourceBox,
    strip_h: u32,
    is_full_width_band: bool,
    temp_dir: PathBuf,
}

async fn decode_and_prepare_sheet(
    cfg: SheetDecodeConfig,
    sheet_index: u32,
    batch_items: Vec<(TimingSlot, PathBuf)>,
) -> Result<PreparedSheet, VlmError> {
    let SheetDecodeConfig {
        worker,
        video_path,
        crop_box,
        strip_h,
        is_full_width_band,
        temp_dir,
    } = cfg;
    let t0 = Instant::now();
    let mut strip_images: HashMap<u64, Arc<RgbImage>> = HashMap::with_capacity(batch_items.len());
    let mut batch_slots = Vec::with_capacity(batch_items.len());

    for (slot, ppm_path) in batch_items {
        batch_slots.push(slot.clone());
        let decoded = tokio::task::spawn_blocking(move || read_ppm_and_unlink(&ppm_path))
            .await
            .map_err(|e| VlmError::Failed {
                code: "VlmDecodePanic".into(),
                message: e.to_string(),
                retryable: false,
            })?;

        let raw_img = match decoded {
            Ok(img) => {
                if is_full_width_band {
                    apply_full_width_horizontal_trim(img, 0.10).map_err(|e| VlmError::Failed {
                        code: "VlmImageError".into(),
                        message: e,
                        retryable: false,
                    })?
                } else {
                    img
                }
            }
            Err(_) => {
                extract_fallback_strip(&*worker, &video_path, &slot, crop_box, &temp_dir)
                    .await
                    .map_err(|e| VlmError::Failed {
                        code: "DecoderFailed".into(),
                        message: e,
                        retryable: false,
                    })?
            }
        };

        let fitted = tokio::task::spawn_blocking(move || {
            fit_strip_with_black_pad(&raw_img, DEFAULT_STRIP_WIDTH, strip_h)
        })
        .await
        .map_err(|e| VlmError::Failed {
            code: "VlmResizePanic".into(),
            message: e.to_string(),
            retryable: false,
        })?;
        strip_images.insert(slot.slot_id, Arc::new(fitted));
    }

    let decode_s = t0.elapsed().as_secs_f64();
    tokio::task::spawn_blocking(move || {
        build_prepared_sheet(
            sheet_index,
            batch_slots,
            strip_images,
            DEFAULT_STRIP_WIDTH,
            strip_h,
            decode_s,
        )
    })
    .await
    .map_err(|e| VlmError::Failed {
        code: "VlmEncodePanic".into(),
        message: e.to_string(),
        retryable: false,
    })?
    .map_err(|e| VlmError::Failed {
        code: "VlmSheetEncodeError".into(),
        message: e,
        retryable: false,
    })
}

fn thinking_mode_str(mode: crate::dto::ThinkingMode) -> &'static str {
    match mode {
        crate::dto::ThinkingMode::Auto => "auto",
        crate::dto::ThinkingMode::None => "none",
        crate::dto::ThinkingMode::Low => "low",
        crate::dto::ThinkingMode::Minimal => "minimal",
        crate::dto::ThinkingMode::Default => "default",
    }
}

#[async_trait]
impl VlmService for NativeVlmService {
    async fn run_vlm_extraction(
        &self,
        task_id: &str,
        params: StartExtractionParams,
        on_event: WorkerEventSink,
    ) -> Result<ExtractionResultDto, VlmError> {
        let (_tx, rx) = watch::channel(false);
        self.run_vlm_extraction_with_cancel(task_id, params, on_event, rx)
            .await
    }

    async fn run_vlm_extraction_with_cancel(
        &self,
        task_id: &str,
        params: StartExtractionParams,
        on_event: WorkerEventSink,
        mut caller_cancel_rx: watch::Receiver<bool>,
    ) -> Result<ExtractionResultDto, VlmError> {
        // Register synchronous cancel handle immediately before any .await
        let (service_cancel_tx, mut service_cancel_rx) = watch::channel(false);
        let (combined_cancel_tx, mut combined_cancel_rx) = watch::channel(false);
        let generation = VLM_TASK_GEN_SEQ.fetch_add(1, Ordering::Relaxed);
        {
            let mut map = self
                .active_tasks
                .lock()
                .expect("vlm active_tasks lock poisoned");
            map.insert(
                task_id.to_string(),
                VlmActiveEntry {
                    generation,
                    cancel_tx: service_cancel_tx,
                },
            );
        }
        let _active_guard = ActiveVlmTaskGuard {
            registry: Arc::clone(&self.active_tasks),
            task_id: task_id.to_string(),
            generation,
        };

        if *caller_cancel_rx.borrow() || *service_cancel_rx.borrow() {
            return Err(VlmError::Cancelled);
        }

        let worker_sub_task_id = format!("{task_id}:strips");
        let cancel_worker = Arc::clone(&self.worker);
        let cancel_sub_id = worker_sub_task_id.clone();
        let cancel_tx_fwd = combined_cancel_tx.clone();
        let mut cancel_rx_internal = combined_cancel_rx.clone();
        let cancel_bridge_handle = tokio::spawn(async move {
            tokio::select! {
                _ = wait_until_cancelled(&mut caller_cancel_rx) => {
                    let _ = cancel_tx_fwd.send(true);
                }
                _ = wait_until_cancelled(&mut service_cancel_rx) => {
                    let _ = cancel_tx_fwd.send(true);
                }
                _ = wait_until_cancelled(&mut cancel_rx_internal) => {}
            }
            let _ = cancel_worker.cancel_task(&cancel_sub_id).await;
        });

        struct CancelBridgeAbort(Option<tokio::task::JoinHandle<()>>);
        impl Drop for CancelBridgeAbort {
            fn drop(&mut self) {
                if let Some(h) = self.0.take() {
                    h.abort();
                }
            }
        }
        let mut cancel_bridge_guard = CancelBridgeAbort(Some(cancel_bridge_handle));

        let (vlm_cfg, eff_vlm_workers) = self.resolve_endpoint_config(params.provider_id.as_deref())?;

        on_event(TaskEvent::Phase {
            task_id: task_id.to_string(),
            sub_phase: ExtractionSubPhase::Probing,
        });

        let video_info = tokio::select! {
            biased;
            _ = wait_until_cancelled(&mut combined_cancel_rx) => return Err(VlmError::Cancelled),
            res = self.worker.probe_video(&params.video_path) => {
                res.map_err(|e| VlmError::Failed {
                    code: e.code().to_string(),
                    message: e.to_string(),
                    retryable: e.is_retryable(),
                })?
            }
        };

        if video_info.width <= 0 || video_info.height <= 0 {
            return Err(VlmError::InvalidConfiguration(format!(
                "invalid video dimensions {}x{}",
                video_info.width, video_info.height
            )));
        }
        if !video_info.display_transform_ok {
            return Err(VlmError::InvalidConfiguration(
                video_info
                    .transform_note
                    .unwrap_or_else(|| "unsupported video display transform".into()),
            ));
        }

        let resolved_region = params.region_box.unwrap_or_else(|| {
            resolve_default_bottom_region(
                video_info.width,
                video_info.height,
                params.config.region_bottom_ratio,
            )
        });
        let crop_box = compute_strip_crop_geometry(
            video_info.width,
            video_info.height,
            resolved_region,
            0.10,
            12,
        )
        .map_err(VlmError::InvalidConfiguration)?;
        let strip_h = compute_target_strip_height(
            crop_box,
            DEFAULT_STRIP_WIDTH,
            DEFAULT_MIN_STRIP_HEIGHT,
            DEFAULT_MAX_STRIP_HEIGHT,
        );
        let is_full_width_band =
            resolved_region.x == 0 && resolved_region.width >= video_info.width;

        let checkpoint_key = compute_vlm_checkpoint_key(
            Path::new(&params.video_path),
            Some(resolved_region),
            params.config.sample_fps,
            params.script,
            &vlm_cfg.model,
        );
        let cached_by_sheet = if self.reuse_cache {
            load_cached_by_sheet(
                &self.checkpoints,
                &self.storage,
                &checkpoint_key,
                &vlm_cfg.model,
            )
        } else {
            HashMap::new()
        };

        let preview_dir = self.storage.preview_cache_dir();
        ensure_private_dir(&preview_dir).map_err(|e| VlmError::Failed {
            code: "StorageError".into(),
            message: e.to_string(),
            retryable: false,
        })?;
        let strips_temp_dir = tempfile::Builder::new()
            .prefix(&format!("vlm_strips_{}_", std::process::id()))
            .tempdir_in(&preview_dir)
            .map_err(|e| VlmError::Failed {
                code: "StorageError".into(),
                message: format!("failed to create strips temp dir: {e}"),
                retryable: false,
            })?;

        let strips_dir_str = strips_temp_dir.path().to_string_lossy().to_string();
        let mut worker_cfg = params.config.clone();
        worker_cfg.subtitle_script = params.script;
        let worker_params = RunExtractionParams {
            video_path: params.video_path.clone(),
            mode: ExtractionMode::PureTimingStrips,
            engine: None,
            region_box: params.region_box,
            config: worker_cfg,
            strips_dir: Some(strips_dir_str),
        };

        let (slot_tx, mut slot_rx) = mpsc::unbounded_channel::<(TimingSlot, PathBuf)>();
        let slot_seq = Arc::new(AtomicU64::new(0));
        let outer_task_id = task_id.to_string();
        let outer_sink = Arc::clone(&on_event);
        let wrapped_sink: WorkerEventSink = Arc::new(move |ev| match ev {
            TaskEvent::TimingSlot {
                start_ms,
                end_ms,
                strip_path,
                ..
            } if end_ms > start_ms => {
                let sid = slot_seq.fetch_add(1, Ordering::Relaxed) + 1;
                let _ = slot_tx.send((
                    TimingSlot::new(sid, start_ms, end_ms),
                    PathBuf::from(strip_path),
                ));
            }
            TaskEvent::Phase { sub_phase, .. } => {
                outer_sink(TaskEvent::Phase {
                    task_id: outer_task_id.clone(),
                    sub_phase,
                });
            }
            TaskEvent::Progress {
                current_ms,
                total_ms,
                fps,
                ..
            } => {
                outer_sink(TaskEvent::Progress {
                    task_id: outer_task_id.clone(),
                    current_ms,
                    total_ms,
                    fps,
                });
            }
            _ => {}
        });

        let worker_for_stage1 = Arc::clone(&self.worker);
        let worker_cancel_rx = combined_cancel_rx.clone();
        let stage1_sub_id = worker_sub_task_id.clone();
        let stage1_handle = tokio::spawn(async move {
            worker_for_stage1
                .run_extraction_with_cancel(
                    &stage1_sub_id,
                    worker_params,
                    wrapped_sink,
                    worker_cancel_rx,
                )
                .await
        });

        let eff_extract_workers = std::thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(4)
            .clamp(2, 8);
        let eff_max_in_flight = (eff_vlm_workers * 2).max(eff_extract_workers + 4);
        let in_flight_sem = Arc::new(Semaphore::new(eff_max_in_flight));
        let vlm_sem = Arc::new(Semaphore::new(eff_vlm_workers));
        let rate_gate = Arc::new(RateLimitGate::new());
        let policy = Arc::new(AdaptivePolicyState::new(
            vlm_cfg.thinking_mode,
            &vlm_cfg.model,
            &vlm_cfg.endpoint,
        ));
        let hedge_coord = Arc::new(HedgeCoordinator::new(
            eff_vlm_workers,
            self.tail_hedge_min_s,
            self.tail_hedge_factor,
        ));
        let fallback_resolver: Arc<dyn LeafFallbackResolver> = Arc::new(
            WorkerLeafFallbackResolver::new(
                Arc::clone(&self.worker),
                strips_temp_dir.path().to_path_buf(),
                params.script,
            ),
        );
        let shared = Arc::new(StdMutex::new(SharedSheetState {
            results_by_idx: BTreeMap::new(),
            specs_by_idx: BTreeMap::new(),
            completed_sheets: 0,
            retried_splits: 0,
            total_dispatched: 0,
            first_err: None,
        }));
        let checkpoint_io_lock = Arc::new(tokio::sync::Mutex::new(()));
        let vlm_phase_emitted = Arc::new(AtomicBool::new(false));
        let pipeline_t0 = Instant::now();
        let vlm_started_at: Arc<StdMutex<Option<Instant>>> = Arc::new(StdMutex::new(None));

        let mut all_slots: Vec<TimingSlot> = Vec::new();
        let mut batch_buf: Vec<(TimingSlot, PathBuf)> = Vec::with_capacity(self.slots_per_sheet);
        let mut sheet_handles: Vec<tokio::task::JoinHandle<()>> = Vec::new();
        let mut sheet_idx: u32 = 0;
        let task_cancel_template = combined_cancel_rx.clone();

        let dispatch_batch = |sheet_idx: u32,
                              batch_items: Vec<(TimingSlot, PathBuf)>,
                              sheet_handles: &mut Vec<tokio::task::JoinHandle<()>>| {
            let expected_ids: Vec<u64> = batch_items.iter().map(|(s, _)| s.slot_id).collect();
            let sheet_name = format!("sheet_{sheet_idx:02}.png");
            let spec = SheetSpec {
                sheet_index: sheet_idx,
                sheet_name: sheet_name.clone(),
                sheet_path: format!("memory://{sheet_name}"),
                expected_slot_ids: expected_ids.clone(),
            };

            {
                let mut g = shared.lock().expect("shared state lock poisoned");
                g.total_dispatched = g.total_dispatched.max(sheet_idx);
            }
            {
                let mut hg = hedge_coord.state.lock().expect("hedge lock poisoned");
                hg.total_dispatched = hg.total_dispatched.max(sheet_idx as usize);
            }

            if let Some(cached) = cached_by_sheet.get(&sheet_name) {
                if cached.expected_slot_ids == expected_ids
                    && validate_and_align_sheet_slots(
                        &json!({ "slots": cached.slots }),
                        &expected_ids,
                        params.script,
                        false,
                    )
                    .is_ok()
                {
                    for (_, ppm_path) in batch_items {
                        let _ = std::fs::remove_file(ppm_path);
                    }
                    hedge_coord.record_cached_hit();
                    let (done, total_known, splits) = {
                        let mut g = shared.lock().expect("shared state lock poisoned");
                        g.results_by_idx.insert(sheet_idx, cached.clone());
                        g.specs_by_idx.insert(sheet_idx, spec);
                        g.completed_sheets += 1;
                        (g.completed_sheets, g.total_dispatched, g.retried_splits)
                    };
                    on_event(TaskEvent::VlmSheetProgress {
                        task_id: task_id.to_string(),
                        completed_sheets: done,
                        total_sheets: total_known,
                        retried_splits: splits,
                    });
                    return;
                }
            }

            let in_flight_sem = Arc::clone(&in_flight_sem);
            let vlm_sem = Arc::clone(&vlm_sem);
            let worker = Arc::clone(&self.worker);
            let video_path = params.video_path.clone();
            let temp_dir = strips_temp_dir.path().to_path_buf();
            let http = self.http_client.clone();
            let cfg = vlm_cfg.clone();
            let policy = Arc::clone(&policy);
            let rate_gate = Arc::clone(&rate_gate);
            let script = params.script;
            let fallback_resolver = Arc::clone(&fallback_resolver);
            let hedge_coord = Arc::clone(&hedge_coord);
            let shared = Arc::clone(&shared);
            let checkpoints = Arc::clone(&self.checkpoints);
            let storage = self.storage.clone();
            let checkpoint_key = checkpoint_key.clone();
            let checkpoint_io_lock = Arc::clone(&checkpoint_io_lock);
            let mut task_cancel_rx = task_cancel_template.clone();
            let cancel_tx_abort = combined_cancel_tx.clone();
            let on_event = Arc::clone(&on_event);
            let task_id_str = task_id.to_string();
            let vlm_phase_emitted = Arc::clone(&vlm_phase_emitted);
            let vlm_started_at = Arc::clone(&vlm_started_at);

            let handle = tokio::spawn(async move {
                let permit = tokio::select! {
                    biased;
                    _ = wait_until_cancelled(&mut task_cancel_rx) => return,
                    p = in_flight_sem.acquire_owned() => match p {
                        Ok(p) => p,
                        Err(_) => return,
                    }
                };

                let prepared = match decode_and_prepare_sheet(
                    SheetDecodeConfig {
                        worker,
                        video_path,
                        crop_box,
                        strip_h,
                        is_full_width_band,
                        temp_dir,
                    },
                    sheet_idx,
                    batch_items,
                )
                .await
                {
                    Ok(p) => p,
                    Err(e) => {
                        let mut g = shared.lock().expect("shared state lock poisoned");
                        if g.first_err.is_none() {
                            g.first_err = Some(e);
                        }
                        let _ = cancel_tx_abort.send(true);
                        drop(permit);
                        return;
                    }
                };

                let _vlm_permit = tokio::select! {
                    biased;
                    _ = wait_until_cancelled(&mut task_cancel_rx) => {
                        drop(permit);
                        return;
                    }
                    p = vlm_sem.acquire_owned() => match p {
                        Ok(p) => p,
                        Err(_) => {
                            drop(permit);
                            return;
                        }
                    }
                };

                if !vlm_phase_emitted.swap(true, Ordering::AcqRel) {
                    *vlm_started_at.lock().expect("vlm_started_at lock") = Some(Instant::now());
                    on_event(TaskEvent::Phase {
                        task_id: task_id_str.clone(),
                        sub_phase: ExtractionSubPhase::VlmSheets,
                    });
                }

                let outcome = transcribe_sheet_with_tail_hedging(
                    prepared,
                    VlmSessionContext {
                        http,
                        config: cfg.clone(),
                        policy,
                        rate_gate,
                        script,
                    },
                    Some(fallback_resolver),
                    true,
                    hedge_coord,
                    task_cancel_rx.clone(),
                )
                .await;

                drop(_vlm_permit);
                drop(permit);

                match outcome {
                    Ok(out) => {
                        let (ordered, total_known, done, splits) = {
                            let mut g = shared.lock().expect("shared state lock poisoned");
                            g.results_by_idx.insert(sheet_idx, out.batch.clone());
                            g.specs_by_idx.insert(sheet_idx, spec);
                            g.completed_sheets += 1;
                            g.retried_splits = g.retried_splits.saturating_add(out.split_count);
                            let ordered: Vec<SheetBatchRecord> =
                                g.results_by_idx.values().cloned().collect();
                            (
                                ordered,
                                g.total_dispatched.max(g.completed_sheets),
                                g.completed_sheets,
                                g.retried_splits,
                            )
                        };

                        {
                            let _io_guard = checkpoint_io_lock.lock().await;
                            let _ = write_sheet_raw_response(
                                &checkpoints,
                                &storage,
                                &checkpoint_key,
                                &out.batch,
                                &cfg.model,
                            );
                            let partial_doc = PartialCheckpointDoc {
                                model: cfg.model.clone(),
                                endpoint: cfg.endpoint.clone(),
                                thinking_mode: thinking_mode_str(cfg.thinking_mode).to_string(),
                                completed_sheets: ordered.len(),
                                total_sheets: total_known as usize,
                                streaming_timing_complete: false,
                                batches: ordered,
                            };
                            let _ = write_partial_checkpoint(&checkpoints, &checkpoint_key, &partial_doc);
                        }

                        on_event(TaskEvent::VlmSheetProgress {
                            task_id: task_id_str,
                            completed_sheets: done,
                            total_sheets: total_known,
                            retried_splits: splits,
                        });
                    }
                    Err(VlmCallError::Cancelled) => {}
                    Err(err) => {
                        let mut g = shared.lock().expect("shared state lock poisoned");
                        if g.first_err.is_none() {
                            g.first_err = Some(err.into_vlm_error());
                        }
                        let _ = cancel_tx_abort.send(true);
                    }
                }
            });
            sheet_handles.push(handle);
        };

        let mut loop_cancelled = false;
        loop {
            let next_item = tokio::select! {
                biased;
                _ = wait_until_cancelled(&mut combined_cancel_rx) => {
                    loop_cancelled = true;
                    break;
                }
                item = slot_rx.recv() => item,
            };
            let Some((slot, ppm_path)) = next_item else {
                break;
            };
            all_slots.push(slot.clone());
            batch_buf.push((slot, ppm_path));
            if batch_buf.len() >= self.slots_per_sheet {
                sheet_idx += 1;
                let batch = std::mem::take(&mut batch_buf);
                dispatch_batch(sheet_idx, batch, &mut sheet_handles);
            }
        }

        let stage1_res = if loop_cancelled {
            for (_, ppm_path) in batch_buf.drain(..) {
                let _ = std::fs::remove_file(ppm_path);
            }
            stage1_handle.await
        } else {
            let s1 = stage1_handle.await;
            let stage1_ok = matches!(s1, Ok(Ok(_)));
            if stage1_ok && !batch_buf.is_empty() && !*combined_cancel_rx.borrow() {
                sheet_idx += 1;
                let batch = std::mem::take(&mut batch_buf);
                dispatch_batch(sheet_idx, batch, &mut sheet_handles);
            } else {
                if !stage1_ok {
                    let _ = combined_cancel_tx.send(true);
                }
                for (_, ppm_path) in batch_buf.drain(..) {
                    let _ = std::fs::remove_file(ppm_path);
                }
            }
            s1
        };

        {
            let mut hg = hedge_coord.state.lock().expect("hedge lock poisoned");
            hg.all_batches_dispatched = true;
            hg.total_dispatched = sheet_idx as usize;
        }

        for h in sheet_handles {
            let _ = h.await;
        }

        if *combined_cancel_rx.borrow() {
            if let Some(h) = cancel_bridge_guard.0.take() {
                let _ = h.await;
            }
        }

        if let Some(err) = shared.lock().expect("shared state lock poisoned").first_err.take() {
            return Err(err);
        }

        let stage1_metrics = match stage1_res {
            Ok(Ok(res)) => {
                if *combined_cancel_rx.borrow() {
                    return Err(VlmError::Cancelled);
                }
                res.metrics
            }
            Ok(Err(crate::worker::WorkerError::Aborted(_))) => return Err(VlmError::Cancelled),
            Ok(Err(e)) => {
                return Err(VlmError::Failed {
                    code: e.code().to_string(),
                    message: e.to_string(),
                    retryable: e.is_retryable(),
                });
            }
            Err(join_err) => {
                return Err(VlmError::Failed {
                    code: "WorkerPanic".into(),
                    message: join_err.to_string(),
                    retryable: true,
                });
            }
        };

        on_event(TaskEvent::Phase {
            task_id: task_id.to_string(),
            sub_phase: ExtractionSubPhase::Finalizing,
        });

        let (ordered_batches, ordered_specs) = {
            let g = shared.lock().expect("shared state lock poisoned");
            let batches: Vec<SheetBatchRecord> = g.results_by_idx.values().cloned().collect();
            let specs: Vec<SheetSpec> = g.specs_by_idx.values().cloned().collect();
            (batches, specs)
        };

        let mut all_transcribed: Vec<TranscribedSlot> = Vec::with_capacity(all_slots.len());
        for (batch, spec) in ordered_batches.iter().zip(ordered_specs.iter()) {
            let payload = json!({ "slots": batch.slots });
            let (validated, _) = validate_and_align_sheet_slots(
                &payload,
                &spec.expected_slot_ids,
                params.script,
                true,
            )
            .map_err(|e| VlmError::Failed {
                code: "VlmSchemaError".into(),
                message: e,
                retryable: false,
            })?;
            all_transcribed.extend(validated);
        }

        let assembly = assemble_cues_from_slots(
            &all_slots,
            &all_transcribed,
            true,
            DEFAULT_MAX_MERGE_GAP_MS,
        )
        .map_err(|e| VlmError::Failed {
            code: "VlmAssemblyError".into(),
            message: e,
            retryable: false,
        })?;
        let entries = assembled_cues_to_subtitle_entries(&assembly.cues);

        let vlm_wall_s = vlm_started_at
            .lock()
            .expect("vlm_started_at lock")
            .map(|t| t.elapsed().as_secs_f64())
            .unwrap_or(0.0);
        let sum_s: f64 = ordered_batches.iter().map(|b| b.elapsed_s).sum();
        let final_doc = FinalResponsesDoc {
            model: vlm_cfg.model.clone(),
            endpoint: vlm_cfg.endpoint.clone(),
            thinking_mode: thinking_mode_str(vlm_cfg.thinking_mode).to_string(),
            wall_s: (vlm_wall_s * 1000.0).round() / 1000.0,
            sum_s: (sum_s * 1000.0).round() / 1000.0,
            batches: ordered_batches.clone(),
        };
        let failed_slots = finalize_checkpoint_files(
            &self.checkpoints,
            &self.storage,
            &checkpoint_key,
            &final_doc,
            &ordered_batches,
        )
        .map_err(|e| VlmError::Failed {
            code: "StorageError".into(),
            message: e,
            retryable: false,
        })?;

        for entry in &entries {
            on_event(TaskEvent::SubtitleEntry {
                task_id: task_id.to_string(),
                entry: entry.clone(),
            });
        }

        let wall_time_ms = pipeline_t0.elapsed().as_millis() as i64;
        let mut metrics = stage1_metrics;
        metrics.total_segments_detected = all_slots.len() as u64;
        metrics.total_entries_produced = entries.len() as u64;
        metrics.wall_time_ms = wall_time_ms.max(metrics.wall_time_ms);

        Ok(ExtractionResultDto {
            entries,
            metrics,
            failed_slot_count: Some(failed_slots.len() as u64),
        })
    }

    async fn re_recognize_entries(
        &self,
        video_path: &str,
        region_box: Option<SourceBox>,
        items: Vec<ReRecognizeItemInput>,
        provider_id: Option<String>,
        script: SubtitleScript,
    ) -> Result<Vec<ReRecognizeResult>, VlmError> {
        if items.is_empty() {
            return Ok(Vec::new());
        }

        let (vlm_cfg, eff_vlm_workers) = self.resolve_endpoint_config(provider_id.as_deref())?;
        let video_info = self
            .worker
            .probe_video(video_path)
            .await
            .map_err(|e| VlmError::Failed {
                code: e.code().to_string(),
                message: e.to_string(),
                retryable: e.is_retryable(),
            })?;
        if video_info.width <= 0 || video_info.height <= 0 {
            return Err(VlmError::InvalidConfiguration(format!(
                "invalid video dimensions {}x{}",
                video_info.width, video_info.height
            )));
        }

        let resolved_region = region_box.unwrap_or_else(|| {
            resolve_default_bottom_region(video_info.width, video_info.height, 0.30)
        });
        let crop_box = compute_strip_crop_geometry(
            video_info.width,
            video_info.height,
            resolved_region,
            0.10,
            12,
        )
        .map_err(VlmError::InvalidConfiguration)?;
        let strip_h = compute_target_strip_height(
            crop_box,
            DEFAULT_STRIP_WIDTH,
            DEFAULT_MIN_STRIP_HEIGHT,
            DEFAULT_MAX_STRIP_HEIGHT,
        );

        let preview_dir = self.storage.preview_cache_dir();
        ensure_private_dir(&preview_dir).map_err(|e| VlmError::Failed {
            code: "StorageError".into(),
            message: e.to_string(),
            retryable: false,
        })?;
        let tmp_dir = tempfile::Builder::new()
            .prefix(&format!("vlm_reocr_{}_", std::process::id()))
            .tempdir_in(&preview_dir)
            .map_err(|e| VlmError::Failed {
                code: "StorageError".into(),
                message: format!("failed to create re-ocr temp dir: {e}"),
                retryable: false,
            })?;

        let mut slot_to_cue: HashMap<u64, u64> = HashMap::with_capacity(items.len());
        let mut results_by_slot: HashMap<u64, ReRecognizeResult> = HashMap::with_capacity(items.len());
        let mut extracted_slots: Vec<TimingSlot> = Vec::with_capacity(items.len());
        let mut extracted_strips: HashMap<u64, Arc<RgbImage>> = HashMap::with_capacity(items.len());

        for (idx, item) in items.iter().enumerate() {
            let slot_id = (idx as u64) + 1;
            slot_to_cue.insert(slot_id, item.cue_id);
            let start_ms = item.start_ms.max(0);
            let end_ms = item.end_ms.max(start_ms + 1);
            let slot = TimingSlot::new(slot_id, start_ms, end_ms);

            match extract_fallback_strip(
                &*self.worker,
                video_path,
                &slot,
                crop_box,
                tmp_dir.path(),
            )
            .await
            {
                Ok(img) => {
                    let fitted = tokio::task::spawn_blocking(move || {
                        fit_strip_with_black_pad(&img, DEFAULT_STRIP_WIDTH, strip_h)
                    })
                    .await
                    .map_err(|e| VlmError::Failed {
                        code: "VlmResizePanic".into(),
                        message: e.to_string(),
                        retryable: false,
                    })?;
                    extracted_strips.insert(slot_id, Arc::new(fitted));
                    extracted_slots.push(slot);
                }
                Err(err) => {
                    results_by_slot.insert(
                        slot_id,
                        ReRecognizeResult {
                            cue_id: item.cue_id,
                            ok: false,
                            text: None,
                            category: None,
                            confidence: None,
                            error_message: Some(err),
                        },
                    );
                }
            }
        }

        if !extracted_slots.is_empty() {
            let policy = Arc::new(AdaptivePolicyState::new(
                vlm_cfg.thinking_mode,
                &vlm_cfg.model,
                &vlm_cfg.endpoint,
            ));
            let rate_gate = Arc::new(RateLimitGate::new());
            let slots_by_id: Arc<HashMap<u64, TimingSlot>> = Arc::new(
                extracted_slots
                    .iter()
                    .map(|s| (s.slot_id, s.clone()))
                    .collect(),
            );
            let strip_images_arc = Arc::new(extracted_strips);
            let failure_recorder: Arc<dyn LeafFallbackResolver> =
                Arc::new(ReRecognizeFailureRecorder);
            let vlm_sem = Arc::new(Semaphore::new(eff_vlm_workers));
            let (chunk_cancel_tx, chunk_cancel_rx) = watch::channel(false);

            let mut handles = Vec::new();
            for (chunk_idx, chunk) in extracted_slots.chunks(self.slots_per_sheet).enumerate() {
                let sheet_index = (chunk_idx as u32) + 1;
                let batch_slots = chunk.to_vec();
                let mut batch_strips = HashMap::with_capacity(batch_slots.len());
                for s in &batch_slots {
                    batch_strips.insert(s.slot_id, Arc::clone(&strip_images_arc[&s.slot_id]));
                }
                let prepared = build_prepared_sheet(
                    sheet_index,
                    batch_slots,
                    batch_strips,
                    DEFAULT_STRIP_WIDTH,
                    strip_h,
                    0.0,
                )
                .map_err(|e| VlmError::Failed {
                    code: "VlmSheetEncodeError".into(),
                    message: e,
                    retryable: false,
                })?;

                let ctx = SplitRescueContext {
                    session: VlmSessionContext {
                        http: self.http_client.clone(),
                        config: vlm_cfg.clone(),
                        policy: Arc::clone(&policy),
                        rate_gate: Arc::clone(&rate_gate),
                        script,
                    },
                    slots_by_id: Arc::clone(&slots_by_id),
                    strip_images_by_id: Arc::clone(&strip_images_arc),
                    strip_w: DEFAULT_STRIP_WIDTH,
                    strip_h,
                    fallback_resolver: Some(Arc::clone(&failure_recorder)),
                    allow_placeholder_leaf: false,
                };
                let sem = Arc::clone(&vlm_sem);
                let mut cancel_rx = chunk_cancel_rx.clone();
                handles.push(tokio::spawn(async move {
                    let _permit = sem.acquire_owned().await.map_err(|e| {
                        VlmError::Failed {
                            code: "VlmSemaphoreClosed".into(),
                            message: e.to_string(),
                            retryable: false,
                        }
                    })?;
                    transcribe_sheet_with_split_rescue(
                        &ctx,
                        prepared.spec,
                        prepared.png_bytes,
                        0,
                        &mut cancel_rx,
                    )
                    .await
                    .map_err(VlmCallError::into_vlm_error)
                }));
            }

            let mut handles_iter = handles.into_iter();
            while let Some(h) = handles_iter.next() {
                let outcome = match h.await {
                    Ok(Ok(o)) => o,
                    Ok(Err(e)) => {
                        let _ = chunk_cancel_tx.send(true);
                        for remaining in handles_iter {
                            remaining.abort();
                        }
                        return Err(e);
                    }
                    Err(e) => {
                        let _ = chunk_cancel_tx.send(true);
                        for remaining in handles_iter {
                            remaining.abort();
                        }
                        return Err(VlmError::Failed {
                            code: "VlmTaskPanic".into(),
                            message: e.to_string(),
                            retryable: false,
                        });
                    }
                };
                for slot_out in outcome.batch.slots {
                    let Some(&cue_id) = slot_to_cue.get(&slot_out.id) else {
                        continue;
                    };
                    if slot_out.category == RE_OCR_FAILED_CATEGORY {
                        let msg = slot_out
                            .lines
                            .into_iter()
                            .next()
                            .unwrap_or_else(|| "VLM re-recognition failed".into());
                        results_by_slot.insert(
                            slot_out.id,
                            ReRecognizeResult {
                                cue_id,
                                ok: false,
                                text: None,
                                category: None,
                                confidence: None,
                                error_message: Some(msg),
                            },
                        );
                        continue;
                    }

                    let joined = slot_out.lines.join("\n");
                    let (category, text) = match slot_out.category.as_str() {
                        "dialogue" if !joined.is_empty() => {
                            (ReRecognizeCategory::Dialogue, joined)
                        }
                        "bracket_note" if !joined.is_empty() => {
                            (ReRecognizeCategory::BracketNote, joined)
                        }
                        "placeholder" if !joined.is_empty() => {
                            (ReRecognizeCategory::Placeholder, joined)
                        }
                        _ => (ReRecognizeCategory::Empty, String::new()),
                    };
                    results_by_slot.insert(
                        slot_out.id,
                        ReRecognizeResult {
                            cue_id,
                            ok: true,
                            text: Some(text),
                            category: Some(category),
                            confidence: Some(0.95),
                            error_message: None,
                        },
                    );
                }
            }
        }

        let ordered: Vec<ReRecognizeResult> = items
            .into_iter()
            .enumerate()
            .map(|(idx, item)| {
                let slot_id = (idx as u64) + 1;
                results_by_slot.remove(&slot_id).unwrap_or(ReRecognizeResult {
                    cue_id: item.cue_id,
                    ok: false,
                    text: None,
                    category: None,
                    confidence: None,
                    error_message: Some("missing re-recognition output".into()),
                })
            })
            .collect();
        Ok(ordered)
    }

    async fn test_provider_connection(
        &self,
        profile: ProviderUpsertDto,
    ) -> Result<ProviderConnectionTestDto, VlmError> {
        let resolved = self
            .providers
            .resolve_transient_provider(&profile)
            .map_err(|e| match e {
                StorageError::NotFound(msg) | StorageError::InvalidInput(msg) => {
                    VlmError::InvalidConfiguration(msg)
                }
                other => VlmError::Failed {
                    code: "StorageError".into(),
                    message: other.to_string(),
                    retryable: false,
                },
            })?;

        let endpoint = normalize_chat_completions_url(&resolved.base_url)?;
        validate_endpoint_and_key_with_headers(
            &endpoint,
            &resolved.api_key,
            &resolved.custom_headers,
        )?;
        let cfg = VlmEndpointConfig {
            endpoint,
            api_key: resolved.api_key,
            model: resolved.model,
            thinking_mode: resolved.thinking_mode,
            timeout_s: 15.0,
            max_retries: 1,
            custom_headers: resolved.custom_headers,
            session_id: uuid::Uuid::new_v4().to_string(),
        };
        super::client::probe_vlm_connectivity(&self.http_client, &cfg).await
    }

    async fn cancel_task(&self, task_id: &str) -> Result<(), VlmError> {
        let sender = {
            let map = self
                .active_tasks
                .lock()
                .expect("vlm active_tasks lock poisoned");
            map.get(task_id).map(|e| e.cancel_tx.clone())
        };
        if let Some(tx) = sender {
            let _ = tx.send(true);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dto::{
        ApiKeyUpdate, AutoRegionDto, CapabilitiesDto, ExtractionConfigDto, ExtractionMetricsDto,
        MergeOptionsDto, ProviderType, ProviderUpsertDto, SubtitleEntry, ThinkingMode, VideoInfoDto,
    };
    use crate::vlm::image::RgbImage;
    use crate::worker::protocol::{
        ExtractFrameResult, FinalizeSrtResult, RecognizeStripItemResult, RecognizeStripsResult,
    };
    use crate::worker::WorkerError;
    use std::sync::atomic::AtomicUsize;
    use tempfile::TempDir;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::TcpListener;

    struct PipelineMockWorker {
        strips_to_emit: Vec<(i64, i64, bool)>,
        recognize_text: Option<String>,
        cancelled_ids: StdMutex<Vec<String>>,
        block_until_cancel: bool,
        extract_frame_calls: AtomicUsize,
    }

    impl PipelineMockWorker {
        fn new(strips_to_emit: Vec<(i64, i64, bool)>, recognize_text: Option<&str>) -> Self {
            Self {
                strips_to_emit,
                recognize_text: recognize_text.map(ToOwned::to_owned),
                cancelled_ids: StdMutex::new(Vec::new()),
                block_until_cancel: false,
                extract_frame_calls: AtomicUsize::new(0),
            }
        }
    }

    #[async_trait]
    impl WorkerBackend for PipelineMockWorker {
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
            _timestamp_ms: i64,
            _crop: Option<SourceBox>,
            _max_width: Option<i32>,
            out_path: &str,
        ) -> Result<ExtractFrameResult, WorkerError> {
            self.extract_frame_calls.fetch_add(1, Ordering::SeqCst);
            let img = RgbImage::new_filled(320, 40, [40, 80, 120]);
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
            task_id: &str,
            params: RunExtractionParams,
            on_event: WorkerEventSink,
        ) -> Result<ExtractionResultDto, WorkerError> {
            let (_tx, rx) = watch::channel(false);
            self.run_extraction_with_cancel(task_id, params, on_event, rx)
                .await
        }

        async fn run_extraction_with_cancel(
            &self,
            task_id: &str,
            params: RunExtractionParams,
            on_event: WorkerEventSink,
            mut cancel_rx: watch::Receiver<bool>,
        ) -> Result<ExtractionResultDto, WorkerError> {
            let strips_dir = PathBuf::from(params.strips_dir.as_deref().unwrap());
            for (idx, (start_ms, end_ms, write_valid_ppm)) in
                self.strips_to_emit.iter().enumerate()
            {
                if *cancel_rx.borrow() {
                    return Err(WorkerError::Aborted("cancelled".into()));
                }
                let p = strips_dir.join(format!("strip_{start_ms}_{end_ms}.ppm"));
                if *write_valid_ppm {
                    let img = RgbImage::new_filled(1920, 120, [10, 20, 30]);
                    write_ppm_atomic(&p, &img).unwrap();
                }
                on_event(TaskEvent::TimingSlot {
                    task_id: task_id.to_string(),
                    index: (idx + 1) as u64,
                    start_ms: *start_ms,
                    end_ms: *end_ms,
                    strip_path: p.to_string_lossy().to_string(),
                });
            }
            if self.block_until_cancel {
                let _ = cancel_rx.wait_for(|&v| v).await;
                return Err(WorkerError::Aborted("cancelled".into()));
            }
            Ok(ExtractionResultDto {
                entries: vec![],
                metrics: ExtractionMetricsDto {
                    total_frames_processed: 50,
                    ..Default::default()
                },
                failed_slot_count: None,
            })
        }

        async fn recognize_strips(
            &self,
            _engine: OcrEngineId,
            strip_paths: Vec<String>,
            _script: SubtitleScript,
        ) -> Result<RecognizeStripsResult, WorkerError> {
            let items = strip_paths
                .into_iter()
                .map(|sp| {
                    assert!(Path::new(&sp).is_file(), "leaf PPM must exist during recognize_strips");
                    match &self.recognize_text {
                        Some(txt) => RecognizeStripItemResult {
                            strip_path: sp,
                            ok: true,
                            text: txt.clone(),
                            confidence: 0.9,
                            category: None,
                            error_message: None,
                        },
                        None => RecognizeStripItemResult {
                            strip_path: sp,
                            ok: true,
                            text: String::new(),
                            confidence: 0.0,
                            category: None,
                            error_message: None,
                        },
                    }
                })
                .collect();
            Ok(RecognizeStripsResult { items })
        }

        async fn finalize_srt(
            &self,
            _entries: Vec<SubtitleEntry>,
            _apply_merge: bool,
            _merge_options: Option<MergeOptionsDto>,
        ) -> Result<FinalizeSrtResult, WorkerError> {
            unreachable!()
        }

        async fn cancel_task(&self, task_id: &str) -> Result<(), WorkerError> {
            self.cancelled_ids.lock().unwrap().push(task_id.to_string());
            Ok(())
        }

        async fn shutdown(&self) {}
    }

    async fn spawn_pipeline_vlm_server<F>(handler: F) -> (String, tokio::task::JoinHandle<()>)
    where
        F: Fn(usize, String) -> (u16, String) + Send + Sync + 'static,
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
                                let idx = seq.fetch_add(1, Ordering::SeqCst);
                                let (status, resp_body) = handler(idx, body);
                                let resp = format!(
                                    "HTTP/1.1 {status} OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{resp_body}",
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

    fn setup_service(
        dir: &TempDir,
        worker: Arc<dyn WorkerBackend>,
        endpoint_url: &str,
        slots_per_sheet: usize,
    ) -> (NativeVlmService, StorageRoot, PathBuf) {
        let storage = StorageRoot::new(dir.path().join("appdata")).unwrap();
        let providers = Arc::new(ProviderStore::new(storage.clone()));
        providers
            .save_provider(ProviderUpsertDto {
                id: Some("test-p".into()),
                name: "Test".into(),
                provider_type: ProviderType::OpenAi,
                base_url: endpoint_url.to_string(),
                model: "test-vl".into(),
                thinking_mode: ThinkingMode::Default,
                concurrency: 2,
                is_default: true,
                api_key_update: ApiKeyUpdate::Clear,
                custom_headers: Vec::new(),
            })
            .unwrap();
        let checkpoints = Arc::new(CheckpointStore::new(storage.clone()));
        let video_dir = dir.path().join("videos");
        std::fs::create_dir_all(&video_dir).unwrap();
        let video_path = video_dir.join("clip.mp4");
        std::fs::write(&video_path, b"fake-video-bytes").unwrap();

        let svc = NativeVlmService::new(worker, providers, checkpoints, storage.clone())
            .with_test_tuning(slots_per_sheet, 12.0, 3.5, true, 1);
        (svc, storage, video_path)
    }

    #[tokio::test]
    async fn streaming_pipeline_assembles_cues_cleans_temp_files_and_resumes_from_checkpoint() {
        let dir = TempDir::new().unwrap();
        let calls = Arc::new(AtomicUsize::new(0));
        let calls_srv = Arc::clone(&calls);
        let (base_url, srv) = spawn_pipeline_vlm_server(move |_, body| {
            calls_srv.fetch_add(1, Ordering::SeqCst);
            let v: serde_json::Value = serde_json::from_str(&body).unwrap();
            let prompt = v["messages"][1]["content"][1]["text"].as_str().unwrap();
            if prompt.contains("[1, 2]") {
                // Slot 2 has missing PPM in test below, verifying extract_frame fallback too!
                (
                    200,
                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":1,\"category\":\"dialogue\",\"lines\":[\"哦  对?!\"]},{\"id\":2,\"category\":\"empty\",\"lines\":[]}]}"}}]}"#.into(),
                )
            } else {
                // Sheet 2 ([3, 4]): simulate content filter on slot 3 so leaf fallback triggers!
                if prompt.contains("[3, 4]") || prompt.contains("[3]") {
                    return (
                        200,
                        r#"{"choices":[{"finish_reason":"content_filter","message":{"content":""}}]}"#.into(),
                    );
                }
                (
                    200,
                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":4,\"category\":\"dialogue\",\"lines\":[\"End line\"]}]}"}}]}"#.into(),
                )
            }
        })
        .await;

        let worker = Arc::new(PipelineMockWorker::new(
            vec![
                (0, 1000, true),
                (1000, 1500, false), // missing PPM -> extract_frame fallback
                (1500, 2500, true),
                (2500, 3500, true),
            ],
            Some("Local Leaf Text"),
        ));
        let (svc, storage, video_path) = setup_service(&dir, worker.clone(), &base_url, 2);

        let params = StartExtractionParams {
            video_path: video_path.to_string_lossy().to_string(),
            mode: ExtractionMode::PureTimingStrips,
            engine: None,
            provider_id: Some("test-p".into()),
            region_box: None,
            config: ExtractionConfigDto::default(),
            script: SubtitleScript::Auto,
        };
        let events = Arc::new(StdMutex::new(Vec::<TaskEvent>::new()));
        let ev_sink = Arc::clone(&events);
        let sink: WorkerEventSink = Arc::new(move |ev| {
            ev_sink.lock().unwrap().push(ev);
        });

        let res = svc
            .run_vlm_extraction("t-stream-1", params.clone(), Arc::clone(&sink))
            .await
            .unwrap();

        assert_eq!(res.entries.len(), 3);
        assert_eq!(res.entries[0].text, "哦 对？！");
        assert_eq!(res.entries[1].text, "Local Leaf Text");
        assert_eq!(res.entries[2].text, "End line");
        assert_eq!(res.failed_slot_count, Some(1));
        assert!(worker.extract_frame_calls.load(Ordering::SeqCst) >= 1);

        // Zero temp files left in preview_cache_dir or video dir
        assert_eq!(
            std::fs::read_dir(storage.preview_cache_dir()).unwrap().count(),
            0
        );
        assert_eq!(
            std::fs::read_dir(video_path.parent().unwrap()).unwrap().count(),
            1
        );

        // Second run: sheet_01 hits cache (not degraded!), sheet_02 (degraded) is retried!
        let calls_before = calls.load(Ordering::SeqCst);
        let res2 = svc
            .run_vlm_extraction("t-stream-2", params, sink)
            .await
            .unwrap();
        assert_eq!(res2.entries.len(), 3);
        // Only sheet_02 split calls ran; sheet_01 was NOT requested again
        assert_eq!(calls.load(Ordering::SeqCst) - calls_before, 3);
        // Normal completion must never spuriously call worker.cancel_task
        assert!(worker.cancelled_ids.lock().unwrap().is_empty());
        srv.abort();
    }

    #[tokio::test]
    async fn zero_timing_slots_completes_cleanly_without_crashing() {
        let dir = TempDir::new().unwrap();
        let worker = Arc::new(PipelineMockWorker::new(vec![], None));
        let (svc, storage, video_path) =
            setup_service(&dir, worker, "http://127.0.0.1:1/v1", 10);

        let params = StartExtractionParams {
            video_path: video_path.to_string_lossy().to_string(),
            mode: ExtractionMode::PureTimingStrips,
            engine: None,
            provider_id: Some("test-p".into()),
            region_box: None,
            config: ExtractionConfigDto::default(),
            script: SubtitleScript::Auto,
        };
        let sink: WorkerEventSink = Arc::new(|_| {});
        let res = svc.run_vlm_extraction("t-zero", params, sink).await.unwrap();
        assert!(res.entries.is_empty());
        assert_eq!(res.failed_slot_count, Some(0));
        assert_eq!(
            std::fs::read_dir(storage.preview_cache_dir()).unwrap().count(),
            0
        );
    }

    #[tokio::test]
    async fn deterministic_cancel_propagates_to_worker_and_cleans_tempdir() {
        let dir = TempDir::new().unwrap();
        let mut w = PipelineMockWorker::new(vec![(0, 1000, true)], None);
        w.block_until_cancel = true;
        let worker = Arc::new(w);
        let (svc, storage, video_path) =
            setup_service(&dir, worker.clone(), "http://127.0.0.1:1/v1", 10);

        let svc = Arc::new(svc);
        let svc_run = Arc::clone(&svc);
        let params = StartExtractionParams {
            video_path: video_path.to_string_lossy().to_string(),
            mode: ExtractionMode::PureTimingStrips,
            engine: None,
            provider_id: Some("test-p".into()),
            region_box: None,
            config: ExtractionConfigDto::default(),
            script: SubtitleScript::Auto,
        };
        let (cancel_tx, cancel_rx) = watch::channel(false);
        let slot_seen = Arc::new(tokio::sync::Notify::new());
        let slot_seen_sink = Arc::clone(&slot_seen);
        let sink: WorkerEventSink = Arc::new(move |_| {
            slot_seen_sink.notify_one();
        });

        let handle = tokio::spawn(async move {
            svc_run
                .run_vlm_extraction_with_cancel("t-cancel", params, sink, cancel_rx)
                .await
        });

        slot_seen.notified().await;
        svc.cancel_task("t-cancel").await.unwrap();
        let _ = cancel_tx.send(true);
        let err = handle.await.unwrap().unwrap_err();
        assert!(matches!(err, VlmError::Cancelled));
        assert!(worker
            .cancelled_ids
            .lock()
            .unwrap()
            .iter()
            .any(|id| id == "t-cancel:strips"));
        assert_eq!(
            std::fs::read_dir(storage.preview_cache_dir()).unwrap().count(),
            0
        );
    }

    #[tokio::test]
    async fn fatal_401_cancels_blocking_stage1_worker_and_returns_vlm_fatal() {
        struct Stage1CancelOnlyWorker {
            cancelled_ids: StdMutex<Vec<String>>,
            cancel_notify: tokio::sync::Notify,
        }

        #[async_trait]
        impl WorkerBackend for Stage1CancelOnlyWorker {
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
                _timestamp_ms: i64,
                _crop: Option<SourceBox>,
                _max_width: Option<i32>,
                _out_path: &str,
            ) -> Result<ExtractFrameResult, WorkerError> {
                unreachable!()
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
                task_id: &str,
                params: RunExtractionParams,
                on_event: WorkerEventSink,
            ) -> Result<ExtractionResultDto, WorkerError> {
                let (_tx, rx) = watch::channel(false);
                self.run_extraction_with_cancel(task_id, params, on_event, rx)
                    .await
            }

            async fn run_extraction_with_cancel(
                &self,
                task_id: &str,
                params: RunExtractionParams,
                on_event: WorkerEventSink,
                _cancel_rx: watch::Receiver<bool>,
            ) -> Result<ExtractionResultDto, WorkerError> {
                let strips_dir = PathBuf::from(params.strips_dir.as_deref().unwrap());
                let p = strips_dir.join("strip_0_1000.ppm");
                let img = RgbImage::new_filled(1920, 120, [10, 20, 30]);
                write_ppm_atomic(&p, &img).unwrap();
                on_event(TaskEvent::TimingSlot {
                    task_id: task_id.to_string(),
                    index: 1,
                    start_ms: 0,
                    end_ms: 1000,
                    strip_path: p.to_string_lossy().to_string(),
                });
                // Intentionally ignore cancel_rx (matching post-dispatch WorkerSupervisor):
                // blocks indefinitely until worker.cancel_task("<id>:strips") is invoked.
                loop {
                    if self
                        .cancelled_ids
                        .lock()
                        .unwrap()
                        .iter()
                        .any(|id| id == task_id)
                    {
                        return Err(WorkerError::Aborted("cancelled via rpc".into()));
                    }
                    self.cancel_notify.notified().await;
                }
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

            async fn cancel_task(&self, task_id: &str) -> Result<(), WorkerError> {
                self.cancelled_ids.lock().unwrap().push(task_id.to_string());
                self.cancel_notify.notify_waiters();
                Ok(())
            }

            async fn shutdown(&self) {}
        }

        let dir = TempDir::new().unwrap();
        let (base_url, srv) = spawn_pipeline_vlm_server(|_, _| {
            (
                401,
                r#"{"error":{"message":"invalid_api_key"}}"#.into(),
            )
        })
        .await;

        let worker = Arc::new(Stage1CancelOnlyWorker {
            cancelled_ids: StdMutex::new(Vec::new()),
            cancel_notify: tokio::sync::Notify::new(),
        });
        let (svc, storage, video_path) = setup_service(&dir, worker.clone(), &base_url, 1);
        let params = StartExtractionParams {
            video_path: video_path.to_string_lossy().to_string(),
            mode: ExtractionMode::PureTimingStrips,
            engine: None,
            provider_id: Some("test-p".into()),
            region_box: None,
            config: ExtractionConfigDto::default(),
            script: SubtitleScript::Auto,
        };
        let sink: WorkerEventSink = Arc::new(|_| {});

        let res = tokio::time::timeout(
            std::time::Duration::from_secs(5),
            svc.run_vlm_extraction("t-fatal-stage1", params, sink),
        )
        .await
        .expect("run_vlm_extraction hung waiting for Stage 1 worker after HTTP 401 Fatal");

        match res {
            Err(VlmError::Failed { code, .. }) => assert_eq!(code, "VlmFatal"),
            other => panic!("expected VlmFatal error, got: {other:?}"),
        }
        assert!(worker
            .cancelled_ids
            .lock()
            .unwrap()
            .iter()
            .any(|id| id == "t-fatal-stage1:strips"));
        assert_eq!(
            std::fs::read_dir(storage.preview_cache_dir()).unwrap().count(),
            0
        );
        srv.abort();
    }

    #[tokio::test]
    async fn resumed_run_with_three_cached_sheets_still_triggers_tail_hedging() {
        let dir = TempDir::new().unwrap();
        let listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let addr = listener.local_addr().unwrap();
        let sheet4_attempts = Arc::new(AtomicUsize::new(0));
        let sheet4_attempts_srv = Arc::clone(&sheet4_attempts);
        let primary_release = Arc::new(tokio::sync::Notify::new());
        let primary_release_srv = Arc::clone(&primary_release);

        let srv = tokio::spawn(async move {
            loop {
                let Ok((mut stream, _)) = listener.accept().await else {
                    break;
                };
                let sheet4_attempts = Arc::clone(&sheet4_attempts_srv);
                let primary_release = Arc::clone(&primary_release_srv);
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
                                let v: serde_json::Value = serde_json::from_str(&body).unwrap();
                                let prompt =
                                    v["messages"][1]["content"][1]["text"].as_str().unwrap();
                                let resp_body = if prompt.contains("[1]") {
                                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":1,\"category\":\"dialogue\",\"lines\":[\"S1\"]}]}"}}]}"#.to_string()
                                } else if prompt.contains("[2]") {
                                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":2,\"category\":\"dialogue\",\"lines\":[\"S2\"]}]}"}}]}"#.to_string()
                                } else if prompt.contains("[3]") {
                                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":3,\"category\":\"dialogue\",\"lines\":[\"S3\"]}]}"}}]}"#.to_string()
                                } else {
                                    let attempt = sheet4_attempts.fetch_add(1, Ordering::SeqCst);
                                    if attempt == 0 {
                                        // Run 1: fail sheet 4 so it is excluded from cache on Run 2
                                        r#"{"choices":[{"finish_reason":"content_filter","message":{"content":""}}]}"#.to_string()
                                    } else if attempt == 1 {
                                        // Run 2 primary request: stall until hedge wins
                                        primary_release.notified().await;
                                        r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":4,\"category\":\"dialogue\",\"lines\":[\"Slow Primary\"]}]}"}}]}"#.to_string()
                                    } else {
                                        // Run 2 hedge request: respond immediately
                                        r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":4,\"category\":\"dialogue\",\"lines\":[\"Hedged S4\"]}]}"}}]}"#.to_string()
                                    }
                                };
                                let resp = format!(
                                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{resp_body}",
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

        let base_url = format!("http://127.0.0.1:{}/v1", addr.port());
        let worker = Arc::new(PipelineMockWorker::new(
            vec![
                (0, 1000, true),
                (1000, 2000, true),
                (2000, 3000, true),
                (3000, 4000, true),
            ],
            None,
        ));
        let (mut svc, storage, video_path) = setup_service(&dir, worker, &base_url, 1);
        svc = svc.with_test_tuning(1, 0.05, 3.5, true, 1);

        let params = StartExtractionParams {
            video_path: video_path.to_string_lossy().to_string(),
            mode: ExtractionMode::PureTimingStrips,
            engine: None,
            provider_id: Some("test-p".into()),
            region_box: None,
            config: ExtractionConfigDto::default(),
            script: SubtitleScript::Auto,
        };
        let sink: WorkerEventSink = Arc::new(|_| {});

        // Run 1: sheets 1-3 succeed (cached), sheet 4 degrades to placeholder (not cached).
        let res1 = svc
            .run_vlm_extraction("t-hedge-resume-1", params.clone(), Arc::clone(&sink))
            .await
            .unwrap();
        assert_eq!(res1.failed_slot_count, Some(1));

        // Run 2: sheets 1-3 hit cache; sheet 4 primary stalls so tail hedging must trigger.
        let res2 = tokio::time::timeout(
            std::time::Duration::from_secs(5),
            svc.run_vlm_extraction("t-hedge-resume-2", params.clone(), sink),
        )
        .await
        .expect("resumed run stalled without triggering tail hedge")
        .unwrap();

        assert_eq!(res2.entries.len(), 4);
        assert_eq!(res2.entries[3].text, "Hedged S4");
        assert_eq!(res2.failed_slot_count, Some(0));

        let checkpoint_key = compute_vlm_checkpoint_key(
            &video_path,
            Some(resolve_default_bottom_region(1920, 1080, params.config.region_bottom_ratio)),
            params.config.sample_fps,
            params.script,
            "test-vl",
        );
        let final_path = storage
            .vlm_checkpoints_dir()
            .join(&checkpoint_key)
            .join("vlm_responses.json");
        let final_doc: FinalResponsesDoc =
            serde_json::from_str(&std::fs::read_to_string(&final_path).unwrap()).unwrap();
        assert_eq!(final_doc.batches.len(), 4);
        assert!(
            final_doc.batches[3].hedged_win,
            "sheet_04 must record hedged_win == true on resumed run"
        );

        primary_release.notify_waiters();
        srv.abort();
    }

    #[test]
    fn active_vlm_guard_generation_prevents_stale_drop_from_evicting_new_entry() {
        let registry: Arc<StdMutex<HashMap<String, VlmActiveEntry>>> =
            Arc::new(StdMutex::new(HashMap::new()));
        let (tx1, _rx1) = watch::channel(false);
        let (tx2, rx2) = watch::channel(false);

        registry.lock().unwrap().insert(
            "same-id".into(),
            VlmActiveEntry {
                generation: 1,
                cancel_tx: tx1,
            },
        );
        let guard1 = ActiveVlmTaskGuard {
            registry: Arc::clone(&registry),
            task_id: "same-id".into(),
            generation: 1,
        };

        // Simulate newer run overwriting the same task_id
        registry.lock().unwrap().insert(
            "same-id".into(),
            VlmActiveEntry {
                generation: 2,
                cancel_tx: tx2,
            },
        );
        let guard2 = ActiveVlmTaskGuard {
            registry: Arc::clone(&registry),
            task_id: "same-id".into(),
            generation: 2,
        };

        drop(guard1);
        assert!(registry.lock().unwrap().contains_key("same-id"));
        assert_eq!(registry.lock().unwrap()["same-id"].generation, 2);
        assert!(!*rx2.borrow());

        drop(guard2);
        assert!(!registry.lock().unwrap().contains_key("same-id"));
    }

    #[tokio::test]
    async fn stage1_worker_crash_after_partial_slots_does_not_waste_trailing_vlm_call() {
        struct CrashAfterOneSlotWorker;

        #[async_trait]
        impl WorkerBackend for CrashAfterOneSlotWorker {
            async fn capabilities(&self) -> Result<CapabilitiesDto, WorkerError> {
                Ok(CapabilitiesDto {
                    engines: vec![OcrEngineId::Paddle],
                    ffmpeg_available: true,
                    hwaccel_modes: vec![],
                })
            }
            async fn probe_video(&self, _: &str) -> Result<VideoInfoDto, WorkerError> {
                Ok(VideoInfoDto {
                    width: 1920,
                    height: 1080,
                    duration_ms: 5000,
                    fps: 24.0,
                    display_transform_ok: true,
                    transform_note: None,
                })
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
                task_id: &str,
                params: RunExtractionParams,
                on_event: WorkerEventSink,
            ) -> Result<ExtractionResultDto, WorkerError> {
                let (_tx, rx) = watch::channel(false);
                self.run_extraction_with_cancel(task_id, params, on_event, rx)
                    .await
            }
            async fn run_extraction_with_cancel(
                &self,
                task_id: &str,
                params: RunExtractionParams,
                on_event: WorkerEventSink,
                _cancel_rx: watch::Receiver<bool>,
            ) -> Result<ExtractionResultDto, WorkerError> {
                let strips_dir = PathBuf::from(params.strips_dir.as_deref().unwrap());
                let p = strips_dir.join("strip_0_1000.ppm");
                let img = RgbImage::new_filled(1920, 120, [10, 20, 30]);
                write_ppm_atomic(&p, &img).unwrap();
                on_event(TaskEvent::TimingSlot {
                    task_id: task_id.to_string(),
                    index: 1,
                    start_ms: 0,
                    end_ms: 1000,
                    strip_path: p.to_string_lossy().to_string(),
                });
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
        let http_calls = Arc::new(AtomicUsize::new(0));
        let http_calls_srv = Arc::clone(&http_calls);
        let (base_url, srv) = spawn_pipeline_vlm_server(move |_, _| {
            http_calls_srv.fetch_add(1, Ordering::SeqCst);
            (200, r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[]}"}}]}"#.into())
        })
        .await;

        let worker = Arc::new(CrashAfterOneSlotWorker);
        let (svc, storage, video_path) = setup_service(&dir, worker, &base_url, 10);
        let params = StartExtractionParams {
            video_path: video_path.to_string_lossy().to_string(),
            mode: ExtractionMode::PureTimingStrips,
            engine: None,
            provider_id: Some("test-p".into()),
            region_box: None,
            config: ExtractionConfigDto::default(),
            script: SubtitleScript::Auto,
        };
        let sink: WorkerEventSink = Arc::new(|_| {});

        let err = svc
            .run_vlm_extraction("t-crash-partial", params, sink)
            .await
            .unwrap_err();
        assert!(matches!(
            err,
            VlmError::Failed {
                ref code,
                retryable: true,
                ..
            } if code == "WorkerCrashed"
        ));
        assert_eq!(
            http_calls.load(Ordering::SeqCst),
            0,
            "trailing partial batch must NOT call remote VLM API after Stage 1 crash"
        );
        assert_eq!(
            std::fs::read_dir(storage.preview_cache_dir()).unwrap().count(),
            0
        );
        srv.abort();
    }

    #[tokio::test]
    async fn re_recognize_entries_multi_sheet_empty_oob_duplicate_cue_id_and_single_failure_isolation() {
        struct ReOcrMockWorker {
            fail_cue2_timestamps: bool,
        }

        #[async_trait]
        impl WorkerBackend for ReOcrMockWorker {
            async fn capabilities(&self) -> Result<CapabilitiesDto, WorkerError> {
                unreachable!()
            }
            async fn probe_video(&self, _: &str) -> Result<VideoInfoDto, WorkerError> {
                Ok(VideoInfoDto {
                    width: 1920,
                    height: 1080,
                    duration_ms: 60_000,
                    fps: 24.0,
                    display_transform_ok: true,
                    transform_note: None,
                })
            }
            async fn extract_frame(
                &self,
                _: &str,
                timestamp_ms: i64,
                _: Option<SourceBox>,
                _: Option<i32>,
                out_path: &str,
            ) -> Result<ExtractFrameResult, WorkerError> {
                // Fail all 3 fallback seeks for cue centered around 2500ms (start 2000..3000 -> mid=2500, start=2000, start-100=1900)
                if self.fail_cue2_timestamps && matches!(timestamp_ms, 2500 | 2000 | 1900) {
                    return Err(WorkerError::Remote {
                        code: "DecoderFailed".into(),
                        message: "seek failed".into(),
                    });
                }
                let img = RgbImage::new_filled(320, 40, [40, 80, 120]);
                write_ppm_atomic(Path::new(out_path), &img).unwrap();
                Ok(ExtractFrameResult {
                    width: 320,
                    height: 40,
                    out_path: Some(out_path.to_string()),
                })
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
                unreachable!()
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
        let (base_url, srv) = spawn_pipeline_vlm_server(move |_, body| {
            let v: serde_json::Value = serde_json::from_str(&body).unwrap();
            let prompt = v["messages"][1]["content"][1]["text"].as_str().unwrap();
            if prompt.contains("[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]") {
                let slots: Vec<_> = (1..=10)
                    .map(|i| json!({"id": i, "category": "dialogue", "lines": [format!("Line {i}")]}))
                    .collect();
                (
                    200,
                    json!({"choices":[{"finish_reason":"stop","message":{"content": json!({"slots": slots}).to_string()}}]}).to_string(),
                )
            } else {
                // Sheet 2 returns local IDs [1, 2] which must remap to slots 11, 12!
                (
                    200,
                    r#"{"choices":[{"finish_reason":"stop","message":{"content":"{\"slots\":[{\"id\":1,\"category\":\"dialogue\",\"lines\":[\"Sheet2 A\"]},{\"id\":2,\"category\":\"bracket_note\",\"lines\":[\"（旁白）\"]}]}"}}]}"#.into(),
                )
            }
        })
        .await;

        let worker = Arc::new(ReOcrMockWorker {
            fail_cue2_timestamps: true,
        });
        let (svc, _storage, video_path) = setup_service(&dir, worker, &base_url, 10);
        let vp = video_path.to_string_lossy().to_string();

        // 1. Empty items -> Ok([])
        let empty_res = svc
            .re_recognize_entries(&vp, None, vec![], Some("test-p".into()), SubtitleScript::Auto)
            .await
            .unwrap();
        assert!(empty_res.is_empty());

        // 2. OOB region_box -> InvalidConfiguration
        let oob_err = svc
            .re_recognize_entries(
                &vp,
                Some(SourceBox {
                    x: 2000,
                    y: 0,
                    width: 200,
                    height: 100,
                }),
                vec![ReRecognizeItemInput {
                    cue_id: 1,
                    start_ms: 0,
                    end_ms: 500,
                }],
                Some("test-p".into()),
                SubtitleScript::Auto,
            )
            .await
            .unwrap_err();
        assert!(matches!(oob_err, VlmError::InvalidConfiguration(_)));

        // 3. 13 items: 1 fails all 3 seeks (cue_id=200), 12 succeed across 2 sheets (with duplicate cue_id=999!)
        let mut items = Vec::new();
        for i in 0..10 {
            items.push(ReRecognizeItemInput {
                cue_id: 100 + i,
                start_ms: (i as i64) * 5000,
                end_ms: (i as i64) * 5000 + 1000,
            });
        }
        // Item at idx 10 fails all 3 seeks (2000..3000)
        items.push(ReRecognizeItemInput {
            cue_id: 200,
            start_ms: 2000,
            end_ms: 3000,
        });
        // Items at idx 11 and 12 share duplicate cue_id=999 and land on Sheet 2 (local [1, 2])
        items.push(ReRecognizeItemInput {
            cue_id: 999,
            start_ms: 50_000,
            end_ms: 51_000,
        });
        items.push(ReRecognizeItemInput {
            cue_id: 999,
            start_ms: 52_000,
            end_ms: 53_000,
        });

        let res = svc
            .re_recognize_entries(&vp, None, items, Some("test-p".into()), SubtitleScript::Cjk)
            .await
            .unwrap();
        assert_eq!(res.len(), 13);
        assert!(res[0].ok);
        assert_eq!(res[0].text.as_deref(), Some("Line 1"));
        assert!(!res[10].ok);
        assert!(res[10].error_message.as_deref().unwrap().contains("seek failed"));
        assert!(res[11].ok);
        assert_eq!(res[11].cue_id, 999);
        assert_eq!(res[11].text.as_deref(), Some("Sheet2 A"));
        assert!(res[12].ok);
        assert_eq!(res[12].cue_id, 999);
        assert_eq!(res[12].category, Some(ReRecognizeCategory::BracketNote));
        assert_eq!(res[12].text.as_deref(), Some("（旁白）"));
        srv.abort();
    }

    #[tokio::test]
    async fn native_vlm_service_test_provider_connection_uses_stored_secret_without_persisting() {
        let listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let addr = listener.local_addr().unwrap();
        let captured_auth = Arc::new(StdMutex::new(Vec::<Option<String>>::new()));
        let captured_session = Arc::new(StdMutex::new(Vec::<Option<String>>::new()));
        let auth_srv = Arc::clone(&captured_auth);
        let sess_srv = Arc::clone(&captured_session);
        let srv = tokio::spawn(async move {
            loop {
                let Ok((mut stream, _)) = listener.accept().await else {
                    break;
                };
                let auth_conn = Arc::clone(&auth_srv);
                let sess_conn = Arc::clone(&sess_srv);
                tokio::spawn(async move {
                    let mut buf = Vec::new();
                    let mut chunk = [0u8; 4096];
                    let mut header_end = None;
                    let mut content_len = 0usize;
                    let mut auth_val = None;
                    let mut sess_val = None;
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
                                        let lower = k.trim().to_ascii_lowercase();
                                        let trimmed = v.trim().to_string();
                                        if lower == "content-length" {
                                            content_len = trimmed.parse().unwrap_or(0);
                                        } else if lower == "authorization" {
                                            auth_val = Some(trimmed);
                                        } else if lower == "x-opencode-session" {
                                            sess_val = Some(trimmed);
                                        }
                                    }
                                }
                            }
                        }
                        if let Some(end) = header_end {
                            if buf.len() >= end + content_len {
                                auth_conn.lock().unwrap().push(auth_val);
                                sess_conn.lock().unwrap().push(sess_val);
                                let body = serde_json::json!({
                                    "choices": [{"finish_reason": "stop", "message": {"content": "OK"}}]
                                })
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

        let dir = TempDir::new().unwrap();
        let storage = StorageRoot::new(dir.path().join("appdata")).unwrap();
        let providers = Arc::new(ProviderStore::new(storage.clone()));
        let checkpoints = Arc::new(CheckpointStore::new(storage.clone()));
        let saved = providers
            .save_provider(ProviderUpsertDto {
                id: Some("p-conn".into()),
                name: "Saved Profile".into(),
                provider_type: ProviderType::OpenAi,
                base_url: "https://original.example.com/v1".into(),
                model: "original-model".into(),
                thinking_mode: ThinkingMode::Auto,
                concurrency: 4,
                is_default: true,
                api_key_update: ApiKeyUpdate::Replace {
                    value: "sk-persisted-key-888".into(),
                },
                custom_headers: Vec::new(),
            })
            .unwrap();

        let worker = Arc::new(PipelineMockWorker::new(vec![], None)) as Arc<dyn WorkerBackend>;
        let svc = NativeVlmService::new(worker, Arc::clone(&providers), checkpoints, storage);

        let res = svc
            .test_provider_connection(ProviderUpsertDto {
                id: Some(saved.id.clone()),
                name: "".into(),
                provider_type: ProviderType::OpenAi,
                base_url: format!("http://127.0.0.1:{}/v1", addr.port()),
                model: "probe-vl".into(),
                thinking_mode: ThinkingMode::Default,
                concurrency: 6,
                is_default: false,
                api_key_update: ApiKeyUpdate::Keep,
                custom_headers: vec![crate::dto::CustomHeaderDto {
                    name: "x-opencode-session".into(),
                    value: "{{session_id}}".into(),
                }],
            })
            .await
            .unwrap();
        srv.abort();

        assert_eq!(res.model, "probe-vl");
        assert_eq!(res.response_preview, "OK");
        let auths = captured_auth.lock().unwrap();
        assert_eq!(auths.as_slice(), &[Some("Bearer sk-persisted-key-888".to_string())]);
        let sess = captured_session.lock().unwrap();
        assert!(uuid::Uuid::parse_str(sess[0].as_deref().unwrap()).is_ok());

        // Stored profile must stay unmodified on disk.
        let stored_after = providers.resolve_provider_secret(Some(&saved.id)).unwrap();
        assert_eq!(stored_after.base_url, "https://original.example.com/v1");
        assert_eq!(stored_after.model, "original-model");
        assert!(stored_after.custom_headers.is_empty());
    }
}

