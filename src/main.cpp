// Phase-1 skeleton of the 9yin C++ server (cross-platform: Windows + Linux).
// Currently implements:
//   - Resource path resolution (identical layout to the Go server)
//   - Transport framing (XOR + EE escape + EE EE terminator)
//   - TCP accept loop that reads complete frames and logs them
//
// Next phases will port login state machine, scene entry, AOI, combat, etc.

#include "9yin/config/paths.hpp"
#include "9yin/transport/connection.hpp"
#include "9yin/util/log.hpp"
#include "9yin/gm/hub.hpp"
#include "9yin/db/role_store.hpp"
#include "9yin/game/systems.hpp"
#include "9yin/game/player_runtime.hpp"
#include "9yin/scene/npc_scene.hpp"
#include "9yin/scene/npc_template.hpp"
#include "9yin/gm/http_server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sstream>
#include <mutex>
#include <map>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <cstdint>
#include <memory>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline int net_close(socket_t s) { return ::closesocket(s); }
inline int net_errno() { return ::WSAGetLastError(); }
inline const char* net_errstr(int e) {
  static thread_local char buf[128];
  std::snprintf(buf, sizeof(buf), "WSA error %d", e);
  return buf;
}
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline int net_close(socket_t s) { return ::close(s); }
inline int net_errno() { return errno; }
inline const char* net_errstr(int e) { return std::strerror(e); }
#endif

namespace {

std::atomic<bool> g_running{true};

void open_browser(const std::string& url) {
#if defined(_WIN32)
  // Use ShellExecute via "start" so we don't need extra link libs beyond shell32
  std::string cmd = "cmd /c start \"\" \"" + url + "\"";
  (void)std::system(cmd.c_str());
#else
  // Linux/macOS best-effort
  std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
  if (std::system(cmd.c_str()) != 0) {
    cmd = "open \"" + url + "\" >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
  }
#endif
}



#if defined(_WIN32)
BOOL WINAPI console_ctrl_handler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    g_running = false;
    return TRUE;
  }
  return FALSE;
}
#else
void on_signal(int) { g_running = false; }
#endif

void print_usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [options]\n"
      << "  --listen HOST:PORT   TCP listen address (default 127.0.0.1:19061)\n"
      << "  --root PATH          Override NINEYIN_SERVER_ROOT\n"
      << "  --help               Show this help\n";
}

bool parse_listen(const std::string& s, std::string& host, uint16_t& port) {
  auto pos = s.rfind(':');
  if (pos == std::string::npos) return false;
  host = s.substr(0, pos);
  try {
    int p = std::stoi(s.substr(pos + 1));
    if (p <= 0 || p > 65535) return false;
    port = static_cast<uint16_t>(p);
  } catch (...) {
    return false;
  }
  return true;
}


std::vector<std::pair<std::string, uint16_t>> parse_listen_list(const std::string& s) {
  std::vector<std::pair<std::string, uint16_t>> out;
  size_t start = 0;
  while (start < s.size()) {
    size_t comma = s.find(',', start);
    std::string part = (comma == std::string::npos) ? s.substr(start) : s.substr(start, comma - start);
    // trim spaces
    while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.erase(part.begin());
    while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.pop_back();
    if (!part.empty()) {
      std::string h;
      uint16_t p = 0;
      if (!parse_listen(part, h, p)) {
        return {};
      }
      out.emplace_back(h, p);
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

socket_t bind_listen(const std::string& host, uint16_t port) {
  socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == kInvalidSocket) {
    NINEYIN_LOG_ERROR("socket %s:%u failed: %s", host.c_str(), port, net_errstr(net_errno()));
    return kInvalidSocket;
  }
  int yes = 1;
#if defined(_WIN32)
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    NINEYIN_LOG_ERROR("invalid host: %s", host.c_str());
    net_close(fd);
    return kInvalidSocket;
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    NINEYIN_LOG_ERROR("bind %s:%u failed: %s", host.c_str(), port, net_errstr(net_errno()));
    net_close(fd);
    return kInvalidSocket;
  }
  if (::listen(fd, 128) < 0) {
    NINEYIN_LOG_ERROR("listen %s:%u failed: %s", host.c_str(), port, net_errstr(net_errno()));
    net_close(fd);
    return kInvalidSocket;
  }
  NINEYIN_LOG_INFO("TCP listening on %s:%u", host.c_str(), port);
  return fd;
}


// --- Low-level binary append helpers (must be before enter-scene / create-role) ---
void append_u16(std::vector<uint8_t>& m, uint16_t v) {
  m.push_back(static_cast<uint8_t>(v & 0xFF));
  m.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void append_u32(std::vector<uint8_t>& m, uint32_t v) {
  m.push_back(static_cast<uint8_t>(v & 0xFF));
  m.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  m.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  m.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
void append_ucs2(std::vector<uint8_t>& m, const std::string& utf8) {
  // Encode UTF-8 (BMP) to UTF-16LE + NUL, length is byte count including NUL
  std::vector<uint16_t> units;
  for (size_t i = 0; i < utf8.size();) {
    unsigned char c = static_cast<unsigned char>(utf8[i]);
    uint32_t cp = 0;
    if (c < 0x80) {
      cp = c;
      i += 1;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
      cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3F);
      i += 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
      cp = ((c & 0x0F) << 12) |
           ((static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(utf8[i + 2]) & 0x3F);
      i += 3;
    } else {
      i += 1;
      continue;
    }
    if (cp <= 0xFFFF) units.push_back(static_cast<uint16_t>(cp));
  }
  units.push_back(0);
  append_u32(m, static_cast<uint32_t>(units.size() * 2));
  for (uint16_t u : units) append_u16(m, u);
}
void append_f64(std::vector<uint8_t>& m, double v) {
  uint64_t bits = 0;
  static_assert(sizeof(double) == 8, "need IEEE754 double");
  std::memcpy(&bits, &v, 8);
  for (int i = 0; i < 8; ++i) {
    m.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
  }
}

std::string appearance_get(const std::vector<std::string>& a, size_t i,
                           const std::string& fallback) {
  if (i < a.size() && !a[i].empty()) return a[i];
  return fallback;
}

// ---- Equipment config → model path (fixes head-only model) ----
struct RoleVisual {
  uint8_t sex = 0;
  std::string photo, face, hair, cloth, pants, shoes, action_set;
};

class EquipmentResolver {
 public:
  void load(const std::string& item_dir) {
    art_by_item_.clear();
    models_.clear();
    parse_ini(item_dir + "/equipment.ini", [this](const std::string& sec, const std::string& key,
                                                 const std::string& val) {
      if (key == "ArtPack") art_by_item_[sec] = val;
    });
    parse_ini(item_dir + "/itemartstatic.ini", [this](const std::string& sec, const std::string& key,
                                                      const std::string& val) {
      if (key == "MaleModel") models_[sec].first = val;
      if (key == "FemaleModel") models_[sec].second = val;
    });
    NINEYIN_LOG_INFO("equipment resolver: art=%zu models=%zu dir=%s", art_by_item_.size(),
                     models_.size(), item_dir.c_str());
  }
  std::string resolve(const std::string& config, uint8_t sex) const {
    if (config.empty()) return config;
    if (config.find('\\') != std::string::npos || config.find('/') != std::string::npos)
      return config;  // already a path
    auto ait = art_by_item_.find(config);
    if (ait == art_by_item_.end()) return config;
    auto mit = models_.find(ait->second);
    if (mit == models_.end()) return config;
    if (sex == 1 && !mit->second.second.empty()) return mit->second.second;
    if (!mit->second.first.empty()) return mit->second.first;
    return config;
  }
 private:
  std::map<std::string, std::string> art_by_item_;
  std::map<std::string, std::pair<std::string, std::string>> models_;
  template <typename Fn>
  void parse_ini(const std::string& path, Fn fn) {
    std::ifstream f(path);
    if (!f) return;
    std::string section, line;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      // trim
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
      while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
      if (line.empty() || line[0] == ';' || line[0] == '#') continue;
      if (line.front() == '[' && line.back() == ']') {
        section = line.substr(1, line.size() - 2);
        continue;
      }
      auto eq = line.find('=');
      if (eq == std::string::npos || section.empty()) continue;
      std::string key = line.substr(0, eq);
      std::string val = line.substr(eq + 1);
      while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
      while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
      fn(section, key, val);
    }
  }
};

EquipmentResolver& equipment_resolver() {
  static EquipmentResolver r;
  return r;
}

RoleVisual resolve_role_visual(const std::vector<std::string>& appearance) {
  RoleVisual v;
  // Infer sex from create-role appearance (Go also used to hardcode 0 — we fix it)
  auto photo = appearance_get(appearance, 1, "");
  auto hair = appearance_get(appearance, 4, "");
  auto cloth = appearance_get(appearance, 5, "");
  auto pants = appearance_get(appearance, 6, "");
  auto shoes = appearance_get(appearance, 7, "");
  auto lower_has = [](const std::string& s, const char* sub) {
    auto a = s;
    for (char& c : a)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return a.find(sub) != std::string::npos;
  };
    // Only trust explicit female markers — do NOT treat bare "1" as female
  // (appearance list often contains "1" for faction/flags).
  bool female = lower_has(hair, "g_hair") || lower_has(cloth, "cloth_g") ||
                lower_has(pants, "pants_g") || lower_has(shoes, "shoes_g") ||
                lower_has(photo, "girl") || lower_has(photo, "girl01") ||
                lower_has(hair, "g_hair") ||
                (lower_has(photo, "players") && lower_has(photo, "g_"));
  v.sex = female ? 1 : 0;
  if (female) {
    v.photo = appearance_get(appearance, 1, "icon\\players\\girl01_create.png");
    v.face = appearance_get(appearance, 2, "333333333333333333333333333333333333333333X38\x01");
    v.hair = appearance_get(appearance, 4, "obj\\char\\g_hair\\g_hair1");
    auto& er = equipment_resolver();
    v.cloth = er.resolve(appearance_get(appearance, 5, "cloth_g0001"), v.sex);
    v.pants = er.resolve(appearance_get(appearance, 6, "pants_g0001"), v.sex);
    v.shoes = er.resolve(appearance_get(appearance, 7, "shoes_g0001"), v.sex);
  } else {
    v.photo = appearance_get(appearance, 1, "icon\\players\\boy01_create.png");
    v.face = appearance_get(appearance, 2, "333333333333333333333333333333333333333333X38\x01");
    v.hair = appearance_get(appearance, 4, "obj\\char\\b_hair\\b_hair1");
    auto& er = equipment_resolver();
    v.cloth = er.resolve(appearance_get(appearance, 5, "cloth_b0001"), v.sex);
    v.pants = er.resolve(appearance_get(appearance, 6, "pants_b0001"), v.sex);
    v.shoes = er.resolve(appearance_get(appearance, 7, "shoes_b0001"), v.sex);
  }
  v.action_set = "0h";
  // Fallback if still bare config ids (no ini loaded)
  if (v.cloth.find('\\') == std::string::npos && v.cloth.rfind("cloth_", 0) == 0) {
    v.cloth = female ? "obj\\char\\g_cloth\\g_cloth_001" : "obj\\char\\b_cloth\\b_cloth_001";
  }
  if (v.pants.find('\\') == std::string::npos && v.pants.rfind("pants_", 0) == 0) {
    v.pants = female ? "obj\\char\\g_pants\\g_pants_001" : "obj\\char\\b_pants\\b_pants_001";
  }
  if (v.shoes.find('\\') == std::string::npos && v.shoes.rfind("shoes_", 0) == 0) {
    v.shoes = female ? "obj\\char\\g_shoes\\g_shoes_001" : "obj\\char\\b_shoes\\b_shoes_001";
  }
  return v;
}






// --- Enter-scene (ChooseRole 0x04) minimal port from Go ---

constexpr uint32_t kPlayerObjectID = 1;
constexpr uint32_t kPlayerOwnerID = 1;

void append_f32(std::vector<uint8_t>& m, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  append_u32(m, bits);
}

void append_string_prop(std::vector<uint8_t>& m, uint16_t index, const std::string& value) {
  append_u16(m, index);
  append_u32(m, static_cast<uint32_t>(value.size() + 1));
  m.insert(m.end(), value.begin(), value.end());
  m.push_back(0);
}

void append_widestr_prop(std::vector<uint8_t>& m, uint16_t index, const std::string& utf8) {
  append_u16(m, index);
  // reuse append_ucs2 which writes len+utf16+nul — but it includes length as first
  // For property form: index, byte_len, utf16...
  std::vector<uint8_t> tmp;
  append_ucs2(tmp, utf8);  // starts with u32 length
  m.insert(m.end(), tmp.begin(), tmp.end());
}

// Property table 0x09 — VisibleNPCCompatV1 core fields (name\\0 type)

// Official property indices (from 官服抓包 0x09 table, 2424 entries).
#include "generated_property_table.inc"

static constexpr uint16_t kPropSelectState = 1559;
static constexpr uint16_t kPropCurSkillID = 459;

// SERVER_OBJECT_PROPERTY 0x10 — set player LastObject (official index 413)
std::vector<uint8_t> encode_last_object(uint32_t player_oid, uint32_t player_owner,
                                        uint32_t target_oid, uint32_t target_owner) {
  std::vector<uint8_t> msg(12, 0);
  msg[0] = 0x10;
  msg[1] = 0;  // scene object
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put32(msg, 2, player_oid);
  put32(msg, 6, player_owner);
  msg[10] = 1;  // property count = 1
  msg[11] = 0;
  // Official LastObject index = 413 (was wrongly 100)
  append_u16(msg, kPropLastObject);
  append_u32(msg, target_oid);
  append_u32(msg, target_owner);
  return msg;
}

// WireObject property on player (LastObject / AutoSelectObject / CurSkillTarget)
std::vector<uint8_t> encode_player_object_prop(uint16_t prop_idx,
                                              uint32_t target_oid, uint32_t target_owner) {
  std::vector<uint8_t> msg(12, 0);
  msg[0] = 0x10;
  msg[1] = 0;
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put32(msg, 2, 1);
  put32(msg, 6, 1);
  msg[10] = 1;
  msg[11] = 0;
  append_u16(msg, prop_idx);
  append_u32(msg, target_oid);
  append_u32(msg, target_owner);
  return msg;
}

std::vector<uint8_t> encode_player_byte_prop(uint16_t prop_idx, uint8_t value);

// Frames to fully clear client selection (LastObject + AutoSelect + skill target + menu)
std::vector<std::vector<uint8_t>> make_clear_selection_frames() {
  std::vector<std::vector<uint8_t>> out;
  out.push_back(encode_last_object(1, 1, 0, 0));
  out.push_back(encode_player_object_prop(414, 0, 0));  // AutoSelectObject
  out.push_back(encode_player_object_prop(626, 0, 0));  // CurSkillTarget
  out.push_back(encode_player_byte_prop(kPropSelectState, 0));
  out.push_back(std::vector<uint8_t>(1, 0x1D));        // ClearMenu
  return out;
}

std::vector<uint8_t> make_property_table() {
  // Full official 2424-entry table so client maps LastObject / SelectState correctly.
  std::vector<uint8_t> msg;
  msg.reserve(36000);
  msg.push_back(0x09);
  append_u16(msg, static_cast<uint16_t>(kOfficialPropCount));
  for (size_t i = 0; i < kOfficialPropCount; ++i) {
    const char* name = kOfficialPropNames[i];
    while (*name) msg.push_back(static_cast<uint8_t>(*name++));
    msg.push_back(0);
    msg.push_back(kOfficialPropTypes[i]);
  }
  return msg;
}

// Empty record table 0x0A
// Official/Go record schemas — shortcut_rec is required before client sends 200/201.
std::vector<uint8_t> make_record_table_empty() {
  // 0x0A | u16 count | (name\0 | u16 ncols | types*)*
  struct Schema {
    const char* name;
    std::vector<uint8_t> types;
  };
  // col type 3=int32, 4=int64, 7=string (FxNet2 record columns)
  const Schema schemas[] = {
      {"QingGongRec", {7}},
      {"ActiveQGSkillRec", {7}},
      {"Task_Accepted", {3, 3, 3, 7, 7, 7, 3}},
      {"Task_Completed", {3}},
      {"shortcut_rec", {3, 7, 7}},  // index, kind, config_id
      {"cooldown_rec", {3, 4, 4, 3}},
  };
  std::vector<uint8_t> msg;
  msg.push_back(0x0A);
  append_u16(msg, static_cast<uint16_t>(sizeof(schemas) / sizeof(schemas[0])));
  for (const auto& s : schemas) {
    for (const char* c = s.name; *c; ++c) msg.push_back(static_cast<uint8_t>(*c));
    msg.push_back(0);
    append_u16(msg, static_cast<uint16_t>(s.types.size()));
    for (uint8_t ty : s.types) msg.push_back(ty);
  }
  return msg;
}

// Record indices matching schemas above
constexpr uint16_t kRecQingGong = 0;
constexpr uint16_t kRecActiveQG = 1;
constexpr uint16_t kRecTaskAccepted = 2;
constexpr uint16_t kRecTaskCompleted = 3;
constexpr uint16_t kRecShortcut = 4;
constexpr uint16_t kRecCooldown = 5;

// Official property indices used by this feature set
constexpr uint16_t kPropOperateMode = 223;
constexpr uint16_t kPropShortcutKey = 198;
constexpr uint16_t kPropShortcutKey1 = 199;
constexpr uint16_t kPropShortcutKey2 = 200;
constexpr uint16_t kPropCustomFuncForm = 202;
constexpr uint16_t kPropHaveMainFormStyles = 282;
constexpr uint16_t kPropTimeTick = 1491;

struct PlayerShortcut {
  int32_t index = 0;
  std::string kind;
  std::string id;
};

std::vector<uint8_t> encode_player_int_prop(uint16_t prop_idx, int32_t value) {
  std::vector<uint8_t> msg(12, 0);
  msg[0] = 0x10;
  msg[1] = 0;
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put32(msg, 2, 1);
  put32(msg, 6, 1);
  msg[10] = 1;
  msg[11] = 0;
  append_u16(msg, prop_idx);
  append_u32(msg, static_cast<uint32_t>(value));
  return msg;
}

// Type-1 (byte) property e.g. LogicState
std::vector<uint8_t> encode_player_byte_prop(uint16_t prop_idx, uint8_t value) {
  std::vector<uint8_t> msg(12, 0);
  msg[0] = 0x10;
  msg[1] = 0;
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put32(msg, 2, 1);
  put32(msg, 6, 1);
  msg[10] = 1;
  msg[11] = 0;
  append_u16(msg, prop_idx);
  msg.push_back(value);
  return msg;
}

// Official: LogicState index 462 (byte). Sitcross=102, Normal=0 (Go tests).
constexpr uint16_t kPropLogicState = 462;
constexpr uint8_t kLogicStateNormal = 0;
constexpr uint8_t kLogicStateSitcross = 102;
constexpr uint16_t kPropState = 183;  // string: stand / sitdown / walk

// Official S2C 0x38 time/tick sync (30 bytes). Pattern from 官服抓包 +
// Go playerTimeSyncFrame (opTimeSync = 0x38).
// Layout (LE):
//   [0]=0x38
//   [1..4]  u32 tick_a   (monotonic ms)
//   [5]     0
//   [6..7]  u16 sequence
//   [8..9]  0
//   [10..11] u16 = 4 (constant in captures)
//   [12]    0
//   [13..16] u32 tick_b  (often same family as tick_a)
//   [17..20] 0
//   [21]    1
//   [22..25] u32 tick_b copy
//   [26..29] 0
std::vector<uint8_t> encode_time_sync_0x38(uint32_t tick_ms, uint16_t seq) {
  std::vector<uint8_t> msg(30, 0);
  msg[0] = 0x38;
  msg[1] = static_cast<uint8_t>(tick_ms & 0xFF);
  msg[2] = static_cast<uint8_t>((tick_ms >> 8) & 0xFF);
  msg[3] = static_cast<uint8_t>((tick_ms >> 16) & 0xFF);
  msg[4] = static_cast<uint8_t>((tick_ms >> 24) & 0xFF);
  msg[5] = 0;
  msg[6] = static_cast<uint8_t>(seq & 0xFF);
  msg[7] = static_cast<uint8_t>((seq >> 8) & 0xFF);
  msg[10] = 4;  // constant in official captures
  msg[11] = 0;
  msg[12] = 0;
  // second counter: use unix seconds as stable wall-clock related field
  std::time_t now = std::time(nullptr);
  uint32_t wall = static_cast<uint32_t>(now & 0xFFFFFFFFu);
  msg[13] = static_cast<uint8_t>(wall & 0xFF);
  msg[14] = static_cast<uint8_t>((wall >> 8) & 0xFF);
  msg[15] = static_cast<uint8_t>((wall >> 16) & 0xFF);
  msg[16] = static_cast<uint8_t>((wall >> 24) & 0xFF);
  msg[21] = 1;
  msg[22] = msg[13];
  msg[23] = msg[14];
  msg[24] = msg[15];
  msg[25] = msg[16];
  return msg;
}


std::vector<uint8_t> encode_player_str_prop(uint16_t prop_idx, const std::string& s) {
  std::vector<uint8_t> msg(12, 0);
  msg[0] = 0x10;
  msg[1] = 0;
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put32(msg, 2, 1);
  put32(msg, 6, 1);
  msg[10] = 1;
  msg[11] = 0;
  append_string_prop(msg, prop_idx, s);
  return msg;
}

// SERVER_RECORD_ADD 0x11 full row for shortcut_rec
std::vector<uint8_t> encode_shortcut_add(const PlayerShortcut& sc) {
  std::vector<uint8_t> msg(16, 0);
  msg[0] = 0x11;
  msg[1] = 0;
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put32(msg, 2, 1);
  put32(msg, 6, 1);
  msg[10] = static_cast<uint8_t>(kRecShortcut & 0xFF);
  msg[11] = static_cast<uint8_t>((kRecShortcut >> 8) & 0xFF);
  msg[12] = 0;  // first row
  msg[13] = 0;
  msg[14] = 1;  // one row
  msg[15] = 0;
  append_u32(msg, static_cast<uint32_t>(sc.index));
  // string columns: u32 len incl NUL? Go uses length-prefixed NUL-terminated
  // Match Go recordString: typically u32 length + bytes + 0
  auto append_rec_str = [](std::vector<uint8_t>& m, const std::string& s) {
    append_u32(m, static_cast<uint32_t>(s.size() + 1));
    for (char c : s) m.push_back(static_cast<uint8_t>(c));
    m.push_back(0);
  };
  append_rec_str(msg, sc.kind);
  append_rec_str(msg, sc.id);
  return msg;
}

std::vector<uint8_t> encode_record_del_row(uint16_t rec_idx, uint16_t row) {
  std::vector<uint8_t> msg(14, 0);
  msg[0] = 0x12;
  msg[1] = 0;
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put32(msg, 2, 1);
  put32(msg, 6, 1);
  msg[10] = static_cast<uint8_t>(rec_idx & 0xFF);
  msg[11] = static_cast<uint8_t>((rec_idx >> 8) & 0xFF);
  msg[12] = static_cast<uint8_t>(row & 0xFF);
  msg[13] = static_cast<uint8_t>((row >> 8) & 0xFF);
  return msg;
}

std::vector<uint8_t> encode_qinggong_add(const std::string& id) {
  std::vector<uint8_t> msg(16, 0);
  msg[0] = 0x11;
  msg[1] = 0;
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put32(msg, 2, 1);
  put32(msg, 6, 1);
  msg[10] = static_cast<uint8_t>(kRecQingGong & 0xFF);
  msg[11] = 0;
  msg[12] = 0;
  msg[13] = 0;
  msg[14] = 1;
  msg[15] = 0;
  append_u32(msg, static_cast<uint32_t>(id.size() + 1));
  for (char c : id) msg.push_back(static_cast<uint8_t>(c));
  msg.push_back(0);
  return msg;
}

// Local time as minutes since midnight for UI clock (best-effort).
int32_t server_clock_minutes() {
  std::time_t now = std::time(nullptr);
  std::tm* t = std::localtime(&now);
  if (!t) return 0;
  return t->tm_hour * 60 + t->tm_min;
}

// Parse 0x0A activity custom: header 21 bytes then u16 count + typed values
struct CustomVal {
  uint8_t type = 0;
  int32_t i32 = 0;
  int64_t i64 = 0;
  float f32 = 0;
  std::string str;
};
struct CustomMsg {
  bool ok = false;
  std::vector<CustomVal> vals;
};

CustomMsg parse_activity_custom(const std::vector<uint8_t>& plain) {
  CustomMsg out;
  const size_t hdr = 21;
  if (plain.size() < hdr + 3 || plain[0] != 0x0A) return out;
  size_t off = hdr;
  uint16_t count = static_cast<uint16_t>(plain[off] | (plain[off + 1] << 8));
  off += 2;
  if (count == 0 || count > 64) return out;
  for (uint16_t i = 0; i < count; ++i) {
    if (off >= plain.size()) return CustomMsg{};
    CustomVal v;
    v.type = plain[off++];
    if (v.type == 2) {
      if (off + 4 > plain.size()) return CustomMsg{};
      std::memcpy(&v.i32, plain.data() + off, 4);
      off += 4;
    } else if (v.type == 3) {
      if (off + 8 > plain.size()) return CustomMsg{};
      std::memcpy(&v.i64, plain.data() + off, 8);
      off += 8;
    } else if (v.type == 4) {
      if (off + 4 > plain.size()) return CustomMsg{};
      std::memcpy(&v.f32, plain.data() + off, 4);
      off += 4;
    } else if (v.type == 6 || v.type == 7) {
      if (off + 4 > plain.size()) return CustomMsg{};
      uint32_t n = 0;
      std::memcpy(&n, plain.data() + off, 4);
      off += 4;
      if (n > 4096 || off + n > plain.size()) return CustomMsg{};
      // narrow string: n includes trailing 0 often
      size_t len = n;
      if (len > 0 && plain[off + len - 1] == 0) len--;
      v.str.assign(reinterpret_cast<const char*>(plain.data() + off), len);
      off += n;
    } else if (v.type == 8) {
      // object 8 bytes
      if (off + 8 > plain.size()) return CustomMsg{};
      off += 8;
    } else {
      return CustomMsg{};
    }
    out.vals.push_back(std::move(v));
  }
  out.ok = !out.vals.empty();
  return out;
}

// EntryScene 0x0B
std::vector<uint8_t> make_entry_scene(const std::string& config, const std::string& resource) {
  std::vector<uint8_t> entry(15, 0);
  entry[0] = 0x0B;
  // write object/owner/version/count into fixed header
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  auto put16 = [](std::vector<uint8_t>& m, size_t off, uint16_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  };
  put32(entry, 1, kPlayerObjectID);
  put32(entry, 5, kPlayerOwnerID);
  put32(entry, 9, 1);
  put16(entry, 13, 3);
  // Official indices: ConfigID=1439 Resource=1440 MaxVisuals=1460
  append_string_prop(entry, 1439, config);
  append_string_prop(entry, 1440, resource);
  append_u16(entry, 1460);
  append_u32(entry, 1000000);
  return entry;
}

// Simplified player AddObject 0x0D (core visual + vitals)
std::vector<uint8_t> make_player_add_object(
    const std::string& name, const std::vector<std::string>& appearance,
    float x, float y, float z, float orient) {
  std::vector<uint8_t> msg(0x3f, 0);
  msg[0] = 0x0D;
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put32(msg, 1, kPlayerObjectID);
  put32(msg, 5, kPlayerOwnerID);
  uint32_t xb = 0, yb = 0, zb = 0, ob = 0;
  std::memcpy(&xb, &x, 4);
  std::memcpy(&yb, &y, 4);
  std::memcpy(&zb, &z, 4);
  std::memcpy(&ob, &orient, 4);
  put32(msg, 0x09, xb);
  put32(msg, 0x0D, yb);
  put32(msg, 0x11, zb);
  put32(msg, 0x15, ob);

  uint16_t count = 0;
  auto add_byte = [&](uint16_t idx, uint8_t v) {
    append_u16(msg, idx);
    msg.push_back(v);
    count++;
  };
  auto add_int = [&](uint16_t idx, int32_t v) {
    append_u16(msg, idx);
    append_u32(msg, static_cast<uint32_t>(v));
    count++;
  };
  auto add_float = [&](uint16_t idx, float v) {
    append_u16(msg, idx);
    append_f32(msg, v);
    count++;
  };
  auto add_str = [&](uint16_t idx, const std::string& s) {
    append_string_prop(msg, idx, s);
    count++;
  };

  const RoleVisual visual = resolve_role_visual(appearance);

  // Indices match official 2424-entry property table (from capture).
  add_byte(181, 2);              // Type = player
  add_byte(182, visual.sex);     // Sex
  add_str(184, visual.photo);    // Photo
  add_str(183, "stand");         // State
  add_str(188, visual.hair);     // Hair
  add_str(185, visual.face);     // Face
  add_str(392, visual.cloth);    // Cloth
  add_str(393, visual.pants);    // Pants
  add_str(394, visual.shoes);    // Shoes
  add_str(405, visual.action_set.empty() ? "player" : visual.action_set);  // ActionSet
  add_str(390, visual.hair);     // Hat (fallback)
  add_float(383, 5.0f);          // MoveSpeed
  add_float(384, 2.5f);          // WalkSpeed
  add_float(385, 7.0f);          // RunSpeed
  add_float(680, 8.0f);          // JumpSpeed
  add_int(389, 100);             // SpeedRatio
  add_int(468, 100);             // HPRatio
  add_int(469, 100);             // MPRatio
  add_int(577, 1000);            // MaxHP
  add_int(578, 500);             // MaxMP
  add_int(464, 1000);            // HP
  append_widestr_prop(msg, 178, name);  // Name
  count++;
  add_byte(1464, 1);             // Level
  // Clear LastObject on spawn (official index 413)
  append_u16(msg, kPropLastObject);
  append_u32(msg, 0);
  append_u32(msg, 0);
  count++;

  msg[0x3d] = static_cast<uint8_t>(count & 0xFF);
  msg[0x3e] = static_cast<uint8_t>((count >> 8) & 0xFF);
  return msg;
}

std::vector<uint8_t> make_server_location(float x, float y, float z, float orient) {
  std::vector<uint8_t> msg(25, 0);
  msg[0] = 0x1F;
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put32(msg, 1, kPlayerObjectID);
  put32(msg, 5, kPlayerOwnerID);
  uint32_t xb = 0, yb = 0, zb = 0, ob = 0;
  std::memcpy(&xb, &x, 4);
  std::memcpy(&yb, &y, 4);
  std::memcpy(&zb, &z, 4);
  std::memcpy(&ob, &orient, 4);
  put32(msg, 9, xb);
  put32(msg, 13, yb);
  put32(msg, 17, zb);
  put32(msg, 21, ob);
  return msg;
}


void handle_client(socket_t fd, const std::string& peer, uint16_t listen_port);

void accept_loop(socket_t listen_fd, uint16_t port) {
  NINEYIN_LOG_INFO("accept_loop started for port %u", port);
  while (g_running) {
    sockaddr_in peer_addr{};
#if defined(_WIN32)
    int peer_len = sizeof(peer_addr);
#else
    socklen_t peer_len = sizeof(peer_addr);
#endif
    socket_t client = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
    if (client == kInvalidSocket) {
      if (!g_running) break;
#if !defined(_WIN32)
      if (errno == EINTR) continue;
#endif
      continue;
    }
    char peer_str[64];
    ::inet_ntop(AF_INET, &peer_addr.sin_addr, peer_str, sizeof(peer_str));
    std::string peer = std::string(peer_str) + ":" + std::to_string(ntohs(peer_addr.sin_port))
                       + " (via :" + std::to_string(port) + ")";
    // Log BEFORE spawning thread so we never miss a connect
    NINEYIN_LOG_INFO("*** ACCEPT on :%u from %s ***", port, peer.c_str());
    std::thread(handle_client, client, peer, port).detach();
  }
  net_close(listen_fd);
  NINEYIN_LOG_INFO("accept_loop ended for port %u", port);
}



// ServerWorldInfo (opcode 0x05): open books for create-role "剧本" selection.
// Matches Go worldInfo(0, "0,book1,0,book2,0,book4,0,book5,0,book8,0,0")
std::vector<uint8_t> make_world_info(uint16_t info_type, const std::string& text_utf8) {
  // Encode text as UTF-16LE with trailing NUL
  std::vector<uint16_t> units;
  // Simple ASCII/UTF-8 path: our book list is pure ASCII
  for (unsigned char c : text_utf8) {
    units.push_back(static_cast<uint16_t>(c));
  }
  units.push_back(0);  // NUL terminator

  std::vector<uint8_t> msg(3 + units.size() * 2, 0);
  msg[0] = 0x05;
  msg[1] = static_cast<uint8_t>(info_type & 0xFF);
  msg[2] = static_cast<uint8_t>((info_type >> 8) & 0xFF);
  for (size_t i = 0; i < units.size(); ++i) {
    msg[3 + i * 2] = static_cast<uint8_t>(units[i] & 0xFF);
    msg[3 + i * 2 + 1] = static_cast<uint8_t>((units[i] >> 8) & 0xFF);
  }
  return msg;
}

// Minimal ServerPlayerRoles (opcode 0x04) with zero roles — matches Go noRoleLoginSuccess.
std::vector<uint8_t> make_no_role_login_success() {
  std::vector<uint8_t> msg(0x29, 0);
  msg[0] = 0x04;
  auto put_u32 = [&](size_t off, uint32_t v) {
    msg[off] = static_cast<uint8_t>(v & 0xFF);
    msg[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    msg[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    msg[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put_u32(0x01, 1);
  // wall-clock fields (year/month/day/hour/min/sec) — client accepts zeros/current
  std::time_t now = std::time(nullptr);
  std::tm* t = std::localtime(&now);
  if (t) {
    put_u32(0x09, static_cast<uint32_t>(t->tm_year + 1900));
    put_u32(0x0D, static_cast<uint32_t>(t->tm_mon + 1));
    put_u32(0x11, static_cast<uint32_t>(t->tm_mday));
    put_u32(0x15, static_cast<uint32_t>(t->tm_hour));
    put_u32(0x19, static_cast<uint32_t>(t->tm_min));
    put_u32(0x1D, static_cast<uint32_t>(t->tm_sec));
  }
  // role count at 0x25 stays 0
  return msg;
}


// --- Create-role helpers (ported from Go messages.go) ---

std::string utf16le_to_utf8(const uint8_t* data, size_t max_bytes) {
  std::string out;
  for (size_t i = 0; i + 1 < max_bytes; i += 2) {
    uint16_t u = static_cast<uint16_t>(data[i] | (data[i + 1] << 8));
    if (u == 0) break;
    if (u < 0x80) {
      out.push_back(static_cast<char>(u));
    } else if (u < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (u >> 6)));
      out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (u >> 12)));
      out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    }
  }
  return out;
}

std::string create_role_name(const std::vector<uint8_t>& msg) {
  if (msg.size() < 7) return "";
  size_t maxb = msg.size() - 5;
  if (maxb > 72) maxb = 72;
  return utf16le_to_utf8(msg.data() + 5, maxb);
}

std::vector<std::string> create_role_strings(const std::vector<uint8_t>& msg) {
  std::vector<std::string> values;
  size_t offset = 0x49;
  while (offset + 5 <= msg.size()) {
    if (msg[offset] != 6) {
      offset++;
      continue;
    }
    uint32_t size = static_cast<uint32_t>(msg[offset + 1]) |
                    (static_cast<uint32_t>(msg[offset + 2]) << 8) |
                    (static_cast<uint32_t>(msg[offset + 3]) << 16) |
                    (static_cast<uint32_t>(msg[offset + 4]) << 24);
    if (size < 1 || size > 4096 || offset + 5 + size > msg.size() ||
        msg[offset + 5 + size - 1] != 0) {
      offset++;
      continue;
    }
    values.emplace_back(reinterpret_cast<const char*>(msg.data() + offset + 5),
                        size - 1);
    offset += 5 + size;
  }
  return values;
}


// ---------- Account key (compatible with Go auth.ParseLoginAccountIdentity) ----------
namespace {
uint32_t rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
void sha256_transform(uint32_t state[8], const uint8_t data[64]) {
  static const uint32_t K[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  uint32_t m[64];
  for (int i = 0; i < 16; ++i)
    m[i] = (static_cast<uint32_t>(data[i * 4]) << 24) | (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(data[i * 4 + 2]) << 8) | static_cast<uint32_t>(data[i * 4 + 3]);
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = rotr32(m[i - 15], 7) ^ rotr32(m[i - 15], 18) ^ (m[i - 15] >> 3);
    uint32_t s1 = rotr32(m[i - 2], 17) ^ rotr32(m[i - 2], 19) ^ (m[i - 2] >> 10);
    m[i] = m[i - 16] + s0 + m[i - 7] + s1;
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + S1 + ch + K[i] + m[i];
    uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}
void sha256(const uint8_t* msg, size_t len, uint8_t out[32]) {
  uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                       0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  uint8_t block[64];
  size_t offset = 0;
  while (len - offset >= 64) {
    sha256_transform(state, msg + offset);
    offset += 64;
  }
  size_t rem = len - offset;
  std::memcpy(block, msg + offset, rem);
  block[rem++] = 0x80;
  if (rem > 56) {
    while (rem < 64) block[rem++] = 0;
    sha256_transform(state, block);
    rem = 0;
  }
  while (rem < 56) block[rem++] = 0;
  uint64_t bitlen = static_cast<uint64_t>(len) * 8;
  for (int i = 0; i < 8; ++i) block[63 - i] = static_cast<uint8_t>((bitlen >> (8 * i)) & 0xFF);
  sha256_transform(state, block);
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<uint8_t>((state[i] >> 24) & 0xFF);
    out[i * 4 + 1] = static_cast<uint8_t>((state[i] >> 16) & 0xFF);
    out[i * 4 + 2] = static_cast<uint8_t>((state[i] >> 8) & 0xFF);
    out[i * 4 + 3] = static_cast<uint8_t>(state[i] & 0xFF);
  }
}
std::string to_hex(const uint8_t* d, size_t n) {
  static const char* H = "0123456789abcdef";
  std::string s(n * 2, '0');
  for (size_t i = 0; i < n; ++i) {
    s[i * 2] = H[d[i] >> 4];
    s[i * 2 + 1] = H[d[i] & 15];
  }
  return s;
}
}  // namespace

std::string parse_login_account_key(const std::vector<uint8_t>& msg) {
  if (msg.size() < 25 || msg[0] != 0x02) return "acct:v1:unknown";
  uint32_t alen = static_cast<uint32_t>(msg[0x15]) | (static_cast<uint32_t>(msg[0x16]) << 8) |
                  (static_cast<uint32_t>(msg[0x17]) << 16) | (static_cast<uint32_t>(msg[0x18]) << 24);
  if (alen < 16 || alen > 256 || (alen % 16) != 0 || 25 + alen > msg.size()) return "acct:v1:unknown";
  std::vector<uint8_t> buf;
  const char* domain = "nineyin/login-account/v1";
  buf.insert(buf.end(), domain, domain + 24);
  buf.push_back(0);
  buf.push_back(static_cast<uint8_t>(alen & 0xFF));
  buf.push_back(static_cast<uint8_t>((alen >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>((alen >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((alen >> 24) & 0xFF));
  buf.insert(buf.end(), msg.begin() + 25, msg.begin() + 25 + static_cast<std::ptrdiff_t>(alen));
  uint8_t hash[32];
  sha256(buf.data(), buf.size(), hash);
  return "acct:v1:" + to_hex(hash, 32);
}

std::string book_display_name(const std::string& book) {
  if (book == "book1") return "千灯镇起";
  if (book == "book2") return "天山洗剑录";
  if (book == "book3") return "千灯镇起";
  if (book == "book4") return "成都风云";
  if (book == "book5") return "鸡鸣驿起";
  if (book == "book6") return "洛阳风云";
  if (book == "book8") return "恶人谷起";
  return book;
}
std::string scene_display_name(const std::string& book) {
  if (book == "book1" || book == "book3") return "千灯镇";
  if (book == "book2") return "烟雨庄";
  if (book == "book4") return "成都";
  if (book == "book5") return "鸡鸣驿";
  if (book == "book6") return "洛阳";
  if (book == "book8") return "恶人谷";
  return "千灯镇";
}
std::string scene_name_key(const std::string& book) {
  if (book == "book1" || book == "book3") return "scene_name_001";
  if (book == "book2") return "scene_name_003";
  if (book == "book4") return "scene_name_017";
  if (book == "book5") return "scene_name_004";
  if (book == "book6") return "scene_name_016";
  if (book == "book8") return "scene_name_002";
  return "scene_name_001";
}

// Spawn from client books.ini RelivePos (sceneId,x,y,z,orient,...)
// SceneId → resource mapping from sencerelive.ini section headers.
struct SpawnPoint {
  const char* config;
  const char* resource;
  float x, y, z, orient;
  const char* label;
};

SpawnPoint spawn_for_book(const std::string& book) {
  // book1 / book3: sceneId=1 → 千灯镇 born04
  static const SpawnPoint kBook1 = {
      "ini\\scene\\born04_QianDengZhen", "born04",
      874.933f, -32.392f, 746.688f, 0.335f, "千灯镇(book1)"};
  // book2: sceneId=3 → 烟雨庄 born03
  static const SpawnPoint kBook2 = {
      "ini\\scene\\born03_YanYuZhuang", "born03",
      460.629f, 12.811f, 659.731f, 5.580f, "烟雨庄(book2)"};
  // book4: sceneId=17 → 成都 city05
  static const SpawnPoint kBook4 = {
      "ini\\scene\\city05_ChengDu", "city05",
      915.753f, 31.192f, 538.193f, 6.170f, "成都(book4)"};
  // book5: sceneId=4 → 鸡鸣驿 born01
  static const SpawnPoint kBook5 = {
      "ini\\scene\\born01_MoHeZhen", "born01",
      693.908f, 24.694f, 404.350f, 3.611f, "鸡鸣驿(book5)"};
  // book6: sceneId=16 → 洛阳 city04
  static const SpawnPoint kBook6 = {
      "ini\\scene\\city04_LuoYang", "city04",
      1141.058f, -15.236f, 649.011f, 4.500f, "洛阳(book6)"};
  // book8: sceneId=2 → 恶人谷 born02
  static const SpawnPoint kBook8 = {
      "ini\\scene\\born02_ERenGU", "born02",
      905.069f, 10.810f, 196.980f, 1.610f, "恶人谷(book8)"};

  if (book == "book1" || book == "book3") return kBook1;
  if (book == "book2") return kBook2;
  if (book == "book4") return kBook4;
  if (book == "book5") return kBook5;
  if (book == "book6") return kBook6;
  if (book == "book8") return kBook8;
  return kBook1;  // default = book1 千灯镇
}



// oneRoleLoginSuccess — Go messages.go (simplified equipment defaults)
std::vector<uint8_t> make_one_role_login_success(
    const std::string& name, const std::vector<std::string>& appearance) {
  auto msg = make_no_role_login_success();
  // role count = 1 at 0x25
  msg[0x25] = 1;
  msg[0x26] = 0;
  msg[0x27] = 0;
  msg[0x28] = 0;

  const RoleVisual visual = resolve_role_visual(appearance);
  const auto& hair = visual.hair;
  const auto& cloth = visual.cloth;
  const auto& pants = visual.pants;
  const auto& shoes = visual.shoes;
  const auto& face = visual.face;
  auto book = appearance_get(appearance, 0, "book1");
  auto faction = appearance_get(appearance, 3, "0");

  // 36 comma-separated role para fields (same indices as Go)
  std::string fields[36];
  for (auto& f : fields) f = "";
  fields[0] = std::to_string(visual.sex);  // 0 male / 1 female
  fields[1] = faction.empty() ? "0" : faction;
  fields[2] = "0";
  fields[3] = "0";
  fields[4] = "1";
  fields[5] = face;
  fields[6] = "1";
  fields[7] = hair;
  fields[9] = cloth;
  fields[10] = pants;
  fields[11] = shoes;
  fields[21] = "stand";
  fields[22] = "127.0.0.1";
  fields[23] = "title001";
  // 选角界面直接显示中文（客户端未做 scene_name_xxx 本地化时也能看懂）
  fields[24] = scene_display_name(book);
  fields[25] = book_display_name(book);
  fields[31] = "0";
  fields[32] = book_display_name(book);
  fields[33] = hair;
  fields[35] = "0";
  std::string role_para;
  for (int i = 0; i < 36; ++i) {
    if (i) role_para += ",";
    role_para += fields[i];
  }

  append_u16(msg, 6);
  msg.push_back(2);
  append_u32(msg, 0);
  msg.push_back(2);
  append_u32(msg, 0);
  msg.push_back(7);
  append_ucs2(msg, name);
  msg.push_back(7);
  append_ucs2(msg, role_para);
  msg.push_back(2);
  append_u32(msg, 0);
  msg.push_back(5);
  append_f64(msg, 0.0);
  return msg;
}


bool send_all(socket_t fd, const uint8_t* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
#if defined(_WIN32)
    int n = ::send(fd, reinterpret_cast<const char*>(data + sent),
                   static_cast<int>(len - sent), 0);
#else
    ssize_t n = ::send(fd, data + sent, len - sent, 0);
#endif
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool send_frame(socket_t fd, nineyin::transport::Connection& conn,
                const std::vector<uint8_t>& plain) {
  auto wire = conn.encode_outgoing(plain);
  return send_all(fd, wire.data(), wire.size());
}


// Shared per-scene NPC world: one authoritative simulation for all players.
// Without this, each connection stepped patrol independently and two clients
// would see the same PathId walker at different positions.
struct SharedPendingLeg {
  std::chrono::steady_clock::time_point at;       // arrive time
  std::chrono::steady_clock::time_point started;  // leg id for per-client dedup
  float x = 0, y = 0, z = 0, o = 0;
  float from_x = 0, from_y = 0, from_z = 0, from_o = 0;
};
struct SharedSceneWorld {
  std::mutex mu;
  nineyin::scene::SceneNpcSet set;
  bool loaded = false;
  std::chrono::steady_clock::time_point last_tick{};
  // object_id -> active walk leg (shared; all clients see same dest)
  std::unordered_map<uint32_t, SharedPendingLeg> legs;
};
std::mutex g_scenes_mu;
std::unordered_map<std::string, std::shared_ptr<SharedSceneWorld>> g_scenes;

std::string scene_cache_key(const std::string& scene_resource) {
  std::string res = scene_resource;
  auto slash = res.find_last_of("/\\");
  if (slash != std::string::npos) res = res.substr(slash + 1);
  for (char& c : res)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return res;
}

std::shared_ptr<SharedSceneWorld> get_or_load_shared_scene(const std::string& scene_resource) {
  const std::string key = scene_cache_key(scene_resource);
  {
    std::lock_guard<std::mutex> lk(g_scenes_mu);
    auto it = g_scenes.find(key);
    if (it != g_scenes.end() && it->second && it->second->loaded) return it->second;
  }
  auto world = std::make_shared<SharedSceneWorld>();
  nineyin::config::ResourcePaths paths = nineyin::config::make_resource_paths(
      nineyin::config::locate_server_root());
  world->set.scene_resource = scene_resource;
  std::string creator_root = paths.npc_creator_dir.string();
  auto cdir = nineyin::scene::creator_dir_for_resource(scene_resource, creator_root);
  nineyin::scene::load_npc_templates(paths.npc_table_dir.string());
  int n = nineyin::scene::load_permanent_from_creator(cdir, world->set);
  // Named patrol routes: creator/mappath/<scene>_npc.ini / _mst.ini
  // Layout mirrors official: resources/modern/share/creator/mappath
  std::string mappath_dir = (paths.modern_share / "creator" / "mappath").string();
  if (const char* e = std::getenv("NINEYIN_MAPPATH_DIR"); e && e[0]) mappath_dir = e;
  nineyin::scene::load_mappath_table(mappath_dir, scene_resource, world->set.path_table);
  // Optional nav mesh main.path (not used for patrol binding)
  std::string map_root = (paths.modern_share / "map").string();
  if (const char* e = std::getenv("NINEYIN_MAP_ROOT"); e && e[0]) map_root = e;
  nineyin::scene::load_scene_path(map_root, scene_resource, world->set.path_all);
  if (world->set.npcs.empty()) {
    NINEYIN_LOG_ERROR(
        "NO NPCs for scene=%s — put creator XML under: %s\\<scene_folder>\\*.xml",
        scene_resource.c_str(), creator_root.c_str());
  }
  // Drop activity / event / escort / clone configs before patrol bind
  {
    auto is_act = [](const std::string& id) {
      auto low = id;
      for (char& c : low)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (low.find("hd_") != std::string::npos) return true;
      if (low.find("event") != std::string::npos) return true;
      if (low.find("escort_") != std::string::npos) return true;
      if (low.find("clone") != std::string::npos) return true;
      if (low.find("funnpc") != std::string::npos) return true;
      if (low.find("sweetemploy") != std::string::npos) return true;
      if (low.find("weibing") != std::string::npos) return true;
      if (low.find("boss_") == 0 && low.find("kill") != std::string::npos) return true;
      if (low.find("japan") != std::string::npos) return true;
      if (low.find("qy_") == 0) return true;
      if (low.find("offline") != std::string::npos) return true;
      if (low.find("2024") != std::string::npos || low.find("2025") != std::string::npos ||
          low.find("2026") != std::string::npos)
        return true;
      return false;
    };
    size_t before = world->set.npcs.size();
    world->set.npcs.erase(
        std::remove_if(world->set.npcs.begin(), world->set.npcs.end(),
                       [&](const nineyin::scene::NpcInstance& n) { return is_act(n.config_id); }),
        world->set.npcs.end());
    NINEYIN_LOG_INFO("activity/event NPCs stripped: %zu -> %zu", before, world->set.npcs.size());
  }
  nineyin::scene::assign_patrol_routes(world->set);
  world->loaded = true;
  world->last_tick = {};
  NINEYIN_LOG_INFO("shared scene loaded key=%s permanent=%zu path_named=%zu", key.c_str(),
                   world->set.npcs.size(), world->set.path_table.size());
  {
    std::lock_guard<std::mutex> lk(g_scenes_mu);
    auto it = g_scenes.find(key);
    if (it != g_scenes.end() && it->second && it->second->loaded) return it->second;
    g_scenes[key] = world;
  }
  return world;
}

// Advance shared patrol: full mappath waypoint legs, one simulation for all clients.
// Position stays at leg start until arrive, then confirm_path_arrive snaps server pose.
void tick_shared_scene(SharedSceneWorld& world, int max_new_legs = 512) {
  auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lk(world.mu);
  for (auto it = world.legs.begin(); it != world.legs.end();) {
    if (now < it->second.at) {
      ++it;
      continue;
    }
    // Arrive: snap authoritative pose to dest waypoint
    for (auto& n : world.set.npcs) {
      if (n.object_id == it->first) {
        nineyin::scene::confirm_path_arrive(n);
        break;
      }
    }
    it = world.legs.erase(it);
  }
  if (world.last_tick.time_since_epoch().count() != 0 &&
      (now - world.last_tick) < std::chrono::milliseconds(800)) {
    return;
  }
  world.last_tick = now;
  int moved = 0;
  for (auto& n : world.set.npcs) {
    if (n.route.size() < 2) continue;
    if (world.legs.count(n.object_id)) continue;
    float fx = n.x, fy = n.y, fz = n.z, fo = n.orient;
    if (!nineyin::scene::step_path_patrol(n)) continue;
    // After step: route_idx points at dest waypoint; n still at fx,fy,fz
    const auto& w = n.route[static_cast<size_t>(n.route_idx)];
    int leg_ms = static_cast<int>(n.patrol_sign);
    if (leg_ms < 600) leg_ms = 600;
    SharedPendingLeg leg;
    leg.started = now;
    leg.at = now + std::chrono::milliseconds(leg_ms);
    leg.x = w.x;
    leg.y = w.y;
    leg.z = w.z;
    leg.o = (w.orient != 0.f) ? w.orient : n.orient;
    leg.from_x = fx;
    leg.from_y = fy;
    leg.from_z = fz;
    leg.from_o = fo;
    world.legs[n.object_id] = leg;
    if (++moved >= max_new_legs) break;
  }
}

void send_scene_npcs(socket_t fd, nineyin::transport::Connection& conn, const std::string& peer,
                     const std::string& scene_resource,
                     float px, float py, float pz,
                     nineyin::scene::SceneNpcSet* out_set = nullptr,
                     std::unordered_set<uint32_t>* out_sent = nullptr,
                     std::shared_ptr<SharedSceneWorld>* out_world = nullptr) {
  auto world = get_or_load_shared_scene(scene_resource);
  if (out_world) *out_world = world;
  tick_shared_scene(*world);
  // Snapshot under lock so patrol cannot race with encoding
  nineyin::scene::SceneNpcSet set;
  {
    std::lock_guard<std::mutex> lk(world->mu);
    set = world->set;
  }

  // Resolve templates; exclude EventNpc (Go aoi.go) and obvious activity IDs
  auto is_activity_id = [](const std::string& id) {
    auto low = id;
    for (char& c : low)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (low.find("hd_") != std::string::npos) return true;       // 活动
    if (low.find("event") != std::string::npos) return true;
    if (low.find("escort_") != std::string::npos) return true;  // 护送
    if (low.find("clone") != std::string::npos) return true;
    if (low.find("funnpc") != std::string::npos) return true;
    if (low.find("sweetemploy") != std::string::npos) return true;
    if (low.find("weibing") != std::string::npos) return true;  // 卫兵/击杀剧情
    if (low.find("boss_") == 0 && low.find("kill") != std::string::npos) return true;
    if (low.find("japan") != std::string::npos) return true;
    if (low.find("qy_") == 0) return true;                      // 奇遇
    if (low.find("offline") != std::string::npos) return true;
    if (low.find("2024") != std::string::npos || low.find("2025") != std::string::npos ||
        low.find("2026") != std::string::npos)
      return true;
    return false;
  };
  auto is_event_script = [](const std::string& sc) {
    auto low = sc;
    for (char& c : low)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return low == "eventnpc";
  };
  auto resident_score = [&](const nineyin::scene::NpcInstance& n) {
    // lower = better (sort ascending with distance)
    auto ti = nineyin::scene::lookup_npc_template(n.config_id);
    int score = 0;
    auto sc = ti.script_class;
    auto res = ti.resource;
    auto id = n.config_id;
    for (char& c : sc)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (char& c : res)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (char& c : id)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    // Prefer dialogue/function NPCs (green-name style resources)
    if (res.find("worldnpc") != std::string::npos) score -= 150;
    if (res.find("fbnpc") != std::string::npos) score -= 80;
    if (id.find("leader") != std::string::npos || id.find("world") != std::string::npos)
      score -= 100;
    if (sc == "commonnpc" || sc.find("common") != std::string::npos) score -= 120;
    // Deprioritize pure combat monsters (red name)
    if (res.find("monster") != std::string::npos) score += 80;
    if (sc == "attacknpc") score += 40;
    if (is_activity_id(n.config_id)) score += 200;
    return score;
  };
  {
    std::vector<nineyin::scene::NpcInstance> hits;
    hits.reserve(set.npcs.size());
    int miss = 0, skip_event = 0, skip_activity = 0;
    for (auto& npc : set.npcs) {
      auto ti = nineyin::scene::lookup_npc_template(npc.config_id);
      if (!ti.found) {
        miss++;
        continue;
      }
      if (is_event_script(ti.script_class)) {
        skip_event++;
        continue;
      }
      // keep activity for now but deprioritize; hard-skip extreme activity ids
      if (is_activity_id(npc.config_id)) {
        skip_activity++;
        continue;  // do not spawn 活动/剧情临时怪 as ambient
      }
      hits.push_back(std::move(npc));
    }
    NINEYIN_LOG_INFO(
        "%s: NPC resolve HIT=%zu MISS=%d skip_EventNpc=%d skip_activity=%d",
        peer.c_str(), hits.size(), miss, skip_event, skip_activity);
    set.npcs = std::move(hits);
  }

  // Pure distance first (resident_score only used for logging preference)
  std::sort(set.npcs.begin(), set.npcs.end(), [&](const nineyin::scene::NpcInstance& a,
                                                   const nineyin::scene::NpcInstance& b) {
    float da = (a.x - px) * (a.x - px) + (a.z - pz) * (a.z - pz);
    float db = (b.x - px) * (b.x - px) + (b.z - pz) * (b.z - pz);
    // Within similar distance, prefer green/worldnpc
    if (std::fabs(da - db) < 100.f) {
      return resident_score(a) < resident_score(b);
    }
    return da < db;
  });

  // Density diagnostic around player
  {
    int d50 = 0, d100 = 0, d200 = 0, d450 = 0, world_near = 0;
    for (auto& n : set.npcs) {
      float d2 = (n.x - px) * (n.x - px) + (n.z - pz) * (n.z - pz);
      if (d2 <= 50.f * 50.f) d50++;
      if (d2 <= 100.f * 100.f) d100++;
      if (d2 <= 200.f * 200.f) d200++;
      if (d2 <= 450.f * 450.f) d450++;
      auto ti = nineyin::scene::lookup_npc_template(n.config_id);
      auto r = ti.resource;
      for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (d2 <= 200.f * 200.f && r.find("worldnpc") != std::string::npos) world_near++;
    }
    NINEYIN_LOG_INFO(
        "%s: density around player r50=%d r100=%d r200=%d r450=%d worldnpc_in_200=%d",
        peer.c_str(), d50, d100, d200, d450, world_near);
  }

  // Client visual AOI is small; only send within radius at REAL positions
  const float kRadius = 450.f;
  const float kR2 = kRadius * kRadius;
  int sent = 0, near_count = 0;
  for (auto& npc : set.npcs) {
    float d2 = (npc.x - px) * (npc.x - px) + (npc.z - pz) * (npc.z - pz);
    if (d2 > kR2) continue;
    near_count++;
    auto pkt = nineyin::scene::encode_npc_add_object(npc);
    if (!send_frame(fd, conn, pkt)) break;
    auto loc = nineyin::scene::encode_npc_location(npc);
    if (!send_frame(fd, conn, loc)) break;
    if (sent < 3) {
      auto ti = nineyin::scene::lookup_npc_template(npc.config_id);
      NINEYIN_LOG_INFO(
          "%s: near NPC[%d] id=%s res=%s dist=%.1f pos=(%.1f,%.1f,%.1f)",
          peer.c_str(), sent, npc.config_id.c_str(), ti.resource.c_str(),
          std::sqrt(d2), npc.x, npc.y, npc.z);
    }
    if (out_sent) out_sent->insert(npc.object_id);
    sent++;
    if (sent >= 200) break;
  }
  NINEYIN_LOG_INFO("%s: scene %s NPCs sent=%d in_radius_%d=%d loaded_hit=%zu player=(%.1f,%.1f,%.1f)",
                   peer.c_str(), scene_resource.c_str(), sent, (int)kRadius, near_count,
                   set.npcs.size(), px, py, pz);
  if (out_set) *out_set = std::move(set);
  // out_sent filled during loop below - track in loop
  (void)out_sent;
}

void handle_client(socket_t fd, const std::string& peer, uint16_t listen_port) {
  NINEYIN_LOG_INFO("%s: connected", peer.c_str());
  nineyin::gm::Hub::instance().client_connected();

  std::vector<uint8_t> buf(64 * 1024);

  // ---- Area port (4000): plain-text "svrlist" protocol, NOT FxNet frames ----
  // Client:  svrlist\r\n
  // Server:  svrlist #Name $IP PORT STATUS\r\n
  // STATUS: 0=good (from historical client docs)
  if (listen_port == 4000) {
#if defined(_WIN32)
    int n = ::recv(fd, reinterpret_cast<char*>(buf.data()),
                   static_cast<int>(buf.size()), 0);
#else
    ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
#endif
    if (n <= 0) {
      NINEYIN_LOG_INFO("%s: area client closed without data", peer.c_str());
      net_close(fd);
      nineyin::gm::Hub::instance().client_disconnected();
      return;
    }
    std::string req(reinterpret_cast<char*>(buf.data()), static_cast<size_t>(n));
    while (!req.empty() && (req.back() == '\r' || req.back() == '\n' || req.back() == '\0'))
      req.pop_back();
    NINEYIN_LOG_INFO("%s: area request: %s", peer.c_str(), req.c_str());

    if (req.find("svrlist") != std::string::npos) {
      // PROVEN working format (client showed this in list and could click enter):
      //   svrlist #LocalTest $127.0.0.1 19061 0\r\n
      // Chinese name in #NAME breaks some client builds; keep ASCII only.
      const char kResp[] = "svrlist #LocalTest $127.0.0.1 19061 0\r\n";
      if (!send_all(fd, reinterpret_cast<const uint8_t*>(kResp), sizeof(kResp) - 1)) {
        NINEYIN_LOG_ERROR("%s: failed to send svrlist", peer.c_str());
      } else {
        NINEYIN_LOG_INFO("%s: sent svrlist #LocalTest $127.0.0.1 19061 0", peer.c_str());
      }
    } else {
      NINEYIN_LOG_WARN("%s: unknown area command, ignoring", peer.c_str());
    }
    net_close(fd);
    nineyin::gm::Hub::instance().client_disconnected();
    NINEYIN_LOG_INFO("%s: disconnected (area)", peer.c_str());
    return;
  }

  // ---- Game port (19061): FxNet2 framed protocol ----
  nineyin::transport::Connection conn;
  bool logged_raw = false;
  std::string session_role_name;
  std::vector<std::string> session_appearance;
  std::string session_account_key;
  std::string session_spawn_config;
  std::string session_spawn_resource;
  float session_x = 0, session_y = 0, session_z = 0, session_orient = 0;
  bool has_role = false;
  bool session_npcs_sent = false;
  std::unordered_set<uint32_t> session_npc_sent_ids;
  nineyin::scene::SceneNpcSet session_npc_set;  // snapshot for config lookups
  std::shared_ptr<SharedSceneWorld> session_world;  // authoritative positions + routes
  float session_npc_last_x = 0, session_npc_last_z = 0;
  auto session_last_patrol = std::chrono::steady_clock::time_point{};
  // oid -> (arrive_at, x,y,z,orient) — Go: 0x20 now, 0x1F after ~1.5s (never same tick)
  struct PendingArrive {
    std::chrono::steady_clock::time_point at;
    float x, y, z, o;
  };
  std::unordered_map<uint32_t, PendingArrive> pending_npc_arrive;
  // object_id -> leg.started already broadcast to this client
  std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> session_leg_sent;
  std::chrono::steady_clock::time_point last_pos_save{};
  // Current target selection (LastObject). Cleared on ESC / oid=0 / out-of-range.
  uint32_t session_selected_oid = 0;
  uint32_t session_selected_owner = 0;
  float session_selected_x = 0, session_selected_z = 0;
  // Official client auto-clears target beyond ~25–30m; use 28m.
  constexpr float kSelectClearRadius = 28.f;
  // Settings / UI / shortcuts (v1)
  int32_t session_operate_mode = 0;
  uint8_t session_logic_state = 0;  // LogicState
  nineyin::game::PlayerRuntime session_pr;
  std::string session_shortcut_key, session_shortcut_key1, session_shortcut_key2;
  std::vector<PlayerShortcut> session_shortcuts;
  auto session_last_clock = std::chrono::steady_clock::time_point{};
  auto session_boot_steady = std::chrono::steady_clock::now();
  uint16_t session_time_seq = 0;
  std::string session_customizing_b64;  // persisted UI customizing blob
  bool session_ui_bootstrapped = false;

  auto load_settings_from_appearance = [&]() {
    session_shortcuts.clear();
    for (const auto& a : session_appearance) {
      if (a.rfind("__om:", 0) == 0) {
        session_operate_mode = static_cast<int32_t>(std::atoi(a.c_str() + 5));
      } else if (a.rfind("__sk0:", 0) == 0) {
        session_shortcut_key = a.substr(6);
      } else if (a.rfind("__sk1:", 0) == 0) {
        session_shortcut_key1 = a.substr(6);
      } else if (a.rfind("__sk2:", 0) == 0) {
        session_shortcut_key2 = a.substr(6);
      } else if (a.rfind("__cz:", 0) == 0) {
        session_customizing_b64 = a.substr(5);
      } else if (a.rfind("__sc:", 0) == 0) {
        // __sc:index:kind:id
        std::string rest = a.substr(5);
        size_t p1 = rest.find(':');
        if (p1 == std::string::npos) continue;
        size_t p2 = rest.find(':', p1 + 1);
        if (p2 == std::string::npos) continue;
        PlayerShortcut sc;
        sc.index = static_cast<int32_t>(std::atoi(rest.substr(0, p1).c_str()));
        sc.kind = rest.substr(p1 + 1, p2 - p1 - 1);
        sc.id = rest.substr(p2 + 1);
        if (!sc.id.empty()) session_shortcuts.push_back(sc);
      }
    }
  };
  auto save_settings_to_store = [&]() {
    if (session_account_key.empty()) return;
    // Strip old tags
    std::vector<std::string> app;
    for (const auto& a : session_appearance) {
      if (a.rfind("__om:", 0) == 0 || a.rfind("__sk", 0) == 0 || a.rfind("__sc:", 0) == 0 || a.rfind("__cz:", 0) == 0)
        continue;
      app.push_back(a);
    }
    app.push_back("__om:" + std::to_string(session_operate_mode));
    if (!session_shortcut_key.empty()) app.push_back("__sk0:" + session_shortcut_key);
    if (!session_shortcut_key1.empty()) app.push_back("__sk1:" + session_shortcut_key1);
    if (!session_shortcut_key2.empty()) app.push_back("__sk2:" + session_shortcut_key2);
    for (const auto& sc : session_shortcuts) {
      app.push_back("__sc:" + std::to_string(sc.index) + ":" + sc.kind + ":" + sc.id);
    }
    if (!session_customizing_b64.empty())
      app.push_back("__cz:" + session_customizing_b64);
    session_appearance = app;
    load_settings_from_appearance();
    nineyin::db::StoredRole role;
    role.name = session_role_name;
    role.appearance = session_appearance;
    role.scene_config = session_spawn_config;
    role.scene_resource = session_spawn_resource;
    role.x = session_x;
    role.y = session_y;
    role.z = session_z;
    role.orient = session_orient;
    nineyin::db::roles().put(session_account_key, role);
  };
  auto send_ui_bootstrap = [&]() {
    // Operate mode + main form flags + clock
    send_frame(fd, conn, encode_player_int_prop(kPropOperateMode, session_operate_mode));
    // Unlock common main-form style bits (client-specific; high value = show more)
    send_frame(fd, conn, encode_player_int_prop(kPropHaveMainFormStyles, -1));
    send_frame(fd, conn, encode_player_int_prop(kPropTimeTick, server_clock_minutes()));
    {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - session_boot_steady)
                         .count();
      send_frame(fd, conn,
                 encode_time_sync_0x38(static_cast<uint32_t>(elapsed & 0xFFFFFFFFu),
                                      session_time_seq++));
    }
    send_frame(fd, conn, encode_player_byte_prop(kPropLogicState, kLogicStateNormal));
    // Re-push OperateMode so client mode UI sees server value
    send_frame(fd, conn, encode_player_int_prop(kPropOperateMode, session_operate_mode));
    if (!session_shortcut_key.empty())
      send_frame(fd, conn, encode_player_str_prop(kPropShortcutKey, session_shortcut_key));
    if (!session_shortcut_key1.empty())
      send_frame(fd, conn, encode_player_str_prop(kPropShortcutKey1, session_shortcut_key1));
    if (!session_shortcut_key2.empty())
      send_frame(fd, conn, encode_player_str_prop(kPropShortcutKey2, session_shortcut_key2));
    // Seed a few QingGong categories so "我的战斗/轻功" related UI has data
    static const char* kQG[] = {"qinggong_1", "qinggong_2", "qinggong_3", "qinggong_4",
                                "qinggong_5", "qinggong_6", "qinggong_gaojie"};
    for (const char* id : kQG) send_frame(fd, conn, encode_qinggong_add(id));
    // Default sitcross shortcut if empty
    if (session_shortcuts.empty()) {
      session_shortcuts.push_back(PlayerShortcut{9, "skill", "zs_default_01"});
    }
    for (const auto& sc : session_shortcuts)
      send_frame(fd, conn, encode_shortcut_add(sc));
    session_ui_bootstrapped = true;
    NINEYIN_LOG_INFO(
        "%s: UI bootstrap mode=%d shortcuts=%zu clock_min=%d", peer.c_str(),
        session_operate_mode, session_shortcuts.size(), server_clock_minutes());
  };

  while (g_running) {
#if defined(_WIN32)
    int n = ::recv(fd, reinterpret_cast<char*>(buf.data()),
                   static_cast<int>(buf.size()), 0);
#else
    ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
#endif
    if (n <= 0) {
      if (n < 0) {
        NINEYIN_LOG_WARN("%s: recv error: %s", peer.c_str(), net_errstr(net_errno()));
      } else {
        NINEYIN_LOG_INFO("%s: client closed connection (no data or done)", peer.c_str());
      }
      break;
    }

    if (!logged_raw) {
      logged_raw = true;
      std::string hex;
      size_t show = static_cast<size_t>(n) < 64 ? static_cast<size_t>(n) : 64;
      char tmp[8];
      for (size_t i = 0; i < show; ++i) {
        std::snprintf(tmp, sizeof(tmp), "%02X ", buf[i]);
        hex += tmp;
      }
      NINEYIN_LOG_INFO("%s: first %zu bytes: %s%s", peer.c_str(), show, hex.c_str(),
                       static_cast<size_t>(n) > show ? "..." : "");
    }

    try {
      bool ready = conn.feed({buf.data(), static_cast<size_t>(n)});
      while (ready) {
        auto plain = conn.take_frame();
        uint8_t opcode = plain.empty() ? 0 : plain[0];
        NINEYIN_LOG_INFO("%s: recv frame size=%zu opcode=0x%02X",
                         peer.c_str(), plain.size(), opcode);

        if (opcode == 0x02) {
          // ClientLogin → load persisted role if any
          std::string account_key = parse_login_account_key(plain);
          session_account_key = account_key;
          nineyin::db::StoredRole stored;
          bool exists = nineyin::db::roles().get(account_key, stored);
          std::vector<uint8_t> payload;
          if (exists) {
            session_role_name = stored.name;
            session_appearance = stored.appearance;
            has_role = true;
            session_spawn_config = stored.scene_config;
            session_spawn_resource = stored.scene_resource;
            session_x = stored.x;
            session_y = stored.y;
            session_z = stored.z;
            session_orient = stored.orient;
            payload = make_one_role_login_success(stored.name, stored.appearance);
            NINEYIN_LOG_INFO("%s: login account=%s role=%s (loaded)", peer.c_str(),
                             account_key.c_str(), stored.name.c_str());
          } else {
            has_role = false;
            payload = make_no_role_login_success();
            NINEYIN_LOG_INFO("%s: login account=%s no role", peer.c_str(),
                             account_key.c_str());
          }
          if (!send_frame(fd, conn, payload)) {
            NINEYIN_LOG_ERROR("%s: failed to send login success", peer.c_str());
            ready = false;
            break;
          }
          NINEYIN_LOG_INFO("%s: sent ServerPlayerRoles (0x04, roles=%d)", peer.c_str(), exists ? 1 : 0);
        } else if (opcode == 0x03) {
          // ClientMsgWorldInfo → open books so 剧本 can be selected
          // Same list as Go protocol-probe
          const char* books = "0,book1,0,book2,0,book4,0,book5,0,book8,0,0";
          auto payload = make_world_info(0, books);
          if (!send_frame(fd, conn, payload)) {
            NINEYIN_LOG_ERROR("%s: failed to send ServerWorldInfo", peer.c_str());
            ready = false;
            break;
          }
          NINEYIN_LOG_INFO("%s: sent ServerWorldInfo (0x05) open books=%s", peer.c_str(), books);
        } else if (opcode == 0x05) {
          // ClientCreateRole → persist in-memory + reply ServerPlayerRoles with 1 role
          std::string name = create_role_name(plain);
          if (name.empty()) name = "LocalHero";
          auto appearance = create_role_strings(plain);
          {
            const auto prev = resolve_role_visual(appearance);
            NINEYIN_LOG_INFO("%s: ClientCreateRole name=%s fields=%zu sex=%u hair=%s cloth=%s",
                             peer.c_str(), name.c_str(), appearance.size(),
                             static_cast<unsigned>(prev.sex),
                             appearance_get(appearance, 4, "").c_str(),
                             appearance_get(appearance, 5, "").c_str());
          }
          auto payload = make_one_role_login_success(name, appearance);
          if (!send_frame(fd, conn, payload)) {
            NINEYIN_LOG_ERROR("%s: failed to send create-role success", peer.c_str());
            ready = false;
            break;
          }
          session_role_name = name;
          session_appearance = appearance;
          load_settings_from_appearance();
          has_role = true;
          {
            const std::string book = appearance_get(appearance, 0, "book1");
            const SpawnPoint sp = spawn_for_book(book);
            session_spawn_config = sp.config;
            session_spawn_resource = sp.resource;
            session_x = sp.x; session_y = sp.y; session_z = sp.z; session_orient = sp.orient;
            nineyin::db::StoredRole sr;
            sr.name = name;
            sr.appearance = appearance;
            sr.scene_config = sp.config;
            sr.scene_resource = sp.resource;
            sr.x = sp.x; sr.y = sp.y; sr.z = sp.z; sr.orient = sp.orient;
            if (session_account_key.empty() || session_account_key == "acct:v1:unknown") {
              NINEYIN_LOG_ERROR("%s: cannot save role — account key missing (re-login required)",
                                peer.c_str());
            } else {
              bool saved = nineyin::db::roles().put(session_account_key, sr);
              NINEYIN_LOG_INFO("%s: role save %s account=%s name=%s book=%s backend=%s",
                               peer.c_str(), saved ? "OK" : "FAIL",
                               session_account_key.c_str(), name.c_str(), book.c_str(),
                               nineyin::db::roles().backend_name().c_str());
            }
          }
          NINEYIN_LOG_INFO("%s: sent ServerPlayerRoles (0x04, 1 role, name=%s)",
                           peer.c_str(), name.c_str());
        } else if (opcode == 0x04) {
          // ClientChooseRole → property/record tables + EntryScene + player spawn
          if (!has_role) {
            NINEYIN_LOG_WARN("%s: ChooseRole without created role", peer.c_str());
          }
          const std::string book = appearance_get(session_appearance, 0, "book1");
          SpawnPoint sp = spawn_for_book(book);
          std::string cfg = sp.config;
          std::string res = sp.resource;
          float px = sp.x, py = sp.y, pz = sp.z, po = sp.orient;
          if (!session_spawn_config.empty()) {
            cfg = session_spawn_config;
            res = session_spawn_resource;
            px = session_x; py = session_y; pz = session_z; po = session_orient;
          }
          auto prop = make_property_table();
          auto rec = make_record_table_empty();
          auto entry = make_entry_scene(cfg, res);
          auto addobj = make_player_add_object(
              session_role_name.empty() ? "LocalHero" : session_role_name,
              session_appearance, px, py, pz, po);
          auto loc = make_server_location(px, py, pz, po);
          bool ok = send_frame(fd, conn, prop) && send_frame(fd, conn, rec) &&
                    send_frame(fd, conn, entry) && send_frame(fd, conn, addobj) &&
                    send_frame(fd, conn, loc);
          if (!ok) {
            NINEYIN_LOG_ERROR("%s: failed to send choose-role enter-scene chain", peer.c_str());
            ready = false;
            break;
          }
          NINEYIN_LOG_INFO(
              "%s: ChooseRole book=%s scene=%s/%s pos=(%.2f,%.2f,%.2f)",
              peer.c_str(), book.c_str(), cfg.c_str(), res.c_str(), px, py, pz);
          // Defer NPCs until first ClientActivity/ClientReady (Go does the same)
          session_npcs_sent = false;
        } else if (opcode == 0x09) {
          NINEYIN_LOG_INFO("%s: ClientReady (0x09) — scene barrier reached", peer.c_str());
          if (!session_spawn_resource.empty()) {
            // Must send AFTER ClientReady; earlier packets are dropped by client
            session_npc_sent_ids.clear();
            session_world.reset();
            send_scene_npcs(fd, conn, peer, session_spawn_resource,
                            session_x, session_y, session_z,
                            &session_npc_set, &session_npc_sent_ids, &session_world);
            session_npc_last_x = session_x;
            session_npc_last_z = session_z;
            session_npcs_sent = true;
            // Clear residual menu + selection so ESC system panel works
            {
              std::vector<uint8_t> clr_menu(1, 0x1D);
              send_frame(fd, conn, clr_menu);
            }
            for (auto& fr : make_clear_selection_frames()) send_frame(fd, conn, fr);
            session_selected_oid = 0;
            session_selected_owner = 0;
            session_last_patrol = {};  // force first patrol soon
            if (!session_ui_bootstrapped) {
              send_ui_bootstrap();
              session_pr.operate_mode = session_operate_mode;
              session_pr.logic_state = session_logic_state;
              session_pr.shortcuts.clear();
              for (const auto& sc : session_shortcuts) {
                nineyin::game::ShortcutSlot s;
                s.index = sc.index; s.kind = sc.kind; s.id = sc.id;
                session_pr.shortcuts.push_back(s);
              }
              auto boot = nineyin::game::bootstrap_player(session_pr);
              for (auto& fr : boot.frames) send_frame(fd, conn, fr);
            }
          }
        } else if (opcode == 0x0A) {
          // Custom activity messages (shortcuts / operate mode / etc.)
          {
            // Protocol discovery: log large 0x0A (customizing / UI blobs)
            if (plain.size() >= 100) {
              std::string hx;
              size_t n = std::min<size_t>(plain.size(), 64);
              char buf[8];
              for (size_t i = 0; i < n; ++i) {
                std::snprintf(buf, sizeof(buf), "%02X ", plain[i]);
                hx += buf;
              }
              NINEYIN_LOG_INFO("%s: 0x0A large len=%zu head=%s", peer.c_str(), plain.size(),
                               hx.c_str());
            }
            auto cm = parse_activity_custom(plain);
            if (cm.ok) {
              nineyin::game::CustomMsg gcm;
              gcm.ok = cm.ok;
              for (const auto& v : cm.vals) {
                nineyin::game::CustomVal gv;
                gv.type = v.type; gv.i32 = v.i32; gv.i64 = v.i64; gv.f32 = v.f32; gv.str = v.str;
                gcm.vals.push_back(std::move(gv));
              }
              nineyin::game::OutFrames gout;
              bool game_handled = nineyin::game::dispatch_custom(session_pr, gcm, gout, peer);
              if (game_handled) {
                for (auto& fr : gout.frames) send_frame(fd, conn, fr);
                session_logic_state = session_pr.logic_state;
                session_operate_mode = session_pr.operate_mode;
                if (!gcm.vals.empty() && gcm.vals[0].type == 2) {
                  int32_t mid = gcm.vals[0].i32;
                  if (mid == 200 || mid == 201) {
                    session_shortcuts.clear();
                    for (const auto& sc : session_pr.shortcuts) {
                      session_shortcuts.push_back(
                          PlayerShortcut{sc.index, sc.kind, sc.id});
                    }
                    save_settings_to_store();
                    NINEYIN_LOG_INFO("%s: shortcuts persisted count=%zu", peer.c_str(),
                                     session_shortcuts.size());
                  }
                  // Skill 211: native msg20 (Go/official) + target HP for float layer
                  if (mid == 211) {
                    std::string sid = session_pr.current_skill_id;
                    uint32_t tid = session_selected_oid ? session_selected_oid : 1u;
                    uint32_t tow = session_selected_oid ? session_selected_owner : 1u;
                    send_frame(fd, conn,
                      nineyin::game::make_native_skill_action(1, 1, sid, tid, tow));
                    if (session_selected_oid != 0) {
                      std::vector<uint8_t> hp = encode_player_int_prop(464, 950);
                      hp[2] = static_cast<uint8_t>(session_selected_oid & 0xFF);
                      hp[3] = static_cast<uint8_t>((session_selected_oid >> 8) & 0xFF);
                      hp[4] = static_cast<uint8_t>((session_selected_oid >> 16) & 0xFF);
                      hp[5] = static_cast<uint8_t>((session_selected_oid >> 24) & 0xFF);
                      hp[6] = static_cast<uint8_t>(session_selected_owner & 0xFF);
                      hp[7] = static_cast<uint8_t>((session_selected_owner >> 8) & 0xFF);
                      hp[8] = static_cast<uint8_t>((session_selected_owner >> 16) & 0xFF);
                      hp[9] = static_cast<uint8_t>((session_selected_owner >> 24) & 0xFF);
                      send_frame(fd, conn, hp);
                    }
                    NINEYIN_LOG_INFO("%s: skill anim msg20 id=%s target=%u", peer.c_str(),
                                     sid.c_str(), tid);
                  }
                }
              }
            }
            // Skip legacy mid handlers for messages already processed by game layer
            if (cm.ok && !cm.vals.empty() && cm.vals[0].type == 2) {
              const int32_t mid = cm.vals[0].i32;
              if (mid == 200 || mid == 201 || mid == 211 || mid == 240) {
                // already handled by game dispatch above
              } else if (mid != 15 && mid != 298 && mid != 924 && mid != 9) {
                std::string args;
                for (size_t vi = 1; vi < cm.vals.size() && vi < 8; ++vi) {
                  const auto& v = cm.vals[vi];
                  args += " |";
                  if (v.type == 2) args += "i32=" + std::to_string(v.i32);
                  else if (v.type == 3) args += "i64=" + std::to_string(v.i64);
                  else if (v.type == 4) args += "f=" + std::to_string(v.f32);
                  else if (v.type == 6 || v.type == 7)
                    args += "s=" + v.str;
                  else
                    args += "t=" + std::to_string(v.type);
                }
                NINEYIN_LOG_INFO("%s: custom0x0A msg=%d argc=%zu%s", peer.c_str(), mid,
                                 cm.vals.size(), args.c_str());
              }
              // --- sitcross: disabled (handled by game dispatch) ---
              if (false && mid == 240 && cm.vals.size() >= 2 && cm.vals[1].type == 2) {
                const int32_t cmd = cm.vals[1].i32;
                uint8_t next = session_logic_state;
                if (cmd == 1)
                  next = kLogicStateSitcross;
                else if (cmd == 0 || cmd == 2)
                  next = kLogicStateNormal;
                if (next != session_logic_state) {
                  session_logic_state = next;
                  send_frame(fd, conn,
                             encode_player_byte_prop(kPropLogicState, session_logic_state));
                  // Also push State string for animation (sitdown / stand)
                  send_frame(fd, conn,
                             encode_player_str_prop(
                                 kPropState,
                                 session_logic_state == kLogicStateSitcross ? "sitdown"
                                                                           : "stand"));
                  NINEYIN_LOG_INFO("%s: sitcross LogicState -> %u (cmd=%d)", peer.c_str(),
                                   static_cast<unsigned>(session_logic_state), cmd);
                } else {
                  NINEYIN_LOG_INFO("%s: sitcross duplicate state=%u ignored", peer.c_str(),
                                   static_cast<unsigned>(session_logic_state));
                }
              }
              // --- operate mode: accept several observed shapes ---
              // A) dedicated: msg X with int 0/1
              // B) any msg whose sole payload is 0/1 when user is in settings
              // C) msg with string "OperateMode" (rare)
              else if (mid == 223 || mid == 5223) {
                // speculative ids
                if (cm.vals.size() >= 2 && cm.vals[1].type == 2) {
                  session_operate_mode = cm.vals[1].i32;
                  send_frame(fd, conn,
                             encode_player_int_prop(kPropOperateMode, session_operate_mode));
                  save_settings_to_store();
                  NINEYIN_LOG_INFO("%s: OperateMode set=%d via msg %d", peer.c_str(),
                                   session_operate_mode, mid);
                }
              } else if (cm.vals.size() == 2 && cm.vals[1].type == 2 &&
                         (cm.vals[1].i32 == 0 || cm.vals[1].i32 == 1) &&
                         (mid == 250 || mid == 251 || mid == 252 || mid == 300 ||
                          mid == 301 || mid == 400 || mid == 401)) {
                session_operate_mode = cm.vals[1].i32;
                send_frame(fd, conn,
                           encode_player_int_prop(kPropOperateMode, session_operate_mode));
                save_settings_to_store();
                NINEYIN_LOG_INFO("%s: OperateMode heuristic set=%d via msg %d", peer.c_str(),
                                 session_operate_mode, mid);
              } else if (cm.vals.size() >= 2 &&
                         (cm.vals[1].str.find("Operate") != std::string::npos ||
                          cm.vals[1].str.find("operate") != std::string::npos ||
                          cm.vals[1].str.find("joystick") != std::string::npos)) {
                int32_t mode = 0;
                if (cm.vals.size() >= 3 && cm.vals[2].type == 2) mode = cm.vals[2].i32;
                else if (cm.vals[1].str.find("JOYSTICK") != std::string::npos ||
                         cm.vals[1].str.find("joystick") != std::string::npos)
                  mode = 1;
                session_operate_mode = mode;
                send_frame(fd, conn,
                           encode_player_int_prop(kPropOperateMode, session_operate_mode));
                save_settings_to_store();
                NINEYIN_LOG_INFO("%s: OperateMode via string set=%d", peer.c_str(),
                                 session_operate_mode);
              }
              // 200 = set shortcut, 201 = remove shortcut (Go)
              else if (false && mid == 200 && cm.vals.size() >= 4)  /* disabled: handled by game dispatch */ {
                int32_t idx = cm.vals[1].type == 2 ? cm.vals[1].i32 : 0;
                std::string kind = cm.vals[2].str;
                std::string id = cm.vals[3].str;
                if (kind.empty() && cm.vals[2].type == 2)
                  kind = "skill";
                // replace existing index
                int old_row = -1;
                for (size_t i = 0; i < session_shortcuts.size(); ++i) {
                  if (session_shortcuts[i].index == idx) {
                    old_row = static_cast<int>(i);
                    break;
                  }
                }
                if (old_row >= 0) {
                  send_frame(fd, conn,
                             encode_record_del_row(kRecShortcut, static_cast<uint16_t>(old_row)));
                  session_shortcuts.erase(session_shortcuts.begin() + old_row);
                }
                PlayerShortcut sc{idx, kind.empty() ? "skill" : kind, id};
                session_shortcuts.push_back(sc);
                send_frame(fd, conn, encode_shortcut_add(sc));
                save_settings_to_store();
                NINEYIN_LOG_INFO("%s: set shortcut idx=%d kind=%s id=%s", peer.c_str(),
                                 idx, sc.kind.c_str(), sc.id.c_str());
              } else if (false && mid == 201 && cm.vals.size() >= 2)  /* disabled: game dispatch */ {
                int32_t row = cm.vals[1].i32;
                if (row >= 0 && row < static_cast<int32_t>(session_shortcuts.size())) {
                  send_frame(fd, conn,
                             encode_record_del_row(kRecShortcut, static_cast<uint16_t>(row)));
                  session_shortcuts.erase(session_shortcuts.begin() + row);
                  save_settings_to_store();
                  NINEYIN_LOG_INFO("%s: remove shortcut row=%d", peer.c_str(), row);
                }
              
              }
              }
            // Periodic time sync: official 0x38 (~2s) + TimeTick property
            auto nowc = std::chrono::steady_clock::now();
            if (session_ui_bootstrapped &&
                (session_last_clock.time_since_epoch().count() == 0 ||
                 nowc - session_last_clock > std::chrono::seconds(2))) {
              session_last_clock = nowc;
              auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 nowc - session_boot_steady)
                                 .count();
              send_frame(fd, conn,
                         encode_time_sync_0x38(static_cast<uint32_t>(elapsed & 0xFFFFFFFFu),
                                              session_time_seq++));
              send_frame(fd, conn,
                         encode_player_int_prop(kPropTimeTick, server_clock_minutes()));
            }
          }
          // 67-byte position update: floats at 0x26 (Go parseMotionPosition)
          if (plain.size() == 67 && plain.size() > 0x26 + 20 &&
              plain[0x26] == 4 && plain[0x26 + 5] == 4 &&
              plain[0x26 + 10] == 4 && plain[0x26 + 15] == 4) {
            // Moving cancels sitcross
            if (session_logic_state == kLogicStateSitcross) {
              session_logic_state = kLogicStateNormal;
              send_frame(fd, conn, encode_player_byte_prop(kPropLogicState, kLogicStateNormal));
              send_frame(fd, conn, encode_player_str_prop(kPropState, "stand"));
              NINEYIN_LOG_INFO("%s: sitcross cancelled by move", peer.c_str());
            }
            float mx, my, mz, mo;
            std::memcpy(&mx, plain.data() + 0x27, 4);
            std::memcpy(&my, plain.data() + 0x2C, 4);
            std::memcpy(&mz, plain.data() + 0x31, 4);
            std::memcpy(&mo, plain.data() + 0x36, 4);
            session_x = mx;
            session_y = my;
            session_z = mz;
            session_orient = mo;
            auto now = std::chrono::steady_clock::now();
            if (session_account_key.size() > 8 &&
                (now - last_pos_save) > std::chrono::seconds(2)) {
              nineyin::db::roles().update_location(session_account_key, mx, my, mz, mo);
              last_pos_save = now;
              NINEYIN_LOG_INFO("%s: pos saved (%.1f,%.1f,%.1f)", peer.c_str(), mx, my, mz);
            }
            // Stream NPCs entering visual range — positions from shared world
            if (session_npcs_sent && session_world) {
              float moved = (session_x - session_npc_last_x) * (session_x - session_npc_last_x)
                          + (session_z - session_npc_last_z) * (session_z - session_npc_last_z);
              if (moved > 1.0f) {
                session_npc_last_x = session_x;
                session_npc_last_z = session_z;
                const float kR2 = 450.f * 450.f;
                int added = 0;
                std::lock_guard<std::mutex> lk(session_world->mu);
                for (auto& npc : session_world->set.npcs) {
                  if (session_npc_sent_ids.count(npc.object_id)) continue;
                  float d2 = (npc.x - session_x) * (npc.x - session_x)
                           + (npc.z - session_z) * (npc.z - session_z);
                  if (d2 > kR2) continue;
                  auto pkt = nineyin::scene::encode_npc_add_object(npc);
                  if (!send_frame(fd, conn, pkt)) break;
                  auto loc = nineyin::scene::encode_npc_location(npc);
                  if (!send_frame(fd, conn, loc)) break;
                  session_npc_sent_ids.insert(npc.object_id);
                  added++;
                  if (added >= 50) break;
                }
                if (added > 0)
                  NINEYIN_LOG_INFO("%s: stream NPCs +%d total_sent=%zu", peer.c_str(), added,
                                   session_npc_sent_ids.size());
              }
            }

          } else {
            // non-67 activity (28/37/42/...): still tick patrol below
          }

            // --- NPC patrol tick (must run on ANY 0x0A, not only 67-byte motion) ---
            // Official: most functional/shop/quest NPCs stand still; only NPCs that
            // already have a route (from PathID / assign_patrol_routes) walk.
            // Do NOT invent local ±2m routes for every nearby green NPC — that was
            // making the entire spawn area "dance".
            {
              auto nowp = std::chrono::steady_clock::now();
              for (auto it = pending_npc_arrive.begin(); it != pending_npc_arrive.end();) {
                if (nowp < it->second.at) { ++it; continue; }
                // Skip stale arrive if a newer shared leg already started (avoids snap-back teleport)
                bool skip = false;
                if (session_world) {
                  std::lock_guard<std::mutex> lk(session_world->mu);
                  auto lit = session_world->legs.find(it->first);
                  if (lit != session_world->legs.end() && lit->second.at > it->second.at)
                    skip = true;
                }
                if (!skip) {
                  nineyin::scene::NpcInstance tmp{};
                  tmp.object_id = it->first;
                  tmp.owner_id = 1;
                  tmp.x = it->second.x;
                  tmp.y = it->second.y;
                  tmp.z = it->second.z;
                  tmp.orient = it->second.o;
                  send_frame(fd, conn, nineyin::scene::encode_npc_location(tmp));
                }
                it = pending_npc_arrive.erase(it);
              }
              // Auto-clear target when player walks far from selected object.
              if (session_selected_oid != 0) {
                float sdx = session_x - session_selected_x;
                float sdz = session_z - session_selected_z;
                if (sdx * sdx + sdz * sdz > kSelectClearRadius * kSelectClearRadius) {
                  for (auto& fr : make_clear_selection_frames()) send_frame(fd, conn, fr);
NINEYIN_LOG_INFO("%s: auto-clear LastObject (out of range) was=%u",
                                   peer.c_str(), session_selected_oid);
                  session_selected_oid = 0;
                  session_selected_owner = 0;
                }
              }
              // Tick shared scene, then push 0x20 for legs this client has not yet seen.
              if (session_npcs_sent && session_world &&
                  (nowp - session_last_patrol) > std::chrono::milliseconds(500)) {
                session_last_patrol = nowp;
                tick_shared_scene(*session_world);
                int moved_n = 0, near_route = 0, near_stand = 0, legs_active = 0;
                std::lock_guard<std::mutex> lk(session_world->mu);
                legs_active = static_cast<int>(session_world->legs.size());
                for (auto& n : session_world->set.npcs) {
                  if (session_npc_sent_ids.count(n.object_id) == 0) continue;
                  float dx = n.x - session_x, dz = n.z - session_z;
                  float d2 = dx * dx + dz * dz;
                  if (d2 > 250.f * 250.f) continue;
                  if (n.route.size() < 2) {
                    if (d2 <= 80.f * 80.f) near_stand++;
                    continue;
                  }
                  near_route++;
                  auto lit = session_world->legs.find(n.object_id);
                  if (lit == session_world->legs.end()) continue;
                  auto sit = session_leg_sent.find(n.object_id);
                  if (sit != session_leg_sent.end() && sit->second == lit->second.started)
                    continue;  // already sent this leg to this client
                  auto mv = nineyin::scene::encode_npc_moving(n, lit->second.x, lit->second.y,
                                                             lit->second.z, lit->second.o);
                  if (!send_frame(fd, conn, mv)) break;
                  session_leg_sent[n.object_id] = lit->second.started;
                  auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    lit->second.at - nowp)
                                    .count();
                  if (remain < 300) remain = 300;
                  pending_npc_arrive[n.object_id] = PendingArrive{
                      nowp + std::chrono::milliseconds(remain), lit->second.x, lit->second.y,
                      lit->second.z, lit->second.o};
                  if (++moved_n >= 32) break;
                }
                if (moved_n > 0 || near_route > 0)
                  NINEYIN_LOG_INFO(
                      "%s: path patrol broadcast moved=%d near_route=%d near_stand=%d "
                      "legs_active=%d pending=%zu act_len=%zu",
                      peer.c_str(), moved_n, near_route, near_stand, legs_active,
                      pending_npc_arrive.size(), plain.size());
              }
            }

        } else if (opcode == 0x07) {
          // C2S Select / object click — 33 bytes: seq@17, oid@21, owner@25, func@29
          // oid==0 or owner==0: client cancelled selection (ESC while target selected).
          if (plain.size() >= 29) {
            uint32_t seq = 0, oid = 0, owner = 0, func = 0;
            std::memcpy(&seq, plain.data() + 17, 4);
            std::memcpy(&oid, plain.data() + 21, 4);
            std::memcpy(&owner, plain.data() + 25, 4);
            if (plain.size() >= 33) std::memcpy(&func, plain.data() + 29, 4);
            if (oid == 0 || owner == 0) {
              // Clear target so ESC can fall through to system menu next press.
              for (auto& fr : make_clear_selection_frames()) send_frame(fd, conn, fr);
session_selected_oid = 0;
              session_selected_owner = 0;
              NINEYIN_LOG_INFO("%s: Select 0x07 clear target (ESC/deselect) seq=%u",
                               peer.c_str(), seq);
            } else if (oid != 0 && owner != 0) {
              // NOTE: do NOT clear on re-select same target — that made skills/选中 flaky.
              // Always clear residual menu first so ESC is not trapped in talk UI
              {
                std::vector<uint8_t> clr_menu(1, 0x1D);
                send_frame(fd, conn, clr_menu);
              }
              auto pkt = encode_last_object(1, 1, oid, owner);
              send_frame(fd, conn, pkt);
              // SelectState=1 so client enters combat-target mode (skills enabled)
              send_frame(fd, conn, encode_player_byte_prop(kPropSelectState, 1));
              session_selected_oid = oid;
              session_selected_owner = owner;
              session_selected_x = session_x;
              session_selected_z = session_z;
              for (auto& n : session_npc_set.npcs) {
                if (n.object_id == oid && n.owner_id == owner) {
                  session_selected_x = n.x;
                  session_selected_z = n.z;
                  break;
                }
              }
              if (func != 0) {
                // menu action / leave — clear selection if leave-like
                if (func == 600000000 || static_cast<int32_t>(func) < 0) {
                  auto clr = encode_last_object(1, 1, 0, 0);
                  send_frame(fd, conn, clr);
                  session_selected_oid = 0;
                  session_selected_owner = 0;
                }
                NINEYIN_LOG_INFO(
                    "%s: Select 0x07 seq=%u target=%u owner=%u func=%u (menu action)",
                    peer.c_str(), seq, oid, owner, func);
              } else {
                std::string cfg;
                bool is_mon = false;
                for (auto& n : session_npc_set.npcs) {
                  if (n.object_id == oid && n.owner_id == owner) {
                    cfg = n.config_id;
                    is_mon = n.is_monster;
                    break;
                  }
                }
                // Do NOT open Npc_Func talk menus here: residual cinematic menus
                // trap the first ESC (cannot deselect / cannot open system settings).
                // Selection = LastObject only (matches combat click on official).
                (void)is_mon;
                NINEYIN_LOG_INFO(
                    "%s: Select 0x07 target=%u cfg=%s -> LastObject only (no menu)",
                    peer.c_str(), oid, cfg.c_str());
              }
            }
          } else {
            NINEYIN_LOG_INFO("%s: Select 0x07 short len=%zu", peer.c_str(), plain.size());
          }
        } else if (opcode == 0x18) {
          // Client ping/heartbeat (12 bytes). Official RTT uses C2S 0x18 -> S2C 0x38.
          // Without this reply the HUD latency sticks at ~3000ms timeout.
          uint32_t tick = 0;
          if (plain.size() >= 5) {
            tick = static_cast<uint32_t>(plain[1]) |
                   (static_cast<uint32_t>(plain[2]) << 8) |
                   (static_cast<uint32_t>(plain[3]) << 16) |
                   (static_cast<uint32_t>(plain[4]) << 24);
          }
          send_frame(fd, conn,
                     nineyin::game::encode_time_sync_0x38(tick, session_time_seq++));
          static int ping_n = 0;
          if ((++ping_n % 10) == 0) {
            send_frame(fd, conn,
                       encode_player_int_prop(kPropTimeTick, server_clock_minutes()));
            NINEYIN_LOG_INFO("%s: ping 0x18->0x38 tick=%u clock_min=%d", peer.c_str(), tick,
                             server_clock_minutes());
          }
        } else {
          // While a target is selected, log unknown opcodes (ESC may use one of these).
          if (session_selected_oid != 0) {
            char hex[96];
            size_t n = plain.size() < 24 ? plain.size() : 24;
            size_t off = 0;
            for (size_t i = 0; i < n && off + 3 < sizeof(hex); ++i)
              off += static_cast<size_t>(
                  std::snprintf(hex + off, sizeof(hex) - off, "%02X ", plain[i]));
            NINEYIN_LOG_INFO(
                "%s: opcode 0x%02X while selected target=%u len=%zu head=%s",
                peer.c_str(), opcode, session_selected_oid, plain.size(), hex);
          } else {
            NINEYIN_LOG_INFO("%s: opcode 0x%02X not handled yet (len=%zu)",
                             peer.c_str(), opcode, plain.size());
          }
        }
        ready = conn.has_frame();
      }
    } catch (const nineyin::transport::FrameError& e) {
      NINEYIN_LOG_ERROR("%s: frame error: %s", peer.c_str(), e.what());
      break;
    }
  }

  net_close(fd);
  nineyin::gm::Hub::instance().client_disconnected();
  // flush pos on disconnect
  if (!session_account_key.empty() && session_account_key != "acct:v1:unknown" && has_role) {
    nineyin::db::roles().update_location(session_account_key, session_x, session_y, session_z,
                                         session_orient);
    NINEYIN_LOG_INFO("%s: pos flushed on disconnect (%.1f,%.1f,%.1f)", peer.c_str(), session_x,
                     session_y, session_z);
  }
  NINEYIN_LOG_INFO("%s: disconnected", peer.c_str());
}


}  // namespace

int main(int argc, char** argv) {
  std::string listen_addr = "0.0.0.0:4000,0.0.0.0:19061";
  std::string root_override;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    }
    if (arg == "--listen" && i + 1 < argc) {
      listen_addr = argv[++i];
      continue;
    }
    if (arg == "--root" && i + 1 < argc) {
      root_override = argv[++i];
      continue;
    }
    std::cerr << "unknown argument: " << arg << "\n";
    print_usage(argv[0]);
    return 1;
  }

  if (!root_override.empty()) {
#if defined(_WIN32)
    _putenv_s("NINEYIN_SERVER_ROOT", root_override.c_str());
#else
    ::setenv("NINEYIN_SERVER_ROOT", root_override.c_str(), 1);
#endif
  }

#if defined(_WIN32)
  WSADATA wsa{};
  if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    NINEYIN_LOG_ERROR("WSAStartup failed");
    return 1;
  }
  ::SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#endif

  auto paths = nineyin::config::resolve_resource_paths();
  NINEYIN_LOG_INFO("server root          = %s", paths.server_root.string().c_str());
  NINEYIN_LOG_INFO("modern share         = %s", paths.modern_share.string().c_str());
  NINEYIN_LOG_INFO("qinggong define      = %s", paths.qinggong_define.string().c_str());
  NINEYIN_LOG_INFO("npc table dir        = %s", paths.npc_table_dir.string().c_str());
  NINEYIN_LOG_INFO("legacy Npc_Func.xml  = %s", paths.legacy_npc_func.string().c_str());
  NINEYIN_LOG_INFO("data dir             = %s", paths.data_dir.string().c_str());

  auto check = [](const char* name, const std::filesystem::path& p) {
    if (std::filesystem::exists(p)) {
      NINEYIN_LOG_INFO("  [OK] %s", name);
    } else {
      NINEYIN_LOG_WARN("  [MISSING] %s -> %s", name, p.string().c_str());
    }
  };
  check("resources/modern/share", paths.modern_share);
  check("qgdefine.ini", paths.qinggong_define);
  check("npcconfig", paths.npc_table_dir);
  check("Npc_Func.xml", paths.legacy_npc_func);
  check("data/", paths.data_dir);

  auto endpoints = parse_listen_list(listen_addr);
  if (endpoints.empty()) {
    NINEYIN_LOG_ERROR("invalid --listen address: %s", listen_addr.c_str());
#if defined(_WIN32)
    ::WSACleanup();
#endif
    return 1;
  }

  std::vector<socket_t> listen_fds;
  std::vector<uint16_t> listen_ports;
  for (const auto& ep : endpoints) {
    socket_t fd = bind_listen(ep.first, ep.second);
    if (fd == kInvalidSocket) {
      for (socket_t x : listen_fds) net_close(x);
#if defined(_WIN32)
      ::WSACleanup();
#endif
      return 1;
    }
    listen_fds.push_back(fd);
    listen_ports.push_back(ep.second);
  }

#if !defined(_WIN32)
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
#endif

  // ---- GM web panel (localhost only) ----
  const auto started_at = std::chrono::steady_clock::now();
  nineyin::gm::ServerStatus gst;
  gst.running = true;
  {
    std::string joined;
    for (size_t i = 0; i < endpoints.size(); ++i) {
      if (i) joined += ",";
      joined += endpoints[i].first + ":" + std::to_string(endpoints[i].second);
    }
    gst.listen_addr = joined;
  }
  gst.server_root = paths.server_root.string();
  gst.connected_clients = 0;
  gst.uptime_sec = 0;
  gst.phase = "Phase-1 (Area 4000 + Game 19061)";
  nineyin::gm::Hub::instance().set_status(gst);

    {
    std::string store_path = (paths.server_root / "data" / "roles.store").string();
    nineyin::db::roles().init(store_path);
    {
      auto item_dir = (paths.modern_share / "item").string();
      equipment_resolver().load(item_dir);
    }
    NINEYIN_LOG_INFO("role store          = %s (%s)", store_path.c_str(),
                     nineyin::db::roles().backend_name().c_str());
  }
  NINEYIN_LOG_INFO("%s", nineyin::game::system_status_table());

  // Npc_Func for dialogue menus
  {
    std::filesystem::path nf = paths.server_root / "resources" / "legacy" / "Npc_Func.xml";
    if (!std::filesystem::exists(nf))
      nf = paths.server_root / "Npc_Func.xml";
    nineyin::scene::load_npc_func_xml(nf.string());
  }

  nineyin::gm::HttpServer gm_http("127.0.0.1", 19080);
  gm_http.start();
  NINEYIN_LOG_INFO("GM panel URL: %s  (opening browser...)", gm_http.url().c_str());

  // Update HTTP disabled — use official update-server.exe on :80 if needed
  NINEYIN_LOG_INFO("Update HTTP : DISABLED (use update-server.exe for launcher)");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  open_browser(gm_http.url());

  std::thread([&started_at] {
    while (g_running) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      auto st = nineyin::gm::Hub::instance().status();
      st.uptime_sec = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - started_at)
                          .count();
      nineyin::gm::Hub::instance().set_status(st);
    }
  }).detach();

  NINEYIN_LOG_INFO("================================================");
  NINEYIN_LOG_INFO("  SERVER READY (matches client server_list.ini)");
  NINEYIN_LOG_INFO("  Area  TCP : 0.0.0.0:4000   <- client AreaPort");
  NINEYIN_LOG_INFO("  Game  TCP : 0.0.0.0:19061  <- client Server port");
  NINEYIN_LOG_INFO("  GM    HTTP: http://127.0.0.1:19080/");
  NINEYIN_LOG_INFO("  UpdateHTTP: DISABLED — use update-server.exe");
  NINEYIN_LOG_INFO("  Waiting for client connections...");
  NINEYIN_LOG_INFO("================================================");

  // One accept thread per listen port
  std::vector<std::thread> acceptors;
  for (size_t i = 0; i < listen_fds.size(); ++i) {
    acceptors.emplace_back(accept_loop, listen_fds[i], listen_ports[i]);
  }
  for (auto& th : acceptors) {
    if (th.joinable()) th.join();
  }

  gm_http.stop();
#if defined(_WIN32)
  ::WSACleanup();
#endif
  NINEYIN_LOG_INFO("server stopped");
  return 0;
}

