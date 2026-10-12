#include "9yin/scene/npc_scene.hpp"
#include "9yin/scene/npc_template.hpp"

#include "9yin/util/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace nineyin::scene {
namespace {

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
void append_f32(std::vector<uint8_t>& m, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  append_u32(m, bits);
}
void append_str_prop(std::vector<uint8_t>& m, uint16_t idx, const std::string& s) {
  append_u16(m, idx);
  append_u32(m, static_cast<uint32_t>(s.size() + 1));
  m.insert(m.end(), s.begin(), s.end());
  m.push_back(0);
}
void append_widestr_prop(std::vector<uint8_t>& m, uint16_t idx, const std::string& utf8) {
  append_u16(m, idx);
  // UTF-16 LE length in bytes including null
  std::vector<uint16_t> units;
  for (size_t i = 0; i < utf8.size();) {
    unsigned char c = static_cast<unsigned char>(utf8[i]);
    uint32_t cp = c;
    if (c < 0x80) {
      i += 1;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
      cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3F);
      i += 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
      cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6) |
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

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

bool parse_float_attr(const std::string& tag, const char* key, float& out) {
  auto k = std::string(key) + "=\"";
  auto p = tag.find(k);
  if (p == std::string::npos) {
    k = std::string(key) + "='";
    p = tag.find(k);
    if (p == std::string::npos) return false;
  }
  p += k.size();
  auto e = tag.find_first_of("\"'", p);
  if (e == std::string::npos) return false;
  try {
    out = std::stof(tag.substr(p, e - p));
    return true;
  } catch (...) {
    return false;
  }
}

std::string parse_attr(const std::string& tag, const char* key) {
  auto k = std::string(key) + "=\"";
  auto p = tag.find(k);
  if (p == std::string::npos) {
    k = std::string(key) + "='";
    p = tag.find(k);
    if (p == std::string::npos) return {};
  }
  p += k.size();
  auto e = tag.find_first_of("\"'", p);
  if (e == std::string::npos) return {};
  return tag.substr(p, e - p);
}

}  // namespace

std::string creator_dir_for_resource(const std::string& scene_resource,
                                     const std::string& creator_root) {
  // Known resource → folder name under npc_creator
  static const std::map<std::string, std::string> kMap = {
      {"born01", "born01_mohezhen"},   {"born02", "born02_erengu"},
      {"born03", "born03_yanyuzhuang"}, {"born04", "born04_qiandengzhen"},
      {"city02", "city02_suzhou"},     {"city04", "city04_luoyang"},
      {"city05", "city05_chengdu"},    {"school09", "school09_gaibang"},
  };
  auto it = kMap.find(lower(scene_resource));
  std::string folder = it != kMap.end() ? it->second : lower(scene_resource);
  namespace fs = std::filesystem;
  fs::path p = fs::path(creator_root) / folder;
  if (fs::is_directory(p)) return p.string();
  // fuzzy: any dir starting with resource name
  if (fs::is_directory(creator_root)) {
    for (auto& e : fs::directory_iterator(creator_root)) {
      if (!e.is_directory()) continue;
      auto name = lower(e.path().filename().string());
      if (name.find(lower(scene_resource)) == 0) return e.path().string();
    }
  }
  return p.string();
}

int load_permanent_from_creator(const std::string& creator_dir, SceneNpcSet& out) {
  namespace fs = std::filesystem;
  if (!fs::is_directory(creator_dir)) {
    NINEYIN_LOG_WARN("npc creator dir missing: %s", creator_dir.c_str());
    return 0;
  }
  int added = 0;
  uint32_t next_id = 10001;
  for (auto& e : fs::directory_iterator(creator_dir)) {
    if (!e.is_regular_file()) continue;
    auto name = e.path().filename().string();
    auto low = lower(name);
    if (low.size() < 4 || low.substr(low.size() - 4) != ".xml") continue;
    // skip temporary / event
    if (low == "eventtrigger.xml") continue;
    if (low.find("clone") != std::string::npos) continue;
    if (low.find("story") != std::string::npos) continue;
    if (low.find("temp") != std::string::npos) continue;

    std::ifstream f(e.path(), std::ios::binary);
    if (!f) continue;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    // naive split on '<'
    size_t pos = 0;
    while (pos < content.size()) {
      auto lt = content.find('<', pos);
      if (lt == std::string::npos) break;
      auto gt = content.find('>', lt);
      if (gt == std::string::npos) break;
      std::string tag = content.substr(lt, gt - lt + 1);
      pos = gt + 1;
      if (tag.size() < 8 || tag[1] == '/' || tag[1] == '!' || tag[1] == '?') continue;
      float x = 0, y = 0, z = 0, ay = 0;
      if (!parse_float_attr(tag, "x", x)) continue;
      if (!parse_float_attr(tag, "y", y)) continue;
      if (!parse_float_attr(tag, "z", z)) continue;
      parse_float_attr(tag, "ay", ay);
      std::string cfg = parse_attr(tag, "id");
      if (cfg.empty()) cfg = parse_attr(tag, "Id");
      if (cfg.empty()) cfg = parse_attr(tag, "config");
      if (cfg.empty()) cfg = parse_attr(tag, "ConfigID");
      if (cfg.empty()) cfg = parse_attr(tag, "Config");
      // Some creators put numeric No= as clutter; prefer string-looking ids
      auto is_all_digit = [](const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) if (c < '0' || c > '9') return false;
        return true;
      };
      if (cfg.empty()) continue;
      // strip path prefix
      auto slash = cfg.find_last_of("\\/");
      if (slash != std::string::npos) cfg = cfg.substr(slash + 1);
      if (cfg.empty() || cfg == "npc_unknown") continue;

      NpcInstance n;
      n.object_id = next_id++;
      n.owner_id = 1;
      n.config_id = cfg;
      n.name = cfg;
      n.x = x;
      n.y = y;
      n.z = z;
      n.orient = ay;
      n.home_x = x;
      n.home_y = y;
      n.home_z = z;
      n.kind = NpcKind::Permanent;
      // crude monster heuristic
      auto cl = lower(cfg);
      if (cl.find("monster") != std::string::npos || cl.find("boss") != std::string::npos ||
          cl.find("mob") != std::string::npos)
        n.is_monster = true;
      out.npcs.push_back(std::move(n));
      ++added;
      if (added >= 500) break;  // safety cap per file batch
    }
    if (out.npcs.size() >= 800) break;
  }
  if (!out.npcs.empty()) {
    NINEYIN_LOG_INFO("npc sample[0] id=%s pos=(%.1f,%.1f,%.1f)", out.npcs[0].config_id.c_str(),
                     out.npcs[0].x, out.npcs[0].y, out.npcs[0].z);
  }
  NINEYIN_LOG_INFO("npc permanent loaded dir=%s count=%d", creator_dir.c_str(), added);
  return added;
}

bool load_scene_path(const std::string& map_root, const std::string& scene_resource,
                     std::vector<Waypoint>& out) {
  namespace fs = std::filesystem;
  // scene_resource may be "born03" or "born03_YanYuZhuang" — try both short and full
  std::string res = scene_resource;
  std::string short_res = res;
  auto us = res.find('_');
  if (us != std::string::npos) short_res = res.substr(0, us);

  std::vector<std::string> names = {
      res + "_main.path",
      short_res + "_main.path",
      res + ".path",
      short_res + ".path",
  };
  std::vector<fs::path> dirs = {
      fs::path(map_root),
      fs::path(map_root) / "path",
      fs::path(map_root) / "map" / "path",
      fs::path(map_root) / "res" / "map" / "path",
  };
  for (auto& d : dirs) {
    for (auto& name : names) {
      fs::path p = d / name;
      if (fs::is_regular_file(p) && load_path_file(p.string(), out)) {
        NINEYIN_LOG_INFO("path loaded %s waypoints=%zu", p.string().c_str(), out.size());
        return true;
      }
    }
  }
  NINEYIN_LOG_WARN("path not found for scene=%s under map_root=%s (tried *_main.path)",
                   scene_resource.c_str(), map_root.c_str());
  return false;
}

void assign_patrol_routes(SceneNpcSet& set) {
  auto segs = segment_path(set.path_all, 12);
  if (segs.empty()) return;
  std::vector<char> used(set.npcs.size(), 0);
  for (auto& seg : segs) {
    if (seg.empty()) continue;
    float cx = seg[0].x, cz = seg[0].z;
    int best = -1;
    float best_d = 1e30f;
    for (size_t i = 0; i < set.npcs.size(); ++i) {
      if (used[i]) continue;
      if (set.npcs[i].kind != NpcKind::Permanent) continue;
      float dx = set.npcs[i].x - cx, dz = set.npcs[i].z - cz;
      float d = dx * dx + dz * dz;
      if (d < best_d) {
        best_d = d;
        best = static_cast<int>(i);
      }
    }
    if (best < 0) break;
    used[static_cast<size_t>(best)] = 1;
    set.npcs[static_cast<size_t>(best)].route = seg;
    set.npcs[static_cast<size_t>(best)].route_index = 0;
    set.npcs[static_cast<size_t>(best)].route_dir = 1;
  }
  int with_route = 0;
  for (auto& n : set.npcs)
    if (!n.route.empty()) with_route++;
  NINEYIN_LOG_INFO("patrol assigned segments=%zu npcs_with_route=%d", segs.size(), with_route);
}

void apply_idle_sway(NpcInstance& n, float t_sec) {
  if (!n.route.empty()) return;
  n.x = n.home_x + 1.5f * std::sin(t_sec * 0.7f + n.object_id * 0.1f);
  n.z = n.home_z + 1.5f * std::cos(t_sec * 0.5f + n.object_id * 0.13f);
  n.y = n.home_y;
}

std::vector<uint8_t> encode_npc_add_object(const NpcInstance& n) {
  // Minimal packet matching Go TestStarterNPCUsesClientResourceIdentity:
  // header 0x0D + id/owner + 4 floats at 0x09..0x18; bytes 0x19..0x3c MUST be 0;
  // then only essential properties (Type, ConfigID, Resource, NpcType, Name, WeaponMode, State).
  std::vector<uint8_t> msg(0x3f, 0);
  msg[0] = 0x0D;
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put32(msg, 1, n.object_id);
  put32(msg, 5, n.owner_id);
  uint32_t xb, yb, zb, ob;
  float x = n.x, y = n.y, z = n.z, o = n.orient;
  std::memcpy(&xb, &x, 4);
  std::memcpy(&yb, &y, 4);
  std::memcpy(&zb, &z, 4);
  std::memcpy(&ob, &o, 4);
  put32(msg, 0x09, xb);
  put32(msg, 0x0D, yb);
  put32(msg, 0x11, zb);
  put32(msg, 0x15, ob);
  // 0x19..0x3c remain zero (required)

  auto tmpl = lookup_npc_template(n.config_id);
  std::string cfg = n.config_id;
  std::string resource = tmpl.found && !tmpl.resource.empty() ? tmpl.resource : cfg;
  std::string display = tmpl.found && !tmpl.name.empty() ? tmpl.name : cfg;
  // NpcType: default 0; table may not expose — use 0 unless we add column later
  int32_t npc_type = 0;

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
  auto add_str = [&](uint16_t idx, const std::string& s) {
    append_str_prop(msg, idx, s);
    count++;
  };
  auto add_wide = [&](uint16_t idx, const std::string& s) {
    append_widestr_prop(msg, idx, s);
    count++;
  };

  add_byte(0, 4);              // Type = NPC
  add_str(7, cfg);             // ConfigID
  add_str(8, resource);        // Resource e.g. npc\fbnpc00006
  add_int(34, npc_type);       // NpcType int32
  add_wide(36, display);       // Name
  add_str(19, "stand");        // State
  add_str(42, "");             // WeaponMode (empty ok if not required)

  msg[0x3d] = static_cast<uint8_t>(count & 0xFF);
  msg[0x3e] = static_cast<uint8_t>((count >> 8) & 0xFF);
  return msg;
}

std::vector<uint8_t> encode_npc_location(const NpcInstance& n) {
  std::vector<uint8_t> msg(25, 0);
  msg[0] = 0x1F;
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  put32(msg, 1, n.object_id);
  put32(msg, 5, n.owner_id);
  uint32_t xb, yb, zb, ob;
  float x = n.x, y = n.y, z = n.z, o = n.orient;
  std::memcpy(&xb, &x, 4);
  std::memcpy(&yb, &y, 4);
  std::memcpy(&zb, &z, 4);
  std::memcpy(&ob, &o, 4);
  put32(msg, 9, xb);
  put32(msg, 13, yb);
  put32(msg, 17, zb);
  put32(msg, 21, ob);
  return msg;
}

}  // namespace nineyin::scene
