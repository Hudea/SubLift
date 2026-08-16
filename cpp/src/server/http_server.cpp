#include "sublift/server/http_server.hpp"

#include <httplib.h>

#include "sublift/server/routes.hpp"

namespace sublift::server {

HttpServer::HttpServer(ServerConfig config)
    : config_(std::move(config)), svr_(std::make_unique<httplib::Server>()) {
  // CORS Headers
  svr_->set_default_headers({
      {"Access-Control-Allow-Origin", "*"},
      {"Access-Control-Allow-Methods", "GET, POST, OPTIONS, HEAD"},
      {"Access-Control-Allow-Headers", "*"},
  });

  register_routes(*svr_, config_.static_dir);
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
