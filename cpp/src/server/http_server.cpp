#include "sublift/server/http_server.hpp"

#include <httplib.h>

#include "sublift/server/routes.hpp"

namespace sublift::server {

HttpServer::HttpServer(ServerConfig config)
    : config_(std::move(config)),
      job_manager_(std::make_shared<JobManager>(1, 50)),
      workspace_manager_(std::make_shared<WorkspaceManager>(config_.media_dir, config_.config_file)),
      svr_(std::make_unique<httplib::Server>()) {
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

  register_routes(*svr_, job_manager_, config_.static_dir, workspace_manager_);
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
