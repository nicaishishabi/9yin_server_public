#pragma once
#include <string>
#include <unordered_map>

namespace nineyin::scene {

struct NpcTemplateInfo {
  std::string config_id;
  std::string resource;
  std::string name;
  std::string script_class;  // CommonNpc / EventNpc / AttackNpc ...
  std::string action_set;
  std::string weapon_mode;
  int level = 1;
  int max_hp = 1000;
  bool found = false;
};

// Load all *.txt under npcconfig (and one level of subdirs). GBK text OK as raw bytes for ASCII IDs.
void load_npc_templates(const std::string& npcconfig_root);

NpcTemplateInfo lookup_npc_template(const std::string& config_id);

size_t npc_template_count();

}  // namespace nineyin::scene
