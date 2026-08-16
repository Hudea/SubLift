#pragma once

#include <memory>
#include <string>

namespace httplib {
class Server;
}

namespace sublift::server {

struct ServerConfig {
  std::string host{"127.0.0.1"};
  int port{8080};
  std::string static_dir{""};
  int thread_pool_size{8};
};

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

 private:
  ServerConfig config_;
  std::unique_ptr<httplib::Server> svr_;
  bool bound_{false};
};

}  // namespace sublift::server
