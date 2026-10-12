#pragma once
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace nineyin::db {

struct StoredRole {
  std::string name;
  std::vector<std::string> appearance;
  std::string scene_config;
  std::string scene_resource;
  float x = 0, y = 0, z = 0, orient = 0;
};

// Unified store: MySQL if NINEYIN_MYSQL_DSN is set and client linked; else file.
class RoleStore {
 public:
  // path used for file backend; dsn from env NINEYIN_MYSQL_DSN
  void init(const std::string& file_path);
  bool get(const std::string& account_key, StoredRole& out);
  bool put(const std::string& account_key, const StoredRole& role);
  // Update only position (keeps appearance/scene resource)
  bool update_location(const std::string& account_key, float x, float y, float z, float orient);
  bool using_mysql() const { return use_mysql_; }
  std::string backend_name() const { return use_mysql_ ? "mysql" : "file"; }

 private:
  bool use_mysql_ = false;
  std::string file_path_;
  std::mutex mu_;
  std::map<std::string, StoredRole> file_map_;

  void load_file();
  void save_file();
  bool mysql_get(const std::string& key, StoredRole& out);
  bool mysql_put(const std::string& key, const StoredRole& role);
};

RoleStore& roles();

}  // namespace nineyin::db
