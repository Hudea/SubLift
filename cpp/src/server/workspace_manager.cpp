#include "sublift/server/workspace_manager.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <nlohmann/json.hpp>

#include "sublift/server/path_sandbox.hpp"

namespace sublift::server {

namespace {

std::filesystem::path default_config_path() {
  const char* cfg_dir = std::getenv("SUBLIFT_CONFIG_DIR");
  if (cfg_dir && *cfg_dir) {
    return std::filesystem::path(cfg_dir) / "workspace.json";
  }
  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::filesystem::path(home) / ".config" / "sublift" / "workspace.json";
  }
  return std::filesystem::temp_directory_path() / "sublift_workspace.json";
}

}  // namespace

WorkspaceManager::WorkspaceManager(const std::string& initial_dir,
                                   const std::string& config_override) {
  if (!config_override.empty()) {
    config_file_path_ = expand_tilde(config_override);
  } else {
    config_file_path_ = default_config_path();
  }

  // 优先级 1: 显式传入的初始目录
  if (!initial_dir.empty()) {
    std::string err;
    if (set_media_directory(initial_dir, &err)) {
      return;
    }
  }

  // 优先级 2: 环境变量 SUBLIFT_MEDIA_DIR 或 SUBLIFT_ALLOWED_MEDIA_ROOT
  if (const char* env_media = std::getenv("SUBLIFT_MEDIA_DIR"); env_media && *env_media) {
    std::string err;
    if (set_media_directory(env_media, &err)) {
      return;
    }
  }
  if (const char* env_root = std::getenv("SUBLIFT_ALLOWED_MEDIA_ROOT"); env_root && *env_root) {
    std::string err;
    if (set_media_directory(env_root, &err)) {
      return;
    }
  }

  // 优先级 3: 本地持久化配置
  load_persisted_config();
}

WorkspaceInfo WorkspaceManager::get_workspace_info() const {
  std::lock_guard<std::mutex> lock(mutex_);
  WorkspaceInfo info;
  if (!media_dir_.has_value() || media_dir_->empty()) {
    info.configured = false;
    info.media_dir = "";
    info.cache_dir = (std::filesystem::temp_directory_path() / "sublift_cache").string();
    info.video_count = 0;
    return info;
  }

  info.configured = true;
  info.media_dir = media_dir_->string();
  info.cache_dir = (*media_dir_ / ".sublift_cache").string();
  info.video_count = count_media_files_locked();
  return info;
}

std::vector<WorkspaceVideoFile> WorkspaceManager::list_media_files() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<WorkspaceVideoFile> result;
  if (!media_dir_.has_value() || media_dir_->empty()) return result;

  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(
      *media_dir_, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) return result;

  for (const auto end = std::filesystem::recursive_directory_iterator{}; it != end;) {
    if (it->is_symlink(ec)) {
      // 跳过软链接
    } else if (it->is_regular_file(ec) && !ec) {
      if (is_allowed_media_extension(it->path())) {
        std::string p_str = it->path().string();
        if (p_str.find(".sublift_cache") == std::string::npos &&
            p_str.find("/.git/") == std::string::npos) {
          WorkspaceVideoFile item;
          item.name = it->path().filename().string();
          item.path = it->path().string();
          item.relative_path = std::filesystem::relative(it->path(), *media_dir_, ec).string();
          item.size_bytes = it->file_size(ec);
          result.push_back(std::move(item));
        }
      }
    }
    if (it->is_directory(ec) && (it.depth() >= 5 || it->path().filename() == ".sublift_cache" ||
                                 it->path().filename() == ".git")) {
      it.disable_recursion_pending();
    }
    it.increment(ec);
    if (ec) break;
  }
  return result;
}

bool WorkspaceManager::set_media_directory(const std::string& raw_path, std::string* error_msg) {
  if (raw_path.empty()) {
    if (error_msg) *error_msg = "路径不能为空";
    return false;
  }

  std::filesystem::path expanded = expand_tilde(raw_path);
  std::error_code ec;

  std::filesystem::path canonical_p = std::filesystem::canonical(expanded, ec);
  if (ec) {
    if (error_msg) *error_msg = "目录不存在或无法访问: " + raw_path;
    return false;
  }

  if (!std::filesystem::is_directory(canonical_p, ec)) {
    if (error_msg) *error_msg = "指定的路径不是文件夹目录: " + canonical_p.string();
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  media_dir_ = canonical_p;
  ensure_cache_directories_locked();
  save_persisted_config();
  return true;
}

void WorkspaceManager::clear_workspace() {
  std::lock_guard<std::mutex> lock(mutex_);
  media_dir_ = std::nullopt;
  std::error_code ec;
  std::filesystem::remove(config_file_path_, ec);
}

std::optional<std::filesystem::path> WorkspaceManager::get_media_dir() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return media_dir_;
}

std::filesystem::path WorkspaceManager::get_cache_dir() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (media_dir_.has_value() && !media_dir_->empty()) {
    return *media_dir_ / ".sublift_cache";
  }
  return std::filesystem::temp_directory_path() / "sublift_cache";
}

std::filesystem::path WorkspaceManager::get_remux_cache_dir() const {
  return get_cache_dir() / "remux";
}

std::filesystem::path WorkspaceManager::get_frames_cache_dir() const {
  return get_cache_dir() / "frames";
}

void WorkspaceManager::ensure_cache_directories_locked() {
  if (!media_dir_.has_value() || media_dir_->empty()) return;

  std::error_code ec;
  auto cache_root = *media_dir_ / ".sublift_cache";
  std::filesystem::create_directories(cache_root / "remux", ec);
  std::filesystem::create_directories(cache_root / "frames", ec);
  std::filesystem::create_directories(cache_root / "exports", ec);
}

int WorkspaceManager::count_media_files_locked() const {
  if (!media_dir_.has_value() || media_dir_->empty()) return 0;

  int count = 0;
  std::error_code ec;
  // 浅层扫描最多 4 层，防超大目录卡死
  std::filesystem::recursive_directory_iterator it(
      *media_dir_, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) return 0;

  for (const auto end = std::filesystem::recursive_directory_iterator{}; it != end;) {
    if (it->is_symlink(ec)) {
      // 跳过软链接
    } else if (it->is_regular_file(ec) && !ec) {
      if (is_allowed_media_extension(it->path())) {
        // 排除 .sublift_cache 目录下的内部文件
        std::string p_str = it->path().string();
        if (p_str.find(".sublift_cache") == std::string::npos) {
          count++;
        }
      }
    }
    if (it->is_directory(ec) && (it.depth() >= 4 || it->path().filename() == ".sublift_cache" ||
                                 it->path().filename() == ".git")) {
      it.disable_recursion_pending();
    }
    it.increment(ec);
    if (ec) break;
  }
  return count;
}

void WorkspaceManager::load_persisted_config() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code ec;
  if (!std::filesystem::exists(config_file_path_, ec) ||
      !std::filesystem::is_regular_file(config_file_path_, ec)) {
    return;
  }

  std::ifstream f(config_file_path_);
  if (!f) return;

  try {
    nlohmann::json j;
    f >> j;
    if (j.contains("media_dir") && j["media_dir"].is_string()) {
      std::string raw = j["media_dir"].get<std::string>();
      std::filesystem::path p = expand_tilde(raw);
      std::filesystem::path canonical_p = std::filesystem::canonical(p, ec);
      if (!ec && std::filesystem::is_directory(canonical_p, ec)) {
        media_dir_ = canonical_p;
        ensure_cache_directories_locked();
      }
    }
  } catch (...) {
    // 损坏配置静默忽略
  }
}

void WorkspaceManager::save_persisted_config() {
  if (config_file_path_.empty()) return;

  std::error_code ec;
  std::filesystem::create_directories(config_file_path_.parent_path(), ec);

  nlohmann::json j;
  if (media_dir_.has_value()) {
    j["media_dir"] = media_dir_->string();
  } else {
    j["media_dir"] = "";
  }

  std::ofstream f(config_file_path_);
  if (f) {
    f << j.dump(2) << std::endl;
  }
}

DiskSaveResult WorkspaceManager::save_subtitles_atomic(
    const std::string& target_path_str,
    const std::string& srt_content,
    ConflictPolicy policy,
    bool allow_empty,
    int entry_count) {
  std::optional<std::filesystem::path> allowed_root = get_media_dir();

  std::filesystem::path validated_target;
  try {
    validated_target = resolve_and_validate_output_path(target_path_str, allowed_root);
  } catch (const PathSecurityException& e) {
    return DiskSaveResult{
        .success = false,
        .status = "error",
        .target_path = target_path_str,
        .saved_path = "",
        .error_message = std::string("Path Security Error: ") + e.what(),
        .empty_result = false,
        .entry_count = entry_count
    };
  }

  // 空字幕保护机制：若字幕条目为 0 或内容空白，且未显式指定 allow_empty，则不落盘虚假 0 字节文件
  bool is_blank = true;
  for (char c : srt_content) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      is_blank = false;
      break;
    }
  }

  if ((entry_count == 0 || is_blank) && !allow_empty) {
    return DiskSaveResult{
        .success = true,
        .status = "empty_result",
        .target_path = validated_target.string(),
        .saved_path = "",
        .error_message = "",
        .empty_result = true,
        .entry_count = 0
    };
  }

  std::error_code ec;
  std::filesystem::path final_target = validated_target;

  // 冲突解决策略
  if (std::filesystem::exists(validated_target, ec)) {
    if (policy == ConflictPolicy::Skip) {
      return DiskSaveResult{
          .success = true,
          .status = "skipped",
          .target_path = validated_target.string(),
          .saved_path = "",
          .error_message = "File already exists and conflict policy is skip",
          .empty_result = false,
          .entry_count = entry_count
      };
    } else if (policy == ConflictPolicy::DeterministicRename) {
      std::string stem = validated_target.stem().string();
      std::string ext = validated_target.extension().string();
      auto parent = validated_target.parent_path();
      int counter = 1;
      while (std::filesystem::exists(final_target, ec) && counter < 10000) {
        final_target = parent / (stem + "_" + std::to_string(counter) + ext);
        counter++;
      }
    } else if (policy == ConflictPolicy::Replace) {
      final_target = validated_target;
    }
  }

  // 确保父目录存在
  auto parent_dir = final_target.parent_path();
  std::filesystem::create_directories(parent_dir, ec);
  if (ec) {
    return DiskSaveResult{
        .success = false,
        .status = "error",
        .target_path = validated_target.string(),
        .saved_path = "",
        .error_message = "Failed to create parent directory: " + ec.message(),
        .empty_result = false,
        .entry_count = entry_count
    };
  }

  // 原子写过程：同目录下临时文件 -> fsync -> rename
  std::string temp_template_str = (parent_dir / ("." + final_target.filename().string() + ".tmp.XXXXXX")).string();
  std::vector<char> temp_buf(temp_template_str.begin(), temp_template_str.end());
  temp_buf.push_back('\0');

  int fd = ::mkstemp(temp_buf.data());
  if (fd == -1) {
    return DiskSaveResult{
        .success = false,
        .status = "error",
        .target_path = validated_target.string(),
        .saved_path = "",
        .error_message = std::string("Failed to create temporary file: ") + std::strerror(errno),
        .empty_result = false,
        .entry_count = entry_count
    };
  }

  std::filesystem::path temp_file_path(temp_buf.data());

  // 写入数据
  size_t total_written = 0;
  const char* data_ptr = srt_content.data();
  size_t data_len = srt_content.size();

  while (total_written < data_len) {
    ssize_t w = ::write(fd, data_ptr + total_written, data_len - total_written);
    if (w < 0) {
      if (errno == EINTR) continue;
      int err = errno;
      ::close(fd);
      std::filesystem::remove(temp_file_path, ec);
      return DiskSaveResult{
          .success = false,
          .status = "error",
          .target_path = validated_target.string(),
          .saved_path = "",
          .error_message = std::string("Failed to write to temporary file: ") + std::strerror(err),
          .empty_result = false,
          .entry_count = entry_count
      };
    }
    total_written += static_cast<size_t>(w);
  }

  // 刷盘落盘保障
  ::fsync(fd);
  ::close(fd);

  // 原子重命名
  std::filesystem::rename(temp_file_path, final_target, ec);
  if (ec) {
    std::filesystem::remove(temp_file_path, ec);
    return DiskSaveResult{
        .success = false,
        .status = "error",
        .target_path = validated_target.string(),
        .saved_path = "",
        .error_message = "Failed to atomically rename temporary file: " + ec.message(),
        .empty_result = false,
        .entry_count = entry_count
    };
  }

  return DiskSaveResult{
      .success = true,
      .status = "saved",
      .target_path = validated_target.string(),
      .saved_path = final_target.string(),
      .error_message = "",
      .empty_result = false,
      .entry_count = entry_count
  };
}

}  // namespace sublift::server
