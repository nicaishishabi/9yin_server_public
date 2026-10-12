#include "9yin/scene/npc_scene.hpp"
#include "9yin/scene/npc_template.hpp"

#include "9yin/util/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <unordered_map>
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
  out.npcs.clear();
  if (!fs::is_directory(creator_dir)) {
    NINEYIN_LOG_WARN("npc creator dir missing: %s", creator_dir.c_str());
    return 0;
  }

  // file.ini order (Go sceneCreatorPaths). Prefer Common/Func over Attack.
  std::vector<std::string> ordered;
  {
    fs::path manifest = fs::path(creator_dir) / "file.ini";
    std::ifstream mf(manifest);
    if (mf) {
      std::string line;
      while (std::getline(mf, line)) {
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto name = line.substr(eq + 1);
        while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
        size_t i = 0;
        while (i < name.size() && (name[i] == ' ' || name[i] == '\t')) i++;
        name = name.substr(i);
        if (name.empty()) continue;
        auto low = lower(name);
        // Event triggers / editor dumps are not permanent scene residents.
        if (low.find("eventtrigger") != std::string::npos) continue;
        if (low.find("_creator") != std::string::npos) continue;  // AttackNpc_Creator.xml etc.
        if (low.find("taskgather") != std::string::npos) continue;
        if (low.find("boxnpc") != std::string::npos) continue;
        if (low.find("lightnpc") != std::string::npos) continue;
        if (low.find("guildbuilding") != std::string::npos) continue;
        fs::path p = fs::path(creator_dir) / name;
        if (fs::is_regular_file(p)) ordered.push_back(p.string());
      }
    }
  }
  // Fallback: all xml except eventtrigger
  if (ordered.empty()) {
    try {
      for (auto& e : fs::directory_iterator(creator_dir)) {
        if (!e.is_regular_file()) continue;
        auto n = e.path().filename().string();
        auto low = lower(n);
        if (low.size() < 4 || low.substr(low.size() - 4) != ".xml") continue;
        if (low.find("eventtrigger") != std::string::npos) continue;
        if (low.find("_creator") != std::string::npos) continue;
        if (low.find("taskgather") != std::string::npos) continue;
        if (low.find("boxnpc") != std::string::npos) continue;
        if (low.find("lightnpc") != std::string::npos) continue;
        if (low.find("guildbuilding") != std::string::npos) continue;
        ordered.push_back(e.path().string());
      }
    } catch (...) {
    }
    // Prefer common/func first
    std::sort(ordered.begin(), ordered.end(), [](const std::string& a, const std::string& b) {
      auto score = [](const std::string& p) {
        auto n = lower(p);
        if (n.find("commonnpc") != std::string::npos) return 0;
        if (n.find("funcnpc") != std::string::npos) return 1;
        if (n.find("itemnpc") != std::string::npos) return 2;
        if (n.find("attack") != std::string::npos) return 10;
        return 5;
      };
      return score(a) < score(b);
    });
  }

  uint32_t next_oid = 10001;
  int added = 0;
  int from_common = 0, from_attack = 0, from_other = 0;

  for (const auto& path_str : ordered) {
    fs::path path(path_str);
    auto fname = lower(path.filename().string());
    bool is_attack = fname.find("attack") != std::string::npos;
    bool is_common = fname.find("common") != std::string::npos || fname.find("func") != std::string::npos;

    std::ifstream f(path, std::ios::binary);
    if (!f) continue;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    while (pos < content.size()) {
      auto s = content.find("<item", pos);
      if (s == std::string::npos) break;
      auto e = content.find('>', s);
      if (e == std::string::npos) break;
      pos = e + 1;
      std::string tag = content.substr(s, e - s + 1);
      // skip self-closing check — attributes are in opening tag
      float x = 0, y = 0, z = 0, ay = 0;
      if (!parse_float_attr(tag, "x", x)) continue;
      if (!parse_float_attr(tag, "y", y)) continue;
      if (!parse_float_attr(tag, "z", z)) continue;
      parse_float_attr(tag, "ay", ay);
      std::string cfg = parse_attr(tag, "id");
      if (cfg.empty()) cfg = parse_attr(tag, "ConfigID");
      if (cfg.empty()) continue;
      // Sub-scene / instance-only placements (not main open world)
      {
        float sub = 0;
        if (parse_float_attr(tag, "SubSceneNo", sub) && sub > 0.5f) continue;
      }

      NpcInstance n;
      n.object_id = next_oid++;
      n.owner_id = 1;
      n.config_id = cfg;
      n.x = x;
      n.y = y;
      n.z = z;
      n.orient = ay;
      n.home_x = x;
      n.home_y = y;
      n.home_z = z;
      n.is_monster = is_attack;
      // crude id heuristic
      auto cl = lower(cfg);
      if (cl.find("monster") != std::string::npos || cl.find("boss") != std::string::npos ||
          cl.find("mon") == 0)
        n.is_monster = true;
      // Creator path binding. Official XML uses "PathId" (camel), not "PathID".
      // Empty PathId = stand at home (do not invent a route).
      n.path_id = parse_attr(tag, "PathId");
      if (n.path_id.empty()) n.path_id = parse_attr(tag, "PathID");  // tolerate old dumps
      n.born_action = parse_attr(tag, "BornAction");
      {
        float pm = 0;
        if (parse_float_attr(tag, "PatrolMode", pm)) n.patrol_mode = static_cast<int>(pm);
      }

      out.npcs.push_back(std::move(n));
      ++added;
      if (is_common) from_common++;
      else if (is_attack) from_attack++;
      else from_other++;
    }
  }

  if (!out.npcs.empty()) {
    NINEYIN_LOG_INFO("npc sample[0] id=%s pos=(%.1f,%.1f,%.1f)", out.npcs[0].config_id.c_str(),
                     out.npcs[0].x, out.npcs[0].y, out.npcs[0].z);
  }
  NINEYIN_LOG_INFO(
      "npc permanent loaded dir=%s count=%d common/func=%d attack=%d other=%d",
      creator_dir.c_str(), added, from_common, from_attack, from_other);
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

int load_mappath_table(const std::string& mappath_dir, const std::string& scene_resource,
                       std::unordered_map<std::string, std::vector<Waypoint>>& out) {
  namespace fs = std::filesystem;
  out.clear();
  if (mappath_dir.empty() || !fs::is_directory(mappath_dir)) {
    NINEYIN_LOG_WARN("mappath dir missing: %s", mappath_dir.c_str());
    return 0;
  }
  // scene_resource may be "born03" or "born03_YanYuZhuang" / full path
  std::string res = scene_resource;
  auto slash = res.find_last_of("/\\");
  if (slash != std::string::npos) res = res.substr(slash + 1);
  std::string short_res = res;
  auto us = res.find('_');
  if (us != std::string::npos) short_res = res.substr(0, us);

  // Accept any ini whose stem starts with scene short name: born03_npc.ini, born03_mst.ini
  int files = 0, sections = 0;
  for (auto& ent : fs::directory_iterator(mappath_dir)) {
    if (!ent.is_regular_file()) continue;
    auto path = ent.path();
    auto ext = path.extension().string();
    for (char& c : ext)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (ext != ".ini") continue;
    auto stem = path.stem().string();
    auto stem_l = stem;
    for (char& c : stem_l)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    auto short_l = short_res;
    for (char& c : short_l)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (stem_l.find(short_l) != 0) continue;
    // Offline AI paths are not NPC patrol routes (would mis-bind / fly).
    if (stem_l.find("offline") != std::string::npos) continue;

    std::ifstream f(path);
    if (!f) continue;
    files++;
    std::string line, cur;
    while (std::getline(f, line)) {
      // trim
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
      size_t i0 = 0;
      while (i0 < line.size() && (line[i0] == ' ' || line[i0] == '\t')) ++i0;
      if (i0) line = line.substr(i0);
      if (line.empty() || line[0] == ';' || line[0] == '#') continue;
      if (line.front() == '[' && line.back() == ']') {
        cur = line.substr(1, line.size() - 2);
        continue;
      }
      if (cur.empty()) continue;
      // r=,x,y,z,orient,wait,stand,nil,walk,nil,1,
      if (line.size() < 2 || (line[0] != 'r' && line[0] != 'R')) continue;
      if (line.find('=') == std::string::npos) continue;
      auto eq = line.find('=');
      std::string rest = line.substr(eq + 1);
      std::vector<std::string> parts;
      {
        std::string curp;
        for (char c : rest) {
          if (c == ',') {
            parts.push_back(curp);
            curp.clear();
          } else {
            curp.push_back(c);
          }
        }
        parts.push_back(curp);
      }
      // parts[0] often empty (leading comma after =)
      size_t base = 0;
      if (!parts.empty() && parts[0].empty()) base = 1;
      if (parts.size() < base + 3) continue;
      try {
        Waypoint w;
        w.x = std::stof(parts[base + 0]);
        w.y = std::stof(parts[base + 1]);
        w.z = std::stof(parts[base + 2]);
        w.orient = (parts.size() > base + 3 && !parts[base + 3].empty())
                       ? std::stof(parts[base + 3])
                       : 0.f;
        // field 6 = stand wait ms (200 / 3000 typical)
        if (parts.size() > base + 4 && !parts[base + 4].empty()) {
          try {
            w.wait_ms = static_cast<int>(std::stof(parts[base + 4]));
          } catch (...) {
            w.wait_ms = 200;
          }
        }
        auto& vec = out[cur];
        if (vec.empty()) sections++;
        vec.push_back(w);
      } catch (...) {
        continue;
      }
    }
    NINEYIN_LOG_INFO("mappath loaded %s sections_now=%zu", path.string().c_str(), out.size());
  }
  NINEYIN_LOG_INFO("mappath table scene=%s files=%d named_paths=%zu", short_res.c_str(), files,
                   out.size());
  return static_cast<int>(out.size());
}

void assign_patrol_routes(SceneNpcSet& set) {
  // Config only: PathId -> mappath section. Snap NPC onto the path so the first
  // leg is not a long straight line through terrain/water/roofs (no navmesh).
  for (auto& n : set.npcs) {
    n.route.clear();
    n.route_idx = 0;
    n.route_dir = 1;
  }
  int walkers = 0, missing_path = 0, stand = 0, monster_walkers = 0, snapped = 0;
  constexpr float kSnapMax = 35.f;  // if home farther than this from every WP, stand
  for (auto& n : set.npcs) {
    if (n.path_id.empty()) {
      stand++;
      continue;
    }
    auto it = set.path_table.find(n.path_id);
    if (it == set.path_table.end() || it->second.size() < 2) {
      missing_path++;
      stand++;
      continue;
    }
    // Drop degenerate consecutive duplicates
    std::vector<Waypoint> cleaned;
    cleaned.reserve(it->second.size());
    for (const auto& w : it->second) {
      if (!cleaned.empty()) {
        float dx = w.x - cleaned.back().x, dy = w.y - cleaned.back().y, dz = w.z - cleaned.back().z;
        if (dx * dx + dy * dy + dz * dz < 0.25f) continue;
      }
      cleaned.push_back(w);
    }
    if (cleaned.size() < 2) {
      missing_path++;
      stand++;
      continue;
    }
    // Nearest waypoint to creator home
    int best = 0;
    float best_d2 = 1e12f;
    for (int i = 0; i < static_cast<int>(cleaned.size()); ++i) {
      float dx = cleaned[i].x - n.home_x, dy = cleaned[i].y - n.home_y, dz = cleaned[i].z - n.home_z;
      float d2 = dx * dx + dy * dy + dz * dz;
      if (d2 < best_d2) {
        best_d2 = d2;
        best = i;
      }
    }
    if (best_d2 > kSnapMax * kSnapMax) {
      // Path exists but is far from spawn — do not stretch across the map.
      stand++;
      missing_path++;
      continue;
    }
    n.route = std::move(cleaned);
    n.route_idx = best;
    n.route_dir = 1;
    // Snap onto path (official NPCs live on their route, not at arbitrary home offset)
    n.x = n.route[static_cast<size_t>(best)].x;
    n.y = n.route[static_cast<size_t>(best)].y;
    n.z = n.route[static_cast<size_t>(best)].z;
    if (n.route[static_cast<size_t>(best)].orient != 0.f)
      n.orient = n.route[static_cast<size_t>(best)].orient;
    snapped++;
    walkers++;
    if (n.is_monster) monster_walkers++;
  }
  NINEYIN_LOG_INFO(
      "patrol policy (config): walkers=%d snapped=%d (monsters=%d) stand=%d "
      "missing_or_far=%d named_paths=%zu",
      walkers, snapped, monster_walkers, stand, missing_path, set.path_table.size());
}

// Begin next mappath leg WITHOUT moving n to the destination.
// Caller sends 0x20(dest=route[next]), keeps n at start until confirm_path_arrive().
// This avoids late 0x1F snapping past a newer 0x20 (client-visible teleport).
bool step_path_patrol(NpcInstance& n) {
  if (n.route.size() < 2) return false;
  // Max single leg on XZ plane — beyond this is "fly through geometry" without navmesh.
  constexpr float kMaxLeg = 28.f;
  constexpr float kMaxDy = 6.f;
  int dir = n.route_dir == 0 ? 1 : n.route_dir;
  for (int attempt = 0; attempt < static_cast<int>(n.route.size()); ++attempt) {
    int next = n.route_idx + dir;
    if (next < 0 || next >= static_cast<int>(n.route.size())) {
      dir = -dir;
      next = n.route_idx + dir;
      if (next < 0 || next >= static_cast<int>(n.route.size())) return false;
    }
    const auto& w = n.route[static_cast<size_t>(next)];
    float ddx = w.x - n.x, ddy = w.y - n.y, ddz = w.z - n.z;
    float dist_xz = std::sqrt(ddx * ddx + ddz * ddz);
    if (dist_xz > kMaxLeg || std::fabs(ddy) > kMaxDy) {
      // Skip unreachable jump: advance index and try again (or reverse at ends)
      n.route_idx = next;
      n.route_dir = dir;
      continue;
    }
    if (dist_xz < 0.15f) {
      n.route_idx = next;
      n.route_dir = dir;
      continue;
    }
    int travel_ms = static_cast<int>(dist_xz / 2.5f * 1000.f);
    if (travel_ms < 700) travel_ms = 700;
    if (travel_ms > 10000) travel_ms = 10000;
    int wait = w.wait_ms > 0 ? w.wait_ms : 200;
    n.orient = std::atan2(ddx, ddz);
    n.route_idx = next;
    n.route_dir = dir;
    n.patrol_sign = static_cast<float>(travel_ms + wait);
    return true;
  }
  return false;
}

void confirm_path_arrive(NpcInstance& n) {
  if (n.route.empty()) return;
  int idx = n.route_idx;
  if (idx < 0 || idx >= static_cast<int>(n.route.size())) return;
  const auto& w = n.route[static_cast<size_t>(idx)];
  n.x = w.x;
  n.y = w.y;
  n.z = w.z;
  if (w.orient != 0.f) n.orient = w.orient;
}

bool peek_path_patrol(const NpcInstance& n, float& ox, float& oy, float& oz, float& oorient,
                      int& out_idx, int& out_dir) {
  // legacy helper: report current intended next full waypoint
  if (n.route.size() < 2) return false;
  int dir = n.route_dir == 0 ? 1 : n.route_dir;
  int next = n.route_idx + dir;
  if (next < 0 || next >= static_cast<int>(n.route.size())) {
    dir = -dir;
    next = n.route_idx + dir;
    if (next < 0 || next >= static_cast<int>(n.route.size())) return false;
  }
  const auto& w = n.route[static_cast<size_t>(next)];
  ox = w.x; oy = w.y; oz = w.z;
  oorient = (w.orient != 0.f) ? w.orient : n.orient;
  out_idx = next; out_dir = dir;
  return true;
}

void apply_idle_sway(NpcInstance& n, float t_sec) {
  // Only combat units may micro-wander inside their refresh radius.
  // Functional NPCs remain exactly at home.
  if (!n.is_monster) return;
  if (!n.route.empty()) return;
  n.x = n.home_x + 1.2f * std::sin(t_sec * 0.35f + n.object_id * 0.07f);
  n.z = n.home_z + 1.2f * std::cos(t_sec * 0.28f + n.object_id * 0.09f);
  n.y = n.home_y;
}

std::vector<uint8_t> encode_npc_add_object(const NpcInstance& n) {
  // Minimal + selectability (Go defaults: NPCCanSelect=1, CollideRadius≈0.9, State=stand)
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

  auto tmpl = lookup_npc_template(n.config_id);
  std::string cfg = n.config_id;
  std::string resource = tmpl.found && !tmpl.resource.empty() ? tmpl.resource : cfg;
  std::string display = tmpl.found && !tmpl.name.empty() ? tmpl.name : cfg;
  std::string action = tmpl.found && !tmpl.action_set.empty() ? tmpl.action_set : "";
  std::string weapon = tmpl.found && !tmpl.weapon_mode.empty() ? tmpl.weapon_mode : "";

  uint16_t count = 0;
  auto add_byte = [&](uint16_t idx, uint8_t v) {
    append_u16(msg, idx);
    msg.push_back(v);
    count++;
  };
  auto add_word = [&](uint16_t idx, uint16_t v) {
    append_u16(msg, idx);
    append_u16(msg, v);
    count++;
  };
  auto add_int = [&](uint16_t idx, int32_t v) {
    append_u16(msg, idx);
    append_u32(msg, static_cast<uint32_t>(v));
    count++;
  };
  auto add_float = [&](uint16_t idx, float v) {
    append_u16(msg, idx);
    uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    append_u32(msg, bits);
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

  // Indices match official 2424-entry property table (LastObject=413).
  add_byte(181, 4);               // Type = NPC
  add_byte(174, n.is_monster ? 2 : 0);  // Camp (byte)
  add_str(1439, cfg);             // ConfigID
  add_str(1440, resource);        // Resource
  if (!action.empty()) add_str(405, action);  // ActionSet
  // BornAction from creator (sit/talk/stand/...). Do not force stand for all.
  {
    std::string st = n.born_action;
    if (st.empty() || st == "nil" || st == "NULL") st = "stand";
    add_str(183, st);             // State
  }
  add_int(468, 100);              // HPRatio
  add_int(577, 1000);             // MaxHP
  add_int(464, 1000);             // HP
  if (n.is_monster) {
    add_int(1557, 1);             // NpcType
  } else {
    add_int(1557, 0);
  }
  add_wide(178, display);         // Name
  add_byte(1559, 0);              // SelectState
  add_int(265, 0);                // CantBeSelect = 0 → selectable
  if (!weapon.empty()) add_str(1597, weapon);  // WeaponMode
  add_byte(1839, 1);              // Collide
  add_float(179, 1.5f);           // CollideRadius

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



std::vector<uint8_t> encode_npc_moving(const NpcInstance& n, float dest_x, float dest_y,
                                       float dest_z, float dest_orient) {
  std::vector<uint8_t> msg(45, 0);
  msg[0] = 0x20;
  auto put32 = [](std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = static_cast<uint8_t>(v & 0xFF);
    m[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    m[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    m[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  auto putf = [&](size_t off, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    put32(msg, off, bits);
  };
  put32(msg, 1, n.object_id);
  put32(msg, 5, n.owner_id);
  putf(9, dest_x);
  putf(13, dest_y);
  putf(17, dest_z);
  putf(21, dest_orient);
  return msg;
}

std::vector<std::vector<uint8_t>> encode_talk_menu_items(
    uint32_t object_id, uint32_t owner_id,
    const std::vector<std::pair<int, std::string>>& items) {
  auto put_u16 = [](std::vector<uint8_t>& m, uint16_t v) {
    m.push_back(static_cast<uint8_t>(v & 0xFF));
    m.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  };
  auto put_u32 = [](std::vector<uint8_t>& m, uint32_t v) {
    m.push_back(static_cast<uint8_t>(v & 0xFF));
    m.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    m.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    m.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  };
  auto append_int = [&](std::vector<uint8_t>& msg, int32_t v) {
    msg.push_back(2);
    put_u32(msg, static_cast<uint32_t>(v));
  };
  auto append_str = [&](std::vector<uint8_t>& msg, const std::string& s) {
    msg.push_back(6);
    put_u32(msg, static_cast<uint32_t>(s.size() + 1));
    msg.insert(msg.end(), s.begin(), s.end());
    msg.push_back(0);
  };
  auto append_obj = [&](std::vector<uint8_t>& msg, uint32_t oid, uint32_t owner) {
    msg.push_back(8);
    put_u32(msg, oid);
    put_u32(msg, owner);
  };
  std::vector<std::vector<uint8_t>> out;
  if (items.empty()) return out;
  {
    std::vector<uint8_t> msg;
    msg.push_back(0x1E);
    put_u16(msg, 1);
    append_int(msg, 4);
    out.push_back(std::move(msg));
  }
  for (auto& it : items) {
    std::vector<uint8_t> msg;
    msg.push_back(0x1E);
    put_u16(msg, 3);
    append_int(msg, 6);
    append_int(msg, it.first);
    append_str(msg, it.second.empty() ? "talk" : it.second);
    out.push_back(std::move(msg));
  }
  {
    std::vector<uint8_t> msg;
    msg.push_back(0x1E);
    put_u16(msg, 2);
    append_int(msg, 7);
    append_obj(msg, object_id, owner_id);
    out.push_back(std::move(msg));
  }
  return out;
}

std::vector<std::vector<uint8_t>> encode_talk_menu(uint32_t object_id, uint32_t owner_id) {
  return encode_talk_menu_items(object_id, owner_id, {{1, "talk"}});
}

namespace {
std::unordered_map<std::string, std::vector<NpcFuncEntry>> g_npc_func;
}

bool load_npc_func_xml(const std::string& path) {
  g_npc_func.clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    NINEYIN_LOG_WARN("Npc_Func.xml missing: %s", path.c_str());
    return false;
  }
  std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  size_t pos = 0;
  int enabled = 0;
  while (true) {
    auto p = data.find("<Property", pos);
    if (p == std::string::npos) break;
    auto e = data.find("/>", p);
    if (e == std::string::npos) e = data.find(">", p);
    if (e == std::string::npos) break;
    std::string tag = data.substr(p, e - p);
    pos = e + 1;
    auto get = [&](const char* k) -> std::string {
      auto key = std::string(k) + "=\"";
      auto a = tag.find(key);
      if (a == std::string::npos) return {};
      a += key.size();
      auto b = tag.find('"', a);
      if (b == std::string::npos) return {};
      return tag.substr(a, b - a);
    };
    if (get("EnableFunc") == "0") continue;
    std::string npc = get("NpcID");
    std::string fid = get("FuncID");
    if (npc.empty() || fid.empty()) continue;
    NpcFuncEntry ent;
    try {
      ent.func_id = std::stoi(fid);
    } catch (...) {
      continue;
    }
    ent.name = get("Name");
    ent.prop_id = get("ID");
    g_npc_func[npc].push_back(ent);
    enabled++;
  }
  NINEYIN_LOG_INFO("Npc_Func loaded entries=%d unique_npc=%zu path=%s",
                   enabled, g_npc_func.size(), path.c_str());
  return !g_npc_func.empty();
}

const std::vector<NpcFuncEntry>* npc_func_for(const std::string& config_id) {
  if (config_id.empty()) return nullptr;
  auto it = g_npc_func.find(config_id);
  if (it != g_npc_func.end() && !it->second.empty()) return &it->second;
  // case-insensitive / substring match (creator id vs Npc_Func NpcID)
  auto lower = [](std::string s) {
    for (char& c : s)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
  };
  std::string key = lower(config_id);
  for (auto& kv : g_npc_func) {
    if (kv.second.empty()) continue;
    std::string k = lower(kv.first);
    if (k == key || k.find(key) != std::string::npos || key.find(k) != std::string::npos)
      return &kv.second;
  }
  return nullptr;
}

}  // namespace nineyin::scene
