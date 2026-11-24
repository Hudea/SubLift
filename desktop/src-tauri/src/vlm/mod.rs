pub mod assembler;
pub mod checkpoint;
pub mod client;
pub mod font;
pub mod image;
pub mod pipeline;
pub mod rate_limit;
pub mod schema;
pub mod sheet_builder;

use async_trait::async_trait;
use thiserror::Error;

use crate::dto::{
    ExtractionResultDto, ProviderConnectionTestDto, ProviderUpsertDto, ReRecognizeItemInput,
    ReRecognizeResult, SourceBox, StartExtractionParams, SubtitleScript, TaskEvent,
};
use crate::worker::WorkerEventSink;

pub use pipeline::NativeVlmService;

pub(crate) async fn wait_until_cancelled(rx: &mut tokio::sync::watch::Receiver<bool>) {
    if *rx.borrow() {
        return;
    }
    if rx.wait_for(|&v| v).await.is_err() {
        std::future::pending::<()>().await;
    }
}

#[derive(Debug, Error)]
pub enum VlmError {
    #[error("VLM service not yet implemented (M4): {0}")]
    NotImplemented(String),
    #[error("VLM task cancelled")]
    Cancelled,
    #[error("invalid VLM configuration: {0}")]
    InvalidConfiguration(String),
    #[error("VLM error [{code}]: {message}")]
    Failed {
        code: String,
        message: String,
        retryable: bool,
    },
}

impl VlmError {
    pub fn code(&self) -> &str {
        match self {
            Self::NotImplemented(_) => "VlmNotImplemented",
            Self::Cancelled => "Aborted",
            Self::InvalidConfiguration(_) => "InvalidConfiguration",
            Self::Failed { code, .. } => code.as_str(),
        }
    }

    pub fn is_retryable(&self) -> bool {
        matches!(self, Self::Failed { retryable: true, .. })
    }
}

#[async_trait]
pub trait VlmService: Send + Sync {
    async fn run_vlm_extraction(
        &self,
        task_id: &str,
        params: StartExtractionParams,
        on_event: WorkerEventSink,
    ) -> Result<ExtractionResultDto, VlmError>;

    async fn run_vlm_extraction_with_cancel(
        &self,
        task_id: &str,
        params: StartExtractionParams,
        on_event: WorkerEventSink,
        mut cancel_rx: tokio::sync::watch::Receiver<bool>,
    ) -> Result<ExtractionResultDto, VlmError> {
        if *cancel_rx.borrow() {
            return Err(VlmError::Cancelled);
        }
        tokio::select! {
            biased;
            _ = cancel_rx.wait_for(|&v| v) => Err(VlmError::Cancelled),
            res = self.run_vlm_extraction(task_id, params, on_event) => res,
        }
    }

    async fn re_recognize_entries(
        &self,
        video_path: &str,
        region_box: Option<SourceBox>,
        items: Vec<ReRecognizeItemInput>,
        provider_id: Option<String>,
        script: SubtitleScript,
    ) -> Result<Vec<ReRecognizeResult>, VlmError>;

    async fn test_provider_connection(
        &self,
        profile: ProviderUpsertDto,
    ) -> Result<ProviderConnectionTestDto, VlmError>;

    async fn cancel_task(&self, task_id: &str) -> Result<(), VlmError>;
}

#[derive(Default)]
pub struct StubVlmService;

#[async_trait]
impl VlmService for StubVlmService {
    async fn run_vlm_extraction(
        &self,
        task_id: &str,
        _params: StartExtractionParams,
        on_event: WorkerEventSink,
    ) -> Result<ExtractionResultDto, VlmError> {
        let msg = "VLM extraction pipeline is scheduled for Milestone M4".to_string();
        on_event(TaskEvent::Failed {
            task_id: task_id.to_string(),
            code: "VlmNotImplemented".to_string(),
            message: msg.clone(),
            retryable: false,
        });
        Err(VlmError::NotImplemented(msg))
    }

    async fn re_recognize_entries(
        &self,
        _video_path: &str,
        _region_box: Option<SourceBox>,
        _items: Vec<ReRecognizeItemInput>,
        _provider_id: Option<String>,
        _script: SubtitleScript,
    ) -> Result<Vec<ReRecognizeResult>, VlmError> {
        Err(VlmError::NotImplemented(
            "VLM partial re-recognition is scheduled for Milestone M4".to_string(),
        ))
    }

    async fn test_provider_connection(
        &self,
        _profile: ProviderUpsertDto,
    ) -> Result<ProviderConnectionTestDto, VlmError> {
        Err(VlmError::NotImplemented(
            "VLM connectivity probe requires NativeVlmService".to_string(),
        ))
    }

    async fn cancel_task(&self, _task_id: &str) -> Result<(), VlmError> {
        Ok(())
    }
}
