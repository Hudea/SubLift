use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;

use regex::Regex;
use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::dto::SubtitleScript;

pub const PLACEHOLDER_CATEGORY: &str = "placeholder";
pub const PLACEHOLDER_TEXT: &str = "[待补录]";

static THINK_BLOCK_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?is)<think>.*?</think>").expect("valid think regex"));
static FENCE_START_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?i)^```(?:json)?\s*").expect("valid fence start regex"));
static FENCE_END_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"\s*```$").expect("valid fence end regex"));
static ELLIPSIS_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"\.{3,}|…{2,}").expect("valid ellipsis regex"));
static OPEN_PAREN_SPACE_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"（\s+").expect("valid open paren regex"));
static CLOSE_PAREN_SPACE_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"\s+）").expect("valid close paren regex"));
static MULTI_SPACE_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r" {2,}").expect("valid multi space regex"));

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct TranscribedSlot {
    pub slot_id: u64,
    pub category: String,
    pub lines: Vec<String>,
}

impl TranscribedSlot {
    pub fn joined_text(&self) -> String {
        self.lines.join("\n")
    }
}

pub fn extract_json_object(raw_text: &str) -> Result<Value, String> {
    let mut cleaned = THINK_BLOCK_RE.replace_all(raw_text, "").trim().to_string();
    if cleaned.starts_with("```") {
        cleaned = FENCE_START_RE.replace(&cleaned, "").to_string();
        cleaned = FENCE_END_RE.replace(&cleaned, "").trim().to_string();
    }

    let parsed: Value = match serde_json::from_str(&cleaned) {
        Ok(v) => v,
        Err(_) => {
            let start = cleaned.find('{');
            let end = cleaned.rfind('}');
            match (start, end) {
                (Some(s), Some(e)) if e > s => serde_json::from_str(&cleaned[s..=e])
                    .map_err(|err| format!("failed to parse extracted JSON object: {err}"))?,
                _ => {
                    let preview: String = cleaned.chars().take(160).collect();
                    return Err(format!("no JSON object found in model output: {preview:?}"));
                }
            }
        }
    };

    if !parsed.is_object() {
        return Err("top-level JSON response must be an object".into());
    }
    Ok(parsed)
}

pub fn validate_payload_against_schema(
    raw_payload: &Value,
    allow_placeholder: bool,
) -> Result<(), String> {
    let obj = raw_payload
        .as_object()
        .ok_or_else(|| "VLM response JSON must be an object containing key 'slots'".to_string())?;

    if !obj.contains_key("slots") {
        return Err("VLM response JSON missing required key 'slots'".into());
    }
    let extra_top: Vec<&str> = obj
        .keys()
        .map(String::as_str)
        .filter(|k| *k != "slots")
        .collect();
    if !extra_top.is_empty() {
        return Err(format!(
            "unexpected top-level keys in VLM JSON: {extra_top:?}"
        ));
    }

    let raw_slots = obj
        .get("slots")
        .and_then(Value::as_array)
        .ok_or_else(|| "'slots' field must be an array".to_string())?;

    for (idx, item) in raw_slots.iter().enumerate() {
        let slot_obj = item
            .as_object()
            .ok_or_else(|| format!("slot entry at index {idx} is not an object"))?;

        for req_key in ["id", "category", "lines"] {
            if !slot_obj.contains_key(req_key) {
                return Err(format!(
                    "slot entry at index {idx} missing required key '{req_key}'"
                ));
            }
        }
        let extra_item: Vec<&str> = slot_obj
            .keys()
            .map(String::as_str)
            .filter(|k| !matches!(*k, "id" | "category" | "lines"))
            .collect();
        if !extra_item.is_empty() {
            return Err(format!(
                "slot entry at index {idx} has unexpected keys: {extra_item:?}"
            ));
        }

        // Strict integer >= 1 (Value::as_i64 rejects booleans, strings, floats)
        let raw_id = slot_obj.get("id").and_then(Value::as_i64).ok_or_else(|| {
            format!(
                "invalid slot 'id' {:?} at index {idx}: must be an integer >= 1",
                slot_obj.get("id")
            )
        })?;
        if raw_id < 1 {
            return Err(format!(
                "invalid slot 'id' {raw_id} at index {idx}: must be >= 1"
            ));
        }

        let raw_cat = slot_obj
            .get("category")
            .and_then(Value::as_str)
            .map(str::trim)
            .ok_or_else(|| format!("invalid category at slot #{raw_id}"))?;
        let valid_cat = matches!(
            raw_cat,
            "dialogue" | "bracket_note" | "watermark" | "scene_text" | "empty"
        ) || (allow_placeholder && raw_cat == PLACEHOLDER_CATEGORY);
        if !valid_cat {
            return Err(format!("invalid category '{raw_cat}' at slot #{raw_id}"));
        }

        let raw_lines = slot_obj
            .get("lines")
            .and_then(Value::as_array)
            .ok_or_else(|| format!("'lines' must be an array at slot #{raw_id}"))?;
        for (line_idx, ln) in raw_lines.iter().enumerate() {
            if !ln.is_string() {
                return Err(format!(
                    "line #{line_idx} at slot #{raw_id} must be a string"
                ));
            }
        }
    }

    Ok(())
}

fn contains_cjk(text: &str) -> bool {
    text.chars().any(|ch| {
        matches!(
            ch as u32,
            0x4E00..=0x9FFF | 0x3400..=0x4DBF | 0x3000..=0x303F | 0xFF00..=0xFFEF
        )
    })
}

pub fn normalize_subtitle_line(line: &str, script: SubtitleScript) -> String {
    let trimmed = line.trim();
    if trimmed.is_empty() {
        return String::new();
    }
    let mut cleaned = ELLIPSIS_RE.replace_all(trimmed, "…").into_owned();
    let use_cjk_punct = match script {
        SubtitleScript::Cjk => true,
        SubtitleScript::Latin => false,
        SubtitleScript::Auto => contains_cjk(&cleaned),
    };
    if use_cjk_punct {
        cleaned = cleaned
            .replace('(', "（")
            .replace(')', "）")
            .replace('?', "？")
            .replace('!', "！");
        cleaned = OPEN_PAREN_SPACE_RE.replace_all(&cleaned, "（").into_owned();
        cleaned = CLOSE_PAREN_SPACE_RE.replace_all(&cleaned, "）").into_owned();
    }
    MULTI_SPACE_RE.replace_all(&cleaned, " ").into_owned()
}

pub fn validate_and_align_sheet_slots(
    raw_payload: &Value,
    expected_slot_ids: &[u64],
    script: SubtitleScript,
    allow_placeholder: bool,
) -> Result<(Vec<TranscribedSlot>, bool), String> {
    validate_payload_against_schema(raw_payload, allow_placeholder)?;
    let raw_slots = raw_payload["slots"]
        .as_array()
        .ok_or_else(|| "'slots' must be an array".to_string())?;

    if raw_slots.len() != expected_slot_ids.len() {
        return Err(format!(
            "slot count mismatch: expected {} slots {expected_slot_ids:?}, got {}",
            expected_slot_ids.len(),
            raw_slots.len()
        ));
    }

    let returned_ids: Vec<u64> = raw_slots
        .iter()
        .map(|item| item["id"].as_i64().unwrap_or(0) as u64)
        .collect();
    let local_seq: Vec<u64> = (1..=(expected_slot_ids.len() as u64)).collect();

    let mut remapped = false;
    let aligned: Vec<(u64, &Value)> = if returned_ids == expected_slot_ids {
        expected_slot_ids
            .iter()
            .copied()
            .zip(raw_slots.iter())
            .collect()
    } else if returned_ids == local_seq {
        remapped = true;
        expected_slot_ids
            .iter()
            .copied()
            .zip(raw_slots.iter())
            .collect()
    } else {
        let unique_ret: HashSet<u64> = returned_ids.iter().copied().collect();
        let expected_set: HashSet<u64> = expected_slot_ids.iter().copied().collect();
        if unique_ret.len() == returned_ids.len() && unique_ret == expected_set {
            let by_id: HashMap<u64, &Value> = returned_ids
                .iter()
                .copied()
                .zip(raw_slots.iter())
                .collect();
            expected_slot_ids
                .iter()
                .map(|sid| (*sid, by_id[sid]))
                .collect()
        } else {
            return Err(format!(
                "returned slot IDs {returned_ids:?} do not match expected {expected_slot_ids:?}"
            ));
        }
    };

    let mut validated = Vec::with_capacity(aligned.len());
    for (sid, item) in aligned {
        let cat = item["category"].as_str().unwrap_or("").trim().to_string();
        let lines = if cat == "empty" {
            Vec::new()
        } else {
            item["lines"]
                .as_array()
                .map(|arr| {
                    arr.iter()
                        .filter_map(Value::as_str)
                        .map(|ln| normalize_subtitle_line(ln, script))
                        .filter(|ln| !ln.is_empty())
                        .collect()
                })
                .unwrap_or_default()
        };
        validated.push(TranscribedSlot {
            slot_id: sid,
            category: cat,
            lines,
        });
    }

    Ok((validated, remapped))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn extract_json_strips_think_and_fences() {
        let raw = "<think>\nreasoning...\n</think>\n```json\n{\"slots\":[{\"id\":1,\"category\":\"dialogue\",\"lines\":[\"你好\"]}]}\n```";
        let obj = extract_json_object(raw).unwrap();
        assert_eq!(obj["slots"][0]["id"], 1);
    }

    #[test]
    fn schema_rejects_bool_float_zero_id_and_unknown_keys() {
        assert!(validate_payload_against_schema(
            &json!({"slots": [{"id": true, "category": "dialogue", "lines": ["x"]}]}),
            false
        )
        .is_err());
        assert!(validate_payload_against_schema(
            &json!({"slots": [{"id": 1.0, "category": "dialogue", "lines": ["x"]}]}),
            false
        )
        .is_err());
        assert!(validate_payload_against_schema(
            &json!({"slots": [{"id": 0, "category": "dialogue", "lines": ["x"]}]}),
            false
        )
        .is_err());
        assert!(validate_payload_against_schema(
            &json!({"slots": [], "extra": 1}),
            false
        )
        .is_err());
        assert!(validate_payload_against_schema(
            &json!({"slots": [{"id": 1, "category": "bogus", "lines": []}]}),
            false
        )
        .is_err());
        assert!(validate_payload_against_schema(
            &json!({"slots": [{"id": 1, "category": "placeholder", "lines": ["[待补录]"]}]}),
            false
        )
        .is_err());
        assert!(validate_payload_against_schema(
            &json!({"slots": [{"id": 1, "category": "placeholder", "lines": ["[待补录]"]}]}),
            true
        )
        .is_ok());
    }

    #[test]
    fn cjk_vs_latin_normalization_preserves_single_internal_spaces() {
        assert_eq!(
            normalize_subtitle_line("哦  对?!", SubtitleScript::Auto),
            "哦 对？！"
        );
        assert_eq!(
            normalize_subtitle_line("( 前情提要... )", SubtitleScript::Cjk),
            "（前情提要…）"
        );
        assert_eq!(
            normalize_subtitle_line("Wait,  what?!", SubtitleScript::Auto),
            "Wait, what?!"
        );
        assert_eq!(
            normalize_subtitle_line("哦 对", SubtitleScript::Auto),
            "哦 对"
        );
    }

    #[test]
    fn validate_and_align_handles_local_id_remap_and_permutation() {
        let payload = json!({
            "slots": [
                {"id": 1, "category": "dialogue", "lines": ["A"]},
                {"id": 2, "category": "empty", "lines": ["noise should clear"]},
                {"id": 3, "category": "dialogue", "lines": ["B"]},
                {"id": 4, "category": "dialogue", "lines": ["B"]}
            ]
        });
        let (slots, remapped) =
            validate_and_align_sheet_slots(&payload, &[41, 42, 43, 44], SubtitleScript::Auto, false)
                .unwrap();
        assert!(remapped);
        assert_eq!(slots[0].slot_id, 41);
        assert_eq!(slots[1].slot_id, 42);
        assert!(slots[1].lines.is_empty());

        // Permutation reorder does not set remapped
        let permuted = json!({
            "slots": [
                {"id": 42, "category": "empty", "lines": []},
                {"id": 41, "category": "dialogue", "lines": ["A"]}
            ]
        });
        let (reordered, remapped2) =
            validate_and_align_sheet_slots(&permuted, &[41, 42], SubtitleScript::Auto, false)
                .unwrap();
        assert!(!remapped2);
        assert_eq!(reordered[0].slot_id, 41);
        assert_eq!(reordered[1].slot_id, 42);
    }

    #[test]
    fn rejects_duplicate_ids_and_count_mismatch_and_invalid_id_types() {
        let dup = json!({
            "slots": [
                {"id": 1, "category": "dialogue", "lines": ["A"]},
                {"id": 1, "category": "dialogue", "lines": ["B"]}
            ]
        });
        assert!(validate_and_align_sheet_slots(&dup, &[1, 2], SubtitleScript::Auto, false).is_err());

        let missing = json!({
            "slots": [{"id": 1, "category": "dialogue", "lines": ["A"]}]
        });
        assert!(validate_and_align_sheet_slots(&missing, &[1, 2], SubtitleScript::Auto, false).is_err());

        let extra = json!({
            "slots": [
                {"id": 1, "category": "dialogue", "lines": ["A"]},
                {"id": 2, "category": "dialogue", "lines": ["B"]},
                {"id": 3, "category": "dialogue", "lines": ["C"]}
            ]
        });
        assert!(validate_and_align_sheet_slots(&extra, &[1, 2], SubtitleScript::Auto, false).is_err());

        let neg = json!({"slots": [{"id": -5, "category": "dialogue", "lines": ["A"]}]});
        assert!(validate_and_align_sheet_slots(&neg, &[1], SubtitleScript::Auto, false).is_err());

        let str_id = json!({"slots": [{"id": "1", "category": "dialogue", "lines": ["A"]}]});
        assert!(validate_and_align_sheet_slots(&str_id, &[1], SubtitleScript::Auto, false).is_err());
    }
}
