#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace nineyin::game {

// Mirrors Go entity.Actor + playerActor combat-relevant fields (v1).
struct BuffSlot {
  std::string buffer_id;
  int32_t stacks = 1;
  int64_t expire_ms = 0;
};

struct ShortcutSlot {
  int32_t index = 0;
  std::string kind;
  std::string id;
};

struct PlayerRuntime {
  uint32_t object_id = 1;
  uint32_t owner_id = 1;

  int32_t hp = 1000, max_hp = 1000;
  int32_t mp = 1000, max_mp = 1000;
  int32_t qg = 100, max_qg = 100;

  // LogicState: 0 normal, 102 ('f') sitcross, fighting uses string State
  uint8_t logic_state = 0;
  std::string action_state = "stand";  // stand / sitdown / walk / attack...

  int32_t operate_mode = 0;  // 0 normal 3D, 1 joystick/特色
  int32_t title_id = 0;
  std::string title_name;

  std::string faction_id;
  int32_t cur_jingmai = 0;
  int32_t last_jingmai = 0;

  std::string current_skill_id;
  std::string mount_id;
  bool mounted = false;
  bool parrying = false;

  std::vector<ShortcutSlot> shortcuts;
  std::vector<BuffSlot> buffs;
  std::vector<std::string> bag_item_ids;       // config ids (v1)
  std::vector<std::string> learned_skills;    // skill config ids
  std::vector<std::string> qinggong_ids;
  std::vector<int32_t> title_ids;
  std::vector<std::string> fwz_card_ids;

  // customizing blob (UI / keybinds) hex-encoded for store
  std::string customizing_hex;

  mutable std::mutex mu;
};

}  // namespace nineyin::game
