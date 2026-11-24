use std::fs::OpenOptions;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use crate::dto::{
    BatchExportConflictPolicy, BatchExportItemInput, BatchExportItemResult, BatchExportItemStatus,
    BatchExportReport, ExportResultDto, MergeOptionsDto, SubtitleEntry,
};
use crate::worker::WorkerBackend;

static EXPORT_TMP_SEQ: AtomicU64 = AtomicU64::new(1);

struct TempExportCleanup {
    path: Option<PathBuf>,
}

impl Drop for TempExportCleanup {
    fn drop(&mut self) {
        if let Some(p) = self.path.take() {
            let _ = std::fs::remove_file(p);
        }
    }
}

pub fn write_bytes_atomic(target: &Path, bytes: &[u8]) -> Result<(), String> {
    if target.as_os_str().is_empty() {
        return Err("[InvalidPath] export path must not be empty".to_string());
    }
    if let Ok(meta) = std::fs::symlink_metadata(target) {
        if meta.file_type().is_symlink() {
            return Err(format!(
                "[SymlinkRejected] refusing to overwrite symlink at {}",
                target.display()
            ));
        }
    }

    let parent = target
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .ok_or_else(|| format!("[InvalidPath] missing parent dir for {}", target.display()))?;
    if !parent.is_dir() {
        return Err(format!(
            "[InvalidPath] export directory does not exist: {}",
            parent.display()
        ));
    }

    let fname = target
        .file_name()
        .and_then(|s| s.to_str())
        .ok_or_else(|| format!("[InvalidPath] invalid filename: {}", target.display()))?;
    let seq = EXPORT_TMP_SEQ.fetch_add(1, Ordering::Relaxed);
    let tmp_path = parent.join(format!(".{fname}.tmp.{}.{seq}", std::process::id()));
    let mut cleanup = TempExportCleanup {
        path: Some(tmp_path.clone()),
    };

    let mut opts = OpenOptions::new();
    opts.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.custom_flags(libc::O_NOFOLLOW);
    }

    {
        let mut f = opts
            .open(&tmp_path)
            .map_err(|e| format!("[ExportIo] failed to create temp file: {e}"))?;
        f.write_all(bytes)
            .map_err(|e| format!("[ExportIo] failed to write temp file: {e}"))?;
        f.sync_all()
            .map_err(|e| format!("[ExportIo] failed to fsync temp file: {e}"))?;
    }

    std::fs::rename(&tmp_path, target)
        .map_err(|e| format!("[ExportIo] failed to rename temp file: {e}"))?;
    cleanup.path = None;
    Ok(())
}

fn path_exists_or_symlink(path: &Path) -> Result<bool, String> {
    match std::fs::symlink_metadata(path) {
        Ok(meta) => {
            if meta.file_type().is_symlink() {
                return Err(format!(
                    "[SymlinkRejected] symlink exists at {}",
                    path.display()
                ));
            }
            Ok(true)
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(e) => Err(format!("[ExportIo] stat failed for {}: {e}", path.display())),
    }
}

fn find_non_conflicting_path(target: &Path) -> Result<PathBuf, String> {
    let parent = target
        .parent()
        .ok_or_else(|| format!("[InvalidPath] missing parent for {}", target.display()))?;
    let stem = target
        .file_stem()
        .and_then(|s| s.to_str())
        .ok_or_else(|| format!("[InvalidPath] invalid file stem for {}", target.display()))?;
    let ext = target.extension().and_then(|s| s.to_str());

    for idx in 1u32..=10_000 {
        let candidate_name = match ext {
            Some(e) if !e.is_empty() => format!("{stem} ({idx}).{e}"),
            _ => format!("{stem} ({idx})"),
        };
        let cand = parent.join(candidate_name);
        if !path_exists_or_symlink(&cand)? {
            return Ok(cand);
        }
    }

    Err(format!(
        "[ExportConflict] exhausted rename attempts for {}",
        target.display()
    ))
}

pub async fn export_srt_impl(
    worker: &dyn WorkerBackend,
    out_path: &str,
    entries: Vec<SubtitleEntry>,
    apply_merge: bool,
    merge_options: Option<MergeOptionsDto>,
) -> Result<ExportResultDto, String> {
    if out_path.trim().is_empty() {
        return Err("[InvalidPath] out_path must not be empty".to_string());
    }

    let finalized = worker
        .finalize_srt(entries, apply_merge, merge_options)
        .await
        .map_err(|e| format!("[{}] {e}", e.code()))?;

    write_bytes_atomic(Path::new(out_path), finalized.srt_text.as_bytes())?;

    Ok(ExportResultDto {
        out_path: out_path.to_string(),
        cue_count: finalized.cue_count,
    })
}

pub async fn batch_export_srt_impl(
    worker: &dyn WorkerBackend,
    items: Vec<BatchExportItemInput>,
    conflict_policy: BatchExportConflictPolicy,
) -> Result<BatchExportReport, String> {
    let mut results = Vec::with_capacity(items.len());
    let mut exported_count = 0usize;
    let mut skipped_count = 0usize;
    let mut failed_count = 0usize;

    for item in items {
        let requested_path = PathBuf::from(&item.out_path);
        let exists = match path_exists_or_symlink(&requested_path) {
            Ok(b) => b,
            Err(err) => {
                failed_count += 1;
                results.push(BatchExportItemResult {
                    video_path: item.video_path,
                    out_path: item.out_path,
                    status: BatchExportItemStatus::Failed,
                    cue_count: 0,
                    error_message: Some(err),
                });
                continue;
            }
        };

        let (dest_path, ok_status) = if exists {
            match conflict_policy {
                BatchExportConflictPolicy::Skip => {
                    skipped_count += 1;
                    results.push(BatchExportItemResult {
                        video_path: item.video_path,
                        out_path: item.out_path,
                        status: BatchExportItemStatus::Skipped,
                        cue_count: 0,
                        error_message: None,
                    });
                    continue;
                }
                BatchExportConflictPolicy::Overwrite => {
                    (requested_path, BatchExportItemStatus::Exported)
                }
                BatchExportConflictPolicy::Rename => match find_non_conflicting_path(&requested_path) {
                    Ok(p) => (p, BatchExportItemStatus::Renamed),
                    Err(err) => {
                        failed_count += 1;
                        results.push(BatchExportItemResult {
                            video_path: item.video_path,
                            out_path: item.out_path,
                            status: BatchExportItemStatus::Failed,
                            cue_count: 0,
                            error_message: Some(err),
                        });
                        continue;
                    }
                },
            }
        } else {
            (requested_path, BatchExportItemStatus::Exported)
        };

        // Batch export always passes apply_merge = false to preserve proofread cue splits
        match worker.finalize_srt(item.entries, false, None).await {
            Ok(finalized) => {
                if let Err(err) = write_bytes_atomic(&dest_path, finalized.srt_text.as_bytes()) {
                    failed_count += 1;
                    results.push(BatchExportItemResult {
                        video_path: item.video_path,
                        out_path: dest_path.to_string_lossy().to_string(),
                        status: BatchExportItemStatus::Failed,
                        cue_count: 0,
                        error_message: Some(err),
                    });
                } else {
                    exported_count += 1;
                    results.push(BatchExportItemResult {
                        video_path: item.video_path,
                        out_path: dest_path.to_string_lossy().to_string(),
                        status: ok_status,
                        cue_count: finalized.cue_count,
                        error_message: None,
                    });
                }
            }
            Err(err) => {
                failed_count += 1;
                results.push(BatchExportItemResult {
                    video_path: item.video_path,
                    out_path: dest_path.to_string_lossy().to_string(),
                    status: BatchExportItemStatus::Failed,
                    cue_count: 0,
                    error_message: Some(format!("[{}] {err}", err.code())),
                });
            }
        }
    }

    Ok(BatchExportReport {
        results,
        exported_count,
        skipped_count,
        failed_count,
    })
}
