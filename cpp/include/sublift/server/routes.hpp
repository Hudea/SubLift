#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "sublift/models.hpp"
#include "sublift/server/job_manager.hpp"

#include "sublift/server/workspace_manager.hpp"

namespace httplib {
class Server;
}

namespace sublift::server {

struct SystemEngineInfo {
  std::string name;
  bool available{false};
  std::string detail;
  std::string model_type;
  std::string model_root;
};

struct SystemInfoDTO {
  std::string version;
  std::string runtime{"cpp"};
  std::vector<std::string> capabilities;
  std::vector<SystemEngineInfo> engines;
  bool ffmpeg_available{false};
  std::string ffmpeg_path;

  [[nodiscard]] nlohmann::json to_json() const;
};

/// 收集并探活系统信息（引擎、运行时、FFmpeg）
[[nodiscard]] SystemInfoDTO collect_system_info();

/// 获取视频文件的 MIME 类型
[[nodiscard]] std::string get_video_mime_type(const std::filesystem::path& path);

/// 将毫秒时间戳格式化为 SRT 标准 HH:MM:SS,mmm
[[nodiscard]] std::string format_srt_timestamp(std::int64_t ms);

/// 将字幕条目列表格式化为标准 UTF-8 SRT 字符串
[[nodiscard]] std::string format_entries_to_srt(const std::vector<sublift::SubtitleEntry>& entries);

/// 注册所有 Web API 路由（REST + 视频流 + SSE + 工作区配置 + SRT 导出）及静态文件托管
void register_routes(httplib::Server& server,
                     std::shared_ptr<JobManager> job_manager,
                     const std::string& static_dir = "",
                     std::shared_ptr<WorkspaceManager> workspace_manager = nullptr,
                     bool workspace_locked = false);

}  // namespace sublift::server
