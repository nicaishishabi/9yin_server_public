#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace nineyin::gm {

/// Local HTTP update mirror (replaces update-server.exe).
/// Binds 127.0.0.1:80 and serves version/config stubs expected by fxlaunch.
/// On Windows, port 80 usually requires running as Administrator.
class UpdateHttpServer {
public:
  explicit UpdateHttpServer(std::string host = "127.0.0.1", uint16_t port = 80);
  ~UpdateHttpServer();

  void start();
  void stop();
  [[nodiscard]] bool running() const { return running_.load(); }
  [[nodiscard]] std::string url() const;

private:
  void run_loop();
  std::string handle(const std::string& method, const std::string& path);

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
