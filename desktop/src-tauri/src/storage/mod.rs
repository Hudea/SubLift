pub mod checkpoints;
pub mod providers;
pub mod sessions;

use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use thiserror::Error;

pub use checkpoints::{CheckpointRetentionPolicy, CheckpointStore, PruneReport};
pub use providers::{mask_api_key, parse_dotenv, ProviderProfile, ProviderStore};
pub use sessions::{compute_video_fingerprint, SessionStore};

static TMP_SEQ: AtomicU64 = AtomicU64::new(1);

#[derive(Debug, Error)]
pub enum StorageError {
    #[error("storage I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("storage serialization error: {0}")]
    Serde(#[from] serde_json::Error),
    #[error("symlink rejected under storage path: {0}")]
    SymlinkRejected(PathBuf),
    #[error("path escapes storage root: {0}")]
    PathEscape(PathBuf),
    #[error("invalid input: {0}")]
    InvalidInput(String),
    #[error("not found: {0}")]
    NotFound(String),
}

#[derive(Clone, Debug)]
pub struct StorageRoot {
    root: PathBuf,
}

impl StorageRoot {
    pub fn from_tauri_app<R: tauri::Runtime>(
        app: &tauri::AppHandle<R>,
    ) -> Result<Self, StorageError> {
        use tauri::Manager;
        let dir = app
            .path()
            .app_data_dir()
            .map_err(|e| StorageError::Io(std::io::Error::other(e.to_string())))?;
        Self::new(dir)
    }

    pub fn new(root: impl Into<PathBuf>) -> Result<Self, StorageError> {
        let root = root.into();
        ensure_private_dir(&root)?;
        let meta = std::fs::symlink_metadata(&root)?;
        if meta.file_type().is_symlink() || !meta.is_dir() {
            return Err(StorageError::SymlinkRejected(root));
        }
        Ok(Self { root })
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn providers_file(&self) -> PathBuf {
        self.root.join("providers.json")
    }

    pub fn sessions_dir(&self) -> PathBuf {
        self.root.join("sessions")
    }

    pub fn vlm_checkpoints_dir(&self) -> PathBuf {
        self.root.join("vlm_checkpoints")
    }

    pub fn preview_cache_dir(&self) -> PathBuf {
        self.root.join("preview_cache")
    }
}

pub fn ensure_private_dir(path: &Path) -> Result<(), StorageError> {
    if let Ok(meta) = std::fs::symlink_metadata(path) {
        if meta.file_type().is_symlink() {
            return Err(StorageError::SymlinkRejected(path.to_path_buf()));
        }
        if !meta.is_dir() {
            return Err(StorageError::InvalidInput(format!(
                "expected directory at {}",
                path.display()
            )));
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700))?;
        }
        return Ok(());
    }

    let mut builder = std::fs::DirBuilder::new();
    builder.recursive(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;
        builder.mode(0o700);
    }
    builder.create(path)?;

    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700))?;
    }
    Ok(())
}

pub fn reject_symlinks_under(root: &Path, target: &Path) -> Result<(), StorageError> {
    let root_meta = std::fs::symlink_metadata(root)?;
    if root_meta.file_type().is_symlink() {
        return Err(StorageError::SymlinkRejected(root.to_path_buf()));
    }

    let rel = target
        .strip_prefix(root)
        .map_err(|_| StorageError::PathEscape(target.to_path_buf()))?;

    let mut cur = root.to_path_buf();
    for comp in rel.components() {
        match comp {
            Component::Normal(part) => {
                cur.push(part);
                match std::fs::symlink_metadata(&cur) {
                    Ok(meta) => {
                        if meta.file_type().is_symlink() {
                            return Err(StorageError::SymlinkRejected(cur));
                        }
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                    Err(e) => return Err(StorageError::Io(e)),
                }
            }
            Component::CurDir => {}
            Component::ParentDir | Component::RootDir | Component::Prefix(_) => {
                return Err(StorageError::PathEscape(target.to_path_buf()));
            }
        }
    }
    Ok(())
}

pub fn open_nofollow_read(root: &Path, target: &Path) -> Result<Option<File>, StorageError> {
    reject_symlinks_under(root, target)?;

    let mut opts = OpenOptions::new();
    opts.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.custom_flags(libc::O_NOFOLLOW);
    }

    match opts.open(target) {
        Ok(file) => {
            if !file.metadata()?.is_file() {
                return Err(StorageError::InvalidInput(format!(
                    "expected regular file at {}",
                    target.display()
                )));
            }
            Ok(Some(file))
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        #[cfg(unix)]
        Err(e) if e.raw_os_error() == Some(libc::ELOOP) => {
            Err(StorageError::SymlinkRejected(target.to_path_buf()))
        }
        Err(e) => Err(StorageError::Io(e)),
    }
}

struct TempFileCleanup {
    path: Option<PathBuf>,
}

impl Drop for TempFileCleanup {
    fn drop(&mut self) {
        if let Some(path) = self.path.take() {
            let _ = std::fs::remove_file(path);
        }
    }
}

pub fn write_atomic_private(
    root: &Path,
    target: &Path,
    bytes: &[u8],
) -> Result<(), StorageError> {
    reject_symlinks_under(root, target)?;
    let parent = target
        .parent()
        .ok_or_else(|| StorageError::PathEscape(target.to_path_buf()))?;
    ensure_private_dir(parent)?;
    reject_symlinks_under(root, parent)?;

    let file_name = target
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or_else(|| StorageError::InvalidInput("invalid target filename".into()))?;
    let seq = TMP_SEQ.fetch_add(1, Ordering::Relaxed);
    let tmp_path = parent.join(format!(".{file_name}.tmp.{}.{seq}", std::process::id()));
    let mut cleanup = TempFileCleanup {
        path: Some(tmp_path.clone()),
    };

    let mut opts = OpenOptions::new();
    opts.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.mode(0o600).custom_flags(libc::O_NOFOLLOW);
    }

    {
        let mut file = opts.open(&tmp_path)?;
        file.write_all(bytes)?;
        file.sync_all()?;
    }

    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&tmp_path, std::fs::Permissions::from_mode(0o600))?;
    }

    reject_symlinks_under(root, target)?;
    std::fs::rename(&tmp_path, target)?;
    cleanup.path = None;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::TempDir;

    #[test]
    fn atomic_private_write_sets_0600_and_rejects_symlinks() {
        let dir = TempDir::new().unwrap();
        let storage = StorageRoot::new(dir.path().join("appdata")).unwrap();
        let target = storage.providers_file();
        write_atomic_private(storage.root(), &target, br#"{"profiles":[]}"#).unwrap();

        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let meta = std::fs::metadata(&target).unwrap();
            assert_eq!(meta.permissions().mode() & 0o777, 0o600);
            let root_meta = std::fs::metadata(storage.root()).unwrap();
            assert_eq!(root_meta.permissions().mode() & 0o777, 0o700);
        }

        // Verify no leftover .tmp files
        let entries: Vec<_> = std::fs::read_dir(storage.root())
            .unwrap()
            .filter_map(Result::ok)
            .collect();
        assert_eq!(entries.len(), 1);

        // Symlink inside storage root must be rejected
        #[cfg(unix)]
        {
            let real_outside = dir.path().join("outside.json");
            std::fs::write(&real_outside, b"{}").unwrap();
            let linked = storage.root().join("linked.json");
            std::os::unix::fs::symlink(&real_outside, &linked).unwrap();

            let err = open_nofollow_read(storage.root(), &linked).unwrap_err();
            assert!(matches!(err, StorageError::SymlinkRejected(_)));

            let err2 = write_atomic_private(storage.root(), &linked, b"bad").unwrap_err();
            assert!(matches!(err2, StorageError::SymlinkRejected(_)));
        }
    }
}
