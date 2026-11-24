use std::collections::HashMap;
use std::fmt;
use std::fs::OpenOptions;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use reqwest::header::{HeaderName, HeaderValue};
use serde::{Deserialize, Serialize};

use crate::dto::{
    ApiKeyUpdate, CustomHeaderDto, ProviderProfileMaskedDto, ProviderType, ProviderUpsertDto,
    ThinkingMode,
};

use super::{open_nofollow_read, write_atomic_private, StorageError, StorageRoot};

const DEFAULT_MINIMAX_BASE_URL: &str = "https://api.minimax.cn/v1";
const DEFAULT_MINIMAX_MODEL: &str = "MiniMax-M3.1-Flash";
const DEFAULT_NVIDIA_BASE_URL: &str = "https://integrate.api.nvidia.com/v1";
const DEFAULT_NVIDIA_MODEL: &str = "nvidia/nemotron-3.5-super-vl-120b-a12b";
const MAX_CUSTOM_HEADERS: usize = 32;
const RESERVED_HTTP_HEADERS: &[&str] = &[
    "host",
    "content-length",
    "transfer-encoding",
    "connection",
    "upgrade",
    "te",
    "trailer",
];

#[derive(Clone, Serialize, Deserialize)]
pub struct ProviderProfile {
    pub id: String,
    pub name: String,
    pub provider_type: ProviderType,
    pub base_url: String,
    pub model: String,
    pub thinking_mode: ThinkingMode,
    pub concurrency: u32,
    #[serde(default)]
    pub api_key: String,
    pub is_default: bool,
    #[serde(default)]
    pub custom_headers: Vec<CustomHeaderDto>,
}

impl fmt::Debug for ProviderProfile {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let masked_indicator = if self.api_key.is_empty() {
            "<empty>"
        } else {
            "[REDACTED]"
        };
        f.debug_struct("ProviderProfile")
            .field("id", &self.id)
            .field("name", &self.name)
            .field("provider_type", &self.provider_type)
            .field("base_url", &self.base_url)
            .field("model", &self.model)
            .field("thinking_mode", &self.thinking_mode)
            .field("concurrency", &self.concurrency)
            .field("api_key", &masked_indicator)
            .field("is_default", &self.is_default)
            .field("custom_headers_count", &self.custom_headers.len())
            .finish()
    }
}

impl ProviderProfile {
    pub fn to_masked_dto(&self) -> ProviderProfileMaskedDto {
        let trimmed = self.api_key.trim();
        ProviderProfileMaskedDto {
            id: self.id.clone(),
            name: self.name.clone(),
            provider_type: self.provider_type,
            base_url: self.base_url.clone(),
            model: self.model.clone(),
            thinking_mode: self.thinking_mode,
            concurrency: self.concurrency,
            has_api_key: !trimmed.is_empty(),
            masked_api_key: mask_api_key(trimmed),
            is_default: self.is_default,
            custom_headers: self.custom_headers.clone(),
        }
    }
}

pub fn normalize_and_validate_custom_headers(
    raw: &[CustomHeaderDto],
) -> Result<Vec<CustomHeaderDto>, StorageError> {
    let mut normalized: Vec<CustomHeaderDto> = Vec::new();
    for item in raw {
        let name = item.name.trim();
        let value = item.value.trim();
        if name.is_empty() && value.is_empty() {
            continue;
        }
        if name.is_empty() {
            return Err(StorageError::InvalidInput(
                "custom header name must not be empty".into(),
            ));
        }
        if name.contains(['\r', '\n', '\0']) || value.contains(['\r', '\n', '\0']) {
            return Err(StorageError::InvalidInput(format!(
                "custom header '{name}' contains invalid control characters"
            )));
        }
        HeaderName::from_bytes(name.as_bytes()).map_err(|_| {
            StorageError::InvalidInput(format!("invalid custom header name '{name}'"))
        })?;
        HeaderValue::from_str(value).map_err(|_| {
            StorageError::InvalidInput(format!("invalid custom header value for '{name}'"))
        })?;
        let lower = name.to_ascii_lowercase();
        if RESERVED_HTTP_HEADERS.contains(&lower.as_str()) {
            return Err(StorageError::InvalidInput(format!(
                "custom header '{name}' is reserved by HTTP transport"
            )));
        }
        normalized.retain(|existing| !existing.name.eq_ignore_ascii_case(name));
        if normalized.len() >= MAX_CUSTOM_HEADERS {
            return Err(StorageError::InvalidInput(format!(
                "custom headers cannot exceed {MAX_CUSTOM_HEADERS} entries"
            )));
        }
        normalized.push(CustomHeaderDto {
            name: name.to_string(),
            value: value.to_string(),
        });
    }
    Ok(normalized)
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
struct ProvidersFile {
    #[serde(default)]
    profiles: Vec<ProviderProfile>,
}

pub fn mask_api_key(raw: &str) -> String {
    let trimmed = raw.trim();
    if trimmed.is_empty() {
        return String::new();
    }
    let chars: Vec<char> = trimmed.chars().collect();
    if chars.len() <= 8 {
        return "****".to_string();
    }
    let prefix: String = chars[..3].iter().collect();
    let suffix: String = chars[chars.len() - 4..].iter().collect();
    format!("{prefix}****{suffix}")
}

pub fn parse_dotenv(env_path: &Path) -> Result<HashMap<String, String>, StorageError> {
    let meta = std::fs::symlink_metadata(env_path).map_err(|e| {
        if e.kind() == std::io::ErrorKind::NotFound {
            StorageError::NotFound(format!("env file not found: {}", env_path.display()))
        } else {
            StorageError::Io(e)
        }
    })?;
    if meta.file_type().is_symlink() {
        return Err(StorageError::SymlinkRejected(env_path.to_path_buf()));
    }
    if !meta.is_file() {
        return Err(StorageError::InvalidInput(format!(
            "env path is not a regular file: {}",
            env_path.display()
        )));
    }

    let mut opts = OpenOptions::new();
    opts.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.custom_flags(libc::O_NOFOLLOW);
    }
    let mut file = opts.open(env_path)?;
    let mut content = String::new();
    file.read_to_string(&mut content)?;

    Ok(parse_dotenv_str(&content))
}

pub(crate) fn parse_dotenv_str(content: &str) -> HashMap<String, String> {
    let content = content.strip_prefix('\u{FEFF}').unwrap_or(content);
    let mut parsed = HashMap::new();
    for raw_line in content.lines() {
        let mut line = raw_line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if let Some(rest) = line.strip_prefix("export ") {
            line = rest.trim();
        }
        let Some((raw_key, raw_val)) = line.split_once('=') else {
            continue;
        };
        let key = raw_key.trim();
        if key.is_empty() {
            continue;
        }
        let val = parse_dotenv_value(raw_val);
        parsed.insert(key.to_string(), val);
    }
    parsed
}

fn parse_dotenv_value(raw_val: &str) -> String {
    let val = raw_val.trim();
    if let Some(rest) = val.strip_prefix('"') {
        let mut out = String::new();
        let mut escaped = false;
        let mut closed_end: Option<usize> = None;
        for (idx, ch) in rest.char_indices() {
            if escaped {
                match ch {
                    '"' | '\\' => out.push(ch),
                    'n' => out.push('\n'),
                    'r' => out.push('\r'),
                    't' => out.push('\t'),
                    other => {
                        out.push('\\');
                        out.push(other);
                    }
                }
                escaped = false;
            } else if ch == '\\' {
                escaped = true;
            } else if ch == '"' {
                closed_end = Some(idx + ch.len_utf8());
                break;
            } else {
                out.push(ch);
            }
        }
        if let Some(end_idx) = closed_end {
            let trailing = rest[end_idx..].trim_start();
            if trailing.is_empty() || trailing.starts_with('#') {
                return out;
            }
        } else {
            return out;
        }
    } else if let Some(rest) = val.strip_prefix('\'') {
        if let Some(end_rel) = rest.find('\'') {
            let trailing = rest[end_rel + 1..].trim_start();
            if trailing.is_empty() || trailing.starts_with('#') {
                return rest[..end_rel].to_string();
            }
        } else {
            return rest.to_string();
        }
    }

    strip_unquoted_comment(val).to_string()
}

fn strip_unquoted_comment(val: &str) -> &str {
    if val.starts_with('#') {
        return "";
    }
    let bytes = val.as_bytes();
    for (i, &b) in bytes.iter().enumerate() {
        if b == b'#' && i > 0 && bytes[i - 1].is_ascii_whitespace() {
            return val[..i].trim_end();
        }
    }
    val.trim_end()
}

pub struct ProviderStore {
    root: StorageRoot,
    lock: Mutex<()>,
}

impl ProviderStore {
    pub fn new(root: StorageRoot) -> Self {
        Self {
            root,
            lock: Mutex::new(()),
        }
    }

    fn load_locked(&self) -> Result<ProvidersFile, StorageError> {
        let path = self.root.providers_file();
        let Some(mut file) = open_nofollow_read(self.root.root(), &path)? else {
            return Ok(ProvidersFile::default());
        };
        let mut buf = Vec::new();
        file.read_to_end(&mut buf)?;
        drop(file);
        if buf.is_empty() {
            return Ok(ProvidersFile::default());
        }
        match serde_json::from_slice::<ProvidersFile>(&buf) {
            Ok(parsed) => Ok(parsed),
            Err(_) => {
                let epoch_ms = std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .map(|d| d.as_millis())
                    .unwrap_or(0);
                let corrupt_path = self
                    .root
                    .root()
                    .join(format!("providers.json.corrupt.{epoch_ms}"));
                eprintln!(
                    "[storage::providers] quarantining corrupt providers file at {}",
                    path.display()
                );
                let _ = std::fs::rename(&path, corrupt_path);
                Ok(ProvidersFile::default())
            }
        }
    }

    fn save_locked(&self, file: &ProvidersFile) -> Result<(), StorageError> {
        let bytes = serde_json::to_vec_pretty(file)?;
        write_atomic_private(self.root.root(), &self.root.providers_file(), &bytes)
    }

    pub fn list_providers(&self) -> Result<Vec<ProviderProfileMaskedDto>, StorageError> {
        let _guard = self.lock.lock().expect("provider store lock poisoned");
        let file = self.load_locked()?;
        Ok(file.profiles.iter().map(ProviderProfile::to_masked_dto).collect())
    }

    pub fn resolve_provider_secret(
        &self,
        provider_id: Option<&str>,
    ) -> Result<ProviderProfile, StorageError> {
        let _guard = self.lock.lock().expect("provider store lock poisoned");
        let file = self.load_locked()?;
        let found = match provider_id {
            Some(id) => file.profiles.into_iter().find(|p| p.id == id),
            None => file
                .profiles
                .iter()
                .find(|p| p.is_default)
                .cloned()
                .or_else(|| file.profiles.into_iter().next()),
        };
        found.ok_or_else(|| {
            StorageError::NotFound(
                provider_id
                    .map(|id| format!("provider profile '{id}' not found"))
                    .unwrap_or_else(|| "no provider profiles configured".to_string()),
            )
        })
    }

    fn validate_base_url(raw_url: &str) -> Result<String, StorageError> {
        let base_url = raw_url.trim().to_string();
        let parsed_url = url::Url::parse(&base_url).map_err(|e| {
            StorageError::InvalidInput(format!("provider base_url is not a valid URL: {e}"))
        })?;
        if !matches!(parsed_url.scheme(), "http" | "https")
            || parsed_url.host_str().is_none_or(str::is_empty)
            || base_url.chars().any(char::is_whitespace)
        {
            return Err(StorageError::InvalidInput(
                "provider base_url must be a valid http:// or https:// URL with a non-empty host"
                    .into(),
            ));
        }
        Ok(base_url)
    }

    pub fn resolve_transient_provider(
        &self,
        upsert: &ProviderUpsertDto,
    ) -> Result<ProviderProfile, StorageError> {
        let base_url = Self::validate_base_url(&upsert.base_url)?;
        let model = upsert.model.trim().to_string();
        if model.is_empty() {
            return Err(StorageError::InvalidInput(
                "provider model must not be empty".into(),
            ));
        }
        let custom_headers = normalize_and_validate_custom_headers(&upsert.custom_headers)?;

        let trimmed_id = upsert
            .id
            .as_deref()
            .map(str::trim)
            .filter(|s| !s.is_empty());

        let api_key = match &upsert.api_key_update {
            ApiKeyUpdate::Keep => {
                if let Some(id) = trimmed_id {
                    let _guard = self.lock.lock().expect("provider store lock poisoned");
                    let file = self.load_locked()?;
                    file.profiles
                        .iter()
                        .find(|p| p.id == id)
                        .map(|p| p.api_key.clone())
                        .unwrap_or_default()
                } else {
                    String::new()
                }
            }
            ApiKeyUpdate::Replace { value } => value.trim().to_string(),
            ApiKeyUpdate::Clear => String::new(),
        };

        Ok(ProviderProfile {
            id: trimmed_id.map(ToOwned::to_owned).unwrap_or_default(),
            name: upsert.name.trim().to_string(),
            provider_type: upsert.provider_type,
            base_url,
            model,
            thinking_mode: upsert.thinking_mode,
            concurrency: upsert.concurrency.clamp(1, 16),
            api_key,
            is_default: upsert.is_default,
            custom_headers,
        })
    }

    pub fn save_provider(
        &self,
        upsert: ProviderUpsertDto,
    ) -> Result<ProviderProfileMaskedDto, StorageError> {
        let name = upsert.name.trim().to_string();
        if name.is_empty() {
            return Err(StorageError::InvalidInput(
                "provider name must not be empty".into(),
            ));
        }
        let base_url = Self::validate_base_url(&upsert.base_url)?;
        let model = upsert.model.trim().to_string();
        if model.is_empty() {
            return Err(StorageError::InvalidInput(
                "provider model must not be empty".into(),
            ));
        }
        if !(1..=16).contains(&upsert.concurrency) {
            return Err(StorageError::InvalidInput(
                "provider concurrency must be between 1 and 16".into(),
            ));
        }
        let custom_headers = normalize_and_validate_custom_headers(&upsert.custom_headers)?;

        let _guard = self.lock.lock().expect("provider store lock poisoned");
        let mut file = self.load_locked()?;

        let trimmed_id = upsert
            .id
            .as_deref()
            .map(str::trim)
            .filter(|s| !s.is_empty());

        let existing_idx = trimmed_id
            .and_then(|id| file.profiles.iter().position(|p| p.id == id));

        let api_key = match upsert.api_key_update {
            ApiKeyUpdate::Keep => existing_idx
                .map(|i| file.profiles[i].api_key.clone())
                .unwrap_or_default(),
            ApiKeyUpdate::Replace { value } => {
                let trimmed = value.trim();
                if trimmed.is_empty() {
                    return Err(StorageError::InvalidInput(
                        "replacement api_key must not be empty".into(),
                    ));
                }
                trimmed.to_string()
            }
            ApiKeyUpdate::Clear => String::new(),
        };

        let id = trimmed_id
            .map(ToOwned::to_owned)
            .unwrap_or_else(|| uuid::Uuid::new_v4().to_string());

        let mut profile = ProviderProfile {
            id: id.clone(),
            name,
            provider_type: upsert.provider_type,
            base_url,
            model,
            thinking_mode: upsert.thinking_mode,
            concurrency: upsert.concurrency,
            api_key,
            is_default: upsert.is_default,
            custom_headers,
        };

        if let Some(idx) = existing_idx {
            file.profiles[idx] = profile.clone();
        } else {
            if file.profiles.is_empty() {
                profile.is_default = true;
            }
            file.profiles.push(profile.clone());
        }

        if profile.is_default {
            for p in &mut file.profiles {
                p.is_default = p.id == id;
            }
        } else if !file.profiles.iter().any(|p| p.is_default) {
            if let Some(first) = file.profiles.first_mut() {
                first.is_default = true;
            }
        }

        self.save_locked(&file)?;
        let saved = file
            .profiles
            .iter()
            .find(|p| p.id == id)
            .expect("saved profile present");
        Ok(saved.to_masked_dto())
    }

    pub fn delete_provider(&self, id: &str) -> Result<(), StorageError> {
        let _guard = self.lock.lock().expect("provider store lock poisoned");
        let mut file = self.load_locked()?;
        let Some(pos) = file.profiles.iter().position(|p| p.id == id) else {
            return Err(StorageError::NotFound(format!(
                "provider profile '{id}' not found"
            )));
        };
        let was_default = file.profiles[pos].is_default;
        file.profiles.remove(pos);
        if was_default && !file.profiles.iter().any(|p| p.is_default) {
            if let Some(first) = file.profiles.first_mut() {
                first.is_default = true;
            }
        }
        self.save_locked(&file)
    }

    pub fn import_providers_from_env(
        &self,
        env_path: Option<&Path>,
    ) -> Result<Vec<ProviderProfileMaskedDto>, StorageError> {
        let cwd = std::env::current_dir().ok();
        self.import_providers_from_env_with_roots(env_path, cwd.as_deref(), cfg!(debug_assertions))
    }

    fn import_providers_from_env_with_roots(
        &self,
        env_path: Option<&Path>,
        cwd_override: Option<&Path>,
        allow_manifest_fallback: bool,
    ) -> Result<Vec<ProviderProfileMaskedDto>, StorageError> {
        let env_map = match env_path {
            Some(explicit) => parse_dotenv(explicit)?,
            None => {
                let mut merged = HashMap::new();
                let candidate = cwd_override
                    .and_then(|cwd| find_ancestor_dotenv(cwd, 6))
                    .or_else(|| {
                        if allow_manifest_fallback {
                            find_ancestor_dotenv(Path::new(env!("CARGO_MANIFEST_DIR")), 4)
                        } else {
                            None
                        }
                    });
                if let Some(dotenv_path) = candidate {
                    merged.extend(parse_dotenv(&dotenv_path)?);
                }
                for key in [
                    "MINIMAX_API_KEY",
                    "MINIMAX_API_BASE_URL",
                    "MINIMAX_MODEL",
                    "NVIDIA_API_KEY",
                    "NVIDIA_API_BASE_URL",
                    "NVIDIA_MODEL",
                    "OPENAI_API_KEY",
                    "OPENCODE_API_KEY",
                    "OPENAI_BASE_URL",
                    "OPENAI_API_BASE_URL",
                    "OPENCODE_BASE_URL",
                    "OPENAI_MODEL",
                    "VLM_PROVIDER",
                    "VLM_API_KEY",
                    "VLM_ENDPOINT",
                    "VLM_API_BASE_URL",
                    "VLM_MODEL",
                ] {
                    if let Ok(val) = std::env::var(key) {
                        if !val.trim().is_empty() {
                            merged.insert(key.to_string(), val);
                        }
                    }
                }
                merged
            }
        };

        let candidates = build_env_candidates(&env_map);
        let _guard = self.lock.lock().expect("provider store lock poisoned");
        let mut file = self.load_locked()?;

        let mut has_default = file.profiles.iter().any(|p| p.is_default);
        for mut cand in candidates {
            if let Some(existing) = file.profiles.iter_mut().find(|p| p.id == cand.id) {
                let keep_default = existing.is_default;
                if cand.custom_headers.is_empty() && !existing.custom_headers.is_empty() {
                    cand.custom_headers = existing.custom_headers.clone();
                }
                cand.is_default = keep_default;
                *existing = cand;
            } else {
                if !has_default {
                    cand.is_default = true;
                    has_default = true;
                }
                file.profiles.push(cand);
            }
        }

        self.save_locked(&file)?;
        Ok(file.profiles.iter().map(ProviderProfile::to_masked_dto).collect())
    }
}

fn find_ancestor_dotenv(start: &Path, max_depth: usize) -> Option<PathBuf> {
    for ancestor in start.ancestors().take(max_depth) {
        let candidate = ancestor.join(".env");
        if std::fs::symlink_metadata(&candidate).is_ok() {
            return Some(candidate);
        }
    }
    None
}

fn first_non_empty<'a>(map: &'a HashMap<String, String>, keys: &[&str]) -> Option<&'a str> {
    for &k in keys {
        if let Some(v) = map.get(k) {
            let trimmed = v.trim();
            if !trimmed.is_empty() {
                return Some(trimmed);
            }
        }
    }
    None
}

fn infer_auto_provider(
    map: &HashMap<String, String>,
    vlm_endpoint: Option<&str>,
    vlm_model: Option<&str>,
) -> &'static str {
    let model_low = vlm_model.unwrap_or("").to_ascii_lowercase();
    let endpoint_low = vlm_endpoint.unwrap_or("").to_ascii_lowercase();
    if model_low.contains("nvidia") || endpoint_low.contains("nvidia.com") {
        return "nvidia";
    }
    if model_low.contains("minimax") || endpoint_low.contains("minimax") {
        return "minimax";
    }
    if first_non_empty(map, &["MINIMAX_API_KEY"]).is_some() {
        return "minimax";
    }
    if first_non_empty(map, &["NVIDIA_API_KEY"]).is_some() {
        return "nvidia";
    }
    if first_non_empty(map, &["OPENAI_API_KEY", "OPENCODE_API_KEY"]).is_some() {
        return "openai";
    }
    "minimax"
}

fn build_env_candidates(map: &HashMap<String, String>) -> Vec<ProviderProfile> {
    let mut out = Vec::new();

    if let Some(key) = first_non_empty(map, &["MINIMAX_API_KEY"]) {
        out.push(ProviderProfile {
            id: "env-minimax".to_string(),
            name: "MiniMax (env)".to_string(),
            provider_type: ProviderType::Minimax,
            base_url: first_non_empty(map, &["MINIMAX_API_BASE_URL"])
                .unwrap_or(DEFAULT_MINIMAX_BASE_URL)
                .to_string(),
            model: first_non_empty(map, &["MINIMAX_MODEL"])
                .unwrap_or(DEFAULT_MINIMAX_MODEL)
                .to_string(),
            thinking_mode: ThinkingMode::Auto,
            concurrency: 4,
            api_key: key.to_string(),
            is_default: false,
            custom_headers: Vec::new(),
        });
    }

    if let Some(key) = first_non_empty(map, &["NVIDIA_API_KEY"]) {
        out.push(ProviderProfile {
            id: "env-nvidia".to_string(),
            name: "NVIDIA NIM (env)".to_string(),
            provider_type: ProviderType::Nvidia,
            base_url: first_non_empty(map, &["NVIDIA_API_BASE_URL"])
                .unwrap_or(DEFAULT_NVIDIA_BASE_URL)
                .to_string(),
            model: first_non_empty(map, &["NVIDIA_MODEL"])
                .unwrap_or(DEFAULT_NVIDIA_MODEL)
                .to_string(),
            thinking_mode: ThinkingMode::Auto,
            concurrency: 4,
            api_key: key.to_string(),
            is_default: false,
            custom_headers: Vec::new(),
        });
    }

    if let Some(key) = first_non_empty(map, &["OPENAI_API_KEY", "OPENCODE_API_KEY"]) {
        let base_url = first_non_empty(
            map,
            &["OPENAI_BASE_URL", "OPENAI_API_BASE_URL", "OPENCODE_BASE_URL"],
        )
        .unwrap_or("https://api.openai.com/v1")
        .to_string();
        let model = first_non_empty(map, &["OPENAI_MODEL"])
            .unwrap_or("gpt-4o-mini")
            .to_string();
        out.push(ProviderProfile {
            id: "env-openai".to_string(),
            name: "OpenAI Compatible (env)".to_string(),
            provider_type: ProviderType::OpenAi,
            base_url,
            model,
            thinking_mode: ThinkingMode::Auto,
            concurrency: 4,
            api_key: key.to_string(),
            is_default: false,
            custom_headers: Vec::new(),
        });
    }

    let vlm_key = first_non_empty(map, &["VLM_API_KEY"]);
    let raw_vlm_provider = first_non_empty(map, &["VLM_PROVIDER"]);
    let vlm_endpoint = first_non_empty(map, &["VLM_ENDPOINT", "VLM_API_BASE_URL"]);
    let vlm_model = first_non_empty(map, &["VLM_MODEL"]);

    if vlm_key.is_some()
        || (raw_vlm_provider.is_some() && (vlm_endpoint.is_some() || vlm_model.is_some()))
    {
        let selected_provider: Option<&'static str> = match raw_vlm_provider
            .map(str::to_ascii_lowercase)
            .as_deref()
        {
            None | Some("auto") => Some(infer_auto_provider(map, vlm_endpoint, vlm_model)),
            Some("minimax") => Some("minimax"),
            Some("nvidia") => Some("nvidia"),
            Some("openai") | Some("openai_compatible") => Some("openai"),
            Some(_) => None,
        };

        struct ProviderDefaults<'a> {
            id: &'a str,
            name: &'a str,
            provider_type: ProviderType,
            key_vars: &'a [&'a str],
            url_vars: &'a [&'a str],
            default_url: &'a str,
            model_vars: &'a [&'a str],
            default_model: &'a str,
        }

        let target_spec: Option<ProviderDefaults<'_>> = match selected_provider {
            Some("minimax") => Some(ProviderDefaults {
                id: "env-minimax",
                name: "MiniMax (env)",
                provider_type: ProviderType::Minimax,
                key_vars: &["MINIMAX_API_KEY"],
                url_vars: &["MINIMAX_API_BASE_URL"],
                default_url: DEFAULT_MINIMAX_BASE_URL,
                model_vars: &["MINIMAX_MODEL"],
                default_model: DEFAULT_MINIMAX_MODEL,
            }),
            Some("nvidia") => Some(ProviderDefaults {
                id: "env-nvidia",
                name: "NVIDIA NIM (env)",
                provider_type: ProviderType::Nvidia,
                key_vars: &["NVIDIA_API_KEY"],
                url_vars: &["NVIDIA_API_BASE_URL"],
                default_url: DEFAULT_NVIDIA_BASE_URL,
                model_vars: &["NVIDIA_MODEL"],
                default_model: DEFAULT_NVIDIA_MODEL,
            }),
            Some("openai") => Some(ProviderDefaults {
                id: "env-openai",
                name: "OpenAI Compatible (env)",
                provider_type: ProviderType::OpenAi,
                key_vars: &["OPENAI_API_KEY", "OPENCODE_API_KEY"],
                url_vars: &["OPENAI_BASE_URL", "OPENAI_API_BASE_URL", "OPENCODE_BASE_URL"],
                default_url: "https://api.openai.com/v1",
                model_vars: &["OPENAI_MODEL"],
                default_model: "gpt-4o-mini",
            }),
            _ => None,
        };

        if let Some(spec) = target_spec {
            if let Some(resolved_key) = vlm_key.or_else(|| first_non_empty(map, spec.key_vars)) {
                let base_url = vlm_endpoint
                    .or_else(|| first_non_empty(map, spec.url_vars))
                    .unwrap_or(spec.default_url)
                    .to_string();
                let model = vlm_model
                    .or_else(|| first_non_empty(map, spec.model_vars))
                    .unwrap_or(spec.default_model)
                    .to_string();
                if let Some(existing) = out.iter_mut().find(|p| p.id == spec.id) {
                    existing.api_key = resolved_key.to_string();
                    existing.base_url = base_url;
                    existing.model = model;
                } else {
                    out.push(ProviderProfile {
                        id: spec.id.to_string(),
                        name: spec.name.to_string(),
                        provider_type: spec.provider_type,
                        base_url,
                        model,
                        thinking_mode: ThinkingMode::Auto,
                        concurrency: 4,
                        api_key: resolved_key.to_string(),
                        is_default: false,
                        custom_headers: Vec::new(),
                    });
                }
            }
        }
    }

    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::TempDir;

    #[test]
    fn mask_api_key_handles_empty_short_and_standard_keys() {
        assert_eq!(mask_api_key(""), "");
        assert_eq!(mask_api_key("   "), "");
        assert_eq!(mask_api_key("12345678"), "****");
        assert_eq!(mask_api_key("short"), "****");
        assert_eq!(mask_api_key("sk-123456789f2a"), "sk-****9f2a");
    }

    #[test]
    fn provider_profile_debug_redacts_api_key() {
        let prof = ProviderProfile {
            id: "p1".into(),
            name: "Test".into(),
            provider_type: ProviderType::Minimax,
            base_url: "https://api.minimax.cn/v1".into(),
            model: "MiniMax-M3.1-Flash".into(),
            thinking_mode: ThinkingMode::Auto,
            concurrency: 4,
            api_key: "secret-should-never-appear-in-debug".into(),
            custom_headers: Vec::new(),
            is_default: true,
        };
        let dbg = format!("{prof:?}");
        assert!(dbg.contains("[REDACTED]"));
        assert!(!dbg.contains("secret-should-never-appear-in-debug"));
    }

    #[test]
    fn save_keep_replace_clear_and_delete_default_promotion() {
        let dir = TempDir::new().unwrap();
        let store = ProviderStore::new(StorageRoot::new(dir.path().join("appdata")).unwrap());

        let saved = store
            .save_provider(ProviderUpsertDto {
                id: Some("p1".into()),
                name: "Primary".into(),
                provider_type: ProviderType::OpenAi,
                base_url: "https://example.invalid/v1".into(),
                model: "test-model".into(),
                thinking_mode: ThinkingMode::Auto,
                concurrency: 4,
                is_default: true,
                api_key_update: ApiKeyUpdate::Replace {
                    value: "tok-abcdef1234".into(),
                },
                custom_headers: Vec::new(),
            })
            .unwrap();
        assert!(saved.has_api_key);
        assert_eq!(saved.masked_api_key, "tok****1234");
        assert!(saved.is_default);

        // Keep preserves secret
        let kept = store
            .save_provider(ProviderUpsertDto {
                id: Some("p1".into()),
                name: "Primary Updated".into(),
                provider_type: ProviderType::OpenAi,
                base_url: "https://example.invalid/v1".into(),
                model: "test-model-2".into(),
                thinking_mode: ThinkingMode::Low,
                concurrency: 2,
                is_default: true,
                api_key_update: ApiKeyUpdate::Keep,
                custom_headers: Vec::new(),
            })
            .unwrap();
        assert!(kept.has_api_key);
        assert_eq!(kept.masked_api_key, "tok****1234");

        // Second profile becomes default when deleted first
        store
            .save_provider(ProviderUpsertDto {
                id: Some("p2".into()),
                name: "Secondary".into(),
                provider_type: ProviderType::Nvidia,
                base_url: "https://integrate.api.nvidia.com/v1".into(),
                model: "nv-model".into(),
                thinking_mode: ThinkingMode::None,
                concurrency: 2,
                is_default: false,
                api_key_update: ApiKeyUpdate::Clear,
                custom_headers: Vec::new(),
            })
            .unwrap();

        store.delete_provider("p1").unwrap();
        let remaining = store.list_providers().unwrap();
        assert_eq!(remaining.len(), 1);
        assert_eq!(remaining[0].id, "p2");
        assert!(remaining[0].is_default);
        assert!(!remaining[0].has_api_key);
    }

    #[test]
    fn import_providers_from_explicit_env_parses_quotes_comments_and_never_exposes_secret() {
        let dir = TempDir::new().unwrap();
        let store = ProviderStore::new(StorageRoot::new(dir.path().join("appdata")).unwrap());
        let env_file = dir.path().join(".env.synthetic");
        let contents = "\
# comment
export MINIMAX_API_KEY=\"mm-synthetic-9988\"
MINIMAX_API_BASE_URL='https://api.minimax.cn/v1'
NVIDIA_API_KEY=nv-synthetic-7766 # trailing comment
OPENAI_API_KEY=oa-synthetic-5544
OPENAI_BASE_URL=https://local.example/v1
OPENAI_MODEL=local-vl
";
        std::fs::write(&env_file, contents).unwrap();

        let imported = store.import_providers_from_env(Some(&env_file)).unwrap();
        assert_eq!(imported.len(), 3);
        let mm = imported.iter().find(|p| p.id == "env-minimax").unwrap();
        assert_eq!(mm.masked_api_key, "mm-****9988");
        let nv = imported.iter().find(|p| p.id == "env-nvidia").unwrap();
        assert_eq!(nv.masked_api_key, "nv-****7766");
        let oa = imported.iter().find(|p| p.id == "env-openai").unwrap();
        assert_eq!(oa.provider_type, ProviderType::OpenAi);
        assert_eq!(oa.masked_api_key, "oa-****5544");
    }

    #[test]
    fn parse_dotenv_str_strips_trailing_comments_after_quotes_and_preserves_inner_hashes() {
        let content = "\
KEY_DQ=\"mm-quoted-secret-1234\" # trailing comment
KEY_SQ='nv-quoted-secret-5678' # trailing comment
KEY_HASH=\"secret#with_hash\" # trailing comment
KEY_HASH_SQ='single#hash'#tight-comment
KEY_UNQUOTED=plain-secret-9999 # comment
";
        let parsed = parse_dotenv_str(content);
        assert_eq!(parsed.get("KEY_DQ").map(String::as_str), Some("mm-quoted-secret-1234"));
        assert_eq!(parsed.get("KEY_SQ").map(String::as_str), Some("nv-quoted-secret-5678"));
        assert_eq!(parsed.get("KEY_HASH").map(String::as_str), Some("secret#with_hash"));
        assert_eq!(parsed.get("KEY_HASH_SQ").map(String::as_str), Some("single#hash"));
        assert_eq!(parsed.get("KEY_UNQUOTED").map(String::as_str), Some("plain-secret-9999"));
    }

    #[test]
    fn build_env_candidates_supports_vlm_auto_omitted_provider_and_preserves_minimax_defaults() {
        // 1. VLM_PROVIDER=auto infers NVIDIA from VLM_ENDPOINT / VLM_MODEL
        let mut auto_map = HashMap::new();
        auto_map.insert("VLM_PROVIDER".into(), "auto".into());
        auto_map.insert("VLM_API_KEY".into(), "nvapi-auto-key-1234".into());
        auto_map.insert("VLM_ENDPOINT".into(), "https://integrate.api.nvidia.com/v1".into());
        auto_map.insert("VLM_MODEL".into(), "nvidia/nemotron-3.5-super-vl-120b-a12b".into());
        let auto_cands = build_env_candidates(&auto_map);
        assert_eq!(auto_cands.len(), 1);
        assert_eq!(auto_cands[0].id, "env-nvidia");
        assert_eq!(auto_cands[0].provider_type, ProviderType::Nvidia);
        assert_eq!(auto_cands[0].api_key, "nvapi-auto-key-1234");
        assert_eq!(auto_cands[0].base_url, "https://integrate.api.nvidia.com/v1");

        // 2. VLM_API_KEY without VLM_PROVIDER infers provider and imports profile
        let mut no_prov_map = HashMap::new();
        no_prov_map.insert("VLM_API_KEY".into(), "vlm-noprov-key-5678".into());
        no_prov_map.insert("VLM_ENDPOINT".into(), "https://custom-vlm.example/v1".into());
        no_prov_map.insert("VLM_MODEL".into(), "custom-vl-model".into());
        let no_prov_cands = build_env_candidates(&no_prov_map);
        assert_eq!(no_prov_cands.len(), 1);
        assert_eq!(no_prov_cands[0].api_key, "vlm-noprov-key-5678");
        assert_eq!(no_prov_cands[0].base_url, "https://custom-vlm.example/v1");
        assert_eq!(no_prov_cands[0].model, "custom-vl-model");

        // 3. VLM_PROVIDER=minimax + VLM_API_KEY preserves MINIMAX_API_BASE_URL & MINIMAX_MODEL
        let mut mm_map = HashMap::new();
        mm_map.insert("MINIMAX_API_KEY".into(), "mm-old-key-0000".into());
        mm_map.insert("MINIMAX_API_BASE_URL".into(), "https://custom-mm.example/v1".into());
        mm_map.insert("MINIMAX_MODEL".into(), "MiniMax-Custom-Model".into());
        mm_map.insert("VLM_PROVIDER".into(), "minimax".into());
        mm_map.insert("VLM_API_KEY".into(), "mm-vlm-key-9999".into());
        let mm_cands = build_env_candidates(&mm_map);
        assert_eq!(mm_cands.len(), 1);
        assert_eq!(mm_cands[0].id, "env-minimax");
        assert_eq!(mm_cands[0].api_key, "mm-vlm-key-9999");
        assert_eq!(mm_cands[0].base_url, "https://custom-mm.example/v1");
        assert_eq!(mm_cands[0].model, "MiniMax-Custom-Model");
    }

    #[test]
    fn save_provider_trims_id_before_existing_lookup() {
        let dir = TempDir::new().unwrap();
        let store = ProviderStore::new(StorageRoot::new(dir.path().join("appdata")).unwrap());

        store
            .save_provider(ProviderUpsertDto {
                id: Some("p1".into()),
                name: "Initial".into(),
                provider_type: ProviderType::Minimax,
                base_url: "https://api.minimax.cn/v1".into(),
                model: "MiniMax-M3.1-Flash".into(),
                thinking_mode: ThinkingMode::Auto,
                concurrency: 4,
                is_default: true,
                api_key_update: ApiKeyUpdate::Replace {
                    value: "secret-key-1234".into(),
                },
                custom_headers: Vec::new(),
            })
            .unwrap();

        let updated = store
            .save_provider(ProviderUpsertDto {
                id: Some(" p1 ".into()),
                name: "Updated".into(),
                provider_type: ProviderType::Minimax,
                base_url: "https://api.minimax.cn/v1".into(),
                model: "MiniMax-M3.1-Flash".into(),
                thinking_mode: ThinkingMode::Low,
                concurrency: 2,
                is_default: true,
                api_key_update: ApiKeyUpdate::Keep,
                custom_headers: Vec::new(),
            })
            .unwrap();

        assert_eq!(updated.id, "p1");
        assert_eq!(updated.name, "Updated");
        assert_eq!(updated.masked_api_key, "sec****1234");
        assert_eq!(store.list_providers().unwrap().len(), 1);
    }

    #[test]
    fn import_providers_from_env_none_walks_ancestors_and_rejects_symlink() {
        let dir = TempDir::new().unwrap();
        let repo_dir = dir.path().join("repo");
        let nested_cwd = repo_dir.join("desktop/src-tauri");
        std::fs::create_dir_all(&nested_cwd).unwrap();

        let env_file = repo_dir.join(".env");
        let synthetic_env = format!(
            "{}={}\n{}={}\n",
            "MINIMAX_API_KEY", "mm-ancestor-key-4321", "NVIDIA_API_KEY", "nv-ancestor-key-8765",
        );
        std::fs::write(&env_file, synthetic_env).unwrap();

        let store = ProviderStore::new(StorageRoot::new(dir.path().join("appdata")).unwrap());
        let imported = store
            .import_providers_from_env_with_roots(None, Some(&nested_cwd), false)
            .unwrap();
        let mm = imported.iter().find(|p| p.id == "env-minimax").unwrap();
        assert_eq!(mm.masked_api_key, "mm-****4321");
        let nv = imported.iter().find(|p| p.id == "env-nvidia").unwrap();
        assert_eq!(nv.masked_api_key, "nv-****8765");

        #[cfg(unix)]
        {
            let sym_repo = dir.path().join("sym_repo");
            let sym_cwd = sym_repo.join("desktop/src-tauri");
            std::fs::create_dir_all(&sym_cwd).unwrap();
            let sym_env = sym_repo.join(".env");
            std::os::unix::fs::symlink(&env_file, &sym_env).unwrap();

            let err = store
                .import_providers_from_env_with_roots(None, Some(&sym_cwd), false)
                .unwrap_err();
            assert!(matches!(err, StorageError::SymlinkRejected(_)));
        }
    }

    #[test]
    fn corrupt_providers_json_quarantines_and_recovers_cleanly() {
        let dir = TempDir::new().unwrap();
        let root = StorageRoot::new(dir.path().join("appdata")).unwrap();
        let store = ProviderStore::new(root.clone());

        write_atomic_private(root.root(), &root.providers_file(), b"{corrupt-json").unwrap();
        assert!(store.list_providers().unwrap().is_empty());
        assert!(!root.providers_file().exists());
        let quarantined = std::fs::read_dir(root.root())
            .unwrap()
            .filter_map(Result::ok)
            .any(|e| {
                e.file_name()
                    .to_string_lossy()
                    .starts_with("providers.json.corrupt.")
            });
        assert!(quarantined);

        let saved = store
            .save_provider(ProviderUpsertDto {
                id: Some("recovered".into()),
                name: "Recovered".into(),
                provider_type: ProviderType::OpenAi,
                base_url: "https://api.example.com/v1".into(),
                model: "gpt-4o-mini".into(),
                thinking_mode: ThinkingMode::Auto,
                concurrency: 2,
                is_default: true,
                api_key_update: ApiKeyUpdate::Replace {
                    value: "sk-recovered-1234".into(),
                },
                custom_headers: Vec::new(),
            })
            .unwrap();
        assert_eq!(saved.id, "recovered");
        assert_eq!(store.list_providers().unwrap().len(), 1);
    }

    #[test]
    fn parse_dotenv_str_strips_utf8_bom_and_unclosed_quotes_and_save_validates_base_url() {
        let bom_env = format!(
            "\u{FEFF}{}=\"mm-bom-12345\"\n{}=\"nv-unclosed-12345\n{}='oa-unclosed-67890\n",
            "MINIMAX_API_KEY", "NVIDIA_API_KEY", "OPENAI_API_KEY"
        );
        let parsed = parse_dotenv_str(&bom_env);
        assert_eq!(
            parsed.get("MINIMAX_API_KEY").map(String::as_str),
            Some("mm-bom-12345")
        );
        assert_eq!(
            parsed.get("NVIDIA_API_KEY").map(String::as_str),
            Some("nv-unclosed-12345")
        );
        assert_eq!(
            parsed.get("OPENAI_API_KEY").map(String::as_str),
            Some("oa-unclosed-67890")
        );

        let dir = TempDir::new().unwrap();
        let store = ProviderStore::new(StorageRoot::new(dir.path().join("appdata")).unwrap());
        for bad_url in ["https://", "http://bad host/v1", "ftp://example.com/v1"] {
            let err = store
                .save_provider(ProviderUpsertDto {
                    id: None,
                    name: "BadUrl".into(),
                    provider_type: ProviderType::OpenAi,
                    base_url: bad_url.into(),
                    model: "m".into(),
                    thinking_mode: ThinkingMode::Auto,
                    concurrency: 1,
                    is_default: true,
                    api_key_update: ApiKeyUpdate::Clear,
                    custom_headers: Vec::new(),
                })
                .unwrap_err();
            assert!(
                matches!(err, StorageError::InvalidInput(_)),
                "expected InvalidInput for {bad_url}"
            );
        }

        // Unknown VLM_PROVIDER does not invent a broken profile
        let mut unknown_map = HashMap::new();
        unknown_map.insert("VLM_PROVIDER".into(), "unknown".into());
        unknown_map.insert("VLM_API_KEY".into(), "sk-unknown-1234".into());
        assert!(build_env_candidates(&unknown_map).is_empty());
    }

    #[test]
    fn custom_headers_round_trip_dedup_validation_and_env_preservation() {
        let dir = TempDir::new().unwrap();
        let store = ProviderStore::new(StorageRoot::new(dir.path().join("appdata")).unwrap());

        let saved = store
            .save_provider(ProviderUpsertDto {
                id: Some("env-openai".into()),
                name: "OpenAI Compatible (env)".into(),
                provider_type: ProviderType::OpenAi,
                base_url: "https://gateway.example.com/v1".into(),
                model: "test-vl".into(),
                thinking_mode: ThinkingMode::Auto,
                concurrency: 4,
                is_default: true,
                api_key_update: ApiKeyUpdate::Replace {
                    value: "sk-test-1234".into(),
                },
                custom_headers: vec![
                    CustomHeaderDto {
                        name: "  ".into(),
                        value: "  ".into(),
                    },
                    CustomHeaderDto {
                        name: "X-Title".into(),
                        value: "OldTitle".into(),
                    },
                    CustomHeaderDto {
                        name: "x-title".into(),
                        value: "SubLift".into(),
                    },
                    CustomHeaderDto {
                        name: "x-opencode-session".into(),
                        value: "{{session_id}}".into(),
                    },
                ],
            })
            .unwrap();

        assert_eq!(
            saved.custom_headers,
            vec![
                CustomHeaderDto {
                    name: "x-title".into(),
                    value: "SubLift".into(),
                },
                CustomHeaderDto {
                    name: "x-opencode-session".into(),
                    value: "{{session_id}}".into(),
                },
            ]
        );

        let secret = store.resolve_provider_secret(Some("env-openai")).unwrap();
        assert_eq!(secret.custom_headers, saved.custom_headers);

        // .env re-import refreshes credentials while preserving manually configured custom_headers
        let env_file = dir.path().join(".env.sync");
        std::fs::write(
            &env_file,
            "OPENAI_API_KEY=sk-refreshed-5678\nOPENAI_BASE_URL=https://gateway.example.com/v1\nOPENAI_MODEL=test-vl-2\n",
        )
        .unwrap();
        let synced = store.import_providers_from_env(Some(&env_file)).unwrap();
        let synced_oa = synced.iter().find(|p| p.id == "env-openai").unwrap();
        assert_eq!(synced_oa.masked_api_key, "sk-****5678");
        assert_eq!(synced_oa.custom_headers, saved.custom_headers);

        // Rejects empty header name when value is non-empty, reserved headers, invalid characters, and CRLF
        for bad_headers in [
            vec![CustomHeaderDto {
                name: "".into(),
                value: "oops".into(),
            }],
            vec![CustomHeaderDto {
                name: "Host".into(),
                value: "evil.example.com".into(),
            }],
            vec![CustomHeaderDto {
                name: "Content-Length".into(),
                value: "123".into(),
            }],
            vec![CustomHeaderDto {
                name: "Bad Name".into(),
                value: "val".into(),
            }],
            vec![CustomHeaderDto {
                name: "X-Test".into(),
                value: "val\r\nX-Injected: 1".into(),
            }],
        ] {
            let err = store
                .save_provider(ProviderUpsertDto {
                    id: None,
                    name: "InvalidHeader".into(),
                    provider_type: ProviderType::OpenAi,
                    base_url: "https://gateway.example.com/v1".into(),
                    model: "m".into(),
                    thinking_mode: ThinkingMode::Auto,
                    concurrency: 1,
                    is_default: false,
                    api_key_update: ApiKeyUpdate::Clear,
                    custom_headers: bad_headers,
                })
                .unwrap_err();
            assert!(matches!(err, StorageError::InvalidInput(_)));
        }
    }

    #[test]
    fn resolve_transient_provider_reads_stored_key_overrides_without_saving_and_allows_empty_name() {
        let dir = TempDir::new().unwrap();
        let store = ProviderStore::new(StorageRoot::new(dir.path().join("appdata")).unwrap());
        let saved = store
            .save_provider(ProviderUpsertDto {
                id: None,
                name: "Primary Gateway".into(),
                provider_type: ProviderType::OpenAi,
                base_url: "https://api.example.com/v1".into(),
                model: "gpt-4o-mini".into(),
                thinking_mode: ThinkingMode::Auto,
                concurrency: 4,
                is_default: true,
                api_key_update: ApiKeyUpdate::Replace {
                    value: "sk-stored-secret-9999".into(),
                },
                custom_headers: vec![CustomHeaderDto {
                    name: "X-Title".into(),
                    value: "SubLift".into(),
                }],
            })
            .unwrap();

        // Keep reads stored secret and ignores empty name without persisting draft overrides.
        let kept = store
            .resolve_transient_provider(&ProviderUpsertDto {
                id: Some(saved.id.clone()),
                name: "".into(),
                provider_type: ProviderType::OpenAi,
                base_url: "https://staging.example.com/v1".into(),
                model: "gpt-4.1-mini".into(),
                thinking_mode: ThinkingMode::None,
                concurrency: 8,
                is_default: false,
                api_key_update: ApiKeyUpdate::Keep,
                custom_headers: vec![CustomHeaderDto {
                    name: "x-opencode-session".into(),
                    value: "{{session_id}}".into(),
                }],
            })
            .unwrap();
        assert_eq!(kept.api_key, "sk-stored-secret-9999");
        assert_eq!(kept.base_url, "https://staging.example.com/v1");
        assert_eq!(kept.custom_headers.len(), 1);

        // Replace overrides in memory only; stored secret stays intact.
        let replaced = store
            .resolve_transient_provider(&ProviderUpsertDto {
                id: Some(saved.id.clone()),
                name: "".into(),
                provider_type: ProviderType::OpenAi,
                base_url: "https://api.example.com/v1".into(),
                model: "gpt-4o-mini".into(),
                thinking_mode: ThinkingMode::Auto,
                concurrency: 4,
                is_default: false,
                api_key_update: ApiKeyUpdate::Replace {
                    value: "sk-temp-probe-1111".into(),
                },
                custom_headers: vec![],
            })
            .unwrap();
        assert_eq!(replaced.api_key, "sk-temp-probe-1111");

        // Clear uses empty key in memory only; stored secret still intact.
        let cleared = store
            .resolve_transient_provider(&ProviderUpsertDto {
                id: Some(saved.id.clone()),
                name: "".into(),
                provider_type: ProviderType::OpenAi,
                base_url: "https://api.example.com/v1".into(),
                model: "gpt-4o-mini".into(),
                thinking_mode: ThinkingMode::Auto,
                concurrency: 4,
                is_default: false,
                api_key_update: ApiKeyUpdate::Clear,
                custom_headers: vec![],
            })
            .unwrap();
        assert!(cleared.api_key.is_empty());

        let still_stored = store.resolve_provider_secret(Some(&saved.id)).unwrap();
        assert_eq!(still_stored.api_key, "sk-stored-secret-9999");
        assert_eq!(still_stored.base_url, "https://api.example.com/v1");
        assert_eq!(
            still_stored.custom_headers,
            vec![CustomHeaderDto {
                name: "X-Title".into(),
                value: "SubLift".into(),
            }]
        );
    }
}
