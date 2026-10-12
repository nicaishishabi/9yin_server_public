#pragma once

#include <filesystem>
#include <string>

namespace nineyin::config {

namespace fs = std::filesystem;

/// Environment variable that forces the server root (same semantics as Go).
inline constexpr const char* kServerRootEnv = "NINEYIN_SERVER_ROOT";
inline constexpr const char* kShareRootEnv  = "NINEYIN_SHARE_ROOT";

/// Resolves the project/server root.
/// Priority:
///   1. NINEYIN_SERVER_ROOT environment variable
///   2. Walk upward from the executable / current working directory looking for
///      resources/modern/share  OR  a CMakeLists.txt / go.mod marker
///   3. Current working directory
[[nodiscard]] fs::path locate_server_root();

/// Convenience helpers that always resolve relative to the server root.
[[nodiscard]] fs::path server_path(std::initializer_list<std::string_view> parts);

struct ResourcePaths {
  fs::path server_root;

  // Modern client data (the bulk of what you already own)
  fs::path modern_share;       // resources/modern/share
  fs::path modern_action;      // resources/modern/ini/action
  fs::path modern_text;        // resources/modern/text

  // Legacy
  fs::path legacy_npc_func;    // resources/legacy/Npc_Func.xml

  // Derived frequently-used paths (same defaults as the Go server)
  fs::path npc_table;          // .../npc/npcconfig/worldnpc/city_commonnpc.txt
  fs::path npc_table_dir;      // .../npc/npcconfig
  fs::path npc_creator;        // .../creator/npc_creator/city05_chengdu/funcnpc.xml
  fs::path npc_creator_dir;    // .../creator/npc_creator
  fs::path qinggong_define;    // .../skill/qinggong/qgdefine.ini
  fs::path door_data;          // .../rule/door_data.ini
  fs::path shop_ini;           // .../trade/shop.ini
  fs::path skill_root;         // .../skill
  fs::path item_root;          // .../item   (or NINEYIN_SHARE_ROOT/item)

  // Server-local data
  fs::path data_dir;           // data/
  fs::path roles_json;         // data/roles.json
  fs::path faculty_json;       // data/faculty.json
  fs::path logs_dir;           // artifacts/local/logs  (or logs/)
};

/// Build the full path set from a resolved server root.
[[nodiscard]] ResourcePaths make_resource_paths(const fs::path& server_root);

/// One-shot convenience that locates the root and builds the path set.
[[nodiscard]] ResourcePaths resolve_resource_paths();

}  // namespace nineyin::config
