#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "sublift/models.hpp"
#include "sublift/ocr_execution.hpp"
#include "sublift/server/job_manager.hpp"

#include "sublift/server/workspace_manager.hpp"

namespace httplib {
class Server;
}

namespace sublift::server {

/// paddle 引擎的可选执行后端信息（ADR-0041 / docs/design/gpu-execution.md §9）。
struct EngineExecutionInfo {
  std::string provider;  // cpu / cuda
  std::optional<std::int32_t> device_id;  // CPU 为空；CUDA 为容器内逻辑编号
  std::string state;  // ready / unavailable
};

/// POST /api/jobs 的 paddle_execution 断言检查结果。
struct JobPaddleExecutionCheck {
  int http_status{0};  // 0 表示通过；否则为待返回的 400/409
  std::string error;
  sublift::PaddleExecutionConfig asserted;
};

/// 校验任务请求的执行后端断言（ADR-0041）：省略字段（整体或子字段为 null
/// 视为缺省）即继承部署绑定；提交与部署相同的值通过；不匹配返回 409；
/// 非法 provider/设备、CPU 携带设备、非 paddle 引擎携带该字段返回 400。
[[nodiscard]] JobPaddleExecutionCheck check_job_paddle_execution(
    const std::string& engine, const nlohmann::json& body,
    const sublift::PaddleExecutionConfig& bound);

struct SystemEngineInfo {
  std::string name;
  bool available{false};
  std::string detail;
  std::string model_type;
  std::string model_root;
  /// 仅 paddle 引擎携带；旧服务缺失时客户端不得猜测。
  std::optional<EngineExecutionInfo> execution;
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

/// 收集并探活系统信息（引擎、运行时、FFmpeg）。
/// paddle 条目按部署绑定执行配置填充可选 execution 对象；默认 CPU。
[[nodiscard]] SystemInfoDTO collect_system_info(
    const sublift::PaddleExecutionConfig& paddle_execution = {});

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
                     bool workspace_locked = false,
                     const sublift::PaddleExecutionConfig& paddle_execution = {});

}  // namespace sublift::server
