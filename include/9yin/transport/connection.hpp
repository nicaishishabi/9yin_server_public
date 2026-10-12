#pragma once

#include "9yin/transport/frame.hpp"
#include "9yin/transport/key.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nineyin::transport {

struct ConnectionConfig {
  size_t max_frame_size = kDefaultMaxFrameSize;
  Key read_key = Key::Initial();
  Key write_key = Key::Initial();
};

/// Owns framing state and directional keys for one TCP session.
/// Read and write keys are intentionally independent because the modern
/// protocol can rotate them at different points in the login state machine.
class Connection {
public:
  using SendFn = std::function<void(std::span<const uint8_t>)>;

  explicit Connection(ConnectionConfig config = {});

  /// Feed raw socket bytes. When a complete frame is available, returns true
  /// and the decoded plaintext can be obtained via take_frame().
  bool feed(std::span<const uint8_t> data);

  [[nodiscard]] std::vector<uint8_t> take_frame();

  /// Encode and return the wire bytes that should be written to the socket.
  [[nodiscard]] std::vector<uint8_t> encode_outgoing(std::span<const uint8_t> plain);

  void set_read_key(Key key);
  void set_write_key(Key key);

  [[nodiscard]] Key read_key() const;
  [[nodiscard]] Key write_key() const;

  [[nodiscard]] bool has_frame() const noexcept { return reader_.has_frame(); }

private:
  mutable std::mutex read_mu_;
  mutable std::mutex write_mu_;
  FrameReader reader_;
  Key read_key_;
  Key write_key_;
  size_t max_frame_size_;
};

}  // namespace nineyin::transport
