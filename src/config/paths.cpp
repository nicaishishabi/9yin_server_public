#include "9yin/config/paths.hpp"

#include <cstdlib>
#include <iostream>

namespace nineyin::config {

namespace {

bool is_server_root(const fs::path& p) {
  std::error_code ec;
  if (fs::is_directory(p / "resources" / "modern" / "share", ec)) {
    return true;
  }
  // Also accept a CMake / go.mod marker for development checkouts
  if (fs::is_regular_file(p / "CMakeLists.txt", ec) ||
      fs::is_regular_file(p / "go.mod", ec)) {
    return true;
  }
  return false;
}

fs::path walk_up_for_root(const fs::path& start) {
  fs::path current = fs::weakly_canonical(start);
  for (;;) {
    if (is_server_root(current)) {
      return current;
    }
    fs::path parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }
  return {};
}

}  // namespace

fs::path locate_server_root() {
  if (const char* env = std::getenv(kServerRootEnv); env && env[0] != '\0') {
    return fs::weakly_canonical(fs::path(env));
  }

  // Try from current working directory
  std::error_code ec;
  fs::path cwd = fs::current_path(ec);
  if (!ec) {
    if (auto root = walk_up_for_root(cwd); !root.empty()) {
      return root;
    }
  }

  // Fallback: current directory itself
  return cwd.empty() ? fs::path(".") : cwd;
}

fs::path server_path(std::initializer_list<std::string_view> parts) {
  fs::path p = locate_server_root();
  for (auto part : parts) {
    p /= part;
  }
  return p;
}

ResourcePaths make_resource_paths(const fs::path& server_root) {
  ResourcePaths r;
  r.server_root = server_root;

  r.modern_share  = server_root / "resources" / "modern" / "share";
  r.modern_action = server_root / "resources" / "modern" / "ini" / "action";
  r.modern_text   = server_root / "resources" / "modern" / "text";

  r.legacy_npc_func = server_root / "resources" / "legacy" / "Npc_Func.xml";

  r.npc_table_dir   = r.modern_share / "npc" / "npcconfig";
  r.npc_table       = r.npc_table_dir / "worldnpc" / "city_commonnpc.txt";
  r.npc_creator_dir = r.modern_share / "creator" / "npc_creator";
  r.npc_creator     = r.npc_creator_dir / "city05_chengdu" / "funcnpc.xml";

  r.qinggong_define = r.modern_share / "skill" / "qinggong" / "qgdefine.ini";
  r.door_data       = r.modern_share / "rule" / "door_data.ini";
  r.shop_ini        = r.modern_share / "trade" / "shop.ini";
  r.skill_root      = r.modern_share / "skill";

  // Item root may be overridden by NINEYIN_SHARE_ROOT
  if (const char* share = std::getenv(kShareRootEnv); share && share[0] != '\0') {
    r.item_root = fs::path(share) / "item";
  } else {
    r.item_root = r.modern_share / "item";
  }

  r.data_dir     = server_root / "data";
  r.roles_json   = r.data_dir / "roles.json";
  r.faculty_json = r.data_dir / "faculty.json";
  r.logs_dir     = server_root / "logs";

  return r;
}

ResourcePaths resolve_resource_paths() {
  return make_resource_paths(locate_server_root());
}

}  // namespace nineyin::config
