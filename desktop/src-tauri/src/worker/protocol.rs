use serde::{Deserialize, Serialize};

use crate::dto::{
    ExtractionConfigDto, ExtractionMetricsDto, ExtractionMode, OcrEngineId, ReRecognizeCategory,
    SourceBox, SubtitleEntry, SubtitleScript,
};

#[derive(Debug, Serialize)]
pub struct WorkerRequest<'a, P: Serialize> {
    pub id: u64,
    pub op: &'a str,
    pub params: P,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum WorkerInbound {
    Event {
        id: u64,
        event: String,
        #[serde(default)]
        data: serde_json::Value,
    },
    Result {
        id: u64,
        ok: bool,
        #[serde(default)]
        data: Option<serde_json::Value>,
        #[serde(default)]
        error: Option<WorkerRemoteError>,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct WorkerRemoteError {
    pub code: String,
    pub message: String,
}

#[derive(Debug, Clone, PartialEq)]
pub enum WorkerStreamEvent {
    Progress {
        current_ms: i64,
        total_ms: i64,
        fps: f64,
    },
    SubtitleEntry(SubtitleEntry),
    TimingSlot {
        index: u64,
        start_ms: i64,
        end_ms: i64,
        strip_path: String,
    },
    Unknown(String),
}

impl WorkerStreamEvent {
    pub fn parse(event: &str, data: serde_json::Value) -> Result<Self, serde_json::Error> {
        match event {
            "progress" => {
                #[derive(Deserialize)]
                struct ProgressPayload {
                    current_ms: i64,
                    total_ms: i64,
                    fps: f64,
                }
                let payload: ProgressPayload = serde_json::from_value(data)?;
                Ok(Self::Progress {
                    current_ms: payload.current_ms,
                    total_ms: payload.total_ms,
                    fps: payload.fps,
                })
            }
            "subtitle_entry" => {
                let entry: SubtitleEntry = serde_json::from_value(data)?;
                Ok(Self::SubtitleEntry(entry))
            }
            "timing_slot" => {
                #[derive(Deserialize)]
                struct TimingSlotPayload {
                    index: u64,
                    start_ms: i64,
                    end_ms: i64,
                    strip_path: String,
                }
                let payload: TimingSlotPayload = serde_json::from_value(data)?;
                Ok(Self::TimingSlot {
                    index: payload.index,
                    start_ms: payload.start_ms,
                    end_ms: payload.end_ms,
                    strip_path: payload.strip_path,
                })
            }
            other => Ok(Self::Unknown(other.to_string())),
        }
    }
}

#[derive(Debug, Serialize)]
pub struct EmptyParams {}

#[derive(Debug, Serialize)]
pub struct ProbeVideoParams {
    pub video_path: String,
}

#[derive(Debug, Serialize)]
pub struct ExtractFrameParams {
    pub video_path: String,
    pub timestamp_ms: i64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub crop: Option<SourceBox>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub max_width: Option<i32>,
    pub out_path: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExtractFrameResult {
    pub width: i32,
    pub height: i32,
    #[serde(default)]
    pub out_path: Option<String>,
}

#[derive(Debug, Serialize)]
pub struct DetectAutoRegionParams {
    pub video_path: String,
    pub engine: OcrEngineId,
    pub script: SubtitleScript,
    pub bottom_ratio: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RunExtractionParams {
    pub video_path: String,
    pub mode: ExtractionMode,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub engine: Option<OcrEngineId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub region_box: Option<SourceBox>,
    pub config: ExtractionConfigDto,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub strips_dir: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WorkerRunExtractionResult {
    #[serde(default)]
    pub entries: Vec<SubtitleEntry>,
    #[serde(default)]
    pub metrics: ExtractionMetricsDto,
    #[serde(default)]
    pub failed_slot_count: Option<u64>,
}

#[derive(Debug, Serialize)]
pub struct RecognizeStripsParams {
    pub engine: OcrEngineId,
    pub strip_paths: Vec<String>,
    pub script: SubtitleScript,
}

fn default_true() -> bool {
    true
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RecognizeStripItemResult {
    #[serde(default)]
    pub strip_path: String,
    #[serde(default = "default_true")]
    pub ok: bool,
    #[serde(default)]
    pub text: String,
    #[serde(default)]
    pub confidence: f64,
    #[serde(default)]
    pub category: Option<ReRecognizeCategory>,
    #[serde(default)]
    pub error_message: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RecognizeStripsResult {
    #[serde(alias = "results")]
    pub items: Vec<RecognizeStripItemResult>,
}

#[derive(Debug, Serialize)]
pub struct FinalizeSrtParams {
    pub entries: Vec<SubtitleEntry>,
    pub apply_merge: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub min_duration_ms: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub merge_gap_ms: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub drop_empty_text: Option<bool>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct FinalizeSrtResult {
    #[serde(alias = "srt")]
    pub srt_text: String,
    pub cue_count: usize,
}

#[derive(Debug, Serialize)]
pub struct CancelTaskParams {
    pub target_id: u64,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_recognize_strips_result_matching_cpp_worker_schema() {
        let json = r#"{
            "results": [
                {"strip_path": "/tmp/s1.ppm", "ok": true, "text": "你好", "confidence": 0.96},
                {"strip_path": "/tmp/s2.ppm", "ok": false, "text": "", "confidence": 0.0, "error_message": "corrupt ppm"}
            ]
        }"#;
        let parsed: RecognizeStripsResult = serde_json::from_str(json).unwrap();
        assert_eq!(parsed.items.len(), 2);
        assert!(parsed.items[0].ok);
        assert_eq!(parsed.items[0].text, "你好");
        assert!(!parsed.items[1].ok);
        assert_eq!(parsed.items[1].error_message.as_deref(), Some("corrupt ppm"));
    }
}
