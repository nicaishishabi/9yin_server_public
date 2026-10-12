#pragma once

#include "9yin/transport/key.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace nineyin::transport {

constexpr size_t kDefaultMaxFrameSize = 1u << 20;  // 1 MiB

class FrameError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class FrameTooLarge : public FrameError {
public:
  explicit FrameTooLarge(size_t size, size_t limit)
      : FrameError("transport frame too large: size=" + std::to_string(size) +
                   " limit=" + std::to_string(limit)) {}
};

class InvalidEscape : public FrameError {
public:
  explicit InvalidEscape(uint8_t next)
      : FrameError(std::string("invalid EE escape: EE ") +
                   (next < 16 ? "0" : "") +
                   std::to_string(static_cast<unsigned>(next))) {}
};

/// Encode plaintext: XOR with key, escape 0xEE -> 0xEE 0x00, terminate with 0xEE 0xEE.
[[nodiscard]] std::vector<uint8_t> encode_frame(std::span<const uint8_t> plain,
                                                 const Key& key,
                                                 size_t limit = kDefaultMaxFrameSize);

/// Decode one complete encoded body (terminator already consumed).
/// Returns the XOR-decrypted plaintext.
[[nodiscard]] std::vector<uint8_t> decode_body(std::span<const uint8_t> encoded_body,
                                               const Key& key);

/// Streaming frame reader that owns a single buffer and never discards
/// prefetched bytes belonging to the next frame.
class FrameReader {
public:
  explicit FrameReader(size_t limit = kDefaultMaxFrameSize);

  /// Feed more bytes from the socket. Returns true when a complete frame is ready.
  /// When true, call take_frame() to obtain the plaintext.
  bool feed(std::span<const uint8_t> data);

  /// Returns the decoded plaintext of the completed frame and resets internal state.
  [[nodiscard]] std::vector<uint8_t> take_frame(const Key& key);

  [[nodiscard]] bool has_frame() const noexcept { return complete_; }
  [[nodiscard]] size_t buffered() const noexcept { return buffer_.size(); }

  void reset() noexcept;

private:
  size_t limit_;
  std::vector<uint8_t> buffer_;   // accumulated encoded body (no terminator)
  bool complete_ = false;
  bool in_escape_ = false;        // true after seeing a lone 0xEE
};

/// Helper that writes a complete encoded frame into a destination buffer.
class FrameWriter {
public:
  explicit FrameWriter(size_t limit = kDefaultMaxFrameSize) : limit_(limit) {}

  [[nodiscard]] std::vector<uint8_t> write_frame(std::span<const uint8_t> plain,
                                                 const Key& key) const {
    return encode_frame(plain, key, limit_);
  }

private:
  size_t limit_;
};

}  // namespace nineyin::transport
