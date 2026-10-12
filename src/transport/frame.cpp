#include "9yin/transport/frame.hpp"

#include <cstring>

namespace nineyin::transport {

std::vector<uint8_t> encode_frame(std::span<const uint8_t> plain, const Key& key,
                                  size_t limit) {
  if (limit == 0) {
    throw FrameError("invalid frame size limit");
  }
  if (plain.size() > limit) {
    throw FrameTooLarge(plain.size(), limit);
  }

  auto xored = key.apply(plain);
  std::vector<uint8_t> wire;
  wire.reserve(xored.size() + 2 + xored.size() / 8);  // rough escape overhead

  for (uint8_t b : xored) {
    wire.push_back(b);
    if (b == 0xEE) {
      wire.push_back(0x00);
    }
  }
  wire.push_back(0xEE);
  wire.push_back(0xEE);
  return wire;
}

std::vector<uint8_t> decode_body(std::span<const uint8_t> encoded_body,
                                 const Key& key) {
  // encoded_body must already be free of the EE EE terminator and unescaped.
  return key.apply(encoded_body);
}

FrameReader::FrameReader(size_t limit) : limit_(limit) {
  if (limit_ == 0) {
    throw FrameError("invalid frame size limit");
  }
  buffer_.reserve(256);
}

void FrameReader::reset() noexcept {
  buffer_.clear();
  complete_ = false;
  in_escape_ = false;
}

bool FrameReader::feed(std::span<const uint8_t> data) {
  if (complete_) {
    return true;
  }

  for (uint8_t b : data) {
    if (in_escape_) {
      in_escape_ = false;
      if (b == 0x00) {
        // EE 00 -> single EE body byte
        buffer_.push_back(0xEE);
      } else if (b == 0xEE) {
        // EE EE -> frame terminator
        complete_ = true;
        return true;
      } else {
        throw InvalidEscape(b);
      }
    } else if (b == 0xEE) {
      in_escape_ = true;
    } else {
      buffer_.push_back(b);
    }

    if (buffer_.size() > limit_) {
      throw FrameTooLarge(buffer_.size(), limit_);
    }
  }
  return complete_;
}

std::vector<uint8_t> FrameReader::take_frame(const Key& key) {
  if (!complete_) {
    throw FrameError("no complete frame available");
  }
  auto plain = key.apply(buffer_);
  reset();
  return plain;
}

}  // namespace nineyin::transport
