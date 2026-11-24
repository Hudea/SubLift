use std::io::Read;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use super::{
    ensure_private_dir, open_nofollow_read, reject_symlinks_under, write_atomic_private,
    StorageError, StorageRoot,
};

#[derive(Clone, Copy, Debug)]
pub struct CheckpointRetentionPolicy {
    pub max_total_bytes: u64,
    pub max_age_secs: u64,
}

impl Default for CheckpointRetentionPolicy {
    fn default() -> Self {
        Self {
            max_total_bytes: 512 * 1024 * 1024,
            max_age_secs: 30 * 24 * 3600,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PruneReport {
    pub removed_dirs: usize,
    pub reclaimed_bytes: u64,
}

pub fn validate_checkpoint_key(key: &str) -> Result<&str, StorageError> {
    let valid_len = (8..=128).contains(&key.len());
    let valid_chars = key
        .bytes()
        .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-');
    if !valid_len || !valid_chars {
        return Err(StorageError::InvalidInput(format!(
            "invalid checkpoint_key: '{key}'"
        )));
    }
    Ok(key)
}

pub struct CheckpointStore {
    root: StorageRoot,
}

impl CheckpointStore {
    pub fn new(root: StorageRoot) -> Self {
        Self { root }
    }

    pub fn vlm_checkpoint_dir(&self, key: &str) -> Result<PathBuf, StorageError> {
        let key = validate_checkpoint_key(key)?;
        let base = self.root.vlm_checkpoints_dir();
        ensure_private_dir(&base)?;
        let dir = base.join(key);
        ensure_private_dir(&dir)?;
        reject_symlinks_under(self.root.root(), &dir)?;
        Ok(dir)
    }

    pub fn vlm_partial_path(&self, key: &str) -> Result<PathBuf, StorageError> {
        Ok(self.vlm_checkpoint_dir(key)?.join("vlm_responses.partial.json"))
    }

    pub fn vlm_raw_responses_dir(&self, key: &str) -> Result<PathBuf, StorageError> {
        let dir = self.vlm_checkpoint_dir(key)?.join("raw_responses");
        ensure_private_dir(&dir)?;
        reject_symlinks_under(self.root.root(), &dir)?;
        Ok(dir)
    }

    pub fn write_vlm_partial_atomic(&self, key: &str, bytes: &[u8]) -> Result<(), StorageError> {
        let target = self.vlm_partial_path(key)?;
        write_atomic_private(self.root.root(), &target, bytes)
    }

    pub fn read_vlm_partial(&self, key: &str) -> Result<Option<Vec<u8>>, StorageError> {
        let key = validate_checkpoint_key(key)?;
        let target = self
            .root
            .vlm_checkpoints_dir()
            .join(key)
            .join("vlm_responses.partial.json");
        let Some(mut file) = open_nofollow_read(self.root.root(), &target)? else {
            return Ok(None);
        };
        let mut buf = Vec::new();
        file.read_to_end(&mut buf)?;
        Ok(Some(buf))
    }

    pub fn prune_vlm_checkpoints(
        &self,
        policy: CheckpointRetentionPolicy,
        active_key: Option<&str>,
    ) -> Result<PruneReport, StorageError> {
        let base = self.root.vlm_checkpoints_dir();
        let base_meta = match std::fs::symlink_metadata(&base) {
            Ok(meta) => meta,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(PruneReport::default()),
            Err(e) => return Err(StorageError::Io(e)),
        };
        if base_meta.file_type().is_symlink() || !base_meta.is_dir() {
            return Err(StorageError::SymlinkRejected(base));
        }
        reject_symlinks_under(self.root.root(), &base)?;

        let now = SystemTime::now();
        let max_age = Duration::from_secs(policy.max_age_secs);
        let mut entries = Vec::new();

        for item in std::fs::read_dir(&base)? {
            let item = item?;
            let path = item.path();
            let meta = std::fs::symlink_metadata(&path)?;
            if meta.file_type().is_symlink() || !meta.is_dir() {
                continue;
            }
            let name = item.file_name().to_string_lossy().to_string();
            if active_key == Some(name.as_str()) {
                continue;
            }
            let (size, newest_mtime) = dir_size_and_mtime(&path, meta.modified().unwrap_or(now))?;
            entries.push((name, path, size, newest_mtime));
        }

        let mut report = PruneReport::default();
        let mut retained = Vec::new();
        for entry in entries {
            let expired = now
                .duration_since(entry.3)
                .map(|age| age > max_age)
                .unwrap_or(false);
            if expired {
                std::fs::remove_dir_all(&entry.1)?;
                report.removed_dirs += 1;
                report.reclaimed_bytes += entry.2;
            } else {
                retained.push(entry);
            }
        }

        retained.sort_by_key(|e| e.3);
        let mut total_bytes: u64 = retained.iter().map(|e| e.2).sum();
        for entry in retained {
            if total_bytes <= policy.max_total_bytes {
                break;
            }
            std::fs::remove_dir_all(&entry.1)?;
            total_bytes = total_bytes.saturating_sub(entry.2);
            report.removed_dirs += 1;
            report.reclaimed_bytes += entry.2;
        }

        Ok(report)
    }
}

fn dir_size_and_mtime(path: &Path, init_mtime: SystemTime) -> Result<(u64, SystemTime), StorageError> {
    let mut size = 0u64;
    let mut newest = init_mtime;
    for entry in std::fs::read_dir(path)? {
        let entry = entry?;
        let p = entry.path();
        let meta = std::fs::symlink_metadata(&p)?;
        if meta.file_type().is_symlink() {
            continue;
        }
        if let Ok(mtime) = meta.modified() {
            if mtime > newest {
                newest = mtime;
            }
        }
        if meta.is_dir() {
            let (sub_size, sub_mtime) = dir_size_and_mtime(&p, newest)?;
            size = size.saturating_add(sub_size);
            if sub_mtime > newest {
                newest = sub_mtime;
            }
        } else if meta.is_file() {
            size = size.saturating_add(meta.len());
        }
    }
    Ok((size, newest))
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::TempDir;

    #[test]
    fn validates_checkpoint_key_and_prunes_oldest_while_skipping_active() {
        let dir = TempDir::new().unwrap();
        let store = CheckpointStore::new(StorageRoot::new(dir.path().join("appdata")).unwrap());

        assert!(validate_checkpoint_key("../escape").is_err());
        assert!(validate_checkpoint_key("short").is_err());

        store
            .write_vlm_partial_atomic("ckpt_key_0001", &[1u8; 64])
            .unwrap();
        store
            .write_vlm_partial_atomic("ckpt_key_0002", &[2u8; 64])
            .unwrap();
        store
            .write_vlm_partial_atomic("ckpt_key_active", &[3u8; 64])
            .unwrap();

        let read_back = store.read_vlm_partial("ckpt_key_0001").unwrap().unwrap();
        assert_eq!(read_back.len(), 64);

        let report = store
            .prune_vlm_checkpoints(
                CheckpointRetentionPolicy {
                    max_total_bytes: 70,
                    max_age_secs: 3600,
                },
                Some("ckpt_key_active"),
            )
            .unwrap();
        assert!(report.removed_dirs >= 1);
        assert!(store.read_vlm_partial("ckpt_key_active").unwrap().is_some());
    }

    #[cfg(unix)]
    #[test]
    fn prune_vlm_checkpoints_rejects_dangling_symlink() {
        let dir = TempDir::new().unwrap();
        let root = StorageRoot::new(dir.path().join("appdata")).unwrap();
        let store = CheckpointStore::new(root.clone());

        let dangling_target = dir.path().join("nonexistent_checkpoints_target");
        std::os::unix::fs::symlink(&dangling_target, root.vlm_checkpoints_dir()).unwrap();

        let err = store
            .prune_vlm_checkpoints(CheckpointRetentionPolicy::default(), None)
            .unwrap_err();
        assert!(matches!(err, StorageError::SymlinkRejected(_)));
    }
}
