#include "9yin/transport/connection.hpp"

namespace nineyin::transport {

Connection::Connection(ConnectionConfig config)
    : reader_(config.max_frame_size),
      read_key_(config.read_key),
      write_key_(config.write_key),
      max_frame_size_(config.max_frame_size) {}

bool Connection::feed(std::span<const uint8_t> data) {
  std::lock_guard lock(read_mu_);
  return reader_.feed(data);
}

std::vector<uint8_t> Connection::take_frame() {
  std::lock_guard lock(read_mu_);
  return reader_.take_frame(read_key_);
}

std::vector<uint8_t> Connection::encode_outgoing(std::span<const uint8_t> plain) {
  std::lock_guard lock(write_mu_);
  return encode_frame(plain, write_key_, max_frame_size_);
}

void Connection::set_read_key(Key key) {
  std::lock_guard lock(read_mu_);
  read_key_ = key;
}

void Connection::set_write_key(Key key) {
  std::lock_guard lock(write_mu_);
  write_key_ = key;
}

Key Connection::read_key() const {
  std::lock_guard lock(read_mu_);
  return read_key_;
}

Key Connection::write_key() const {
  std::lock_guard lock(write_mu_);
  return write_key_;
}

}  // namespace nineyin::transport
