use std::collections::{HashMap, VecDeque};
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::UNIX_EPOCH;

use serde::Deserialize;
use tokio::process::Command;
use tokio::sync::RwLock;

use super::MediaError;

const PROBE_CACHE_CAP: usize = 64;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MediaClassification {
    NativeDirect { mime: &'static str },
    RemuxCopy { video_codec: String },
    TranscodeVideo,
}

#[derive(Clone, Debug, PartialEq)]
pub struct MediaProbeInfo {
    pub classification: MediaClassification,
    pub duration_sec: f64,
    pub size_bytes: u64,
}

#[derive(Hash, PartialEq, Eq, Clone)]
struct ProbeCacheKey {
    canonical_path: PathBuf,
    byte_size: u64,
    mtime_ns: i128,
}

pub struct MediaProbeCache {
    inner: RwLock<ProbeCacheInner>,
}

struct ProbeCacheInner {
    map: HashMap<ProbeCacheKey, MediaProbeInfo>,
    order: VecDeque<ProbeCacheKey>,
}

impl Default for MediaProbeCache {
    fn default() -> Self {
        Self::new()
    }
}

impl MediaProbeCache {
    pub fn new() -> Self {
        Self {
            inner: RwLock::new(ProbeCacheInner {
                map: HashMap::new(),
                order: VecDeque::new(),
            }),
        }
    }

    pub async fn probe(&self, canonical_path: &Path) -> Result<MediaProbeInfo, MediaError> {
        let meta = tokio::fs::metadata(canonical_path)
            .await
            .map_err(|e| MediaError::NotFound(format!("{}: {e}", canonical_path.display())))?;
        if !meta.is_file() {
            return Err(MediaError::NotFound(format!(
                "not a regular file: {}",
                canonical_path.display()
            )));
        }
        let byte_size = meta.len();
        let mtime_ns: i128 = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map(|d| d.as_nanos() as i128)
            .unwrap_or(0);

        let key = ProbeCacheKey {
            canonical_path: canonical_path.to_path_buf(),
            byte_size,
            mtime_ns,
        };

        {
            let guard = self.inner.read().await;
            if let Some(cached) = guard.map.get(&key) {
                return Ok(cached.clone());
            }
        }

        let info = run_ffprobe(canonical_path, byte_size).await?;

        let mut guard = self.inner.write().await;
        Self::insert_locked(&mut guard, key, info.clone());
        Ok(info)
    }

    fn insert_locked(guard: &mut ProbeCacheInner, key: ProbeCacheKey, info: MediaProbeInfo) {
        if !guard.map.contains_key(&key) {
            if guard.order.len() >= PROBE_CACHE_CAP {
                if let Some(evicted) = guard.order.pop_front() {
                    guard.map.remove(&evicted);
                }
            }
            guard.order.push_back(key.clone());
        }
        guard.map.insert(key, info);
    }

    #[cfg(test)]
    pub(crate) async fn insert_for_test(&self, path: PathBuf, info: MediaProbeInfo) {
        let key = ProbeCacheKey {
            canonical_path: path,
            byte_size: info.size_bytes,
            mtime_ns: 0,
        };
        let mut guard = self.inner.write().await;
        Self::insert_locked(&mut guard, key, info);
    }

    #[cfg(test)]
    pub(crate) async fn contains_for_test(&self, path: &Path, byte_size: u64) -> bool {
        let key = ProbeCacheKey {
            canonical_path: path.to_path_buf(),
            byte_size,
            mtime_ns: 0,
        };
        self.inner.read().await.map.contains_key(&key)
    }

    #[cfg(test)]
    pub(crate) async fn cached_len(&self) -> usize {
        self.inner.read().await.map.len()
    }
}

async fn run_ffprobe(path: &Path, fallback_size: u64) -> Result<MediaProbeInfo, MediaError> {
    let ffprobe_bin = super::resolve_ffprobe_bin().map_err(MediaError::ProbeFailed)?;
    let output = Command::new(&ffprobe_bin)
        .arg("-v")
        .arg("error")
        .arg("-show_entries")
        .arg("format=format_name,duration,size:stream=index,codec_type,codec_name,profile,codec_tag_string,pix_fmt:stream_disposition=attached_pic")
        .arg("-of")
        .arg("json")
        .arg(path)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .await
        .map_err(|e| MediaError::ProbeFailed(format!("failed to execute ffprobe: {e}")))?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        return Err(MediaError::ProbeFailed(format!(
            "ffprobe exited with {}: {}",
            output.status,
            stderr.trim()
        )));
    }

    let stdout = String::from_utf8_lossy(&output.stdout);
    let mut info = classify_ffprobe_json(&stdout, path)?;
    if info.size_bytes == 0 {
        info.size_bytes = fallback_size;
    }
    Ok(info)
}

#[derive(Deserialize)]
struct FfprobeOutput {
    #[serde(default)]
    format: FfprobeFormat,
    #[serde(default)]
    streams: Vec<FfprobeStream>,
}

#[derive(Default, Deserialize)]
struct FfprobeFormat {
    #[serde(default)]
    format_name: String,
    #[serde(default)]
    duration: Option<String>,
    #[serde(default)]
    size: Option<String>,
}

#[derive(Default, Deserialize)]
struct FfprobeDisposition {
    #[serde(default)]
    attached_pic: i32,
}

#[derive(Deserialize)]
struct FfprobeStream {
    #[serde(default)]
    codec_type: String,
    #[serde(default)]
    codec_name: String,
    #[serde(default)]
    profile: String,
    #[serde(default)]
    codec_tag_string: String,
    #[serde(default)]
    pix_fmt: String,
    #[serde(default)]
    disposition: FfprobeDisposition,
}

pub fn classify_ffprobe_json(json_str: &str, path: &Path) -> Result<MediaProbeInfo, MediaError> {
    let parsed: FfprobeOutput = serde_json::from_str(json_str)
        .map_err(|e| MediaError::ProbeFailed(format!("invalid ffprobe json: {e}")))?;

    let format_tokens: Vec<String> = parsed
        .format
        .format_name
        .split(',')
        .map(|s| s.trim().to_ascii_lowercase())
        .filter(|s| !s.is_empty())
        .collect();

    let has_incompatible_container = format_tokens
        .iter()
        .any(|t| matches!(t.as_str(), "matroska" | "webm" | "avi" | "mpegts" | "flv" | "ogg"));
    let has_native_container = !has_incompatible_container
        && format_tokens
            .iter()
            .any(|t| matches!(t.as_str(), "mp4" | "mov" | "m4a" | "3gp"));

    let video_stream = parsed
        .streams
        .iter()
        .find(|s| s.codec_type.eq_ignore_ascii_case("video") && s.disposition.attached_pic == 0)
        .ok_or_else(|| MediaError::ProbeFailed("no non-cover video stream found".into()))?;

    let vcodec = video_stream.codec_name.to_ascii_lowercase();
    let vpix = video_stream.pix_fmt.to_ascii_lowercase();
    let vprofile = video_stream.profile.to_ascii_lowercase();
    let vtag = video_stream.codec_tag_string.to_ascii_lowercase();

    let is_h264_8bit = vcodec == "h264"
        && matches!(vpix.as_str(), "yuv420p" | "yuvj420p" | "nv12")
        && !vprofile.contains("high 10");
    let is_hevc_webkit = vcodec == "hevc";
    let hevc_tag_ok = vtag == "hvc1";

    let audio_stream = parsed
        .streams
        .iter()
        .find(|s| s.codec_type.eq_ignore_ascii_case("audio"));

    let audio_native = match audio_stream {
        None => true,
        Some(a) => matches!(
            a.codec_name.to_ascii_lowercase().as_str(),
            "aac" | "mp3" | "alac"
        ),
    };

    let classification = if has_native_container
        && audio_native
        && (is_h264_8bit || (is_hevc_webkit && hevc_tag_ok))
    {
        let is_mov = path
            .extension()
            .and_then(|e| e.to_str())
            .is_some_and(|ext| ext.eq_ignore_ascii_case("mov"));
        let mime = if is_mov {
            "video/quicktime"
        } else {
            "video/mp4"
        };
        MediaClassification::NativeDirect { mime }
    } else if is_h264_8bit || is_hevc_webkit {
        MediaClassification::RemuxCopy {
            video_codec: vcodec,
        }
    } else {
        MediaClassification::TranscodeVideo
    };

    let duration_sec = parsed
        .format
        .duration
        .as_deref()
        .and_then(|s| s.parse::<f64>().ok())
        .filter(|d| d.is_finite() && *d >= 0.0)
        .unwrap_or(0.0);

    let size_bytes = parsed
        .format
        .size
        .as_deref()
        .and_then(|s| s.parse::<u64>().ok())
        .unwrap_or(0);

    Ok(MediaProbeInfo {
        classification,
        duration_sec,
        size_bytes,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn classifies_8bit_h264_aac_mp4_as_native_direct() {
        let json = r#"{
            "format": {"format_name": "mov,mp4,m4a,3gp,3g2,mj2", "duration": "12.5", "size": "102400"},
            "streams": [
                {"codec_type": "video", "codec_name": "h264", "profile": "High", "codec_tag_string": "avc1", "pix_fmt": "yuv420p"},
                {"codec_type": "audio", "codec_name": "aac", "profile": "LC"}
            ]
        }"#;
        let info = classify_ffprobe_json(json, Path::new("/tmp/test.mp4")).unwrap();
        assert_eq!(
            info.classification,
            MediaClassification::NativeDirect { mime: "video/mp4" }
        );
    }

    #[test]
    fn classifies_10bit_h264_high10_as_transcode_video() {
        let json = r#"{
            "format": {"format_name": "mov,mp4,m4a,3gp,3g2,mj2", "duration": "254.272", "size": "76370747"},
            "streams": [
                {"codec_type": "video", "codec_name": "h264", "profile": "High 10", "codec_tag_string": "avc1", "pix_fmt": "yuv420p10le"},
                {"codec_type": "audio", "codec_name": "aac", "profile": "LC"}
            ]
        }"#;
        let info = classify_ffprobe_json(json, Path::new("/tmp/Zootopia_clip_1080p.mp4")).unwrap();
        assert_eq!(info.classification, MediaClassification::TranscodeVideo);
    }

    #[test]
    fn classifies_mkv_hevc_eac3_and_hev1_mp4_as_remux_copy() {
        let mkv_json = r#"{
            "format": {"format_name": "matroska,webm", "duration": "254.272", "size": "225354657"},
            "streams": [
                {"codec_type": "video", "codec_name": "hevc", "profile": "Main 10", "codec_tag_string": "", "pix_fmt": "yuv420p10le"},
                {"codec_type": "audio", "codec_name": "eac3"}
            ]
        }"#;
        let mkv_info = classify_ffprobe_json(mkv_json, Path::new("/tmp/hardsub1.mkv")).unwrap();
        assert_eq!(
            mkv_info.classification,
            MediaClassification::RemuxCopy {
                video_codec: "hevc".into()
            }
        );

        let hev1_mp4_json = r#"{
            "format": {"format_name": "mov,mp4,m4a,3gp,3g2,mj2", "duration": "10.0", "size": "50000"},
            "streams": [
                {"codec_type": "video", "codec_name": "hevc", "profile": "Main", "codec_tag_string": "hev1", "pix_fmt": "yuv420p"},
                {"codec_type": "audio", "codec_name": "aac"}
            ]
        }"#;
        let hev1_info = classify_ffprobe_json(hev1_mp4_json, Path::new("/tmp/hev1.mp4")).unwrap();
        assert_eq!(
            hev1_info.classification,
            MediaClassification::RemuxCopy {
                video_codec: "hevc".into()
            }
        );
    }

    #[tokio::test]
    async fn probes_smoke_test_samples_when_present_on_disk() {
        let repo_root = Path::new(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .unwrap()
            .parent()
            .unwrap();
        let mp4_10bit = repo_root.join("debug/smoke-test-data/Zootopia_clip_1080p.mp4");
        let mkv_hevc = repo_root.join("debug/smoke-test-data/Zootopia_clip_hardsub1.mkv");
        let cache = MediaProbeCache::new();

        if mp4_10bit.is_file() {
            let info = cache.probe(&mp4_10bit).await.unwrap();
            assert_eq!(info.classification, MediaClassification::TranscodeVideo);
        }
        if mkv_hevc.is_file() {
            let info = cache.probe(&mkv_hevc).await.unwrap();
            assert_eq!(
                info.classification,
                MediaClassification::RemuxCopy {
                    video_codec: "hevc".into()
                }
            );
        }
    }

    #[test]
    fn skips_attached_pic_cover_art_and_classifies_audioless_and_opus_mp4() {
        // Cover-art video stream skipped in favor of real video stream
        let with_cover_json = r#"{
            "format": {"format_name": "mp4", "duration": "10.0", "size": "12345"},
            "streams": [
                {"codec_type": "video", "codec_name": "mjpeg", "disposition": {"attached_pic": 1}},
                {"codec_type": "video", "codec_name": "h264", "profile": "Main", "pix_fmt": "yuv420p", "disposition": {"attached_pic": 0}}
            ]
        }"#;
        let info_cover = classify_ffprobe_json(with_cover_json, Path::new("/tmp/cover.mp4")).unwrap();
        assert_eq!(
            info_cover.classification,
            MediaClassification::NativeDirect { mime: "video/mp4" }
        );

        // Only cover art -> error
        let only_cover_json = r#"{
            "format": {"format_name": "mp4", "duration": "10.0"},
            "streams": [
                {"codec_type": "video", "codec_name": "png", "disposition": {"attached_pic": 1}},
                {"codec_type": "audio", "codec_name": "aac"}
            ]
        }"#;
        let err = classify_ffprobe_json(only_cover_json, Path::new("/tmp/audio_cover.mp4")).unwrap_err();
        assert!(err.to_string().contains("no non-cover video stream found"));

        // Audio-less MOV -> NativeDirect video/quicktime
        let audioless_mov = r#"{
            "format": {"format_name": "mov,mp4", "duration": "5.0"},
            "streams": [
                {"codec_type": "video", "codec_name": "h264", "profile": "High", "pix_fmt": "yuv420p"}
            ]
        }"#;
        let mov_info = classify_ffprobe_json(audioless_mov, Path::new("/tmp/silent.MOV")).unwrap();
        assert_eq!(
            mov_info.classification,
            MediaClassification::NativeDirect { mime: "video/quicktime" }
        );

        // Opus / FLAC in MP4 -> RemuxCopy
        for codec in ["opus", "flac"] {
            let json = format!(
                r#"{{
                    "format": {{"format_name": "mp4", "duration": "5.0"}},
                    "streams": [
                        {{"codec_type": "video", "codec_name": "h264", "profile": "Main", "pix_fmt": "yuv420p"}},
                        {{"codec_type": "audio", "codec_name": "{codec}"}}
                    ]
                }}"#
            );
            let info = classify_ffprobe_json(&json, Path::new("/tmp/audio.mp4")).unwrap();
            assert_eq!(
                info.classification,
                MediaClassification::RemuxCopy {
                    video_codec: "h264".into()
                }
            );
        }
    }

    #[tokio::test]
    async fn probe_cache_evicts_oldest_entry_above_cap() {
        let cache = MediaProbeCache::new();
        for i in 0..=PROBE_CACHE_CAP {
            cache
                .insert_for_test(
                    PathBuf::from(format!("/tmp/vid_{i}.mp4")),
                    MediaProbeInfo {
                        classification: MediaClassification::NativeDirect { mime: "video/mp4" },
                        duration_sec: 1.0,
                        size_bytes: 100 + i as u64,
                    },
                )
                .await;
        }
        assert_eq!(cache.cached_len().await, PROBE_CACHE_CAP);
        assert!(!cache.contains_for_test(Path::new("/tmp/vid_0.mp4"), 100).await);
        assert!(cache.contains_for_test(Path::new("/tmp/vid_1.mp4"), 101).await);
        assert!(
            cache
                .contains_for_test(
                    &PathBuf::from(format!("/tmp/vid_{PROBE_CACHE_CAP}.mp4")),
                    100 + PROBE_CACHE_CAP as u64
                )
                .await
        );
    }
}
