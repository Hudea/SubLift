use std::io::Read;
use std::path::Path;
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

use sha2::{Digest, Sha256};

use crate::dto::{SessionDraftDto, SessionSummaryDto, TaskStatus};

use super::{
    ensure_private_dir, open_nofollow_read, write_atomic_private, StorageError, StorageRoot,
};

const MAX_RECENT_SESSIONS: usize = 50;

pub fn compute_video_fingerprint(video_path: &Path) -> Result<String, StorageError> {
    let canonical = std::fs::canonicalize(video_path)?;
    let meta = std::fs::metadata(&canonical)?;
    if !meta.is_file() {
        return Err(StorageError::InvalidInput(format!(
            "video_path is not a regular file: {}",
            video_path.display()
        )));
    }

    let byte_size = meta.len();
    let mtime_ns: i128 = meta
        .modified()
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_nanos() as i128)
        .unwrap_or(0);

    let mut hasher = Sha256::new();
    hasher.update(canonical.to_string_lossy().as_bytes());
    hasher.update(b"\0");
    hasher.update(byte_size.to_le_bytes());
    hasher.update(b"\0");
    hasher.update(mtime_ns.to_le_bytes());
    Ok(hex::encode(hasher.finalize()))
}

pub struct SessionStore {
    root: StorageRoot,
    lock: Mutex<()>,
}

impl SessionStore {
    pub fn new(root: StorageRoot) -> Self {
        Self {
            root,
            lock: Mutex::new(()),
        }
    }

    pub fn load_session_draft(
        &self,
        video_path: &Path,
    ) -> Result<Option<SessionDraftDto>, StorageError> {
        if !video_path.is_file() {
            return Ok(None);
        }
        let fp = compute_video_fingerprint(video_path)?;
        let _guard = self.lock.lock().expect("session store lock poisoned");
        let sessions_dir = self.root.sessions_dir();
        let target = sessions_dir.join(format!("{fp}.json"));

        let Some(mut file) = open_nofollow_read(self.root.root(), &target)? else {
            return Ok(None);
        };
        let mut buf = Vec::new();
        file.read_to_end(&mut buf)?;
        drop(file);

        match serde_json::from_slice::<SessionDraftDto>(&buf) {
            Ok(draft) => Ok(Some(draft)),
            Err(_) => {
                let epoch_ms = SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .map(|d| d.as_millis())
                    .unwrap_or(0);
                let corrupt_path =
                    sessions_dir.join(format!("{fp}.json.corrupt.{epoch_ms}"));
                eprintln!(
                    "[storage::sessions] quarantining corrupt session draft at {}",
                    target.display()
                );
                let _ = std::fs::rename(&target, corrupt_path);
                Ok(None)
            }
        }
    }

    pub fn save_session_draft(&self, draft: &SessionDraftDto) -> Result<(), StorageError> {
        if draft.status == TaskStatus::Extracting {
            return Err(StorageError::InvalidInput(
                "cannot persist session draft with status 'extracting'".into(),
            ));
        }

        let fp = compute_video_fingerprint(Path::new(&draft.video_path))?;
        let _guard = self.lock.lock().expect("session store lock poisoned");
        let sessions_dir = self.root.sessions_dir();
        ensure_private_dir(&sessions_dir)?;
        let target = sessions_dir.join(format!("{fp}.json"));
        let bytes = serde_json::to_vec_pretty(draft)?;
        write_atomic_private(self.root.root(), &target, &bytes)
    }

    pub fn list_recent_sessions(&self) -> Result<Vec<SessionSummaryDto>, StorageError> {
        let _guard = self.lock.lock().expect("session store lock poisoned");
        let sessions_dir = self.root.sessions_dir();
        let dir_meta = match std::fs::symlink_metadata(&sessions_dir) {
            Ok(meta) => meta,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
            Err(e) => return Err(StorageError::Io(e)),
        };
        if dir_meta.file_type().is_symlink() || !dir_meta.is_dir() {
            return Err(StorageError::SymlinkRejected(sessions_dir));
        }
        super::reject_symlinks_under(self.root.root(), &sessions_dir)?;

        let mut summaries = Vec::new();
        for entry in std::fs::read_dir(&sessions_dir)? {
            let entry = entry?;
            let fname = entry.file_name();
            let Some(name) = fname.to_str() else {
                continue;
            };
            if !name.ends_with(".json") || name.starts_with('.') || name.contains(".corrupt.") {
                continue;
            }
            let path = entry.path();
            let meta = std::fs::symlink_metadata(&path)?;
            if meta.file_type().is_symlink() || !meta.is_file() {
                continue;
            }

            let Some(mut file) = open_nofollow_read(self.root.root(), &path)? else {
                continue;
            };
            let mut buf = Vec::new();
            if file.read_to_end(&mut buf).is_err() {
                continue;
            }
            if let Ok(draft) = serde_json::from_slice::<SessionDraftDto>(&buf) {
                summaries.push(SessionSummaryDto {
                    video_path: draft.video_path,
                    cue_count: draft.entries.len(),
                    status: draft.status,
                    updated_at_ms: draft.updated_at_ms,
                });
            }
        }

        summaries.sort_by_key(|b| std::cmp::Reverse(b.updated_at_ms));
        summaries.truncate(MAX_RECENT_SESSIONS);
        Ok(summaries)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dto::{RoiMode, SubtitleEntry, SubtitleScript, WorkbenchEngine};
    use tempfile::TempDir;

    fn sample_draft(video_path: &Path, updated_at_ms: i64) -> SessionDraftDto {
        SessionDraftDto {
            video_path: video_path.to_string_lossy().to_string(),
            engine: WorkbenchEngine::Paddle,
            provider_id: None,
            script: SubtitleScript::Auto,
            sample_fps: 5.0,
            roi_mode: RoiMode::Auto,
            region_box: None,
            status: TaskStatus::Ready,
            entries: vec![SubtitleEntry {
                id: 1,
                start_ms: 100,
                end_ms: 1200,
                text: "line".into(),
                confidence: 0.9,
            }],
            updated_at_ms,
        }
    }

    #[test]
    fn saves_loads_and_quarantines_corrupt_drafts_without_touching_video_dir() {
        let dir = TempDir::new().unwrap();
        let video_dir = dir.path().join("videos");
        std::fs::create_dir_all(&video_dir).unwrap();
        let video_path = video_dir.join("ep01.mp4");
        std::fs::write(&video_path, b"video-bytes-v1").unwrap();

        let store = SessionStore::new(StorageRoot::new(dir.path().join("appdata")).unwrap());
        let draft = sample_draft(&video_path, 1_700_000_000_000);
        store.save_session_draft(&draft).unwrap();

        // Video directory must contain ONLY the original video file
        let video_dir_entries: Vec<_> = std::fs::read_dir(&video_dir)
            .unwrap()
            .filter_map(Result::ok)
            .collect();
        assert_eq!(video_dir_entries.len(), 1);

        let loaded = store.load_session_draft(&video_path).unwrap().unwrap();
        assert_eq!(loaded, draft);

        // Recent sessions includes it
        let recent = store.list_recent_sessions().unwrap();
        assert_eq!(recent.len(), 1);
        assert_eq!(recent[0].cue_count, 1);

        // Mutating file size changes fingerprint -> miss
        std::fs::write(&video_path, b"video-bytes-v2-longer").unwrap();
        assert!(store.load_session_draft(&video_path).unwrap().is_none());

        // Corrupt JSON is quarantined cleanly
        let fp = compute_video_fingerprint(&video_path).unwrap();
        let corrupt_target = store.root.sessions_dir().join(format!("{fp}.json"));
        write_atomic_private(store.root.root(), &corrupt_target, b"{not-json").unwrap();
        assert!(store.load_session_draft(&video_path).unwrap().is_none());
        assert!(!corrupt_target.exists());
    }

    #[test]
    fn save_session_draft_rejects_extracting_status() {
        let dir = TempDir::new().unwrap();
        let video_path = dir.path().join("ep01.mp4");
        std::fs::write(&video_path, b"bytes").unwrap();
        let store = SessionStore::new(StorageRoot::new(dir.path().join("appdata")).unwrap());
        let mut draft = sample_draft(&video_path, 123);
        draft.status = TaskStatus::Extracting;
        let err = store.save_session_draft(&draft).unwrap_err();
        assert!(matches!(err, StorageError::InvalidInput(_)));
    }

    #[cfg(unix)]
    #[test]
    fn list_recent_sessions_rejects_dangling_symlink() {
        let dir = TempDir::new().unwrap();
        let root = StorageRoot::new(dir.path().join("appdata")).unwrap();
        let store = SessionStore::new(root.clone());

        let dangling_target = dir.path().join("does_not_exist_target");
        std::os::unix::fs::symlink(&dangling_target, root.sessions_dir()).unwrap();

        let err = store.list_recent_sessions().unwrap_err();
        assert!(matches!(err, StorageError::SymlinkRejected(_)));
    }

    #[test]
    fn list_recent_sessions_truncates_at_50_skips_corrupt_and_missing_video_behavior() {
        let dir = TempDir::new().unwrap();
        let root = StorageRoot::new(dir.path().join("appdata")).unwrap();
        let store = SessionStore::new(root.clone());

        let missing_video = dir.path().join("does_not_exist.mp4");
        assert!(store.load_session_draft(&missing_video).unwrap().is_none());
        let missing_draft = sample_draft(&missing_video, 1000);
        let save_err = store.save_session_draft(&missing_draft).unwrap_err();
        assert!(matches!(save_err, StorageError::Io(ref e) if e.kind() == std::io::ErrorKind::NotFound));

        ensure_private_dir(&root.sessions_dir()).unwrap();
        // Write 55 valid session files + 1 corrupt json file
        for i in 0..55i64 {
            let p = root.sessions_dir().join(format!("sess_{i:03}.json"));
            let draft = sample_draft(&dir.path().join(format!("ep_{i}.mp4")), 10_000 + i);
            let bytes = serde_json::to_vec(&draft).unwrap();
            write_atomic_private(root.root(), &p, &bytes).unwrap();
        }
        let corrupt_file = root.sessions_dir().join("corrupt_item.json");
        write_atomic_private(root.root(), &corrupt_file, b"{broken").unwrap();

        let recent = store.list_recent_sessions().unwrap();
        assert_eq!(recent.len(), MAX_RECENT_SESSIONS);
        assert_eq!(recent[0].updated_at_ms, 10_054);
        assert_eq!(recent[49].updated_at_ms, 10_005);
    }
}
