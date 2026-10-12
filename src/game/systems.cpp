#include "9yin/game/systems.hpp"
#include "9yin/util/log.hpp"
#include <cstring>
#include <ctime>
#include <cstdio>
#include <cmath>

namespace nineyin::game {
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

std::vector<uint8_t> prop_header(uint32_t oid, uint32_t owner) {
  std::vector<uint8_t> msg(12, 0);
  msg[0] = 0x10;
  msg[1] = 0;
  msg[2] = static_cast<uint8_t>(oid & 0xFF);
  msg[3] = static_cast<uint8_t>((oid >> 8) & 0xFF);
  msg[4] = static_cast<uint8_t>((oid >> 16) & 0xFF);
  msg[5] = static_cast<uint8_t>((oid >> 24) & 0xFF);
  msg[6] = static_cast<uint8_t>(owner & 0xFF);
  msg[7] = static_cast<uint8_t>((owner >> 8) & 0xFF);
  msg[8] = static_cast<uint8_t>((owner >> 16) & 0xFF);
  msg[9] = static_cast<uint8_t>((owner >> 24) & 0xFF);
  msg[10] = 1;
  msg[11] = 0;
  return msg;
}

}  // namespace

std::vector<uint8_t> prop_byte(uint32_t oid, uint32_t owner, uint16_t idx, uint8_t v) {
  auto msg = prop_header(oid, owner);
  append_u16(msg, idx);
  msg.push_back(v);
  return msg;
}

std::vector<uint8_t> prop_int(uint32_t oid, uint32_t owner, uint16_t idx, int32_t v) {
  auto msg = prop_header(oid, owner);
  append_u16(msg, idx);
  append_u32(msg, static_cast<uint32_t>(v));
  return msg;
}

std::vector<uint8_t> prop_str(uint32_t oid, uint32_t owner, uint16_t idx, const std::string& s) {
  auto msg = prop_header(oid, owner);
  append_u16(msg, idx);
  append_u32(msg, static_cast<uint32_t>(s.size() + 1));
  msg.insert(msg.end(), s.begin(), s.end());
  msg.push_back(0);
  return msg;
}

std::vector<uint8_t> prop_float(uint32_t oid, uint32_t owner, uint16_t idx, float v) {
  auto msg = prop_header(oid, owner);
  append_u16(msg, idx);
  uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  append_u32(msg, bits);
  return msg;
}

std::vector<uint8_t> encode_time_sync_0x38(uint32_t tick_ms, uint16_t seq) {
  std::vector<uint8_t> msg(30, 0);
  msg[0] = 0x38;
  msg[1] = static_cast<uint8_t>(tick_ms & 0xFF);
  msg[2] = static_cast<uint8_t>((tick_ms >> 8) & 0xFF);
  msg[3] = static_cast<uint8_t>((tick_ms >> 16) & 0xFF);
  msg[4] = static_cast<uint8_t>((tick_ms >> 24) & 0xFF);
  msg[6] = static_cast<uint8_t>(seq & 0xFF);
  msg[7] = static_cast<uint8_t>((seq >> 8) & 0xFF);
  msg[10] = 4;
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

// --- RECORD ADD for shortcut_rec (index 4) minimal ---
static std::vector<uint8_t> record_shortcut_del(uint32_t oid, uint32_t owner, uint16_t row) {
  std::vector<uint8_t> msg(14, 0);
  msg[0] = 0x12;
  msg[2] = static_cast<uint8_t>(oid & 0xFF);
  msg[3] = static_cast<uint8_t>((oid >> 8) & 0xFF);
  msg[4] = static_cast<uint8_t>((oid >> 16) & 0xFF);
  msg[5] = static_cast<uint8_t>((oid >> 24) & 0xFF);
  msg[6] = static_cast<uint8_t>(owner & 0xFF);
  msg[7] = static_cast<uint8_t>((owner >> 8) & 0xFF);
  msg[8] = static_cast<uint8_t>((owner >> 16) & 0xFF);
  msg[9] = static_cast<uint8_t>((owner >> 24) & 0xFF);
  msg[10] = 4;  // shortcut_rec
  msg[12] = static_cast<uint8_t>(row & 0xFF);
  msg[13] = static_cast<uint8_t>((row >> 8) & 0xFF);
  return msg;
}

static std::vector<uint8_t> record_shortcut_add(uint32_t oid, uint32_t owner,
                                                const ShortcutSlot& sc) {
  std::vector<uint8_t> msg(16, 0);
  msg[0] = 0x11;
  msg[2] = static_cast<uint8_t>(oid & 0xFF);
  msg[3] = static_cast<uint8_t>((oid >> 8) & 0xFF);
  msg[4] = static_cast<uint8_t>((oid >> 16) & 0xFF);
  msg[5] = static_cast<uint8_t>((oid >> 24) & 0xFF);
  msg[6] = static_cast<uint8_t>(owner & 0xFF);
  msg[7] = static_cast<uint8_t>((owner >> 8) & 0xFF);
  msg[8] = static_cast<uint8_t>((owner >> 16) & 0xFF);
  msg[9] = static_cast<uint8_t>((owner >> 24) & 0xFF);
  msg[10] = 4;  // shortcut_rec
  msg[14] = 1;  // one row
  append_u32(msg, static_cast<uint32_t>(sc.index));
  auto put_str = [&](const std::string& s) {
    append_u32(msg, static_cast<uint32_t>(s.size() + 1));
    msg.insert(msg.end(), s.begin(), s.end());
    msg.push_back(0);
  };
  put_str(sc.kind);
  put_str(sc.id);
  return msg;
}

static std::vector<uint8_t> record_qinggong_add(uint32_t oid, uint32_t owner,
                                               const std::string& id) {
  std::vector<uint8_t> msg(16, 0);
  msg[0] = 0x11;
  msg[2] = static_cast<uint8_t>(oid & 0xFF);
  msg[6] = static_cast<uint8_t>(owner & 0xFF);
  msg[10] = 0;  // QingGongRec
  msg[14] = 1;
  append_u32(msg, static_cast<uint32_t>(id.size() + 1));
  msg.insert(msg.end(), id.begin(), id.end());
  msg.push_back(0);
  return msg;
}

// System announcement as simple 0x1E-style is complex; use log + optional chat-like
// For v1: push a CustomString if we know format — skip body, rely on GM later.



// --- View protocol (0x15 CreateView, 0x18 ViewAdd) ---
// Official SkillContainer View=40 property layout from 官服抓包 (zs_default_01).
constexpr uint16_t kViewportSkill = 40;
constexpr uint16_t kViewItemType = 1887;
constexpr uint16_t kViewConfigID = 1439;
constexpr uint16_t kViewCanUse = 2234;
constexpr uint16_t kViewStaticData = 2235;
constexpr uint16_t kViewUseLimit = 2236;
constexpr uint16_t kViewLevel = 1464;
constexpr uint16_t kViewVarPropNo = 2237;
constexpr uint16_t kViewMaxLevel = 2081;
constexpr uint16_t kViewQuality = 2117;
constexpr uint16_t kViewWuXing = 2241;
constexpr uint16_t kViewCoolDownTime = 2032;
constexpr uint16_t kViewHPHealMul = 2263;
constexpr uint16_t kViewMPHealMul = 2264;
constexpr uint16_t kViewPhyDamageMul = 517;
constexpr uint16_t kViewStiDamageMul = 518;
constexpr uint16_t kViewJujDamageMul = 519;
constexpr uint16_t kViewNegDamageMul = 520;
constexpr uint16_t kViewMasDamageMul = 521;
constexpr uint16_t kViewFlow = 2265;
constexpr uint16_t kViewLeadTime = 2268;
constexpr uint16_t kViewLeadSepTime = 2269;

struct StarterSkill {
  const char* config_id;
  int32_t static_data;
  int32_t item_type;
  int32_t max_level;
  int32_t cooldown_ms;
  int32_t var_prop_no;
};

static const StarterSkill kStarterSkills[] = {
  {"zs_default_01", 22, 1000, 1, 5000, 421},
  {"CS_jh_xfz01", 13189, 1001, 12, 1500, 0},
  {"CS_jh_xfz02", 13190, 1000, 12, 1500, 0},
  {"CS_jh_xfz03", 13205, 1000, 1, 1500, 0},
  {"CS_jh_xfz04", 13206, 1000, 1, 1500, 0},
  {"CS_yhwq_hsqs01", 12636, 1000, 11, 1500, 0},
  {"CS_yhwq_hsqs02", 12637, 1001, 11, 1500, 0},
  {"CS_yhwq_hsqs03", 12638, 1000, 11, 1500, 0},
};

static void append_u16v(std::vector<uint8_t>& m, uint16_t v) {
  m.push_back(static_cast<uint8_t>(v & 0xFF));
  m.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
static void append_u32v(std::vector<uint8_t>& m, uint32_t v) {
  m.push_back(static_cast<uint8_t>(v & 0xFF));
  m.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  m.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  m.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
static void append_f32v(std::vector<uint8_t>& m, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  append_u32v(m, bits);
}

static std::vector<uint8_t> encode_create_view(uint16_t view_id, uint16_t capacity) {
  std::vector<uint8_t> msg(7, 0);
  msg[0] = 0x15;
  msg[1] = static_cast<uint8_t>(view_id & 0xFF);
  msg[2] = static_cast<uint8_t>((view_id >> 8) & 0xFF);
  msg[3] = static_cast<uint8_t>(capacity & 0xFF);
  msg[4] = static_cast<uint8_t>((capacity >> 8) & 0xFF);
  return msg;
}

// Matches official 21-prop zs_default_01 ViewAdd (prop_count=0x14).
static std::vector<uint8_t> encode_skill_view_add(uint16_t index, const StarterSkill& sk) {
  std::vector<uint8_t> msg;
  msg.reserve(160);
  msg.push_back(0x18);
  append_u16v(msg, kViewportSkill);
  append_u16v(msg, index);
  append_u16v(msg, 21);  // property count

  // ItemType int
  append_u16v(msg, kViewItemType);
  append_u32v(msg, static_cast<uint32_t>(sk.item_type));
  // ConfigID string
  append_u16v(msg, kViewConfigID);
  std::string id = sk.config_id;
  append_u32v(msg, static_cast<uint32_t>(id.size() + 1));
  msg.insert(msg.end(), id.begin(), id.end());
  msg.push_back(0);
  // CanUse byte
  append_u16v(msg, kViewCanUse);
  msg.push_back(1);
  // StaticData int
  append_u16v(msg, kViewStaticData);
  append_u32v(msg, static_cast<uint32_t>(sk.static_data));
  // UseLimit int
  append_u16v(msg, kViewUseLimit);
  append_u32v(msg, 9);
  // Level byte
  append_u16v(msg, kViewLevel);
  msg.push_back(1);
  // VarPropNo int
  append_u16v(msg, kViewVarPropNo);
  append_u32v(msg, static_cast<uint32_t>(sk.var_prop_no));
  // MaxLevel int
  append_u16v(msg, kViewMaxLevel);
  append_u32v(msg, static_cast<uint32_t>(sk.max_level));
  // Quality byte
  append_u16v(msg, kViewQuality);
  msg.push_back(1);
  // WuXing int
  append_u16v(msg, kViewWuXing);
  append_u32v(msg, 5);
  // CoolDownTime int ms
  append_u16v(msg, kViewCoolDownTime);
  append_u32v(msg, static_cast<uint32_t>(sk.cooldown_ms));
  // Heal/Damage muls as float 1.0 (type 5 in official table)
  for (uint16_t idx : {kViewHPHealMul, kViewMPHealMul, kViewPhyDamageMul, kViewStiDamageMul,
                       kViewJujDamageMul, kViewNegDamageMul, kViewMasDamageMul}) {
    append_u16v(msg, idx);
    append_f32v(msg, 1.0f);
  }
  // Flow, LeadTime, LeadSepTime
  append_u16v(msg, kViewFlow);
  append_u32v(msg, 5);
  append_u16v(msg, kViewLeadTime);
  append_u32v(msg, 10000);
  append_u16v(msg, kViewLeadSepTime);
  append_u32v(msg, 1500);
  return msg;
}

static void cv_i32(std::vector<uint8_t>& m, int32_t v) {
  m.push_back(2);
  for (int i = 0; i < 4; ++i)
    m.push_back(static_cast<uint8_t>((static_cast<uint32_t>(v) >> (8 * i)) & 0xFF));
}
static void cv_f32(std::vector<uint8_t>& m, float v) {
  m.push_back(4);
  uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  for (int i = 0; i < 4; ++i)
    m.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
}
static void cv_str(std::vector<uint8_t>& m, const std::string& s) {
  m.push_back(6);
  uint32_t ln = static_cast<uint32_t>(s.size() + 1);
  for (int i = 0; i < 4; ++i)
    m.push_back(static_cast<uint8_t>((ln >> (8 * i)) & 0xFF));
  m.insert(m.end(), s.begin(), s.end());
  m.push_back(0);
}
static void cv_obj(std::vector<uint8_t>& m, uint32_t oid, uint32_t owner) {
  m.push_back(8);
  for (int i = 0; i < 4; ++i)
    m.push_back(static_cast<uint8_t>((oid >> (8 * i)) & 0xFF));
  for (int i = 0; i < 4; ++i)
    m.push_back(static_cast<uint8_t>((owner >> (8 * i)) & 0xFF));
}

static std::vector<uint8_t> encode_skill_action_switch() {
  std::vector<uint8_t> msg;
  msg.push_back(0x1E);
  msg.push_back(4);
  msg.push_back(0);
  cv_i32(msg, 414);
  cv_str(msg, "reset");
  cv_i32(msg, 1223);
  cv_i32(msg, 1);
  return msg;
}

static std::vector<uint8_t> encode_native_skill_action(
    uint32_t caster_oid, uint32_t caster_owner,
    const std::string& skill_id,
    uint32_t target_oid, uint32_t target_owner) {
  std::vector<uint8_t> msg;
  msg.push_back(0x1E);
  msg.push_back(6);
  msg.push_back(0);
  cv_i32(msg, 20);
  cv_obj(msg, caster_oid, caster_owner);
  cv_str(msg, skill_id);
  cv_obj(msg, target_oid, target_owner);
  cv_i32(msg, 0);
  cv_f32(msg, 0.0f);
  return msg;
}

static void grant_skill_container(OutFrames& out, PlayerRuntime& pr) {
  const size_t n = sizeof(kStarterSkills) / sizeof(kStarterSkills[0]);
  // Official create uses capacity 800
  out.push(encode_create_view(kViewportSkill, 800));
  pr.learned_skills.clear();
  for (size_t i = 0; i < n; ++i) {
    out.push(encode_skill_view_add(static_cast<uint16_t>(i + 1), kStarterSkills[i]));
    pr.learned_skills.push_back(kStarterSkills[i].config_id);
  }
  bool has_sit = false;
  for (const auto& sc : pr.shortcuts) {
    if (sc.index == 9 && sc.id == "zs_default_01") has_sit = true;
  }
  if (!has_sit) {
    ShortcutSlot sc{9, "skill", "zs_default_01"};
    pr.shortcuts.push_back(sc);
    out.push(record_shortcut_add(pr.object_id, pr.owner_id, sc));
  }
  NINEYIN_LOG_INFO("SkillContainer view=40 granted skills=%zu (official 21-prop layout)", n);
}

OutFrames bootstrap_player(PlayerRuntime& pr) {
  OutFrames out;
  std::lock_guard<std::mutex> lock(pr.mu);
  if (pr.shortcuts.empty()) {
    pr.shortcuts.push_back({9, "skill", "zs_default_01"});
  }
  if (pr.qinggong_ids.empty()) {
    for (const char* id : {"qinggong_1", "qinggong_2", "qinggong_3", "qinggong_4",
                           "qinggong_5", "qinggong_6", "qinggong_gaojie"}) {
      pr.qinggong_ids.push_back(id);
    }
  }
  if (pr.learned_skills.empty()) {
    // Starter combat skills seen in 官服抓包
    for (const char* id : {"CS_jh_bxjf01", "CS_jh_bxjf02", "CS_jh_bxjf03", "CS_jh_bxjf04",
                           "CS_jh_bxjf05", "zs_default_01"}) {
      pr.learned_skills.push_back(id);
    }
  }

  out.push(prop_int(pr.object_id, pr.owner_id, kPropOperateMode, pr.operate_mode));
  out.push(prop_byte(pr.object_id, pr.owner_id, kPropLogicState, pr.logic_state));
  out.push(prop_str(pr.object_id, pr.owner_id, kPropState, pr.action_state));
  out.push(prop_int(pr.object_id, pr.owner_id, kPropHP, pr.hp));
  out.push(prop_int(pr.object_id, pr.owner_id, kPropMaxHP, pr.max_hp));

  std::time_t now = std::time(nullptr);
  std::tm* t = std::localtime(&now);
  int32_t mins = t ? (t->tm_hour * 60 + t->tm_min) : 0;
  out.push(prop_int(pr.object_id, pr.owner_id, kPropTimeTick, mins));

  for (const auto& id : pr.qinggong_ids)
    out.push(record_qinggong_add(pr.object_id, pr.owner_id, id));
  for (const auto& sc : pr.shortcuts)
    out.push(record_shortcut_add(pr.object_id, pr.owner_id, sc));

  // Learned skills as SkillContainer View=40 (required for UI + 打坐/技能)
  grant_skill_container(out, pr);
  out.push(encode_skill_action_switch());
  NINEYIN_LOG_INFO("skill action switch 414/1223 enabled");

  // Time sync 0x38 immediately
  out.push(encode_time_sync_0x38(0, 0));

  NINEYIN_LOG_INFO("game bootstrap: skills=%zu qg=%zu shortcuts=%zu mode=%d + SkillContainer + 0x38",
                   pr.learned_skills.size(), pr.qinggong_ids.size(),
                   pr.shortcuts.size(), pr.operate_mode);
  return out;
}

static void handle_sit(PlayerRuntime& pr, int32_t cmd, OutFrames& out) {
  uint8_t next = pr.logic_state;
  if (cmd == 1) next = kLSSit;
  else if (cmd == 0 || cmd == 2) next = kLSNormal;
  if (next == pr.logic_state) return;
  pr.logic_state = next;
  pr.action_state = (next == kLSSit) ? "sitdown" : "stand";
  out.push(prop_byte(pr.object_id, pr.owner_id, kPropLogicState, pr.logic_state));
  out.push(prop_str(pr.object_id, pr.owner_id, kPropState, pr.action_state));
  NINEYIN_LOG_INFO("sitcross -> %u action=%s", static_cast<unsigned>(pr.logic_state),
                   pr.action_state.c_str());
}


static void handle_skill(PlayerRuntime& pr, const CustomMsg& msg, OutFrames& out) {
  // Port of Go handleSkillCustom + nativeSkillActionFrame (msg 20 on 0x1E)
  std::string skill_id;
  for (size_t i = 1; i < msg.vals.size(); ++i) {
    if ((msg.vals[i].type == 6 || msg.vals[i].type == 7) && !msg.vals[i].str.empty()) {
      skill_id = msg.vals[i].str;
      break;
    }
  }
  if (skill_id.empty()) {
    NINEYIN_LOG_INFO("use skill: empty id argc=%zu", msg.vals.size());
    return;
  }
  if (pr.logic_state == kLSSit) {
    pr.logic_state = kLSNormal;
    out.push(prop_byte(pr.object_id, pr.owner_id, kPropLogicState, kLSNormal));
  }
  pr.current_skill_id = skill_id;

  // Target: use selected combat target if any, else self (self-buff skills)
  uint32_t tid = pr.object_id;
  uint32_t tow = pr.owner_id;
  // selected target is applied by main.cpp after dispatch via HP stub;
  // for native action frame, prefer self if unknown — main can override by
  // also sending msg20. Here we use self as safe default for self skills.
  (void)tid; (void)tow;

  // 1) CurSkillID (official 459)
  out.push(prop_str(pr.object_id, pr.owner_id, 459, skill_id));
  // 2) Native skill action 0x1E msg=20 — this is what drives client animation
  //    Main will re-send with real target if selected; self-cast path here.
  out.push(encode_native_skill_action(pr.object_id, pr.owner_id, skill_id,
                                      pr.object_id, pr.owner_id));
  // 3) State property (official 183) — action name hints; map known IDs
  std::string action = "attack";
  if (skill_id.find("hsqs01") != std::string::npos) action = "hsqs_01";
  else if (skill_id.find("hsqs02") != std::string::npos) action = "hsqs_02";
  else if (skill_id.find("hsqs03") != std::string::npos) action = "hsqs_03";
  else if (skill_id.find("hsqs04") != std::string::npos) action = "hsqs_04";
  else if (skill_id.find("hsqs05") != std::string::npos) action = "hsqs_05";
  else if (skill_id.find("hsqs06") != std::string::npos) action = "hsqs_06";
  else if (skill_id.find("hsqs07") != std::string::npos) action = "hsqs_07";
  else if (skill_id.find("xfz") != std::string::npos) action = "attack";
  pr.action_state = action;
  out.push(prop_str(pr.object_id, pr.owner_id, kPropState, action));
  NINEYIN_LOG_INFO("use skill id=%s action=%s (native msg20+State)", skill_id.c_str(),
                   action.c_str());
}

static void handle_shortcut_set(PlayerRuntime& pr, const CustomMsg& msg, OutFrames& out) {
  if (msg.vals.size() < 4) return;
  int32_t idx = msg.vals[1].type == 2 ? msg.vals[1].i32 : 0;
  std::string kind = msg.vals[2].str.empty() ? "skill" : msg.vals[2].str;
  std::string id = msg.vals[3].str;
  // Replace by visual index: delete record ROW (position), then append
  for (size_t i = 0; i < pr.shortcuts.size(); ++i) {
    if (pr.shortcuts[i].index == idx) {
      out.push(record_shortcut_del(pr.object_id, pr.owner_id, static_cast<uint16_t>(i)));
      pr.shortcuts.erase(pr.shortcuts.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
  }
  ShortcutSlot sc{idx, kind, id};
  pr.shortcuts.push_back(sc);
  out.push(record_shortcut_add(pr.object_id, pr.owner_id, sc));
  NINEYIN_LOG_INFO("shortcut set idx=%d %s:%s total=%zu", idx, kind.c_str(), id.c_str(),
                   pr.shortcuts.size());
}

static void handle_shop(int32_t mid, const CustomMsg& msg, OutFrames& out) {
  (void)out;
  std::string shop;
  int32_t count = 1;
  for (const auto& v : msg.vals) {
    if (v.type == 6 || v.type == 7) shop = v.str;
    if (v.type == 2 && v.i32 > 0 && v.i32 < 100) count = v.i32;
  }
  NINEYIN_LOG_INFO("shop msg=%d shop=%s count=%d (v1: ack log only, bag grant TBD)",
                   mid, shop.c_str(), count);
}

std::vector<uint8_t> make_native_skill_action(
    uint32_t caster_oid, uint32_t caster_owner,
    const std::string& skill_id,
    uint32_t target_oid, uint32_t target_owner) {
  return encode_native_skill_action(caster_oid, caster_owner, skill_id,
                                    target_oid, target_owner);
}
std::vector<uint8_t> make_skill_action_switch() {
  return encode_skill_action_switch();
}

bool dispatch_custom(PlayerRuntime& pr, const CustomMsg& msg, OutFrames& out,
                     const std::string& peer_tag) {
  if (!msg.ok || msg.vals.empty() || msg.vals[0].type != 2) return false;
  const int32_t mid = msg.vals[0].i32;
  std::lock_guard<std::mutex> lock(pr.mu);

  switch (mid) {
    case kMsgSitCross:
      if (msg.vals.size() >= 2 && msg.vals[1].type == 2)
        handle_sit(pr, msg.vals[1].i32, out);
      return true;
    case kMsgUseSkill:
      handle_skill(pr, msg, out);
      return true;
    case kMsgSetShortcut:
      handle_shortcut_set(pr, msg, out);
      return true;
    case kMsgRemoveShortcut:
      if (msg.vals.size() >= 2 && msg.vals[1].type == 2) {
        int row = msg.vals[1].i32;
        if (row >= 0 && row < static_cast<int>(pr.shortcuts.size())) {
          out.push(record_shortcut_del(pr.object_id, pr.owner_id, static_cast<uint16_t>(row)));
          pr.shortcuts.erase(pr.shortcuts.begin() + row);
          NINEYIN_LOG_INFO("%s: shortcut remove row=%d remain=%zu", peer_tag.c_str(), row,
                           pr.shortcuts.size());
        }
      }
      return true;
    case kMsgShopBuyZyb:
    case kMsgShopWeapon:
    case kMsgShopWeaponConfirm:
      handle_shop(mid, msg, out);
      return true;
    case kMsgNeiGong:
      NINEYIN_LOG_INFO("%s: neigong/faculty msg 215 argc=%zu (v1 log)", peer_tag.c_str(),
                       msg.vals.size());
      return true;
    case kMsgFaculty:
      NINEYIN_LOG_INFO("%s: faculty msg 1006 argc=%zu (jingmai/v1 log)", peer_tag.c_str(),
                       msg.vals.size());
      return true;
    case kMsgItemOp:
      NINEYIN_LOG_INFO("%s: item op 517 argc=%zu (bag/v1 log)", peer_tag.c_str(),
                       msg.vals.size());
      return true;
    case 195:  // keybind / customizing map (client pushes full binding string)
      if (msg.vals.size() >= 3 && (msg.vals[2].type == 6 || msg.vals[2].type == 7)) {
        pr.customizing_hex = msg.vals[2].str;  // store raw binding text
        NINEYIN_LOG_INFO("%s: keybind map saved len=%zu", peer_tag.c_str(),
                         pr.customizing_hex.size());
      }
      return true;
    case 8:
      // Observed: (8, 21, 301, 1) / (8, 21, 302, 2) — UI/category setting
      NINEYIN_LOG_INFO("%s: settings msg=8 argc=%zu", peer_tag.c_str(), msg.vals.size());
      return true;
    case 421:
    case 423:
      // Possibly operate-mode related probes
      if (msg.vals.size() >= 2 && msg.vals[1].type == 2) {
        NINEYIN_LOG_INFO("%s: mode-like msg=%d val=%d", peer_tag.c_str(), mid, msg.vals[1].i32);
      }
      return true;
    default:
      if (mid != 15 && mid != 298 && mid != 924 && mid != 9 && mid != 431 && mid != 197 &&
          mid != 54) {
        NINEYIN_LOG_INFO("%s: unhandled custom msg=%d argc=%zu", peer_tag.c_str(), mid,
                         msg.vals.size());
      }
      return false;
  }
}

const char* system_status_table() {
  return
      "=== system port status ===\n"
      "[v1] sit240 LogicState=102 | time 0x38 | shortcut 200/201 | SkillContainer view40\n"
      "[v1] skill211 state-only | keybind msg195 saved\n"
      "[log] shop 64/70/71 bag517 neigong215 jingmai1006\n"
      "[todo] damage float | chat 0x1A | full UI unlock | operate-mode packet\n";
}

}  // namespace nineyin::game
