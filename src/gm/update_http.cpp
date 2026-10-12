#include "9yin/gm/update_http.hpp"
#include "9yin/util/log.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalid = INVALID_SOCKET;
inline void sock_close(socket_t s) { ::closesocket(s); }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalid = -1;
inline void sock_close(socket_t s) { ::close(s); }
#endif

namespace nineyin::gm {
namespace {

std::string http_ok(const std::string& body,
                    const char* ctype = "application/octet-stream") {
  std::ostringstream o;
  o << "HTTP/1.1 200 OK\r\n"
    << "Content-Type: " << ctype << "\r\n"
    << "Content-Length: " << body.size() << "\r\n"
    << "Connection: close\r\n"
    << "Cache-Control: no-cache\r\n"
    << "\r\n"
    << body;
  return o.str();
}

std::string http_404() {
  const std::string body = "not found";
  std::ostringstream o;
  o << "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: "
    << body.size() << "\r\nConnection: close\r\n\r\n"
    << body;
  return o.str();
}

std::string webroot_base() {
  const char* env = std::getenv("NINEYIN_SERVER_ROOT");
  if (env && *env) {
    return std::string(env) + "/webroot";
  }
  return "webroot";
}

bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

const char kFallbackConfigFiles[] =
    "files|3\r\n"
    "updater\\updater_cfg\\server_list.ini\r\n"
    "updater\\updater_cfg\\url_config.ini\r\n"
    "updater\\updater_cfg\\content.txt\r\n";

const char kFallbackVersion[] = "[main]\r\nversion=1.0.2.591\r\n";

}  // namespace

UpdateHttpServer::UpdateHttpServer(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {}

UpdateHttpServer::~UpdateHttpServer() { stop(); }

std::string UpdateHttpServer::url() const {
  return "http://" + host_ + (port_ == 80 ? "" : (":" + std::to_string(port_))) +
         "/";
}

void UpdateHttpServer::start() {
  if (running_) return;
  running_ = true;
  thread_ = std::thread([this] { run_loop(); });
}

void UpdateHttpServer::stop() {
  if (!running_) return;
  running_ = false;
  if (listen_fd_ != static_cast<decltype(listen_fd_)>(kInvalid) &&
      listen_fd_ != 0) {
    sock_close(static_cast<socket_t>(listen_fd_));
    listen_fd_ = static_cast<decltype(listen_fd_)>(kInvalid);
  }
  if (thread_.joinable()) thread_.join();
}

std::string UpdateHttpServer::handle(const std::string& method,
                                     const std::string& path) {
  std::string p = path;
  if (p.empty()) p = "/";
  NINEYIN_LOG_INFO("update-http >>> %s %s", method.c_str(), p.c_str());

  if (method != "GET" && method != "HEAD") {
    return http_404();
  }

  const std::string root = webroot_base();
  std::string rel = p;
  if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
  std::string fs_path = root + "/" + rel;

  std::string body;
  if (read_file(fs_path, body)) {
    NINEYIN_LOG_INFO("update-http served %s (%zu bytes)", fs_path.c_str(),
                     body.size());
    return http_ok(body);
  }

  auto contains = [&](const char* s) {
    return p.find(s) != std::string::npos;
  };

  if (contains("config.files")) {
    if (read_file(root + "/update/config/config.files", body)) {
      NINEYIN_LOG_INFO("update-http config.files disk (%zu bytes)", body.size());
      return http_ok(body);
    }
    NINEYIN_LOG_INFO("update-http config.files fallback size+md5");
    return http_ok(kFallbackConfigFiles);
  }

  if (contains("version.ini") || p == "/update/config" ||
      p == "/update/config/") {
    if (read_file(root + "/update/config/version.ini", body)) return http_ok(body);
    return http_ok(kFallbackVersion);
  }

  if (contains("server_list")) {
    if (read_file(root + "/update/config/updater/updater_cfg/server_list.ini", body) ||
        read_file(root + "/update/config/server_list.ini", body))
      return http_ok(body);
  }
  if (contains("url_config")) {
    if (read_file(root + "/update/config/updater/updater_cfg/url_config.ini", body) ||
        read_file(root + "/update/config/url_config.ini", body))
      return http_ok(body);
  }
  if (contains("content")) {
    if (read_file(root + "/update/config/updater/updater_cfg/content.txt", body) ||
        read_file(root + "/update/config/content.txt", body))
      return http_ok(body);
    return http_ok("");
  }
  if (contains("maintain")) {
    return http_ok("[maintain]\r\nContent=\r\nURL=\r\n");
  }
  if (contains("/update/patch") || contains("/update/repair")) {
    return http_ok("");
  }
  if (contains("/update/")) {
    return http_ok(kFallbackVersion);
  }
  if (p == "/" || contains("index")) {
    return http_ok("9yin local update mirror OK\n");
  }

  NINEYIN_LOG_WARN("update-http 404: %s webroot=%s", p.c_str(), root.c_str());
  return http_404();
}

void UpdateHttpServer::run_loop() {
  socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd == kInvalid) {
    NINEYIN_LOG_ERROR("update-http: socket failed");
    running_ = false;
    return;
  }
  int yes = 1;
#if defined(_WIN32)
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
    NINEYIN_LOG_ERROR("update-http: bad host");
    sock_close(fd);
    running_ = false;
    return;
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    NINEYIN_LOG_ERROR(
        "update-http: bind %s:%u FAILED — need Administrator for port 80",
        host_.c_str(), port_);
    sock_close(fd);
    running_ = false;
    return;
  }
  if (::listen(fd, 16) < 0) {
    NINEYIN_LOG_ERROR("update-http: listen failed");
    sock_close(fd);
    running_ = false;
    return;
  }
  listen_fd_ = static_cast<decltype(listen_fd_)>(fd);
  NINEYIN_LOG_INFO("update-http listening on %s  webroot=%s", url().c_str(),
                   webroot_base().c_str());

  while (running_) {
    sockaddr_in peer{};
#if defined(_WIN32)
    int plen = sizeof(peer);
#else
    socklen_t plen = sizeof(peer);
#endif
    socket_t c = ::accept(fd, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (c == kInvalid) {
      if (!running_) break;
      continue;
    }
    std::string req;
    char buf[8192];
    for (;;) {
#if defined(_WIN32)
      int n = ::recv(c, buf, sizeof(buf), 0);
#else
      ssize_t n = ::recv(c, buf, sizeof(buf), 0);
#endif
      if (n <= 0) break;
      req.append(buf, static_cast<size_t>(n));
      if (req.find("\r\n\r\n") != std::string::npos) break;
      if (req.size() > 64 * 1024) break;
    }
    std::string method = "GET", path = "/";
    if (!req.empty()) {
      auto sp1 = req.find(' ');
      auto sp2 = req.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
      if (sp1 != std::string::npos && sp2 != std::string::npos) {
        method = req.substr(0, sp1);
        path = req.substr(sp1 + 1, sp2 - sp1 - 1);
        auto q = path.find('?');
        if (q != std::string::npos) path = path.substr(0, q);
      }
    }
    std::string resp = handle(method, path);
    size_t sent = 0;
    while (sent < resp.size()) {
#if defined(_WIN32)
      int n = ::send(c, resp.data() + sent,
                     static_cast<int>(resp.size() - sent), 0);
#else
      ssize_t n = ::send(c, resp.data() + sent, resp.size() - sent, 0);
#endif
      if (n <= 0) break;
      sent += static_cast<size_t>(n);
    }
    sock_close(c);
  }
  if (fd != kInvalid) sock_close(fd);
  listen_fd_ = static_cast<decltype(listen_fd_)>(kInvalid);
}

}  // namespace nineyin::gm
