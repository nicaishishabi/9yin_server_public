#include "9yin/transport/key.hpp"

namespace nineyin::transport {

std::vector<uint8_t> Key::apply(std::span<const uint8_t> src) const {
  std::vector<uint8_t> dst(src.size());
  for (size_t i = 0; i < src.size(); ++i) {
    dst[i] = static_cast<uint8_t>(src[i] ^ bytes_[i & 3]);
  }
  return dst;
}

void Key::apply_inplace(std::span<uint8_t> buf) const noexcept {
  for (size_t i = 0; i < buf.size(); ++i) {
    buf[i] = static_cast<uint8_t>(buf[i] ^ bytes_[i & 3]);
  }
}

}  // namespace nineyin::transport
