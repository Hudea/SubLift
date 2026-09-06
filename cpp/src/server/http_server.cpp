#include "sublift/server/http_server.hpp"

#include <algorithm>
#include <string>
#include <string_view>

#include <httplib.h>

#include "sublift/server/routes.hpp"

namespace sublift::server {

namespace {

/// 常量时间比较，避免通过响应耗时逐字节猜测共享 token。
bool constant_time_equals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

std::string_view trim(std::string_view value) noexcept {
  const auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (!value.empty() && is_space(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_space(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

}  // namespace

bool is_loopback_host(const std::string& host) noexcept {
  const std::string_view h = trim(host);
  if (h.empty()) {
    return false;
  }
  if (h == "localhost" || h == "localhost.") {
    return true;
  }
  // IPv6 loopback 的常见书写形式（含 IPv6-mapped IPv4 ::ffff:127.0.0.1）
  if (h == "::1" || h == "[::1]" || h == "0:0:0:0:0:0:0:1" || h == "[0:0:0:0:0:0:0:1]") {
    return true;
  }
  const std::string_view mapped = "::ffff:127.0.0.1";
  if (h.size() >= mapped.size() && h.compare(0, mapped.size(), mapped) == 0) {
    return true;
  }
  // IPv4 loopback：整个 127.0.0.0/8 都是本机回环
  if (h.compare(0, 4, "127.") == 0) {
    // 仅接受 127.x.x.x 形式的点分十进制，避免把 "127.example.com" 误判为回环
    int dots = 0;
    bool all_digits_or_dot = true;
    for (char c : h) {
      if (c == '.') {
        ++dots;
      } else if (c < '0' || c > '9') {
        all_digits_or_dot = false;
        break;
      }
    }
    return all_digits_or_dot && dots == 3;
  }
  return false;
}

std::string validate_remote_exposure(const ServerConfig& config, bool media_root_configured) {
  if (is_loopback_host(config.host)) {
    return "";
  }
  if (!media_root_configured) {
    return "拒绝以非 loopback 地址 " + config.host +
           " 启动：必须先配置媒体授权根（SUBLIFT_MEDIA_DIR / --media-dir）。"
           "远程暴露工作区未定的服务会让任意访问者重新指定宿主目录。";
  }
  if (!config.workspace_locked) {
    return "拒绝以非 loopback 地址 " + config.host +
           " 启动：媒体授权根必须锁定（配置来自启动参数或环境变量），"
           "否则远程访问者可修改工作区。";
  }
  return "";
}

std::string validate_remote_paddle(const ServerConfig& config, bool paddle_available) {
  if (is_loopback_host(config.host) || paddle_available) {
    return "";
  }
  return "拒绝以非 loopback 地址 " + config.host +
         " 启动：Paddle OCR 不可用。局域网自托管不得静默降级 mock（ADR-0038/0040）。";
}

bool authorize_request(const ServerConfig& config, const std::string& auth_header) {
  if (config.access_token.empty()) {
    return true;
  }
  const std::string_view header = trim(auth_header);
  const std::string_view prefix = "Bearer ";
  if (header.size() <= prefix.size()) {
    return false;
  }
  if (header.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  return constant_time_equals(trim(header.substr(prefix.size())), config.access_token);
}

HttpServer::HttpServer(ServerConfig config)
    : config_(std::move(config)),
      job_manager_(std::make_shared<JobManager>(1, 50)),
      workspace_manager_(std::make_shared<WorkspaceManager>(config_.media_dir, config_.config_file)),
      svr_(std::make_unique<httplib::Server>()) {
  job_manager_->set_paddle_execution(config_.paddle_execution);
  if (auto media_p = workspace_manager_->get_media_dir(); media_p.has_value()) {
    job_manager_->set_state_file_path(workspace_manager_->get_cache_dir() / "jobs_state.v1.json");
  }

  // CORS 默认关闭（同源部署的安全默认）；显式配置 cors_origin 时才放行，
  // 避免"任意网页跨域读取本地文件/提交任务"的攻击面。
  if (!config_.cors_origin.empty()) {
    svr_->set_default_headers({
        {"Access-Control-Allow-Origin", config_.cors_origin},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS, HEAD, DELETE"},
        {"Access-Control-Allow-Headers", "Content-Type, Last-Event-ID"},
    });
  }

  // 共享 token 作为局域网自托管的最小访问控制（ADR-0040）。静态资源与首页保持可访问，
  // 以便未持 token 的访问者得到明确提示而不是空白页；API 与 SSE 一律要求 Bearer。
  if (!config_.access_token.empty()) {
    svr_->set_pre_routing_handler(
        [this](const httplib::Request& req, httplib::Response& res) -> httplib::Server::HandlerResponse {
          if (req.path.rfind("/api/", 0) != 0) {
            return httplib::Server::HandlerResponse::Unhandled;
          }
          if (authorize_request(config_, req.get_header_value("Authorization"))) {
            return httplib::Server::HandlerResponse::Unhandled;
          }
          res.status = 401;
          res.set_header("WWW-Authenticate", "Bearer");
          res.set_content(R"({"error":"unauthorized","detail":"需要 Bearer 访问令牌"})",
                          "application/json");
          return httplib::Server::HandlerResponse::Handled;
        });
  }

  register_routes(*svr_, job_manager_, config_.static_dir, workspace_manager_,
                  config_.workspace_locked, config_.paddle_execution);
}

HttpServer::~HttpServer() {
  stop();
}

HttpServer::HttpServer(HttpServer&&) noexcept = default;
HttpServer& HttpServer::operator=(HttpServer&&) noexcept = default;

int HttpServer::bind_to_any_port(const std::string& host) {
  config_.host = host;
  int assigned_port = svr_->bind_to_any_port(host.c_str());
  if (assigned_port > 0) {
    config_.port = assigned_port;
    bound_ = true;
  }
  return assigned_port;
}

bool HttpServer::bind(const std::string& host, int port) {
  config_.host = host;
  config_.port = port;
  bound_ = svr_->bind_to_port(host.c_str(), port);
  return bound_;
}

bool HttpServer::listen() {
  if (bound_) {
    return svr_->listen_after_bind();
  }
  return svr_->listen(config_.host.c_str(), config_.port);
}

bool HttpServer::listen_after_bind() {
  return svr_->listen_after_bind();
}

void HttpServer::wait_until_ready() const {
  if (svr_) {
    svr_->wait_until_ready();
  }
}

void HttpServer::stop() {
  if (svr_ && svr_->is_running()) {
    svr_->stop();
  }
}

bool HttpServer::is_running() const noexcept {
  return svr_ && svr_->is_running();
}

httplib::Server& HttpServer::raw_server() {
  return *svr_;
}

}  // namespace sublift::server
