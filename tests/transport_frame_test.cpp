#include "9yin/transport/frame.hpp"
#include "9yin/transport/key.hpp"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace nineyin::transport;

int main() {
  Key key = Key::Initial();

  // Basic round-trip
  {
    std::vector<uint8_t> plain = {0x01, 0x02, 0x03, 0xEE, 0x04};
    auto wire = encode_frame(plain, key);
    assert(wire.size() >= 2);
    assert(wire[wire.size() - 2] == 0xEE);
    assert(wire[wire.size() - 1] == 0xEE);

    FrameReader reader;
    bool done = reader.feed(wire);
    assert(done);
    auto decoded = reader.take_frame(key);
    assert(decoded == plain);
  }

  // Escape only
  {
    std::vector<uint8_t> plain = {0xEE};
    auto wire = encode_frame(plain, key);
    FrameReader reader;
    assert(reader.feed(wire));
    auto decoded = reader.take_frame(key);
    assert(decoded == plain);
  }

  // Streaming in tiny chunks
  {
    std::vector<uint8_t> plain = {0x22, 0xD5, 0x80, 0x18, 0xEE, 9, 10};
    auto wire = encode_frame(plain, key);
    FrameReader reader;
    bool done = false;
    for (size_t i = 0; i < wire.size(); ++i) {
      uint8_t byte = wire[i];
      done = reader.feed({&byte, 1});
    }
    if (!done || !reader.has_frame()) {
      std::printf("FAIL: streaming did not complete (done=%d has=%d)\n",
                  static_cast<int>(done), static_cast<int>(reader.has_frame()));
      return 1;
    }
    auto decoded = reader.take_frame(key);
    if (decoded != plain) {
      std::printf("FAIL: decoded mismatch\n");
      return 1;
    }
  }

  // Invalid escape
  {
    FrameReader reader;
    std::vector<uint8_t> bad = {0x01, 0xEE, 0x7F};
    bool threw = false;
    try {
      reader.feed(bad);
    } catch (const InvalidEscape&) {
      threw = true;
    }
    if (!threw) {
      std::printf("FAIL: expected InvalidEscape\n");
      return 1;
    }
  }

  std::printf("transport_frame_test: OK\n");
  return 0;
}
