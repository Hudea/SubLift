#pragma once

#include <memory>
#include <string>

#include "sublift/ocr_execution.hpp"
#include "sublift/server/job_manager.hpp"
#include "sublift/server/workspace_manager.hpp"

namespace httplib {
class Server;
}

namespace sublift::server {

struct ServerConfig {
  std::string host{"127.0.0.1"};
  int port{8080};
  std::string static_dir{""};
  int thread_pool_size{8};
  /// CORS 放行来源。默认为空 = 不发送任何 CORS 头（同源部署即 Web UI 由本服务
  /// 托管时的安全默认）；仅在跨源部署（如 Vite dev server 调试）时显式设置，
  /// 例如 "*" 或具体 origin。空字符串之外的值将原样写入 Allow-Origin。
  std::string cors_origin{""};
  /// 媒体工作区目录（若指定则自动激活，无需前端二次配置）
  std::string media_dir{""};
  /// 工作区持久化配置文件覆盖路径
  std::string config_file{""};
  /// 共享访问令牌。非空时 /api/* 要求 `Authorization: Bearer <token>`。
  /// 局域网自托管的最小访问控制手段：不提供账号体系，只防止端口被误暴露后被任意调用。
  std::string access_token{""};
  /// 工作区是否由启动期配置锁定。锁定后不接受运行时修改媒体授权根。
  bool workspace_locked{false};
  /// Paddle 执行后端（ADR-0041）。默认 CPU；显式请求 cuda 而构建不支持时
  /// 启动期拒绝。该配置贯通提取、批量与智能选区的引擎创建。
  sublift::PaddleExecutionConfig paddle_execution{};
};

/// 判断监听地址是否属于 loopback。
///
/// 非 loopback 暴露改变 Web 入口的信任边界（ADR-0039/ADR-0040），启动前必须已具备
/// 媒体授权根；本函数用于在启动时执行该前置检查。
[[nodiscard]] bool is_loopback_host(const std::string& host) noexcept;

/// 启动期访问控制检查。返回错误信息；空字符串表示通过。
///
/// 约束：
/// 1. 非 loopback 监听必须有已配置的媒体根；
/// 2. 非 loopback 监听必须锁定工作区，避免远程把授权根改到宿主任意路径。
[[nodiscard]] std::string validate_remote_exposure(const ServerConfig& config,
                                                   bool media_root_configured);

/// 启动期 OCR 能力检查。返回错误信息；空字符串表示通过。
///
/// 非 loopback 自托管要求 Paddle 可用（ADR-0038/0040）：不得静默降级 mock。
/// loopback 允许 Paddle 缺失，以便本机开发与显式 `engine=mock` 诊断。
[[nodiscard]] std::string validate_remote_paddle(const ServerConfig& config,
                                                 bool paddle_available);

/// 校验请求的 Bearer token；未配置 token 时直接放行。
[[nodiscard]] bool authorize_request(const ServerConfig& config, const std::string& auth_header);

class HttpServer {
 public:
  explicit HttpServer(ServerConfig config = {});
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&&) noexcept;
  HttpServer& operator=(HttpServer&&) noexcept;

  /// 绑定到随机可用端口（port 0），返回分配的实际端口号
  [[nodiscard]] int bind_to_any_port(const std::string& host = "127.0.0.1");

  /// 绑定到指定主机与端口
  [[nodiscard]] bool bind(const std::string& host, int port);

  /// 启动监听（阻塞当前线程，直到调用 stop）
  bool listen();

  /// 在已绑定端口上启动监听（阻塞当前线程，直到调用 stop）
  bool listen_after_bind();

  /// 等待服务就绪并进入监听循环
  void wait_until_ready() const;

  /// 优雅停止服务
  void stop();

  /// 查询服务是否正在运行
  [[nodiscard]] bool is_running() const noexcept;

  /// 获取当前监听端口
  [[nodiscard]] int port() const noexcept { return config_.port; }

  /// 获取当前监听主机
  [[nodiscard]] const std::string& host() const noexcept { return config_.host; }

  /// 获取底层 httplib::Server 引用（供扩展路由）
  [[nodiscard]] httplib::Server& raw_server();

  /// 获取绑定的 JobManager 引用
  [[nodiscard]] std::shared_ptr<JobManager> job_manager() const noexcept { return job_manager_; }

  /// 获取绑定的 WorkspaceManager 引用
  [[nodiscard]] std::shared_ptr<WorkspaceManager> workspace_manager() const noexcept {
    return workspace_manager_;
  }

 private:
  ServerConfig config_;
  std::shared_ptr<JobManager> job_manager_;
  std::shared_ptr<WorkspaceManager> workspace_manager_;
  std::unique_ptr<httplib::Server> svr_;
  bool bound_{false};
};

}  // namespace sublift::server
