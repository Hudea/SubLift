use std::collections::{HashMap, HashSet};

use crate::dto::SubtitleEntry;

use super::schema::{TranscribedSlot, PLACEHOLDER_CATEGORY};
use super::sheet_builder::TimingSlot;

pub const DEFAULT_MAX_MERGE_GAP_MS: i64 = 200;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AssembledCue {
    pub cue_index: u64,
    pub slot_id: u64,
    pub start_ms: i64,
    pub end_ms: i64,
    pub category: String,
    pub text: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FilteredNoiseSlot {
    pub slot_id: u64,
    pub start_ms: i64,
    pub end_ms: i64,
    pub category: String,
    pub lines: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MergedCueGroup {
    pub primary_slot_id: u64,
    pub merged_slot_ids: Vec<u64>,
    pub start_ms: i64,
    pub end_ms: i64,
    pub category: String,
    pub text: String,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct AssemblyOutput {
    pub cues: Vec<AssembledCue>,
    pub filtered_noise: Vec<FilteredNoiseSlot>,
    pub merged_groups: Vec<MergedCueGroup>,
}

pub fn merge_adjacent_cues(
    cues: Vec<AssembledCue>,
    max_gap_ms: i64,
    blocked_merge_slot_ids: &HashSet<u64>,
) -> (Vec<AssembledCue>, Vec<MergedCueGroup>) {
    if cues.is_empty() {
        return (Vec::new(), Vec::new());
    }

    let mut merged: Vec<AssembledCue> = Vec::with_capacity(cues.len());
    let mut merged_groups: Vec<MergedCueGroup> = Vec::new();

    for cue in cues {
        let should_merge = if let Some(prev) = merged.last() {
            let gap_ms = cue.start_ms - prev.end_ms;
            !blocked_merge_slot_ids.contains(&cue.slot_id)
                && prev.text == cue.text
                && !prev.text.is_empty()
                && prev.category == cue.category
                && (-max_gap_ms..=max_gap_ms).contains(&gap_ms)
        } else {
            false
        };

        if should_merge {
            let prev = merged.last_mut().expect("checked non-empty");
            let new_start = prev.start_ms.min(cue.start_ms);
            let new_end = prev.end_ms.max(cue.end_ms);
            prev.start_ms = new_start;
            prev.end_ms = new_end;

            if let Some(last_grp) = merged_groups.last_mut() {
                if last_grp.primary_slot_id == prev.slot_id {
                    last_grp.merged_slot_ids.push(cue.slot_id);
                    last_grp.end_ms = new_end;
                    continue;
                }
            }
            merged_groups.push(MergedCueGroup {
                primary_slot_id: prev.slot_id,
                merged_slot_ids: vec![prev.slot_id, cue.slot_id],
                start_ms: new_start,
                end_ms: new_end,
                category: prev.category.clone(),
                text: prev.text.clone(),
            });
            continue;
        }

        let cue_index = (merged.len() as u64) + 1;
        merged.push(AssembledCue {
            cue_index,
            slot_id: cue.slot_id,
            start_ms: cue.start_ms,
            end_ms: cue.end_ms,
            category: cue.category,
            text: cue.text,
        });
    }

    (merged, merged_groups)
}

pub fn assemble_cues_from_slots(
    timing_slots: &[TimingSlot],
    transcribed_slots: &[TranscribedSlot],
    merge_adjacent: bool,
    max_merge_gap_ms: i64,
) -> Result<AssemblyOutput, String> {
    let mut slot_map: HashMap<u64, &TranscribedSlot> = HashMap::with_capacity(transcribed_slots.len());
    for t_slot in transcribed_slots {
        if slot_map.insert(t_slot.slot_id, t_slot).is_some() {
            return Err(format!("duplicate transcribed slot_id #{}", t_slot.slot_id));
        }
    }

    let missing: Vec<u64> = timing_slots
        .iter()
        .filter(|ts| !slot_map.contains_key(&ts.slot_id))
        .map(|ts| ts.slot_id)
        .collect();
    if !missing.is_empty() {
        return Err(format!(
            "missing transcribed slots for timing slot IDs: {missing:?}"
        ));
    }

    let mut raw_cues: Vec<AssembledCue> = Vec::new();
    let mut filtered_noise: Vec<FilteredNoiseSlot> = Vec::new();
    let mut blocked_merge_slot_ids: HashSet<u64> = HashSet::new();
    let mut saw_intervening_non_subtitle = false;

    for ts in timing_slots {
        let t_slot = slot_map[&ts.slot_id];
        let is_placeholder = t_slot.category == PLACEHOLDER_CATEGORY && !t_slot.lines.is_empty();
        let is_subtitle_cat = matches!(t_slot.category.as_str(), "dialogue" | "bracket_note");

        if (!is_subtitle_cat && !is_placeholder) || t_slot.lines.is_empty() {
            filtered_noise.push(FilteredNoiseSlot {
                slot_id: ts.slot_id,
                start_ms: ts.start_ms,
                end_ms: ts.end_ms,
                category: t_slot.category.clone(),
                lines: t_slot.lines.clone(),
            });
            saw_intervening_non_subtitle = true;
            continue;
        }

        if is_placeholder {
            blocked_merge_slot_ids.insert(ts.slot_id);
            raw_cues.push(AssembledCue {
                cue_index: (raw_cues.len() as u64) + 1,
                slot_id: ts.slot_id,
                start_ms: ts.start_ms,
                end_ms: ts.end_ms,
                category: t_slot.category.clone(),
                text: t_slot.joined_text(),
            });
            saw_intervening_non_subtitle = true;
            continue;
        }

        if saw_intervening_non_subtitle {
            blocked_merge_slot_ids.insert(ts.slot_id);
        }
        saw_intervening_non_subtitle = false;
        raw_cues.push(AssembledCue {
            cue_index: (raw_cues.len() as u64) + 1,
            slot_id: ts.slot_id,
            start_ms: ts.start_ms,
            end_ms: ts.end_ms,
            category: t_slot.category.clone(),
            text: t_slot.joined_text(),
        });
    }

    let (cues, merged_groups) = if merge_adjacent {
        merge_adjacent_cues(raw_cues, max_merge_gap_ms, &blocked_merge_slot_ids)
    } else {
        (raw_cues, Vec::new())
    };

    Ok(AssemblyOutput {
        cues,
        filtered_noise,
        merged_groups,
    })
}

pub fn assembled_cues_to_subtitle_entries(cues: &[AssembledCue]) -> Vec<SubtitleEntry> {
    cues.iter()
        .map(|cue| SubtitleEntry {
            id: cue.cue_index,
            start_ms: cue.start_ms,
            end_ms: cue.end_ms,
            text: cue.text.clone(),
            confidence: if cue.category == PLACEHOLDER_CATEGORY {
                0.0
            } else {
                0.95
            },
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::vlm::schema::PLACEHOLDER_TEXT;

    #[test]
    fn barrier_aware_merge_blocks_across_noise_and_placeholders() {
        let timing = vec![
            TimingSlot::new(41, 0, 1000),
            TimingSlot::new(42, 1000, 1200),
            TimingSlot::new(43, 1200, 2000),
            TimingSlot::new(44, 2000, 3000),
            TimingSlot::new(45, 3000, 3500),
            TimingSlot::new(46, 3500, 4000),
            TimingSlot::new(47, 4000, 4500),
        ];
        let transcribed = vec![
            TranscribedSlot {
                slot_id: 41,
                category: "dialogue".into(),
                lines: vec!["Hello".into()],
            },
            TranscribedSlot {
                slot_id: 42,
                category: "empty".into(),
                lines: vec![],
            },
            TranscribedSlot {
                slot_id: 43,
                category: "dialogue".into(),
                lines: vec!["Hello".into()],
            },
            TranscribedSlot {
                slot_id: 44,
                category: "dialogue".into(),
                lines: vec!["Hello".into()],
            },
            TranscribedSlot {
                slot_id: 45,
                category: PLACEHOLDER_CATEGORY.into(),
                lines: vec![PLACEHOLDER_TEXT.into()],
            },
            TranscribedSlot {
                slot_id: 46,
                category: PLACEHOLDER_CATEGORY.into(),
                lines: vec![PLACEHOLDER_TEXT.into()],
            },
            TranscribedSlot {
                slot_id: 47,
                category: "watermark".into(),
                lines: vec!["LOGO".into()],
            },
        ];

        let out = assemble_cues_from_slots(&timing, &transcribed, true, 200).unwrap();
        // #41 kept separate from #43 because #42 empty blocked it; #43 + #44 merged (0..0 gap); #45 & #46 placeholders never merge; #47 watermark filtered
        assert_eq!(out.cues.len(), 4);
        assert_eq!(out.cues[0].text, "Hello");
        assert_eq!((out.cues[0].start_ms, out.cues[0].end_ms), (0, 1000));
        assert_eq!(out.cues[1].text, "Hello");
        assert_eq!((out.cues[1].start_ms, out.cues[1].end_ms), (1200, 3000));
        assert_eq!(out.cues[2].text, "[待补录]");
        assert_eq!(out.cues[3].text, "[待补录]");
        assert_eq!(out.filtered_noise.len(), 2);

        let entries = assembled_cues_to_subtitle_entries(&out.cues);
        assert_eq!(entries[2].confidence, 0.0);
        assert_eq!(entries[0].confidence, 0.95);
    }
}
