#include "9yin/gm/http_server.hpp"
#include "9yin/gm/hub.hpp"
#include "9yin/util/log.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

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

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:   o += c; break;
    }
  }
  return o;
}

std::string http_response(int code, const char* status, const std::string& body,
                          const char* content_type = "application/json; charset=utf-8") {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << code << " " << status << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Cache-Control: no-store\r\n"
      << "\r\n"
      << body;
  return oss.str();
}

// ---- Embedded GM panel HTML (self-contained, no external deps) ----
const char* kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>9yin GM Console</title>
<style>
body{margin:0;font-family:Segoe UI,system-ui,sans-serif;background:#0f1419;color:#e7ecf3}
header{padding:14px 20px;border-bottom:1px solid #2d3a4d;display:flex;gap:12px;align-items:center;flex-wrap:wrap}
h1{margin:0;font-size:18px}
.badge{font-size:12px;padding:2px 8px;border-radius:999px;background:#1e3a5f;color:#93c5fd}
main{padding:18px 20px;max-width:1100px;margin:0 auto}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:10px;margin-bottom:14px}
.card{background:#1a2332;border:1px solid #2d3a4d;border-radius:10px;padding:12px 14px;margin-bottom:14px}
.card h2{margin:0 0 8px;font-size:12px;color:#8b9bb4;font-weight:500}
.val{font-size:20px;font-weight:600}
.sub{font-size:12px;color:#8b9bb4;margin-top:4px;word-break:break-all}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{text-align:left;padding:8px;border-bottom:1px solid #2d3a4d}
th{color:#8b9bb4}
button,select,input{background:#243044;color:#e7ecf3;border:1px solid #2d3a4d;border-radius:6px;padding:7px 10px;font-size:13px}
button{cursor:pointer;background:#3b82f6;border-color:transparent}
button.sec{background:#243044;border-color:#2d3a4d}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:8px}
.hint{color:#8b9bb4;font-size:12px;margin-top:8px;line-height:1.5}
#log{font-family:Consolas,monospace;font-size:12px;max-height:150px;overflow:auto;background:#0b1016;border-radius:6px;padding:10px;border:1px solid #2d3a4d;white-space:pre-wrap}
.empty{color:#8b9bb4;padding:16px;text-align:center}
</style>
</head>
<body>
<header>
  <h1 id="title">9yin GM Console</h1>
  <span class="badge" id="phase">Phase-1</span>
  <span class="badge" id="link">connecting</span>
</header>
<main>
  <div class="grid">
    <div class="card"><h2>Status</h2><div class="val" id="stRun">-</div></div>
    <div class="card"><h2>Clients</h2><div class="val" id="stCli">0</div></div>
    <div class="card"><h2>Uptime</h2><div class="val" id="stUp">0s</div></div>
    <div class="card"><h2>Listen</h2><div class="val" style="font-size:14px" id="stLis">-</div><div class="sub" id="stRoot"></div></div>
  </div>
  <div class="card">
    <h2>Online Players</h2>
    <div id="sess"><div class="empty">No players online (login system not ported yet)</div></div>
  </div>
  <div class="card">
    <h2>GM Commands</h2>
    <div class="row">
      <input id="sid" type="number" min="1" placeholder="session id">
      <select id="act">
        <option value="set_silver">set silver</option>
        <option value="set_sp">set SP</option>
        <option value="add_sp">add SP</option>
        <option value="teleport_preset">teleport preset</option>
        <option value="teleport">teleport</option>
        <option value="set_faction">set faction</option>
        <option value="give_item">give item</option>
        <option value="kick">kick</option>
      </select>
      <input id="val" placeholder="value / preset / faction">
      <input id="extra" placeholder="extra">
      <button onclick="doCmd()">Run</button>
      <button class="sec" onclick="refresh()">Refresh</button>
    </div>
    <p class="hint">Phase-1: commands are queued. Faction keys: shaolin wudang emei gaibang tangmen mingjiao ...</p>
  </div>
  <div class="card">
    <h2>Log</h2>
    <div id="log">GM page loaded OK.</div>
  </div>
</main>
<script>
function log(m){var el=document.getElementById('log');var t=new Date().toLocaleTimeString();el.textContent='['+t+'] '+m+'\n'+el.textContent;}
function refresh(){
  fetch('/api/status').then(function(r){return r.json();}).then(function(st){
    document.getElementById('stRun').textContent=st.running?'RUNNING':'STOPPED';
    document.getElementById('stCli').textContent=st.connected_clients;
    document.getElementById('stUp').textContent=st.uptime_sec+'s';
    document.getElementById('stLis').textContent=st.listen_addr||'-';
    document.getElementById('stRoot').textContent=st.server_root||'';
    document.getElementById('phase').textContent=st.phase||'Phase-1';
    document.getElementById('link').textContent='connected';
  }).catch(function(e){document.getElementById('link').textContent='offline';log('status fail: '+e);});
  fetch('/api/sessions').then(function(r){return r.json();}).then(function(list){
    var box=document.getElementById('sess');
    if(!list.length){box.innerHTML='<div class="empty">No players online (login system not ported yet)</div>';return;}
    var h='<table><tr><th>ID</th><th>Name</th><th>Scene</th><th>Pos</th><th>Silver</th><th>SP</th><th>Faction</th><th></th></tr>';
    for(var i=0;i<list.length;i++){
      var s=list[i];
      h+='<tr><td>'+s.sessionId+'</td><td>'+(s.name||'-')+'</td><td>'+(s.scene||'-')+'</td>';
      h+='<td>'+(s.x||0).toFixed(1)+','+(s.y||0).toFixed(1)+','+(s.z||0).toFixed(1)+'</td>';
      h+='<td>'+s.silver+'</td><td>'+s.sp+'</td><td>'+(s.faction||'-')+'</td>';
      h+='<td><button class="sec" onclick="document.getElementById(\'sid\').value='+s.sessionId+'">Select</button></td></tr>';
    }
    h+='</table>';
    box.innerHTML=h;
  }).catch(function(e){log('sessions fail: '+e);});
}
function doCmd(){
  var id=document.getElementById('sid').value;
  var action=document.getElementById('act').value;
  var value=document.getElementById('val').value;
  var extra=document.getElementById('extra').value;
  if(!id){alert('session id required');return;}
  var body={action:action};
  if(action==='set_silver'||action==='set_sp'||action==='add_sp'){body.silver=Number(value)||0;body.sp=Number(value)||0;}
  if(action==='teleport_preset')body.preset=value;
  if(action==='teleport')body.destination=value||extra;
  if(action==='set_faction')body.faction=value;
  if(action==='give_item'){body.itemId=value;body.amount=Number(extra)||1;}
  fetch('/api/sessions/'+id+'/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(function(r){return r.json().then(function(j){return {ok:r.ok,j:j};});})
    .then(function(x){if(!x.ok)throw new Error(x.j.error||'fail');log('OK '+action+' session='+id+' '+(x.j.message||''));refresh();})
    .catch(function(e){log('CMD fail: '+e);alert('fail: '+e);});
}
log('page ready');
refresh();
setInterval(refresh,3000);
</script>
</body>
</html>
)HTML";



std::string status_json() {
  auto st = Hub::instance().status();
  std::ostringstream o;
  o << "{\"running\":" << (st.running ? "true" : "false")
    << ",\"listen_addr\":\"" << json_escape(st.listen_addr) << "\""
    << ",\"server_root\":\"" << json_escape(st.server_root) << "\""
    << ",\"connected_clients\":" << st.connected_clients
    << ",\"uptime_sec\":" << st.uptime_sec
    << ",\"phase\":\"" << json_escape(st.phase) << "\"}";
  return o.str();
}

std::string sessions_json() {
  auto snaps = Hub::instance().snapshots();
  std::ostringstream o;
  o << "[";
  for (size_t i = 0; i < snaps.size(); ++i) {
    const auto& s = snaps[i];
    if (i) o << ",";
    o << "{\"sessionId\":" << s.session_id
      << ",\"remote\":\"" << json_escape(s.remote) << "\""
      << ",\"name\":\"" << json_escape(s.name) << "\""
      << ",\"scene\":\"" << json_escape(s.scene_config) << "\""
      << ",\"resource\":\"" << json_escape(s.scene_resource) << "\""
      << ",\"x\":" << s.x << ",\"y\":" << s.y << ",\"z\":" << s.z << ",\"orient\":" << s.orient
      << ",\"silver\":" << s.silver << ",\"sp\":" << s.sp
      << ",\"faction\":\"" << json_escape(s.faction) << "\""
      << ",\"lastAction\":\"" << json_escape(s.last_action) << "\"}";
  }
  o << "]";
  return o.str();
}

// Minimal JSON field extractors (enough for GM command body)
std::string json_str_field(const std::string& body, const char* key) {
  std::string pat = std::string("\"") + key + "\"";
  auto p = body.find(pat);
  if (p == std::string::npos) return {};
  p = body.find(':', p);
  if (p == std::string::npos) return {};
  p = body.find('"', p);
  if (p == std::string::npos) return {};
  ++p;
  std::string out;
  while (p < body.size() && body[p] != '"') {
    if (body[p] == '\\' && p + 1 < body.size()) { out += body[p + 1]; p += 2; continue; }
    out += body[p++];
  }
  return out;
}

int32_t json_int_field(const std::string& body, const char* key) {
  std::string pat = std::string("\"") + key + "\"";
  auto p = body.find(pat);
  if (p == std::string::npos) return 0;
  p = body.find(':', p);
  if (p == std::string::npos) return 0;
  ++p;
  while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
  return static_cast<int32_t>(std::strtol(body.c_str() + p, nullptr, 10));
}

}  // namespace

HttpServer::HttpServer(std::string bind_host, uint16_t port)
    : host_(std::move(bind_host)), port_(port) {}

HttpServer::~HttpServer() { stop(); }

std::string HttpServer::url() const {
  return "http://" + host_ + ":" + std::to_string(port_) + "/";
}

void HttpServer::start() {
  if (running_) return;
  running_ = true;
  thread_ = std::thread([this] { run_loop(); });
}

void HttpServer::stop() {
  if (!running_) return;
  running_ = false;
  if (listen_fd_ != static_cast<decltype(listen_fd_)>(kInvalid) &&
      listen_fd_ != 0) {
    sock_close(static_cast<socket_t>(listen_fd_));
    listen_fd_ = static_cast<decltype(listen_fd_)>(kInvalid);
  }
  if (thread_.joinable()) thread_.join();
}

std::string HttpServer::handle_request(const std::string& method,
                                       const std::string& path,
                                       const std::string& body) {
  if (method == "GET" && (path == "/" || path == "/index.html")) {
    return http_response(200, "OK", kIndexHtml, "text/html; charset=utf-8");
  }
  if (method == "GET" && path == "/api/status") {
    return http_response(200, "OK", status_json());
  }
  if (method == "GET" && path == "/api/sessions") {
    return http_response(200, "OK", sessions_json());
  }
  // POST /api/sessions/{id}/command
  if (method == "POST" && path.find("/api/sessions/") == 0 &&
      path.size() > 14 && path.find("/command") != std::string::npos) {
    auto id_str = path.substr(14);
    auto slash = id_str.find('/');
    if (slash != std::string::npos) id_str = id_str.substr(0, slash);
    uint64_t id = std::strtoull(id_str.c_str(), nullptr, 10);

    Hub::Command cmd;
    cmd.session_id = id;
    cmd.action = json_str_field(body, "action");
    cmd.silver = json_int_field(body, "silver");
    cmd.sp = json_int_field(body, "sp");
    if (cmd.sp == 0 && cmd.silver != 0) cmd.sp = cmd.silver;
    cmd.preset = json_str_field(body, "preset");
    cmd.destination = json_str_field(body, "destination");
    cmd.faction = json_str_field(body, "faction");
    cmd.item_id = json_str_field(body, "itemId");
    cmd.amount = json_int_field(body, "amount");
    if (cmd.amount <= 0) cmd.amount = 1;

    if (cmd.action.empty()) {
      return http_response(400, "Bad Request",
                           "{\"error\":\"missing action\"}");
    }

    // Phase-1: accept into queue if session exists; otherwise still acknowledge
    // so the UI can be exercised before login is ported.
    bool ok = Hub::instance().enqueue(cmd);
    if (!ok) {
      // Soft-accept for testing when no sessions yet
      NINEYIN_LOG_INFO("GM command (no live session yet): id=%llu action=%s",
                       static_cast<unsigned long long>(id), cmd.action.c_str());
      return http_response(200, "OK",
                           "{\"ok\":true,\"message\":\"queued (session offline or Phase-1 stub)\"}");
    }
    NINEYIN_LOG_INFO("GM command queued: id=%llu action=%s",
                     static_cast<unsigned long long>(id), cmd.action.c_str());
    return http_response(200, "OK", "{\"ok\":true,\"message\":\"queued\"}");
  }

  if (method == "OPTIONS") {
    return http_response(204, "No Content", "");
  }
  return http_response(404, "Not Found", "{\"error\":\"not found\"}");
}

void HttpServer::run_loop() {
  socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd == kInvalid) {
    NINEYIN_LOG_ERROR("GM HTTP: socket failed");
    running_ = false;
    return;
  }
  int yes = 1;
#if defined(_WIN32)
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
    NINEYIN_LOG_ERROR("GM HTTP: invalid bind host %s", host_.c_str());
    sock_close(fd);
    running_ = false;
    return;
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    NINEYIN_LOG_ERROR("GM HTTP: bind %s:%u failed (port in use?)", host_.c_str(), port_);
    sock_close(fd);
    running_ = false;
    return;
  }
  if (::listen(fd, 16) < 0) {
    NINEYIN_LOG_ERROR("GM HTTP: listen failed");
    sock_close(fd);
    running_ = false;
    return;
  }
  listen_fd_ = static_cast<decltype(listen_fd_)>(fd);
  NINEYIN_LOG_INFO("GM panel: %s", url().c_str());

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

    // Read request (simple, single buffer)
    std::string req;
    char buf[4096];
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
    std::string body;
    if (!req.empty()) {
      auto sp1 = req.find(' ');
      auto sp2 = req.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
      if (sp1 != std::string::npos && sp2 != std::string::npos) {
        method = req.substr(0, sp1);
        path = req.substr(sp1 + 1, sp2 - sp1 - 1);
        auto q = path.find('?');
        if (q != std::string::npos) path = path.substr(0, q);
      }
      auto hdr_end = req.find("\r\n\r\n");
      if (hdr_end != std::string::npos) body = req.substr(hdr_end + 4);
      // Content-Length body remainder
      auto cl_pos = req.find("Content-Length:");
      if (cl_pos == std::string::npos) cl_pos = req.find("content-length:");
      if (cl_pos != std::string::npos) {
        int need = std::atoi(req.c_str() + cl_pos + 15);
        while (static_cast<int>(body.size()) < need) {
#if defined(_WIN32)
          int n = ::recv(c, buf, sizeof(buf), 0);
#else
          ssize_t n = ::recv(c, buf, sizeof(buf), 0);
#endif
          if (n <= 0) break;
          body.append(buf, static_cast<size_t>(n));
        }
      }
    }

    std::string resp = handle_request(method, path, body);
    size_t sent_total = 0;
    while (sent_total < resp.size()) {
#if defined(_WIN32)
      int n = ::send(c, resp.data() + sent_total,
                     static_cast<int>(resp.size() - sent_total), 0);
#else
      ssize_t n = ::send(c, resp.data() + sent_total, resp.size() - sent_total, 0);
#endif
      if (n <= 0) break;
      sent_total += static_cast<size_t>(n);
    }
    sock_close(c);
  }

  if (fd != kInvalid) sock_close(fd);
  listen_fd_ = static_cast<decltype(listen_fd_)>(kInvalid);
}

}  // namespace nineyin::gm
