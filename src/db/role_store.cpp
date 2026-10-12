#include "9yin/db/role_store.hpp"
#include "9yin/util/log.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <sstream>

#if defined(NINEYIN_HAS_MYSQL)
#include <mysql.h>
#endif

namespace nineyin::db {
namespace {

std::string esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '\\' || c == '|' || c == '\n') {
      o.push_back('\\');
      o.push_back(c == '\n' ? 'n' : static_cast<char>(c));
    } else {
      o.push_back(static_cast<char>(c));
    }
  }
  return o;
}

std::string unesc(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char n = s[++i];
      o.push_back(n == 'n' ? '\n' : n);
    } else {
      o.push_back(s[i]);
    }
  }
  return o;
}

std::string appearance_to_json(const std::vector<std::string>& app) {
  std::ostringstream os;
  os << '[';
  for (size_t i = 0; i < app.size(); ++i) {
    if (i) os << ',';
    os << '"';
    for (char c : app[i]) {
      if (c == '\\' || c == '"') os << '\\';
      if (c == '\n') os << "\\n";
      else os << c;
    }
    os << '"';
  }
  os << ']';
  return os.str();
}

// Minimal JSON string array parser: ["a","b"]
std::vector<std::string> appearance_from_json(const std::string& j) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < j.size() && j[i] != '"') ++i;
  while (i < j.size()) {
    if (j[i] != '"') {
      ++i;
      continue;
    }
    ++i;
    std::string s;
    while (i < j.size() && j[i] != '"') {
      if (j[i] == '\\' && i + 1 < j.size()) {
        ++i;
        s.push_back(j[i] == 'n' ? '\n' : j[i]);
      } else {
        s.push_back(j[i]);
      }
      ++i;
    }
    out.push_back(s);
    if (i < j.size() && j[i] == '"') ++i;
  }
  return out;
}

#if defined(NINEYIN_HAS_MYSQL)
struct ParsedDsn {
  std::string user, password, host, db;
  unsigned port = 3306;
};

// Go style: user:pass@tcp(host:port)/db
bool parse_go_dsn(const std::string& dsn, ParsedDsn& out) {
  auto at = dsn.find('@');
  if (at == std::string::npos) return false;
  auto colon = dsn.find(':');
  if (colon == std::string::npos || colon > at) return false;
  out.user = dsn.substr(0, colon);
  out.password = dsn.substr(colon + 1, at - colon - 1);
  auto tcp = dsn.find("tcp(", at);
  if (tcp == std::string::npos) return false;
  auto host_start = tcp + 4;
  auto host_end = dsn.find(')', host_start);
  if (host_end == std::string::npos) return false;
  auto hostport = dsn.substr(host_start, host_end - host_start);
  auto pc = hostport.find(':');
  if (pc == std::string::npos) {
    out.host = hostport;
  } else {
    out.host = hostport.substr(0, pc);
    out.port = static_cast<unsigned>(std::atoi(hostport.c_str() + pc + 1));
  }
  auto slash = dsn.find('/', host_end);
  if (slash == std::string::npos) return false;
  out.db = dsn.substr(slash + 1);
  auto q = out.db.find('?');
  if (q != std::string::npos) out.db = out.db.substr(0, q);
  return !out.user.empty() && !out.host.empty() && !out.db.empty();
}

MYSQL* mysql_connect_env() {
  const char* dsn_env = std::getenv("NINEYIN_MYSQL_DSN");
  if (!dsn_env || !dsn_env[0]) return nullptr;
  ParsedDsn d;
  if (!parse_go_dsn(dsn_env, d)) {
    NINEYIN_LOG_ERROR("invalid NINEYIN_MYSQL_DSN (want user:pass@tcp(host:port)/db)");
    return nullptr;
  }
  MYSQL* m = mysql_init(nullptr);
  if (!m) return nullptr;
  if (!mysql_real_connect(m, d.host.c_str(), d.user.c_str(), d.password.c_str(), d.db.c_str(),
                          d.port, nullptr, 0)) {
    NINEYIN_LOG_ERROR("mysql connect failed: %s", mysql_error(m));
    mysql_close(m);
    return nullptr;
  }
  mysql_set_character_set(m, "utf8mb4");
  return m;
}
#endif

}  // namespace

void RoleStore::init(const std::string& file_path) {
  std::lock_guard<std::mutex> lock(mu_);
  file_path_ = file_path;
  use_mysql_ = false;
  // ALWAYS load file backup first (survives restart even if MySQL put failed)
  load_file();
  NINEYIN_LOG_INFO("role file store: path=%s loaded=%zu accounts", file_path_.c_str(),
                   file_map_.size());
#if defined(NINEYIN_HAS_MYSQL)
  if (std::getenv("NINEYIN_MYSQL_DSN") && std::getenv("NINEYIN_MYSQL_DSN")[0]) {
    MYSQL* m = mysql_connect_env();
    if (m) {
      mysql_close(m);
      use_mysql_ = true;
      NINEYIN_LOG_INFO("role store backend = mysql (+ file backup %s)", file_path_.c_str());
      return;
    }
    NINEYIN_LOG_WARN("MySQL DSN set but connect failed; falling back to file only");
  }
#endif
  NINEYIN_LOG_INFO("role store backend = file only (%s)", file_path_.c_str());
}

bool RoleStore::get(const std::string& account_key, StoredRole& out) {
  std::lock_guard<std::mutex> lock(mu_);
  if (account_key.empty() || account_key == "acct:v1:unknown") {
    NINEYIN_LOG_WARN("role get: invalid account key");
    return false;
  }
  if (use_mysql_) {
    if (mysql_get(account_key, out)) return true;
    NINEYIN_LOG_INFO("mysql get miss key=%s, try file", account_key.substr(0, 24).c_str());
  }
  auto it = file_map_.find(account_key);
  if (it == file_map_.end()) return false;
  out = it->second;
  return true;
}

bool RoleStore::put(const std::string& account_key, const StoredRole& role) {
  std::lock_guard<std::mutex> lock(mu_);
  if (account_key.empty() || account_key == "acct:v1:unknown") {
    NINEYIN_LOG_ERROR("role put REJECTED: invalid account key (login identity missing)");
    return false;
  }
  // Always write file as safety net
  file_map_[account_key] = role;
  save_file();
  bool ok = true;
  if (use_mysql_) {
    ok = mysql_put(account_key, role);
    if (!ok) {
      NINEYIN_LOG_ERROR("mysql put FAILED key=%s name=%s (file backup kept)",
                        account_key.substr(0, 24).c_str(), role.name.c_str());
    } else {
      NINEYIN_LOG_INFO("mysql put OK key=%s name=%s", account_key.substr(0, 24).c_str(),
                       role.name.c_str());
    }
  } else {
    NINEYIN_LOG_INFO("file put OK key=%s name=%s", account_key.substr(0, 24).c_str(),
                     role.name.c_str());
  }
  return ok || true;  // file always saved
}

void RoleStore::save_file() {
  if (file_path_.empty()) {
    NINEYIN_LOG_ERROR("role save_file: path empty");
    return;
  }
  // ensure parent directory exists
  try {
    auto parent = std::filesystem::path(file_path_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
  } catch (...) {
  }
  std::ofstream f(file_path_, std::ios::binary | std::ios::trunc);
  if (!f) {
    NINEYIN_LOG_ERROR("role save_file FAILED to open: %s", file_path_.c_str());
    return;
  }
  for (const auto& kv : file_map_) {
    f << "KEY|" << esc(kv.first) << "\n";
    f << "NAME|" << esc(kv.second.name) << "\n";
    f << "APP";
    for (const auto& a : kv.second.appearance) f << "|" << esc(a);
    f << "\n";
    f << "SCENE|" << esc(kv.second.scene_config) << "|" << esc(kv.second.scene_resource) << "\n";
    f << "POS|" << kv.second.x << "|" << kv.second.y << "|" << kv.second.z << "|"
      << kv.second.orient << "\n";
    f << "END\n";
  }
  f.flush();
  if (!f) {
    NINEYIN_LOG_ERROR("role file write error: %s", file_path_.c_str());
  } else {
    NINEYIN_LOG_INFO("role file written: %s accounts=%zu", file_path_.c_str(), file_map_.size());
  }
}

void RoleStore::load_file() {
  file_map_.clear();
  std::ifstream f(file_path_, std::ios::binary);
  if (!f) return;
  StoredRole cur;
  std::string key, line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("KEY|", 0) == 0) {
      key = unesc(line.substr(4));
      cur = StoredRole{};
    } else if (line.rfind("NAME|", 0) == 0) {
      cur.name = unesc(line.substr(5));
    } else if (line.rfind("APP", 0) == 0) {
      cur.appearance.clear();
      size_t p = 3;
      if (p < line.size() && line[p] == '|') ++p;
      while (p <= line.size()) {
        size_t bar = line.find('|', p);
        std::string part = bar == std::string::npos ? line.substr(p) : line.substr(p, bar - p);
        cur.appearance.push_back(unesc(part));
        if (bar == std::string::npos) break;
        p = bar + 1;
      }
    } else if (line.rfind("SCENE|", 0) == 0) {
      auto rest = line.substr(6);
      auto bar = rest.find('|');
      if (bar != std::string::npos) {
        cur.scene_config = unesc(rest.substr(0, bar));
        cur.scene_resource = unesc(rest.substr(bar + 1));
      }
    } else if (line.rfind("POS|", 0) == 0) {
      std::sscanf(line.c_str() + 4, "%f|%f|%f|%f", &cur.x, &cur.y, &cur.z, &cur.orient);
    } else if (line == "END" && !key.empty()) {
      file_map_[key] = cur;
      key.clear();
    }
  }
}

#if defined(NINEYIN_HAS_MYSQL)
bool RoleStore::mysql_get(const std::string& account_key, StoredRole& out) {
  MYSQL* m = mysql_connect_env();
  if (!m) return false;
  char ek[512];
  unsigned long el = mysql_real_escape_string(m, ek, account_key.c_str(),
                                              static_cast<unsigned long>(account_key.size()));
  // account_key is VARBINARY (Go migration); compare via CONVERT for string keys
  std::string key_sql(ek, el);
  std::string q = "SELECT r.name, a.appearance, l.scene_config, l.scene_resource, "
                  "l.pos_x, l.pos_y, l.pos_z, l.orient "
                  "FROM accounts acc "
                  "JOIN roles r ON r.account_id = acc.account_id "
                  "JOIN role_appearances a ON a.role_id = r.role_id "
                  "JOIN role_locations l ON l.role_id = r.role_id "
                  "WHERE CONVERT(acc.account_key USING utf8mb4) = '" + key_sql + "' "
                  "OR acc.account_key = '" + key_sql + "' "
                  "ORDER BY r.slot ASC LIMIT 1";
  if (mysql_query(m, q.c_str()) != 0) {
    NINEYIN_LOG_ERROR("mysql get: %s", mysql_error(m));
    mysql_close(m);
    return false;
  }
  MYSQL_RES* res = mysql_store_result(m);
  if (!res) {
    mysql_close(m);
    return false;
  }
  MYSQL_ROW row = mysql_fetch_row(res);
  if (!row) {
    mysql_free_result(res);
    mysql_close(m);
    return false;
  }
  out.name = row[0] ? row[0] : "";
  out.appearance = appearance_from_json(row[1] ? row[1] : "[]");
  out.scene_config = row[2] ? row[2] : "";
  out.scene_resource = row[3] ? row[3] : "";
  out.x = row[4] ? std::strtof(row[4], nullptr) : 0;
  out.y = row[5] ? std::strtof(row[5], nullptr) : 0;
  out.z = row[6] ? std::strtof(row[6], nullptr) : 0;
  out.orient = row[7] ? std::strtof(row[7], nullptr) : 0;
  mysql_free_result(res);
  mysql_close(m);
  return true;
}

bool RoleStore::mysql_put(const std::string& account_key, const StoredRole& role) {
  MYSQL* m = mysql_connect_env();
  if (!m) return false;
  auto esc_s = [&](const std::string& s) {
    std::string buf(s.size() * 2 + 1, '\0');
    unsigned long n = mysql_real_escape_string(m, buf.data(), s.c_str(),
                                               static_cast<unsigned long>(s.size()));
    buf.resize(n);
    return buf;
  };
  std::string ek = esc_s(account_key);
  std::string en = esc_s(role.name);
  std::string eapp = esc_s(appearance_to_json(role.appearance));
  std::string ecfg = esc_s(role.scene_config);
  std::string eres = esc_s(role.scene_resource);

  if (mysql_query(m, "START TRANSACTION") != 0) {
    mysql_close(m);
    return false;
  }
  std::string q1 = "INSERT INTO accounts(account_key) VALUES ('" + ek +
                   "') ON DUPLICATE KEY UPDATE account_id=LAST_INSERT_ID(account_id), "
                   "updated_at=CURRENT_TIMESTAMP(6)";
  // ON DUPLICATE with LAST_INSERT_ID trick may not work on all versions —
  // use select after insert
  // Store key as binary-compatible string
  std::string qins = "INSERT INTO accounts(account_key) SELECT '" + ek +
                     "' FROM DUAL WHERE NOT EXISTS ("
                     "SELECT 1 FROM accounts WHERE CONVERT(account_key USING utf8mb4)='" + ek +
                     "' OR account_key='" + ek + "')";
  if (mysql_query(m, qins.c_str()) != 0) {
    NINEYIN_LOG_ERROR("mysql insert account: %s", mysql_error(m));
    mysql_query(m, "ROLLBACK");
    mysql_close(m);
    return false;
  }
  std::string qid = "SELECT account_id FROM accounts WHERE CONVERT(account_key USING utf8mb4)='" +
                   ek + "' OR account_key='" + ek + "'";
  if (mysql_query(m, qid.c_str()) != 0) {
    mysql_query(m, "ROLLBACK");
    mysql_close(m);
    return false;
  }
  MYSQL_RES* res = mysql_store_result(m);
  MYSQL_ROW row = res ? mysql_fetch_row(res) : nullptr;
  if (!row) {
    if (res) mysql_free_result(res);
    mysql_query(m, "ROLLBACK");
    mysql_close(m);
    return false;
  }
  unsigned long long account_id = std::strtoull(row[0], nullptr, 10);
  mysql_free_result(res);

  // upsert role slot 0
  std::string qrole =
      "INSERT INTO roles(account_id, slot, name) VALUES (" + std::to_string(account_id) +
      ",0,'" + en +
      "') ON DUPLICATE KEY UPDATE name=VALUES(name), role_id=LAST_INSERT_ID(role_id)";
  if (mysql_query(m, qrole.c_str()) != 0) {
    // name unique conflict or slot unique - try update by account_id
    std::string qup = "UPDATE roles SET name='" + en + "' WHERE account_id=" +
                      std::to_string(account_id) + " AND slot=0";
    if (mysql_query(m, qup.c_str()) != 0) {
      NINEYIN_LOG_ERROR("mysql upsert role: %s", mysql_error(m));
      mysql_query(m, "ROLLBACK");
      mysql_close(m);
      return false;
    }
  }
  std::string qrid = "SELECT role_id FROM roles WHERE account_id=" + std::to_string(account_id) +
                     " AND slot=0";
  if (mysql_query(m, qrid.c_str()) != 0) {
    mysql_query(m, "ROLLBACK");
    mysql_close(m);
    return false;
  }
  res = mysql_store_result(m);
  row = res ? mysql_fetch_row(res) : nullptr;
  if (!row) {
    if (res) mysql_free_result(res);
    mysql_query(m, "ROLLBACK");
    mysql_close(m);
    return false;
  }
  unsigned long long role_id = std::strtoull(row[0], nullptr, 10);
  mysql_free_result(res);

  std::string qapp =
      "INSERT INTO role_appearances(role_id, format_version, appearance) VALUES (" +
      std::to_string(role_id) + ",1,'" + eapp +
      "') ON DUPLICATE KEY UPDATE appearance=VALUES(appearance), version=version+1";
  std::string qloc =
      "INSERT INTO role_locations(role_id, scene_config, scene_resource, pos_x, pos_y, pos_z, "
      "orient) VALUES (" +
      std::to_string(role_id) + ",'" + ecfg + "','" + eres + "'," + std::to_string(role.x) + "," +
      std::to_string(role.y) + "," + std::to_string(role.z) + "," + std::to_string(role.orient) +
      ") ON DUPLICATE KEY UPDATE scene_config=VALUES(scene_config), "
      "scene_resource=VALUES(scene_resource), pos_x=VALUES(pos_x), pos_y=VALUES(pos_y), "
      "pos_z=VALUES(pos_z), orient=VALUES(orient), version=version+1";

  if (mysql_query(m, qapp.c_str()) != 0 || mysql_query(m, qloc.c_str()) != 0) {
    NINEYIN_LOG_ERROR("mysql appearance/location: %s", mysql_error(m));
    mysql_query(m, "ROLLBACK");
    mysql_close(m);
    return false;
  }
  mysql_query(m, "COMMIT");
  mysql_close(m);
  return true;
}
#endif


bool RoleStore::update_location(const std::string& account_key, float x, float y, float z, float orient) {
  std::lock_guard<std::mutex> lock(mu_);
  if (account_key.empty() || account_key == "acct:v1:unknown") return false;
  auto it = file_map_.find(account_key);
  if (it != file_map_.end()) {
    it->second.x = x;
    it->second.y = y;
    it->second.z = z;
    it->second.orient = orient;
    save_file();
  } else {
    // create stub so reconnect can still recover last pos if only mysql later
    StoredRole stub;
    stub.x = x; stub.y = y; stub.z = z; stub.orient = orient;
    file_map_[account_key] = stub;
    save_file();
  }
  if (!use_mysql_) return true;
#if defined(NINEYIN_HAS_MYSQL)
  MYSQL* m = mysql_connect_env();
  if (!m) return true;  // file already updated
  char ek[512];
  unsigned long el = mysql_real_escape_string(m, ek, account_key.c_str(),
                                              static_cast<unsigned long>(account_key.size()));
  std::string key_sql(ek, el);
  std::string q =
      "UPDATE role_locations l "
      "JOIN roles r ON r.role_id = l.role_id "
      "JOIN accounts a ON a.account_id = r.account_id "
      "SET l.pos_x=" + std::to_string(x) + ", l.pos_y=" + std::to_string(y) +
      ", l.pos_z=" + std::to_string(z) + ", l.orient=" + std::to_string(orient) +
      ", l.version=l.version+1 "
      "WHERE CONVERT(a.account_key USING utf8mb4)='" + key_sql +
      "' OR a.account_key='" + key_sql + "'";
  if (mysql_query(m, q.c_str()) != 0) {
    NINEYIN_LOG_ERROR("mysql update_location: %s", mysql_error(m));
    mysql_close(m);
    return true;  // file ok
  }
  mysql_close(m);
#endif
  return true;
}

RoleStore& roles() {
  static RoleStore s;
  return s;
}

}  // namespace nineyin::db
