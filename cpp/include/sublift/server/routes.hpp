#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

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

/// 注册所有 Web API 路由及静态文件托管
void register_routes(httplib::Server& server, const std::string& static_dir = "");

}  // namespace sublift::server
