#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nineyin::gm {

struct PlayerSnapshot {
  uint64_t session_id = 0;
  std::string remote;
  std::string name;
  std::string scene_config;
  std::string scene_resource;
  float x = 0, y = 0, z = 0, orient = 0;
  int32_t silver = 0;
  int32_t sp = 0;
  std::string faction;
  std::string last_action;
  int64_t updated_at_ms = 0;
};

struct ServerStatus {
  bool running = true;
  std::string listen_addr;
  std::string server_root;
  int connected_clients = 0;
  int64_t uptime_sec = 0;
  std::string phase = "Phase-1 (transport only)";
};

/// Thread-safe registry of online sessions + server status for the GM panel.
class Hub {
public:
  static Hub& instance();

  void set_status(ServerStatus s);
  ServerStatus status() const;

  void client_connected();
  void client_disconnected();

  void attach(uint64_t id, PlayerSnapshot snap);
  void detach(uint64_t id);
  void update(uint64_t id, const PlayerSnapshot& snap);
  std::vector<PlayerSnapshot> snapshots() const;

  // Command queue for future game-loop integration (Phase 2+)
  struct Command {
    uint64_t session_id = 0;
    std::string action;
    int32_t silver = 0;
    int32_t sp = 0;
    float value = 0;
    std::string destination;
    std::string preset;
    std::string position;
    std::string faction;
    std::string item_id;
    std::string shop_id;
    int32_t amount = 0;
  };

  /// Returns false if session not found or queue full.
  bool enqueue(Command cmd);
  /// Pop one command for a session (called by game loop later). Non-blocking.
  bool try_pop(uint64_t session_id, Command& out);

private:
  Hub() = default;
  mutable std::mutex mu_;
  ServerStatus status_;
  std::unordered_map<uint64_t, PlayerSnapshot> sessions_;
  std::unordered_map<uint64_t, std::vector<Command>> queues_;
};

}  // namespace nineyin::gm
