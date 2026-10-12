#include "9yin/gm/hub.hpp"

#include <chrono>

namespace nineyin::gm {

Hub& Hub::instance() {
  static Hub hub;
  return hub;
}

void Hub::set_status(ServerStatus s) {
  std::lock_guard lock(mu_);
  status_ = std::move(s);
}

ServerStatus Hub::status() const {
  std::lock_guard lock(mu_);
  return status_;
}

void Hub::client_connected() {
  std::lock_guard lock(mu_);
  status_.connected_clients++;
}

void Hub::client_disconnected() {
  std::lock_guard lock(mu_);
  if (status_.connected_clients > 0) status_.connected_clients--;
}

void Hub::attach(uint64_t id, PlayerSnapshot snap) {
  std::lock_guard lock(mu_);
  snap.session_id = id;
  snap.updated_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  sessions_[id] = std::move(snap);
}

void Hub::detach(uint64_t id) {
  std::lock_guard lock(mu_);
  sessions_.erase(id);
  queues_.erase(id);
}

void Hub::update(uint64_t id, const PlayerSnapshot& snap) {
  std::lock_guard lock(mu_);
  auto it = sessions_.find(id);
  if (it == sessions_.end()) return;
  auto copy = snap;
  copy.session_id = id;
  copy.updated_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  it->second = std::move(copy);
}

std::vector<PlayerSnapshot> Hub::snapshots() const {
  std::lock_guard lock(mu_);
  std::vector<PlayerSnapshot> out;
  out.reserve(sessions_.size());
  for (const auto& [_, s] : sessions_) out.push_back(s);
  return out;
}

bool Hub::enqueue(Command cmd) {
  std::lock_guard lock(mu_);
  if (!sessions_.count(cmd.session_id)) return false;
  auto& q = queues_[cmd.session_id];
  if (q.size() >= 32) return false;
  q.push_back(std::move(cmd));
  return true;
}

bool Hub::try_pop(uint64_t session_id, Command& out) {
  std::lock_guard lock(mu_);
  auto it = queues_.find(session_id);
  if (it == queues_.end() || it->second.empty()) return false;
  out = std::move(it->second.front());
  it->second.erase(it->second.begin());
  return true;
}

}  // namespace nineyin::gm
