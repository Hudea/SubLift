use std::collections::HashMap;
use std::io::Read;
use std::path::Path;

use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};

use crate::dto::{SourceBox, SubtitleScript};
use crate::storage::{
    compute_video_fingerprint, open_nofollow_read, reject_symlinks_under, write_atomic_private,
    CheckpointRetentionPolicy, CheckpointStore, StorageRoot,
};

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct SheetSlotJson {
    pub id: u64,
    pub category: String,
    pub lines: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct FailedSlotRecord {
    pub slot_id: u64,
    pub start_ms: i64,
    pub end_ms: i64,
    pub reason: String,
    pub resolution: String,
    pub fallback_text: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct SheetBatchRecord {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub model: Option<String>,
    pub sheet: String,
    pub expected_slot_ids: Vec<u64>,
    #[serde(default)]
    pub remapped_local_ids: bool,
    #[serde(default)]
    pub split_rescued: bool,
    #[serde(default)]
    pub hedged_win: bool,
    #[serde(default)]
    pub reasoning_effort_used: String,
    #[serde(default)]
    pub elapsed_s: f64,
    #[serde(default)]
    pub usage: Value,
    #[serde(default)]
    pub degraded_slot_ids: Vec<u64>,
    #[serde(default)]
    pub failed_slots: Vec<FailedSlotRecord>,
    pub slots: Vec<SheetSlotJson>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PartialCheckpointDoc {
    pub model: String,
    pub endpoint: String,
    pub thinking_mode: String,
    pub completed_sheets: usize,
    pub total_sheets: usize,
    pub streaming_timing_complete: bool,
    pub batches: Vec<SheetBatchRecord>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct FinalResponsesDoc {
    pub model: String,
    pub endpoint: String,
    pub thinking_mode: String,
    pub wall_s: f64,
    pub sum_s: f64,
    pub batches: Vec<SheetBatchRecord>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct FailedSlotsDoc {
    pub failed_count: usize,
    pub slots: Vec<FailedSlotRecord>,
}

pub fn compute_vlm_checkpoint_key(
    video_path: &Path,
    region_box: Option<SourceBox>,
    sample_fps: f64,
    script: SubtitleScript,
    model: &str,
) -> String {
    let video_fp = compute_video_fingerprint(video_path)
        .unwrap_or_else(|_| format!("path:{}", video_path.to_string_lossy()));
    let mut hasher = Sha256::new();
    hasher.update(video_fp.as_bytes());
    hasher.update(b"|");
    if let Some(b) = region_box {
        hasher.update(format!("{},{},{},{}", b.x, b.y, b.width, b.height).as_bytes());
    } else {
        hasher.update(b"auto_bottom");
    }
    hasher.update(b"|");
    hasher.update(format!("{sample_fps:.3}").as_bytes());
    hasher.update(b"|");
    let script_tag = match script {
        SubtitleScript::Auto => "auto",
        SubtitleScript::Cjk => "cjk",
        SubtitleScript::Latin => "latin",
    };
    hasher.update(script_tag.as_bytes());
    hasher.update(b"|");
    hasher.update(model.trim().as_bytes());
    hex::encode(hasher.finalize())
}

pub fn load_cached_by_sheet(
    checkpoints: &CheckpointStore,
    root: &StorageRoot,
    key: &str,
    expected_model: &str,
) -> HashMap<String, SheetBatchRecord> {
    let mut cached: HashMap<String, SheetBatchRecord> = HashMap::new();
    let Ok(dir) = checkpoints.vlm_checkpoint_dir(key) else {
        return cached;
    };

    for file_name in ["vlm_responses.json", "vlm_responses.partial.json"] {
        let path = dir.join(file_name);
        if let Ok(Some(mut file)) = open_nofollow_read(root.root(), &path) {
            let mut buf = Vec::new();
            if file.read_to_end(&mut buf).is_ok() && !buf.is_empty() {
                if let Ok(doc) = serde_json::from_slice::<Value>(&buf) {
                    let doc_model = doc.get("model").and_then(Value::as_str).unwrap_or("");
                    if !doc_model.is_empty() && doc_model != expected_model {
                        continue;
                    }
                    if let Some(batches) = doc.get("batches").and_then(Value::as_array) {
                        for b_val in batches {
                            if let Ok(batch) = serde_json::from_value::<SheetBatchRecord>(b_val.clone())
                            {
                                if !batch.sheet.is_empty() && batch.degraded_slot_ids.is_empty() {
                                    cached.entry(batch.sheet.clone()).or_insert(batch);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if let Ok(raw_dir) = checkpoints.vlm_raw_responses_dir(key) {
        if let Ok(entries) = std::fs::read_dir(&raw_dir) {
            let mut files: Vec<_> = entries
                .filter_map(Result::ok)
                .map(|e| e.path())
                .filter(|p| {
                    p.file_name()
                        .and_then(|n| n.to_str())
                        .is_some_and(|n| {
                            n.starts_with("sheet_") && n.ends_with(".json") && !n.starts_with('.')
                        })
                })
                .collect();
            files.sort();
            for raw_file in files {
                if let Ok(Some(mut file)) = open_nofollow_read(root.root(), &raw_file) {
                    let mut buf = Vec::new();
                    if file.read_to_end(&mut buf).is_ok() {
                        if let Ok(batch) = serde_json::from_slice::<SheetBatchRecord>(&buf) {
                            if let Some(m) = batch.model.as_deref() {
                                if !m.is_empty() && m != expected_model {
                                    continue;
                                }
                            }
                            if !batch.sheet.is_empty() && batch.degraded_slot_ids.is_empty() {
                                cached.entry(batch.sheet.clone()).or_insert(batch);
                            }
                        }
                    }
                }
            }
        }
    }

    cached
}

pub fn write_sheet_raw_response(
    checkpoints: &CheckpointStore,
    root: &StorageRoot,
    key: &str,
    batch: &SheetBatchRecord,
    model: &str,
) -> Result<(), String> {
    let sheet_name = batch.sheet.trim();
    if sheet_name.is_empty() {
        return Ok(());
    }
    let stem = Path::new(sheet_name)
        .file_stem()
        .and_then(|s| s.to_str())
        .filter(|s| !s.is_empty())
        .unwrap_or("sheet");
    let raw_dir = checkpoints
        .vlm_raw_responses_dir(key)
        .map_err(|e| e.to_string())?;
    let mut payload = batch.clone();
    if payload.model.as_deref().unwrap_or("").is_empty() {
        payload.model = Some(model.to_string());
    }
    let bytes = serde_json::to_vec_pretty(&payload).map_err(|e| e.to_string())?;
    let target = raw_dir.join(format!("{stem}.json"));
    write_atomic_private(root.root(), &target, &bytes).map_err(|e| e.to_string())
}

pub fn write_partial_checkpoint(
    checkpoints: &CheckpointStore,
    key: &str,
    doc: &PartialCheckpointDoc,
) -> Result<(), String> {
    let bytes = serde_json::to_vec_pretty(doc).map_err(|e| e.to_string())?;
    checkpoints
        .write_vlm_partial_atomic(key, &bytes)
        .map_err(|e| e.to_string())
}

pub fn finalize_checkpoint_files(
    checkpoints: &CheckpointStore,
    root: &StorageRoot,
    key: &str,
    doc: &FinalResponsesDoc,
    batches: &[SheetBatchRecord],
) -> Result<Vec<FailedSlotRecord>, String> {
    let dir = checkpoints
        .vlm_checkpoint_dir(key)
        .map_err(|e| e.to_string())?;
    let out_bytes = serde_json::to_vec_pretty(doc).map_err(|e| e.to_string())?;
    let out_target = dir.join("vlm_responses.json");
    write_atomic_private(root.root(), &out_target, &out_bytes).map_err(|e| e.to_string())?;

    let mut all_failed: Vec<FailedSlotRecord> = batches
        .iter()
        .flat_map(|b| b.failed_slots.iter().cloned())
        .collect();
    all_failed.sort_by_key(|r| r.slot_id);

    let failed_target = dir.join("failed_slots.json");
    if all_failed.is_empty() {
        if failed_target.exists() {
            reject_symlinks_under(root.root(), &failed_target).map_err(|e| e.to_string())?;
            let _ = std::fs::remove_file(&failed_target);
        }
    } else {
        let failed_doc = FailedSlotsDoc {
            failed_count: all_failed.len(),
            slots: all_failed.clone(),
        };
        let failed_bytes = serde_json::to_vec_pretty(&failed_doc).map_err(|e| e.to_string())?;
        write_atomic_private(root.root(), &failed_target, &failed_bytes)
            .map_err(|e| e.to_string())?;
    }

    if let Ok(partial_path) = checkpoints.vlm_partial_path(key) {
        if partial_path.exists() {
            reject_symlinks_under(root.root(), &partial_path).map_err(|e| e.to_string())?;
            let _ = std::fs::remove_file(&partial_path);
        }
    }

    let _ = checkpoints.prune_vlm_checkpoints(CheckpointRetentionPolicy::default(), Some(key));
    Ok(all_failed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use tempfile::TempDir;

    fn sample_batch(sheet: &str, ids: Vec<u64>, degraded: Vec<u64>, model: Option<&str>) -> SheetBatchRecord {
        SheetBatchRecord {
            model: model.map(ToOwned::to_owned),
            sheet: sheet.into(),
            expected_slot_ids: ids.clone(),
            remapped_local_ids: false,
            split_rescued: false,
            hedged_win: false,
            reasoning_effort_used: "low".into(),
            elapsed_s: 0.42,
            usage: json!({"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}),
            degraded_slot_ids: degraded.clone(),
            failed_slots: degraded
                .iter()
                .map(|&id| FailedSlotRecord {
                    slot_id: id,
                    start_ms: 0,
                    end_ms: 500,
                    reason: "content_filter".into(),
                    resolution: "placeholder".into(),
                    fallback_text: "[待补录]".into(),
                })
                .collect(),
            slots: ids
                .into_iter()
                .map(|id| SheetSlotJson {
                    id,
                    category: "dialogue".into(),
                    lines: vec!["hi".into()],
                })
                .collect(),
        }
    }

    #[test]
    fn checkpoint_load_skips_model_mismatch_and_degraded_batches() {
        let dir = TempDir::new().unwrap();
        let root = StorageRoot::new(dir.path().join("appdata")).unwrap();
        let ckpts = CheckpointStore::new(root.clone());
        let key = "ckpt_resume_0001";

        let partial = PartialCheckpointDoc {
            model: "MiniMax-M3.1-Flash".into(),
            endpoint: "https://api.minimax.cn/v1/chat/completions".into(),
            thinking_mode: "auto".into(),
            completed_sheets: 2,
            total_sheets: 2,
            streaming_timing_complete: false,
            batches: vec![
                sample_batch("sheet_01.png", vec![1, 2], vec![], None),
                sample_batch("sheet_02.png", vec![3, 4], vec![3], None),
            ],
        };
        write_partial_checkpoint(&ckpts, key, &partial).unwrap();

        // Matching model hits sheet_01, excludes degraded sheet_02
        let loaded = load_cached_by_sheet(&ckpts, &root, key, "MiniMax-M3.1-Flash");
        assert!(loaded.contains_key("sheet_01.png"));
        assert!(!loaded.contains_key("sheet_02.png"));

        // Mismatched model skips entire cache
        let mismatched = load_cached_by_sheet(&ckpts, &root, key, "other-model");
        assert!(mismatched.is_empty());

        // Finalize with no failed slots removes .partial.json and failed_slots.json
        let final_doc = FinalResponsesDoc {
            model: "MiniMax-M3.1-Flash".into(),
            endpoint: "https://api.minimax.cn/v1/chat/completions".into(),
            thinking_mode: "auto".into(),
            wall_s: 1.0,
            sum_s: 0.8,
            batches: vec![sample_batch("sheet_01.png", vec![1, 2], vec![], None)],
        };
        let failed = finalize_checkpoint_files(&ckpts, &root, key, &final_doc, &final_doc.batches).unwrap();
        assert!(failed.is_empty());
        assert!(ckpts.read_vlm_partial(key).unwrap().is_none());
        assert!(ckpts.vlm_checkpoint_dir(key).unwrap().join("vlm_responses.json").is_file());
    }

    #[test]
    fn corrupt_partial_falls_back_to_raw_responses_and_finalize_clears_stale_failed_slots() {
        let dir = TempDir::new().unwrap();
        let root = StorageRoot::new(dir.path().join("appdata")).unwrap();
        let ckpts = CheckpointStore::new(root.clone());
        let key = "ckpt_corrupt_partial";

        // 1. Write valid raw_responses/sheet_01.json
        let s1 = sample_batch("sheet_01.png", vec![1, 2], vec![], Some("MiniMax-M3.1-Flash"));
        write_sheet_raw_response(&ckpts, &root, key, &s1, "MiniMax-M3.1-Flash").unwrap();

        // 2. Write corrupt/truncated vlm_responses.partial.json
        ckpts.write_vlm_partial_atomic(key, b"{truncated_partial").unwrap();

        // 3. load_cached_by_sheet recovers sheet_01 from raw_responses
        let recovered = load_cached_by_sheet(&ckpts, &root, key, "MiniMax-M3.1-Flash");
        assert!(recovered.contains_key("sheet_01.png"));
        assert_eq!(recovered["sheet_01.png"].slots.len(), 2);

        // 4. Stale failed_slots.json is removed when finalized with 0 failed slots
        let failed_path = ckpts.vlm_checkpoint_dir(key).unwrap().join("failed_slots.json");
        write_atomic_private(root.root(), &failed_path, b"{\"failed_count\":1,\"slots\":[]}").unwrap();
        assert!(failed_path.is_file());

        let final_doc = FinalResponsesDoc {
            model: "MiniMax-M3.1-Flash".into(),
            endpoint: "https://api.minimax.cn/v1/chat/completions".into(),
            thinking_mode: "auto".into(),
            wall_s: 1.0,
            sum_s: 0.5,
            batches: vec![s1],
        };
        let failed = finalize_checkpoint_files(&ckpts, &root, key, &final_doc, &final_doc.batches).unwrap();
        assert!(failed.is_empty());
        assert!(!failed_path.exists());
    }
}
