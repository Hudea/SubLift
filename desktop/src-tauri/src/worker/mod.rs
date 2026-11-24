pub mod discovery;
pub mod protocol;
pub mod supervisor;

use std::path::PathBuf;
use std::sync::Arc;

use async_trait::async_trait;
use thiserror::Error;

use crate::dto::{
    AutoRegionDto, CapabilitiesDto, ExtractionResultDto, MergeOptionsDto, OcrEngineId, SourceBox,
    SubtitleEntry, SubtitleScript, TaskEvent, VideoInfoDto,
};

use protocol::{
    DetectAutoRegionParams, EmptyParams, ExtractFrameParams, ExtractFrameResult,
    FinalizeSrtParams, FinalizeSrtResult, ProbeVideoParams, RecognizeStripsParams,
    RecognizeStripsResult, RunExtractionParams, WorkerRunExtractionResult,
};
pub use supervisor::WorkerSupervisor;

pub type WorkerEventSink = Arc<dyn Fn(TaskEvent) + Send + Sync>;

#[derive(Clone, Debug, Default)]
pub struct WorkerConfig {
    pub override_bin: Option<PathBuf>,
    pub resource_dir: Option<PathBuf>,
    pub repo_root: Option<PathBuf>,
}

#[derive(Debug, Error)]
pub enum WorkerError {
    #[error("sublift_worker binary not found (searched: {searched_paths:?})")]
    BinaryNotFound { searched_paths: Vec<PathBuf> },
    #[error("failed to spawn sublift_worker: {0}")]
    SpawnFailed(String),
    #[error("worker I/O error: {0}")]
    Io(String),
    #[error("worker protocol serialization error: {0}")]
    Serialization(String),
    #[error("worker error [{code}]: {message}")]
    Remote { code: String, message: String },
    #[error("worker task aborted: {0}")]
    Aborted(String),
    #[error("worker exited unexpectedly (status={status:?})")]
    Crashed {
        status: Option<i32>,
        stderr_tail: Vec<String>,
        retryable: bool,
    },
}

impl WorkerError {
    pub fn is_retryable(&self) -> bool {
        matches!(self, Self::Crashed { retryable: true, .. })
    }

    pub fn code(&self) -> &str {
        match self {
            Self::BinaryNotFound { .. } => "WorkerBinaryNotFound",
            Self::SpawnFailed(_) => "WorkerSpawnFailed",
            Self::Io(_) => "WorkerIo",
            Self::Serialization(_) => "WorkerProtocolError",
            Self::Remote { code, .. } => code.as_str(),
            Self::Aborted(_) => "Aborted",
            Self::Crashed { .. } => "WorkerCrashed",
        }
    }
}

#[async_trait]
pub trait WorkerBackend: Send + Sync {
    async fn capabilities(&self) -> Result<CapabilitiesDto, WorkerError>;
    async fn probe_video(&self, video_path: &str) -> Result<VideoInfoDto, WorkerError>;
    async fn extract_frame(
        &self,
        video_path: &str,
        timestamp_ms: i64,
        crop: Option<SourceBox>,
        max_width: Option<i32>,
        out_path: &str,
    ) -> Result<ExtractFrameResult, WorkerError>;
    async fn detect_auto_region(
        &self,
        video_path: &str,
        engine: OcrEngineId,
        script: SubtitleScript,
        bottom_ratio: f64,
    ) -> Result<AutoRegionDto, WorkerError>;
    async fn run_extraction(
        &self,
        task_id: &str,
        params: RunExtractionParams,
        on_event: WorkerEventSink,
    ) -> Result<ExtractionResultDto, WorkerError>;
    async fn run_extraction_with_cancel(
        &self,
        task_id: &str,
        params: RunExtractionParams,
        on_event: WorkerEventSink,
        mut cancel_rx: tokio::sync::watch::Receiver<bool>,
    ) -> Result<ExtractionResultDto, WorkerError> {
        if *cancel_rx.borrow() {
            return Err(WorkerError::Aborted(
                "task cancelled before worker start".into(),
            ));
        }
        tokio::select! {
            biased;
            _ = cancel_rx.wait_for(|&v| v) => {
                Err(WorkerError::Aborted(
                    "task cancelled before worker start".into(),
                ))
            }
            res = self.run_extraction(task_id, params, on_event) => res,
        }
    }
    async fn recognize_strips(
        &self,
        engine: OcrEngineId,
        strip_paths: Vec<String>,
        script: SubtitleScript,
    ) -> Result<RecognizeStripsResult, WorkerError>;
    async fn finalize_srt(
        &self,
        entries: Vec<SubtitleEntry>,
        apply_merge: bool,
        merge_options: Option<MergeOptionsDto>,
    ) -> Result<FinalizeSrtResult, WorkerError>;
    async fn cancel_task(&self, task_id: &str) -> Result<(), WorkerError>;
    async fn shutdown(&self);
}

#[async_trait]
impl WorkerBackend for WorkerSupervisor {
    async fn capabilities(&self) -> Result<CapabilitiesDto, WorkerError> {
        let val = self.call_unary("capabilities", EmptyParams {}).await?;
        serde_json::from_value(val).map_err(|e| WorkerError::Serialization(e.to_string()))
    }

    async fn probe_video(&self, video_path: &str) -> Result<VideoInfoDto, WorkerError> {
        let val = self
            .call_unary(
                "probe_video",
                ProbeVideoParams {
                    video_path: video_path.to_string(),
                },
            )
            .await?;
        serde_json::from_value(val).map_err(|e| WorkerError::Serialization(e.to_string()))
    }

    async fn extract_frame(
        &self,
        video_path: &str,
        timestamp_ms: i64,
        crop: Option<SourceBox>,
        max_width: Option<i32>,
        out_path: &str,
    ) -> Result<ExtractFrameResult, WorkerError> {
        let val = self
            .call_unary(
                "extract_frame",
                ExtractFrameParams {
                    video_path: video_path.to_string(),
                    timestamp_ms,
                    crop,
                    max_width,
                    out_path: out_path.to_string(),
                },
            )
            .await?;
        serde_json::from_value(val).map_err(|e| WorkerError::Serialization(e.to_string()))
    }

    async fn detect_auto_region(
        &self,
        video_path: &str,
        engine: OcrEngineId,
        script: SubtitleScript,
        bottom_ratio: f64,
    ) -> Result<AutoRegionDto, WorkerError> {
        let val = self
            .call_unary(
                "detect_auto_region",
                DetectAutoRegionParams {
                    video_path: video_path.to_string(),
                    engine,
                    script,
                    bottom_ratio,
                },
            )
            .await?;
        serde_json::from_value(val).map_err(|e| WorkerError::Serialization(e.to_string()))
    }

    async fn run_extraction(
        &self,
        task_id: &str,
        params: RunExtractionParams,
        on_event: WorkerEventSink,
    ) -> Result<ExtractionResultDto, WorkerError> {
        let val = self
            .call_streaming(task_id, "run_extraction", params, on_event)
            .await?;
        let raw: WorkerRunExtractionResult =
            serde_json::from_value(val).map_err(|e| WorkerError::Serialization(e.to_string()))?;
        Ok(ExtractionResultDto {
            entries: raw.entries,
            metrics: raw.metrics,
            failed_slot_count: raw.failed_slot_count,
        })
    }

    async fn run_extraction_with_cancel(
        &self,
        task_id: &str,
        params: RunExtractionParams,
        on_event: WorkerEventSink,
        cancel_rx: tokio::sync::watch::Receiver<bool>,
    ) -> Result<ExtractionResultDto, WorkerError> {
        let val = self
            .call_streaming_with_cancel(
                task_id,
                "run_extraction",
                params,
                on_event,
                Some(cancel_rx),
            )
            .await?;
        let raw: WorkerRunExtractionResult =
            serde_json::from_value(val).map_err(|e| WorkerError::Serialization(e.to_string()))?;
        Ok(ExtractionResultDto {
            entries: raw.entries,
            metrics: raw.metrics,
            failed_slot_count: raw.failed_slot_count,
        })
    }

    async fn recognize_strips(
        &self,
        engine: OcrEngineId,
        strip_paths: Vec<String>,
        script: SubtitleScript,
    ) -> Result<RecognizeStripsResult, WorkerError> {
        let val = self
            .call_unary(
                "recognize_strips",
                RecognizeStripsParams {
                    engine,
                    strip_paths,
                    script,
                },
            )
            .await?;
        serde_json::from_value(val).map_err(|e| WorkerError::Serialization(e.to_string()))
    }

    async fn finalize_srt(
        &self,
        entries: Vec<SubtitleEntry>,
        apply_merge: bool,
        merge_options: Option<MergeOptionsDto>,
    ) -> Result<FinalizeSrtResult, WorkerError> {
        let (min_duration_ms, merge_gap_ms, drop_empty_text) = match merge_options {
            Some(opts) => (
                Some(opts.min_duration_ms),
                Some(opts.merge_gap_ms),
                Some(opts.drop_empty_text),
            ),
            None => (None, None, None),
        };
        let val = self
            .call_unary(
                "finalize_srt",
                FinalizeSrtParams {
                    entries,
                    apply_merge,
                    min_duration_ms,
                    merge_gap_ms,
                    drop_empty_text,
                },
            )
            .await?;
        serde_json::from_value(val).map_err(|e| WorkerError::Serialization(e.to_string()))
    }

    async fn cancel_task(&self, task_id: &str) -> Result<(), WorkerError> {
        self.cancel_task_by_id(task_id).await
    }

    async fn shutdown(&self) {
        WorkerSupervisor::shutdown(self).await;
    }
}
