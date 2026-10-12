#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace nineyin::transport {

/// Immutable 4-byte repeating XOR key used by the modern FxClient framing.
/// Layout matches the little-endian uint32 observed on the wire.
class Key {
public:
  static constexpr uint32_t kInitialValue = 0xFA6D3BCC;

  constexpr Key() noexcept : bytes_{0, 0, 0, 0} {}

  explicit constexpr Key(uint32_t value) noexcept
      : bytes_{static_cast<uint8_t>(value & 0xFF),
               static_cast<uint8_t>((value >> 8) & 0xFF),
               static_cast<uint8_t>((value >> 16) & 0xFF),
               static_cast<uint8_t>((value >> 24) & 0xFF)} {}

  static constexpr Key Initial() noexcept { return Key(kInitialValue); }

  [[nodiscard]] constexpr uint32_t to_uint32() const noexcept {
    return static_cast<uint32_t>(bytes_[0]) |
           (static_cast<uint32_t>(bytes_[1]) << 8) |
           (static_cast<uint32_t>(bytes_[2]) << 16) |
           (static_cast<uint32_t>(bytes_[3]) << 24);
  }

  /// XOR every byte of src with the repeating 4-byte key. Symmetric.
  [[nodiscard]] std::vector<uint8_t> apply(std::span<const uint8_t> src) const;

  /// In-place version (useful when buffer ownership is already local).
  void apply_inplace(std::span<uint8_t> buf) const noexcept;

  [[nodiscard]] constexpr const std::array<uint8_t, 4>& bytes() const noexcept {
    return bytes_;
  }

  friend constexpr bool operator==(const Key& a, const Key& b) noexcept {
    return a.bytes_ == b.bytes_;
  }

private:
  std::array<uint8_t, 4> bytes_;
};

}  // namespace nineyin::transport
