#include "9yin/scene/npc_template.hpp"
#include "9yin/util/log.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace nineyin::scene {
namespace {

std::unordered_map<std::string, NpcTemplateInfo> g_templates;
bool g_loaded = false;

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  return s.substr(i);
}

std::vector<std::string> split_tsv(const std::string& line) {
  std::vector<std::string> cols;
  std::string cur;
  bool in_q = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (c == '"') {
      in_q = !in_q;
      continue;
    }
    if (!in_q && c == '\t') {
      cols.push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  cols.push_back(cur);
  return cols;
}

void load_one_file(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return;
  std::string line;
  // skip comment / type lines until header with ID
  std::vector<std::string> header;
  int id_col = -1, res_col = -1, name_col = -1, level_col = -1, hp_col = -1;
  int rows = 0;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#' || line[0] == '/') continue;
    auto cols = split_tsv(line);
    if (cols.empty()) continue;
    // detect header
    if (id_col < 0) {
      for (size_t i = 0; i < cols.size(); ++i) {
        auto h = trim(cols[i]);
        // strip UTF-8 BOM
        if (h.size() >= 3 && (unsigned char)h[0] == 0xEF) h = h.substr(3);
        if (h == "ID" || h == "Id" || h == "id" || h == "ConfigID") id_col = static_cast<int>(i);
        else if (h == "Resource" || h == "resource" || h == "Res") res_col = static_cast<int>(i);
        else if (h == "Name" || h == "name") name_col = static_cast<int>(i);
        else if (h == "Level" || h == "level") level_col = static_cast<int>(i);
        else if (h == "MaxHP" || h == "maxhp" || h == "HP") hp_col = static_cast<int>(i);
      }
      if (id_col >= 0) {
        header = cols;
        continue;
      }
      continue;
    }
    if (id_col >= static_cast<int>(cols.size())) continue;
    std::string id = trim(cols[id_col]);
    if (!id.empty() && id.front() == '"' && id.back() == '"')
      id = id.substr(1, id.size() - 2);
    if (id.empty()) continue;
    NpcTemplateInfo info;
    info.config_id = id;
    info.found = true;
    if (res_col >= 0 && res_col < static_cast<int>(cols.size()))
      info.resource = trim(cols[res_col]);
    if (name_col >= 0 && name_col < static_cast<int>(cols.size()))
      info.name = trim(cols[name_col]);
    if (level_col >= 0 && level_col < static_cast<int>(cols.size())) {
      try { info.level = std::stoi(cols[level_col]); } catch (...) {}
    }
    if (hp_col >= 0 && hp_col < static_cast<int>(cols.size())) {
      try { info.max_hp = std::stoi(cols[hp_col]); } catch (...) {}
    }
    // first wins for ambiguous
    if (!g_templates.count(id)) {
      g_templates.emplace(id, std::move(info));
      rows++;
    }
  }
  if (rows > 0)
    NINEYIN_LOG_INFO("npc template file %s rows=%d", path.filename().string().c_str(), rows);
}

}  // namespace

void load_npc_templates(const std::string& npcconfig_root) {
  if (g_loaded) return;
  g_loaded = true;
  namespace fs = std::filesystem;
  if (!fs::is_directory(npcconfig_root)) {
    NINEYIN_LOG_WARN("npcconfig missing: %s", npcconfig_root.c_str());
    return;
  }
  try {
    for (auto& e : fs::recursive_directory_iterator(npcconfig_root)) {
      if (!e.is_regular_file()) continue;
      auto ext = e.path().extension().string();
      for (char& c : ext)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (ext != ".txt") continue;
      // skip pure category files at root named npc_type
      if (e.path().stem() == "npc_type") continue;
      load_one_file(e.path());
    }
  } catch (const std::exception& ex) {
    NINEYIN_LOG_WARN("npc template scan error: %s", ex.what());
  }
  NINEYIN_LOG_INFO("npc templates total=%zu from %s", g_templates.size(), npcconfig_root.c_str());
}

NpcTemplateInfo lookup_npc_template(const std::string& config_id) {
  auto it = g_templates.find(config_id);
  if (it != g_templates.end()) return it->second;
  // try without path prefix
  auto slash = config_id.find_last_of("\\/");
  if (slash != std::string::npos) {
    it = g_templates.find(config_id.substr(slash + 1));
    if (it != g_templates.end()) return it->second;
  }
  return {};
}

size_t npc_template_count() { return g_templates.size(); }

}  // namespace nineyin::scene
