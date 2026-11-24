use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::UNIX_EPOCH;

use http::{header, Method, Request, StatusCode};
use sublift_desktop_lib::commands::export::export_srt_impl;
use sublift_desktop_lib::dto::{
    ExtractionConfigDto, ExtractionMode, ExtractionSubPhase, ReRecognizeCategory,
    ReRecognizeItemInput, SourceBox, StartExtractionParams, SubtitleScript, TaskEvent,
};
use sublift_desktop_lib::media::{build_media_url, MediaStreamManager};
use sublift_desktop_lib::storage::{CheckpointStore, ProviderStore, StorageRoot};
use sublift_desktop_lib::vlm::checkpoint::{compute_vlm_checkpoint_key, FinalResponsesDoc};
use sublift_desktop_lib::vlm::{NativeVlmService, VlmService};
use sublift_desktop_lib::worker::{WorkerBackend, WorkerConfig, WorkerEventSink, WorkerSupervisor};
use tempfile::TempDir;

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("desktop dir")
        .parent()
        .expect("repo root")
        .to_path_buf()
}

fn snapshot_dir(dir: &Path) -> BTreeMap<PathBuf, (u64, i128)> {
    let mut out = BTreeMap::new();
    let Ok(entries) = std::fs::read_dir(dir) else {
        return out;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let rel = path.strip_prefix(dir).unwrap_or(&path).to_path_buf();
        if let Ok(meta) = std::fs::symlink_metadata(&path) {
            let mtime_ns = meta
                .modified()
                .ok()
                .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
                .map(|d| d.as_nanos() as i128)
                .unwrap_or(0);
            out.insert(rel, (meta.len(), mtime_ns));
        }
    }
    out
}

fn count_dir_entries(dir: &Path) -> usize {
    std::fs::read_dir(dir)
        .map(|rd| rd.flatten().count())
        .unwrap_or(0)
}

fn contains_cjk(text: &str) -> bool {
    text.chars()
        .any(|ch| ('\u{4E00}'..='\u{9FFF}').contains(&ch) || ('\u{3400}'..='\u{4DBF}').contains(&ch))
}

fn batch_total_tokens(usage: &serde_json::Value) -> u64 {
    let total = usage.get("total_tokens").and_then(|v| v.as_u64()).unwrap_or(0);
    if total > 0 {
        return total;
    }
    let prompt = usage
        .get("prompt_tokens")
        .or_else(|| usage.get("input_tokens"))
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    let completion = usage
        .get("completion_tokens")
        .or_else(|| usage.get("output_tokens"))
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    prompt.saturating_add(completion)
}

#[ignore = "Live VLM smoke test hits real .env endpoints and debug/smoke-test-data/ media"]
#[tokio::test]
async fn live_vlm_smoke_test_end_to_end() {
    let root = repo_root();
    let env_path = root.join(".env");
    assert!(env_path.is_file(), "workspace .env must exist");

    let smoke_dir = root.join("debug/smoke-test-data");
    let mp4_path = smoke_dir.join("Zootopia_clip_1080p.mp4");
    let mkv_path = smoke_dir.join("Zootopia_clip_hardsub1.mkv");
    assert!(mp4_path.is_file(), "Zootopia_clip_1080p.mp4 must exist");
    assert!(mkv_path.is_file(), "Zootopia_clip_hardsub1.mkv must exist");

    let snap_before = snapshot_dir(&smoke_dir);

    let appdata_tmp = TempDir::new().unwrap();
    let out_tmp = TempDir::new().unwrap();
    let storage = StorageRoot::new(appdata_tmp.path().join("appdata")).unwrap();
    let providers = Arc::new(ProviderStore::new(storage.clone()));
    let checkpoints = Arc::new(CheckpointStore::new(storage.clone()));

    // Stage A: .env import, permissions, and key masking (ZERO secret logging)
    let imported = providers
        .import_providers_from_env(Some(&env_path))
        .expect("import_providers_from_env must succeed");
    let mm = imported
        .iter()
        .find(|p| p.id == "env-minimax")
        .expect("env-minimax profile must be imported");
    let nv = imported
        .iter()
        .find(|p| p.id == "env-nvidia")
        .expect("env-nvidia profile must be imported");

    assert!(mm.is_default, "env-minimax should become default on empty store");
    assert_eq!(mm.model, "MiniMax-M3.1-Flash");
    assert!(mm.has_api_key);
    assert!(mm.masked_api_key.contains("****"));

    assert_eq!(nv.model, "nvidia/nemotron-3.5-super-vl-120b-a12b");
    assert!(nv.has_api_key);
    assert!(nv.masked_api_key.contains("****"));

    // Verify in-memory without formatting raw keys into panic messages
    let mm_secret = providers
        .resolve_provider_secret(Some("env-minimax"))
        .expect("resolve minimax secret");
    assert!(
        !mm.masked_api_key.contains(mm_secret.api_key.trim()),
        "masked DTO must never contain raw minimax API key"
    );
    let nv_secret = providers
        .resolve_provider_secret(Some("env-nvidia"))
        .expect("resolve nvidia secret");
    assert!(
        !nv.masked_api_key.contains(nv_secret.api_key.trim()),
        "masked DTO must never contain raw nvidia API key"
    );
    drop(mm_secret);
    drop(nv_secret);

    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let root_mode = std::fs::metadata(storage.root()).unwrap().permissions().mode() & 0o777;
        assert_eq!(root_mode, 0o700, "StorageRoot must be 0o700");
        let prov_mode = std::fs::metadata(storage.providers_file())
            .unwrap()
            .permissions()
            .mode()
            & 0o777;
        assert_eq!(prov_mode, 0o600, "providers.json must be 0o600");
    }

    // Stage B: Real NativeVlmService extraction on Zootopia_clip_1080p.mp4 + finalize_srt export
    let worker = Arc::new(WorkerSupervisor::new(WorkerConfig {
        repo_root: Some(root.clone()),
        ..Default::default()
    }));
    let vlm = NativeVlmService::new(
        Arc::clone(&worker) as Arc<dyn WorkerBackend>,
        Arc::clone(&providers),
        Arc::clone(&checkpoints),
        storage.clone(),
    );

    let events: Arc<Mutex<Vec<TaskEvent>>> = Arc::new(Mutex::new(Vec::new()));
    let events_sink = Arc::clone(&events);
    let sink: WorkerEventSink = Arc::new(move |ev| {
        events_sink.lock().expect("events lock").push(ev);
    });

    let roi = SourceBox {
        x: 0,
        y: 848,
        width: 1920,
        height: 87,
    };
    let mp4_str = mp4_path.to_string_lossy().to_string();
    let extraction = vlm
        .run_vlm_extraction(
            "live-vlm-smoke",
            StartExtractionParams {
                video_path: mp4_str.clone(),
                mode: ExtractionMode::PureTimingStrips,
                engine: None,
                provider_id: Some("env-minimax".into()),
                region_box: Some(roi),
                config: ExtractionConfigDto {
                    sample_fps: 5.0,
                    ..Default::default()
                },
                script: SubtitleScript::Cjk,
            },
            sink,
        )
        .await
        .expect("Stage B NativeVlmService::run_vlm_extraction must succeed");

    assert_eq!(
        extraction.failed_slot_count,
        Some(0),
        "Stage B must finish with zero failed VLM slots (no placeholder or local-OCR fallback)"
    );
    assert!(
        extraction.entries.len() >= 60,
        "Stage B must produce >= 60 genuine VLM cues on Zootopia_clip_1080p.mp4, got {}",
        extraction.entries.len()
    );
    for entry in &extraction.entries {
        assert!(entry.end_ms > entry.start_ms, "entry end_ms must exceed start_ms");
        assert!(!entry.text.trim().is_empty(), "entry text must be non-empty");
        assert!(
            !entry.text.contains("[待补录]") && entry.confidence > 0.0,
            "Stage B must not emit placeholder or zero-confidence fallback entry: {entry:?}"
        );
    }

    let cjk_cues = extraction
        .entries
        .iter()
        .filter(|e| contains_cjk(&e.text))
        .count();
    assert!(
        cjk_cues >= 50,
        "Stage B must recognize >= 50 CJK subtitle cues on Zootopia_clip_1080p.mp4, got {cjk_cues} out of {}",
        extraction.entries.len()
    );
    let joined_text: String = extraction
        .entries
        .iter()
        .map(|e| e.text.as_str())
        .collect::<Vec<_>>()
        .join("\n");
    let expected_terms = [
        "我流血",
        "可以收工",
        "动物方城市",
        "搭档",
        "狐狸",
        "检查员",
    ];
    let matched_terms: Vec<&str> = expected_terms
        .iter()
        .copied()
        .filter(|term| joined_text.contains(term))
        .collect();
    assert!(
        matched_terms.len() >= 4,
        "Stage B must recognize genuine Zootopia Simplified Chinese dialogue (matched {matched_terms:?})"
    );

    {
        let recorded = events.lock().expect("events lock");
        let has_phase = |target: ExtractionSubPhase| {
            recorded.iter().any(|e| {
                matches!(e, TaskEvent::Phase { sub_phase, .. } if *sub_phase == target)
            })
        };
        assert!(has_phase(ExtractionSubPhase::Probing), "missing Probing phase");
        assert!(has_phase(ExtractionSubPhase::Timing), "missing Timing phase");
        assert!(has_phase(ExtractionSubPhase::VlmSheets), "missing VlmSheets phase");
        assert!(has_phase(ExtractionSubPhase::Finalizing), "missing Finalizing phase");
        assert!(
            recorded
                .iter()
                .any(|e| matches!(e, TaskEvent::VlmSheetProgress { .. })),
            "missing VlmSheetProgress event"
        );
        assert!(
            recorded
                .iter()
                .any(|e| matches!(e, TaskEvent::SubtitleEntry { .. })),
            "missing SubtitleEntry events"
        );
    }

    let ckpt_key = compute_vlm_checkpoint_key(
        &mp4_path,
        Some(roi),
        5.0,
        SubtitleScript::Cjk,
        "MiniMax-M3.1-Flash",
    );
    let ckpt_dir = checkpoints.vlm_checkpoint_dir(&ckpt_key).unwrap();
    let vlm_responses_path = ckpt_dir.join("vlm_responses.json");
    assert!(
        vlm_responses_path.is_file(),
        "vlm_responses.json must exist after finalization"
    );
    assert!(
        !ckpt_dir.join("failed_slots.json").exists(),
        "failed_slots.json must not exist when zero slots failed"
    );
    assert!(
        !ckpt_dir.join("vlm_responses.partial.json").exists(),
        "vlm_responses.partial.json must be removed after finalization"
    );

    let final_doc_bytes = std::fs::read(&vlm_responses_path).expect("read vlm_responses.json");
    let final_doc: FinalResponsesDoc = serde_json::from_slice(&final_doc_bytes)
        .expect("vlm_responses.json must deserialize as FinalResponsesDoc");
    assert_eq!(final_doc.model, "MiniMax-M3.1-Flash");
    assert!(
        !final_doc.batches.is_empty(),
        "vlm_responses.json must contain non-empty VLM batches"
    );
    let mut total_vlm_slots = 0usize;
    let mut total_vlm_tokens = 0u64;
    for batch in &final_doc.batches {
        assert!(
            batch.failed_slots.is_empty(),
            "sheet {} recorded failed_slots: {:?}",
            batch.sheet,
            batch.failed_slots
        );
        assert!(
            batch.degraded_slot_ids.is_empty(),
            "sheet {} recorded degraded_slot_ids: {:?}",
            batch.sheet,
            batch.degraded_slot_ids
        );
        assert_ne!(
            batch.reasoning_effort_used, "fallback",
            "sheet {} fell back to leaf fallback instead of real VLM",
            batch.sheet
        );
        assert!(
            !batch.slots.is_empty(),
            "sheet {} must contain transcribed VLM slots",
            batch.sheet
        );
        let batch_tokens = batch_total_tokens(&batch.usage);
        assert!(
            batch_tokens > 0,
            "sheet {} must record non-zero VLM token usage, got {:?}",
            batch.sheet,
            batch.usage
        );
        total_vlm_slots += batch.slots.len();
        total_vlm_tokens = total_vlm_tokens.saturating_add(batch_tokens);
    }
    assert!(
        total_vlm_slots >= 60,
        "vlm_responses.json must record >= 60 transcribed VLM slots, got {total_vlm_slots}"
    );
    assert!(
        total_vlm_tokens > 0,
        "vlm_responses.json must record non-zero total token usage"
    );

    assert_eq!(
        count_dir_entries(&storage.preview_cache_dir()),
        0,
        "preview_cache/ must be clean after Stage B"
    );

    let out_srt = out_tmp.path().join("zootopia_live_vlm.srt");
    let export_res = export_srt_impl(
        &*worker,
        out_srt.to_str().unwrap(),
        extraction.entries.clone(),
        false,
        None,
    )
    .await
    .expect("Stage B export_srt_impl must succeed");
    assert_eq!(export_res.cue_count, extraction.entries.len());
    let srt_content = std::fs::read_to_string(&out_srt).expect("read exported srt");
    assert!(srt_content.contains(" --> "), "exported SRT must contain cue timestamps");
    assert_eq!(
        snapshot_dir(&smoke_dir),
        snap_before,
        "debug/smoke-test-data/ must not be modified by Stage B"
    );

    // Stage C: Real re_recognize_entries live VLM partial re-recognition (MiniMax + NVIDIA)
    assert!(
        extraction.entries.len() >= 2,
        "Stage B must produce >= 2 genuine cues for Stage C re_recognize_entries"
    );
    let mm_items: Vec<ReRecognizeItemInput> = extraction
        .entries
        .iter()
        .take(2)
        .map(|e| ReRecognizeItemInput {
            cue_id: e.id,
            start_ms: e.start_ms,
            end_ms: e.end_ms,
        })
        .collect();

    let mm_reocr = vlm
        .re_recognize_entries(
            &mp4_str,
            Some(roi),
            mm_items.clone(),
            Some("env-minimax".into()),
            SubtitleScript::Cjk,
        )
        .await
        .expect("Stage C MiniMax re_recognize_entries must succeed");
    assert_eq!(mm_reocr.len(), mm_items.len());
    for res in &mm_reocr {
        assert!(res.ok, "MiniMax re_recognize_entries failed: {res:?}");
        assert!(matches!(
            res.category,
            Some(ReRecognizeCategory::Dialogue | ReRecognizeCategory::BracketNote)
        ));
        assert!(
            res.text.as_deref().is_some_and(|t| !t.trim().is_empty()),
            "MiniMax re_recognize_entries returned empty text: {res:?}"
        );
        assert_eq!(res.confidence, Some(0.95));
    }

    let nv_items = vec![mm_items[0].clone()];
    let nv_reocr = vlm
        .re_recognize_entries(
            &mp4_str,
            Some(roi),
            nv_items,
            Some("env-nvidia".into()),
            SubtitleScript::Cjk,
        )
        .await
        .expect("Stage C NVIDIA re_recognize_entries must succeed");
    assert_eq!(nv_reocr.len(), 1);
    assert!(nv_reocr[0].ok, "NVIDIA re_recognize_entries failed: {:?}", nv_reocr[0]);
    assert!(matches!(
        nv_reocr[0].category,
        Some(ReRecognizeCategory::Dialogue | ReRecognizeCategory::BracketNote)
    ));
    assert!(
        nv_reocr[0]
            .text
            .as_deref()
            .is_some_and(|t| !t.trim().is_empty()),
        "NVIDIA re_recognize_entries returned empty text: {:?}",
        nv_reocr[0]
    );
    assert_eq!(nv_reocr[0].confidence, Some(0.95));
    assert_eq!(
        count_dir_entries(&storage.preview_cache_dir()),
        0,
        "preview_cache/ must be clean after Stage C"
    );

    // Stage D: Real Zootopia_clip_hardsub1.mkv sublift:// streaming remux + clean process teardown
    let media = MediaStreamManager::new();
    let req_seek_1 = Request::builder()
        .method(Method::GET)
        .uri(build_media_url(&mkv_path, Some(34.0)))
        .header(header::RANGE, "bytes=0-65535")
        .body(Vec::new())
        .unwrap();
    let resp_1 = media.handle_http_request(&req_seek_1).await;
    assert_eq!(resp_1.status(), StatusCode::PARTIAL_CONTENT);
    assert_eq!(
        resp_1
            .headers()
            .get(header::CONTENT_TYPE)
            .and_then(|v| v.to_str().ok()),
        Some("video/mp4")
    );
    assert!(!resp_1.body().is_empty());
    assert!(
        resp_1.body().windows(4).any(|w| w == b"ftyp"),
        "fMP4 remux stream must contain ftyp box"
    );
    assert!(
        resp_1.body().windows(4).any(|w| w == b"moov" || w == b"moof"),
        "fMP4 remux stream must contain moov/moof box"
    );

    let req_seek_2 = Request::builder()
        .method(Method::GET)
        .uri(build_media_url(&mkv_path, Some(60.0)))
        .header(header::RANGE, "bytes=0-65535")
        .body(Vec::new())
        .unwrap();
    let resp_2 = media.handle_http_request(&req_seek_2).await;
    assert_eq!(resp_2.status(), StatusCode::PARTIAL_CONTENT);
    assert_eq!(
        resp_2
            .headers()
            .get(header::CONTENT_TYPE)
            .and_then(|v| v.to_str().ok()),
        Some("video/mp4")
    );
    assert!(!resp_2.body().is_empty());
    assert!(media.has_active_session().await);

    let worker_pid = worker
        .current_pid()
        .await
        .expect("sublift_worker PID must be active before shutdown");
    media.stop_active_stream().await;
    worker.shutdown().await;

    assert!(!media.has_active_session().await);
    assert!(worker.current_pid().await.is_none());

    #[cfg(unix)]
    {
        let rc = unsafe { libc::kill(worker_pid as libc::pid_t, 0) };
        assert_eq!(rc, -1, "sublift_worker process must have exited");

        let pgrep = std::process::Command::new("pgrep")
            .arg("-P")
            .arg(std::process::id().to_string())
            .output();
        if let Ok(out) = pgrep {
            let children = String::from_utf8_lossy(&out.stdout);
            assert!(
                children.trim().is_empty(),
                "zero child processes may remain after teardown, found: {}",
                children.trim()
            );
        }
    }

    assert_eq!(
        snapshot_dir(&smoke_dir),
        snap_before,
        "debug/smoke-test-data/ must remain completely untouched"
    );
}
