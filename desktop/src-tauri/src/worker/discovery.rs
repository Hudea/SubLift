use std::ffi::OsStr;
use std::path::{Path, PathBuf};

use super::{WorkerConfig, WorkerError};

const WORKER_BIN_NAME: &str = "sublift_worker";

fn worker_filename() -> String {
    format!("{WORKER_BIN_NAME}{}", std::env::consts::EXE_SUFFIX)
}

fn is_executable_file(path: &Path) -> bool {
    let Ok(meta) = std::fs::metadata(path) else {
        return false;
    };
    if !meta.is_file() {
        return false;
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        meta.permissions().mode() & 0o111 != 0
    }
    #[cfg(not(unix))]
    {
        true
    }
}

fn collect_repo_candidates(start: &Path, out: &mut Vec<PathBuf>) {
    let bin = worker_filename();
    let rel_build_paths = if cfg!(debug_assertions) {
        [
            PathBuf::from("build/debug/tools/worker").join(&bin),
            PathBuf::from("build/release/tools/worker").join(&bin),
        ]
    } else {
        [
            PathBuf::from("build/release/tools/worker").join(&bin),
            PathBuf::from("build/debug/tools/worker").join(&bin),
        ]
    };

    for ancestor in start.ancestors().take(6) {
        let looks_like_repo = ancestor.join("CMakeLists.txt").is_file()
            || ancestor.join("build").is_dir()
            || ancestor.join("tools").is_dir();
        if !looks_like_repo {
            continue;
        }
        for rel in &rel_build_paths {
            let cand = ancestor.join(rel);
            if !out.contains(&cand) {
                out.push(cand);
            }
        }
    }
}

pub fn resolve_worker_binary(config: &WorkerConfig) -> Result<PathBuf, WorkerError> {
    let env_val = std::env::var_os("SUBLIFT_WORKER_BIN");
    resolve_worker_binary_with_env(config, env_val.as_deref())
}

pub(crate) fn resolve_worker_binary_with_env(
    config: &WorkerConfig,
    env_override: Option<&OsStr>,
) -> Result<PathBuf, WorkerError> {
    if let Some(raw_env) = env_override {
        if !raw_env.is_empty() {
            let path = PathBuf::from(raw_env);
            if is_executable_file(&path) {
                return Ok(path);
            }
            return Err(WorkerError::BinaryNotFound {
                searched_paths: vec![path],
            });
        }
    }

    if let Some(override_bin) = &config.override_bin {
        if is_executable_file(override_bin) {
            return Ok(override_bin.clone());
        }
        return Err(WorkerError::BinaryNotFound {
            searched_paths: vec![override_bin.clone()],
        });
    }

    let bin = worker_filename();
    let mut candidates = Vec::new();

    if let Some(resource_dir) = &config.resource_dir {
        candidates.push(resource_dir.join(&bin));
        candidates.push(resource_dir.join("bin").join(&bin));
    }

    if let Ok(exe_path) = std::env::current_exe() {
        if let Some(exe_dir) = exe_path.parent() {
            let cand = exe_dir.join(&bin);
            if !candidates.contains(&cand) {
                candidates.push(cand);
            }
        }
    }

    if let Some(repo_root) = &config.repo_root {
        collect_repo_candidates(repo_root, &mut candidates);
    }
    if let Ok(cwd) = std::env::current_dir() {
        collect_repo_candidates(&cwd, &mut candidates);
    }
    collect_repo_candidates(Path::new(env!("CARGO_MANIFEST_DIR")), &mut candidates);

    for cand in &candidates {
        if is_executable_file(cand) {
            return Ok(cand.clone());
        }
    }

    Err(WorkerError::BinaryNotFound {
        searched_paths: candidates,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::OsString;
    use tempfile::TempDir;

    fn make_exec(path: &Path) {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).unwrap();
        }
        std::fs::write(path, b"#!/bin/sh\nexit 0\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o755)).unwrap();
        }
    }

    #[test]
    fn env_override_fails_closed_when_missing() {
        let dir = TempDir::new().unwrap();
        let fallback = dir.path().join("build/debug/tools/worker").join(worker_filename());
        make_exec(&fallback);

        let cfg = WorkerConfig {
            repo_root: Some(dir.path().to_path_buf()),
            ..Default::default()
        };
        let missing = dir.path().join("missing_worker");
        let err = resolve_worker_binary_with_env(&cfg, Some(missing.as_os_str())).unwrap_err();
        match err {
            WorkerError::BinaryNotFound { searched_paths } => {
                assert_eq!(searched_paths, vec![missing]);
            }
            other => panic!("unexpected error: {other:?}"),
        }
    }

    #[test]
    fn override_bin_fails_closed_when_non_executable() {
        let dir = TempDir::new().unwrap();
        let non_exec = dir.path().join("non_exec_worker");
        std::fs::write(&non_exec, b"not executable").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&non_exec, std::fs::Permissions::from_mode(0o644)).unwrap();
        }

        let cfg = WorkerConfig {
            override_bin: Some(non_exec.clone()),
            repo_root: Some(dir.path().to_path_buf()),
            ..Default::default()
        };
        let err = resolve_worker_binary_with_env(&cfg, None).unwrap_err();
        assert!(matches!(err, WorkerError::BinaryNotFound { searched_paths } if searched_paths == vec![non_exec]));
    }

    #[test]
    fn priority_order_prefers_resource_dir_over_repo_build() {
        let dir = TempDir::new().unwrap();
        let resource_dir = dir.path().join("res");
        let res_bin = resource_dir.join(worker_filename());
        let repo_bin = dir
            .path()
            .join("repo/build/debug/tools/worker")
            .join(worker_filename());
        make_exec(&res_bin);
        make_exec(&repo_bin);

        let cfg = WorkerConfig {
            resource_dir: Some(resource_dir),
            repo_root: Some(dir.path().join("repo")),
            ..Default::default()
        };
        let resolved = resolve_worker_binary_with_env(&cfg, Some(OsString::new().as_os_str())).unwrap();
        assert_eq!(resolved, res_bin);
    }

    #[test]
    fn repo_fallback_discovers_debug_or_release_worker() {
        let dir = TempDir::new().unwrap();
        let release_bin = dir
            .path()
            .join("build/release/tools/worker")
            .join(worker_filename());
        make_exec(&release_bin);

        let cfg = WorkerConfig {
            repo_root: Some(dir.path().to_path_buf()),
            ..Default::default()
        };
        let resolved = resolve_worker_binary_with_env(&cfg, None).unwrap();
        assert_eq!(resolved, release_bin);
    }
}
