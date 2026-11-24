use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum OcrEngineId {
    Paddle,
    AppleVision,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum WorkbenchEngine {
    Paddle,
    AppleVision,
    Vlm,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SubtitleScript {
    #[default]
    Auto,
    Cjk,
    Latin,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RoiMode {
    Auto,
    Band,
    Rect,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ExtractionMode {
    LocalOcr,
    PureTimingStrips,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ExtractionSubPhase {
    Probing,
    Timing,
    LocalOcr,
    VlmSheets,
    Finalizing,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ProviderType {
    Minimax,
    Nvidia,
    #[serde(rename = "openai", alias = "openai_compatible")]
    OpenAi,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ThinkingMode {
    Auto,
    None,
    Low,
    Minimal,
    Default,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ReRecognizeCategory {
    Dialogue,
    BracketNote,
    Placeholder,
    Empty,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BatchExportConflictPolicy {
    Overwrite,
    Rename,
    Skip,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BatchExportItemStatus {
    Exported,
    Renamed,
    Skipped,
    Failed,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TaskStatus {
    Idle,
    Extracting,
    Ready,
    Exported,
    Cancelled,
    Failed,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct SourceBox {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct CapabilitiesDto {
    pub engines: Vec<OcrEngineId>,
    #[serde(alias = "ffmpeg")]
    pub ffmpeg_available: bool,
    #[serde(default)]
    pub hwaccel_modes: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct VideoInfoDto {
    pub width: i32,
    pub height: i32,
    pub duration_ms: i64,
    pub fps: f64,
    pub display_transform_ok: bool,
    #[serde(default)]
    pub transform_note: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct FramePreviewDto {
    pub image_url: String,
    pub width: i32,
    pub height: i32,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct SubtitleProfileDto {
    pub script: SubtitleScript,
    pub center_x: i32,
    pub center_y: i32,
    pub height: i32,
    pub y_min: i32,
    pub y_max: i32,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct AutoRegionDto {
    pub detected: bool,
    pub region_box: SourceBox,
    #[serde(default)]
    pub profile: Option<SubtitleProfileDto>,
    pub sample_timestamp_ms: i64,
    pub preview_text: String,
    pub confidence: f64,
    pub error_message: String,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct SubtitleEntry {
    pub id: u64,
    pub start_ms: i64,
    pub end_ms: i64,
    pub text: String,
    pub confidence: f64,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct ExtractionConfigDto {
    pub sample_fps: f64,
    pub region_bottom_ratio: f64,
    #[serde(default)]
    pub subtitle_script: SubtitleScript,
    #[serde(default)]
    pub confidence_threshold: Option<f64>,
    #[serde(default)]
    pub min_duration_ms: Option<i64>,
    #[serde(default)]
    pub merge_gap_ms: Option<i64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub predetected_profile: Option<SubtitleProfileDto>,
}

impl Default for ExtractionConfigDto {
    fn default() -> Self {
        Self {
            sample_fps: 5.0,
            region_bottom_ratio: 0.30,
            subtitle_script: SubtitleScript::Auto,
            confidence_threshold: Some(0.5),
            min_duration_ms: Some(500),
            merge_gap_ms: Some(1000),
            predetected_profile: None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct StartExtractionParams {
    pub video_path: String,
    pub mode: ExtractionMode,
    #[serde(default)]
    pub engine: Option<OcrEngineId>,
    #[serde(default)]
    pub provider_id: Option<String>,
    #[serde(default)]
    pub region_box: Option<SourceBox>,
    pub config: ExtractionConfigDto,
    #[serde(default)]
    pub script: SubtitleScript,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct ExtractionMetricsDto {
    pub total_frames_processed: u64,
    pub total_segments_detected: u64,
    pub total_ocr_calls: u64,
    pub total_entries_produced: u64,
    pub wall_time_ms: i64,
    pub effective_fps: f64,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct ExtractionResultDto {
    pub entries: Vec<SubtitleEntry>,
    pub metrics: ExtractionMetricsDto,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub failed_slot_count: Option<u64>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum TaskEvent {
    Phase {
        task_id: String,
        sub_phase: ExtractionSubPhase,
    },
    Progress {
        task_id: String,
        current_ms: i64,
        total_ms: i64,
        fps: f64,
    },
    SubtitleEntry {
        task_id: String,
        entry: SubtitleEntry,
    },
    TimingSlot {
        task_id: String,
        index: u64,
        start_ms: i64,
        end_ms: i64,
        strip_path: String,
    },
    VlmSheetProgress {
        task_id: String,
        completed_sheets: u32,
        total_sheets: u32,
        retried_splits: u32,
    },
    Completed {
        task_id: String,
        result: ExtractionResultDto,
    },
    Cancelled {
        task_id: String,
    },
    Failed {
        task_id: String,
        code: String,
        message: String,
        retryable: bool,
    },
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct CustomHeaderDto {
    pub name: String,
    pub value: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ProviderProfileMaskedDto {
    pub id: String,
    pub name: String,
    pub provider_type: ProviderType,
    pub base_url: String,
    pub model: String,
    pub thinking_mode: ThinkingMode,
    pub concurrency: u32,
    pub has_api_key: bool,
    pub masked_api_key: String,
    pub is_default: bool,
    #[serde(default)]
    pub custom_headers: Vec<CustomHeaderDto>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "mode", rename_all = "snake_case")]
pub enum ApiKeyUpdate {
    Keep,
    Replace { value: String },
    Clear,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ProviderUpsertDto {
    #[serde(default)]
    pub id: Option<String>,
    pub name: String,
    pub provider_type: ProviderType,
    pub base_url: String,
    pub model: String,
    pub thinking_mode: ThinkingMode,
    pub concurrency: u32,
    pub is_default: bool,
    pub api_key_update: ApiKeyUpdate,
    #[serde(default)]
    pub custom_headers: Vec<CustomHeaderDto>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ProviderConnectionTestDto {
    pub latency_ms: u64,
    pub endpoint: String,
    pub model: String,
    pub response_preview: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ReRecognizeItemInput {
    pub cue_id: u64,
    pub start_ms: i64,
    pub end_ms: i64,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct ReRecognizeResult {
    pub cue_id: u64,
    pub ok: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub text: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub category: Option<ReRecognizeCategory>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub confidence: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub error_message: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct MergeOptionsDto {
    pub min_duration_ms: i64,
    pub merge_gap_ms: i64,
    pub drop_empty_text: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExportResultDto {
    pub out_path: String,
    pub cue_count: usize,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct BatchExportItemInput {
    pub video_path: String,
    pub out_path: String,
    pub entries: Vec<SubtitleEntry>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct BatchExportItemResult {
    pub video_path: String,
    pub out_path: String,
    pub status: BatchExportItemStatus,
    pub cue_count: usize,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub error_message: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct BatchExportReport {
    pub results: Vec<BatchExportItemResult>,
    pub exported_count: usize,
    pub skipped_count: usize,
    pub failed_count: usize,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct SessionDraftDto {
    pub video_path: String,
    pub engine: WorkbenchEngine,
    #[serde(default)]
    pub provider_id: Option<String>,
    pub script: SubtitleScript,
    pub sample_fps: f64,
    pub roi_mode: RoiMode,
    #[serde(default)]
    pub region_box: Option<SourceBox>,
    pub status: TaskStatus,
    pub entries: Vec<SubtitleEntry>,
    pub updated_at_ms: i64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct SessionSummaryDto {
    pub video_path: String,
    pub cue_count: usize,
    pub status: TaskStatus,
    pub updated_at_ms: i64,
}
