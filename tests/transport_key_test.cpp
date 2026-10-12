#include "9yin/transport/key.hpp"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace nineyin::transport;

int main() {
  Key key = Key::Initial();
  assert(key.to_uint32() == 0xFA6D3BCC);

  // From the original Go key_test.go
  std::vector<uint8_t> input = {0x00, 0x00, 0x00, 0x00, 0x01};
  auto got = key.apply(input);
  assert(got.size() == 5);
  assert(got[0] == 0xCC);
  assert(got[1] == 0x3B);
  assert(got[2] == 0x6D);
  assert(got[3] == 0xFA);
  assert(got[4] == 0xCD);

  // Symmetric
  auto roundtrip = key.apply(got);
  assert(roundtrip == input);

  std::printf("transport_key_test: OK\n");
  return 0;
}
