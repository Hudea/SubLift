pub use crate::dto::*;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn task_event_serializes_discriminated_union_with_snake_case() {
        let events = [
            TaskEvent::Phase {
                task_id: "t1".into(),
                sub_phase: ExtractionSubPhase::VlmSheets,
            },
            TaskEvent::Progress {
                task_id: "t1".into(),
                current_ms: 1000,
                total_ms: 5000,
                fps: 60.5,
            },
            TaskEvent::SubtitleEntry {
                task_id: "t1".into(),
                entry: SubtitleEntry {
                    id: 1,
                    start_ms: 100,
                    end_ms: 900,
                    text: "测试".into(),
                    confidence: 0.92,
                },
            },
            TaskEvent::TimingSlot {
                task_id: "t1".into(),
                index: 0,
                start_ms: 100,
                end_ms: 900,
                strip_path: "/tmp/s0.ppm".into(),
            },
            TaskEvent::VlmSheetProgress {
                task_id: "t1".into(),
                completed_sheets: 1,
                total_sheets: 3,
                retried_splits: 0,
            },
            TaskEvent::Completed {
                task_id: "t1".into(),
                result: ExtractionResultDto {
                    entries: vec![],
                    metrics: ExtractionMetricsDto::default(),
                    failed_slot_count: Some(0),
                },
            },
            TaskEvent::Cancelled {
                task_id: "t1".into(),
            },
            TaskEvent::Failed {
                task_id: "t1".into(),
                code: "WorkerCrashed".into(),
                message: "boom".into(),
                retryable: true,
            },
        ];

        let json_vals: Vec<serde_json::Value> = events
            .iter()
            .map(|e| serde_json::to_value(e).unwrap())
            .collect();
        assert_eq!(json_vals[0]["type"], "phase");
        assert_eq!(json_vals[0]["sub_phase"], "vlm_sheets");
        assert_eq!(json_vals[1]["type"], "progress");
        assert_eq!(json_vals[2]["type"], "subtitle_entry");
        assert_eq!(json_vals[3]["type"], "timing_slot");
        assert_eq!(json_vals[4]["type"], "vlm_sheet_progress");
        assert_eq!(json_vals[5]["type"], "completed");
        assert_eq!(json_vals[6]["type"], "cancelled");
        assert_eq!(json_vals[7]["type"], "failed");
        assert_eq!(json_vals[7]["retryable"], true);

        for (orig, val) in events.iter().zip(json_vals.iter()) {
            let decoded: TaskEvent = serde_json::from_value(val.clone()).unwrap();
            assert_eq!(&decoded, orig);
        }
    }

    #[test]
    fn provider_type_and_api_key_update_match_ui_design_contract() {
        let openai: ProviderType = serde_json::from_str(r#""openai""#).unwrap();
        assert_eq!(openai, ProviderType::OpenAi);
        assert_eq!(serde_json::to_string(&openai).unwrap(), r#""openai""#);

        let compat: ProviderType = serde_json::from_str(r#""openai_compatible""#).unwrap();
        assert_eq!(compat, ProviderType::OpenAi);

        let keep: ApiKeyUpdate = serde_json::from_str(r#"{"mode":"keep"}"#).unwrap();
        let replace: ApiKeyUpdate =
            serde_json::from_str(r#"{"mode":"replace","value":"abc"}"#).unwrap();
        let clear: ApiKeyUpdate = serde_json::from_str(r#"{"mode":"clear"}"#).unwrap();
        assert_eq!(keep, ApiKeyUpdate::Keep);
        assert_eq!(replace, ApiKeyUpdate::Replace { value: "abc".into() });
        assert_eq!(clear, ApiKeyUpdate::Clear);
    }
}
