#pragma once
#include "9yin/game/player_runtime.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace nineyin::game {

// Typed value from C2S 0x0A activity custom (same as main.cpp parser).
struct CustomVal {
  uint8_t type = 0;  // 2=i32 3=i64 4=f32 6/7=str
  int32_t i32 = 0;
  int64_t i64 = 0;
  float f32 = 0;
  std::string str;
};

struct CustomMsg {
  bool ok = false;
  std::vector<CustomVal> vals;
};

// Outbound frames produced by system handlers.
struct OutFrames {
  std::vector<std::vector<uint8_t>> frames;
  void push(std::vector<uint8_t> f) { if (!f.empty()) frames.push_back(std::move(f)); }
};

// Property helpers (official 2424-entry indices).
std::vector<uint8_t> prop_byte(uint32_t oid, uint32_t owner, uint16_t idx, uint8_t v);
std::vector<uint8_t> prop_int(uint32_t oid, uint32_t owner, uint16_t idx, int32_t v);
std::vector<uint8_t> prop_str(uint32_t oid, uint32_t owner, uint16_t idx, const std::string& s);
std::vector<uint8_t> prop_float(uint32_t oid, uint32_t owner, uint16_t idx, float v);

// Known custom message IDs (from Go protocol-probe + 官服抓包).
enum CustomMsgId : int32_t {
  kMsgSetShortcut = 200,
  kMsgRemoveShortcut = 201,
  kMsgUseSkill = 211,
  kMsgNeiGong = 215,
  kMsgSitCross = 240,
  kMsgFaculty = 1006,
  kMsgShopBuyZyb = 64,
  kMsgShopWeapon = 70,
  kMsgShopWeaponConfirm = 71,
  kMsgItemOp = 517,
  kMsgUiEvent = 770,
};

// Official props
constexpr uint16_t kPropLogicState = 462;
constexpr uint16_t kPropState = 183;
constexpr uint16_t kPropOperateMode = 223;
constexpr uint16_t kPropHP = 464;
constexpr uint16_t kPropMaxHP = 577;
constexpr uint16_t kPropTimeTick = 1491;
constexpr uint8_t kLSNormal = 0;
constexpr uint8_t kLSSit = 102;  // 'f'

std::vector<uint8_t> encode_time_sync_0x38(uint32_t tick_ms, uint16_t seq);

// Bootstrap frames after ClientReady: vitals, mode, qinggong seed, shortcuts, announcement.
OutFrames bootstrap_player(PlayerRuntime& pr);

// Dispatch one parsed custom message. Returns true if handled.
std::vector<uint8_t> make_native_skill_action(
    uint32_t caster_oid, uint32_t caster_owner,
    const std::string& skill_id,
    uint32_t target_oid, uint32_t target_owner);
std::vector<uint8_t> make_skill_action_switch();

bool dispatch_custom(PlayerRuntime& pr, const CustomMsg& msg, OutFrames& out,
                     const std::string& peer_tag);

// System status one-liners for logs / docs
const char* system_status_table();

}  // namespace nineyin::game
