#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sublift::server {

struct WorkspaceInfo {
  bool configured = false;
  std::string media_dir;
  std::string cache_dir;
  int video_count = 0;
};

struct WorkspaceVideoFile {
  std::string name;
  std::string path;
  std::string relative_path;
  std::uintmax_t size_bytes = 0;
};

enum class ConflictPolicy {
  DeterministicRename,
  Skip,
  Replace
};

inline ConflictPolicy parse_conflict_policy(const std::string& str) {
  if (str == "skip") return ConflictPolicy::Skip;
  if (str == "replace") return ConflictPolicy::Replace;
  return ConflictPolicy::DeterministicRename;
}

inline std::string to_string(ConflictPolicy policy) {
  switch (policy) {
    case ConflictPolicy::Skip: return "skip";
    case ConflictPolicy::Replace: return "replace";
    case ConflictPolicy::DeterministicRename: return "deterministic_rename";
  }
  return "deterministic_rename";
}

struct DiskSaveResult {
  bool success = false;
  std::string status;  // "saved", "skipped", "error", "empty_result"
  std::string target_path;
  std::string saved_path;
  std::string error_message;
  bool empty_result = false;
  int entry_count = 0;
};

class WorkspaceManager {
 public:
  explicit WorkspaceManager(const std::string& initial_dir = "",
                            const std::string& config_override = "");

  /// 获取当前工作区状态与统计
  [[nodiscard]] WorkspaceInfo get_workspace_info() const;

  /// 列出工作区内的所有可用视频文件
  [[nodiscard]] std::vector<WorkspaceVideoFile> list_media_files() const;

  /// 设置并校验新的媒体工作目录
  /// @param raw_path 用户输入的路径（支持 ~）
  /// @param error_msg 失败时的错误信息
  /// @return true 表示设置成功，false 表示校验失败
  bool set_media_directory(const std::string& raw_path, std::string* error_msg = nullptr);

  /// 重置/清除工作区配置
  void clear_workspace();

  /// 获取当前配置的媒体根目录（若未配置返回 std::nullopt）
  [[nodiscard]] std::optional<std::filesystem::path> get_media_dir() const;

  /// 获取当前工作区下的专属 .sublift_cache 目录
  [[nodiscard]] std::filesystem::path get_cache_dir() const;

  /// 获取转封装 MP4 缓存目录 (<media_dir>/.sublift_cache/remux)
  [[nodiscard]] std::filesystem::path get_remux_cache_dir() const;

  /// 获取抽帧 JPEG 缓存目录 (<media_dir>/.sublift_cache/frames)
  [[nodiscard]] std::filesystem::path get_frames_cache_dir() const;

  /// 原子落盘保存字幕文件（临时文件 -> fsync -> rename），支持冲突策略与空字幕检测
  [[nodiscard]] DiskSaveResult save_subtitles_atomic(
      const std::string& target_path_str,
      const std::string& srt_content,
      ConflictPolicy policy = ConflictPolicy::DeterministicRename,
      bool allow_empty = false,
      int entry_count = 0);

 private:
  void load_persisted_config();
  void save_persisted_config();
  void ensure_cache_directories_locked();
  int count_media_files_locked() const;

  mutable std::mutex mutex_;
  std::filesystem::path config_file_path_;
  std::optional<std::filesystem::path> media_dir_;
};

}  // namespace sublift::server
