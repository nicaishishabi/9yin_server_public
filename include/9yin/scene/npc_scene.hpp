#pragma once

#include "9yin/scene/path_file.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nineyin::scene {

struct NpcInstance {
  uint32_t object_id = 0;
  uint32_t owner_id = 1;
  std::string config_id;
  std::string resource;
  std::string name;
  std::string script_class;
  float x = 0, y = 0, z = 0, orient = 0;
  float home_x = 0, home_y = 0, home_z = 0;
  bool is_monster = false;
  int patrol_mode = 0;
  std::string path_id;
  std::string born_action;
  float patrol_sign = 1.f;
  std::vector<Waypoint> route;
  int route_idx = 0;
  int route_dir = 1;
};

struct SceneNpcSet {
  std::string scene_resource;
  std::vector<NpcInstance> npcs;
  std::vector<Waypoint> path_all;  // optional main.path (nav mesh style), not patrol routes
  // Named patrol routes from creator/mappath/<scene>_*.ini  (section = PathId)
  std::unordered_map<std::string, std::vector<Waypoint>> path_table;
};

std::string creator_dir_for_resource(const std::string& scene_resource,
                                     const std::string& creator_root);

int load_permanent_from_creator(const std::string& creator_dir, SceneNpcSet& out);

bool load_scene_path(const std::string& map_root, const std::string& scene_resource,
                     std::vector<Waypoint>& out);

// Load creator/mappath/*.ini for this scene (born03_npc.ini, born03_mst.ini, ...).
// Sections like [AI_yyz_001] / [mst_yyz_01] become path_table entries.
int load_mappath_table(const std::string& mappath_dir, const std::string& scene_resource,
                       std::unordered_map<std::string, std::vector<Waypoint>>& out);

// Bind routes strictly from creator PathId → path_table. No invented routes.
void assign_patrol_routes(SceneNpcSet& set);
void apply_idle_sway(NpcInstance& n, float t_sec);
bool step_path_patrol(NpcInstance& n);
void confirm_path_arrive(NpcInstance& n);
// Compute next waypoint without moving; returns false if no route.
bool peek_path_patrol(const NpcInstance& n, float& ox, float& oy, float& oz, float& oorient,
                      int& out_idx, int& out_dir);

std::vector<uint8_t> encode_npc_add_object(const NpcInstance& n);
std::vector<uint8_t> encode_npc_location(const NpcInstance& n);
std::vector<uint8_t> encode_npc_moving(const NpcInstance& n, float dest_x, float dest_y,
                                       float dest_z, float dest_orient);

std::vector<std::vector<uint8_t>> encode_talk_menu(uint32_t object_id, uint32_t owner_id);
std::vector<std::vector<uint8_t>> encode_talk_menu_items(
    uint32_t object_id, uint32_t owner_id,
    const std::vector<std::pair<int, std::string>>& items);

struct NpcFuncEntry {
  int func_id = 0;
  std::string name;
  std::string prop_id;
};
bool load_npc_func_xml(const std::string& path);
const std::vector<NpcFuncEntry>* npc_func_for(const std::string& config_id);

}  // namespace nineyin::scene
