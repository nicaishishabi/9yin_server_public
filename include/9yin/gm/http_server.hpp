#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace nineyin::gm {

/// Minimal localhost-only HTTP server for the GM web panel.
/// Default: 127.0.0.1:19080  (does not accept external connections by design)
class HttpServer {
public:
  explicit HttpServer(std::string bind_host = "127.0.0.1", uint16_t port = 19080);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  void start();
  void stop();

  [[nodiscard]] std::string url() const;

private:
  void run_loop();
  std::string handle_request(const std::string& method, const std::string& path,
                             const std::string& body);

  std::string host_;
  uint16_t port_;
  std::atomic<bool> running_{false};
  std::thread thread_;
#if defined(_WIN32)
  uintptr_t listen_fd_ = static_cast<uintptr_t>(-1);
#else
  int listen_fd_ = -1;
#endif
};

}  // namespace nineyin::gm
