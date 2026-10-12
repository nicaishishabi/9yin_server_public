#include "9yin/scene/path_file.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace nineyin::scene {
namespace {

bool finite_f(float v) {
  return std::isfinite(v);
}

bool plausible(float x, float y, float z) {
  if (!finite_f(x) || !finite_f(y) || !finite_f(z)) return false;
  if (std::fabs(x) > 20000.f || std::fabs(z) > 20000.f) return false;
  if (std::fabs(y) > 5000.f) return false;
  return true;
}

// Strict: entire file size % 24 == 0, each record 3x float + 3x float (or orient + pad)
bool try_simple_24(const std::vector<uint8_t>& data, std::vector<Waypoint>& out) {
  if (data.size() < 24 || (data.size() % 24) != 0) return false;
  const size_t n = data.size() / 24;
  out.clear();
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    float vals[6];
    std::memcpy(vals, data.data() + i * 24, 24);
    if (!plausible(vals[0], vals[1], vals[2])) return false;
    Waypoint w;
    w.x = vals[0];
    w.y = vals[1];
    w.z = vals[2];
    // bytes 12-23: often orient + padding or another xyz; use first float as orient if small
    if (finite_f(vals[3]) && std::fabs(vals[3]) <= 20.f)
      w.orient = vals[3];
    out.push_back(w);
  }
  // reject if all zeros
  int nonzero = 0;
  for (auto& w : out)
    if (std::fabs(w.x) + std::fabs(w.z) > 0.01f) nonzero++;
  return nonzero > 0;
}

// Variable: optional header uint32 count, then scan 12-byte xyz triplets
bool try_variable(const std::vector<uint8_t>& data, std::vector<Waypoint>& out) {
  out.clear();
  size_t off = 0;
  uint32_t header_count = 0;
  if (data.size() >= 4) {
    std::memcpy(&header_count, data.data(), 4);
    if (header_count > 0 && header_count < 100000 && data.size() >= 4 + header_count * 12)
      off = 4;
    else
      header_count = 0;
  }
  for (; off + 12 <= data.size(); off += 4) {
    float x, y, z;
    std::memcpy(&x, data.data() + off, 4);
    std::memcpy(&y, data.data() + off + 4, 4);
    std::memcpy(&z, data.data() + off + 8, 4);
    if (!plausible(x, y, z)) continue;
    // require some spacing from last point to avoid scanning noise
    if (!out.empty()) {
      float dx = x - out.back().x, dz = z - out.back().z;
      if (dx * dx + dz * dz < 0.01f) continue;
    }
    Waypoint w;
    w.x = x;
    w.y = y;
    w.z = z;
    out.push_back(w);
    off += 8;  // total +12 with loop +4 = wait, fix: advance 12
    // loop does +4, we need +12 total -> add 8 more via adjusting
  }
  // Fix scan properly
  out.clear();
  off = header_count ? 4 : 0;
  for (; off + 12 <= data.size();) {
    float x, y, z;
    std::memcpy(&x, data.data() + off, 4);
    std::memcpy(&y, data.data() + off + 4, 4);
    std::memcpy(&z, data.data() + off + 8, 4);
    if (plausible(x, y, z)) {
      if (out.empty() || (x - out.back().x) * (x - out.back().x) +
                                 (z - out.back().z) * (z - out.back().z) >
                             0.25f) {
        Waypoint w{x, y, z, 0};
        out.push_back(w);
      }
      off += 12;
      if (header_count && out.size() >= header_count) break;
    } else {
      off += 4;
    }
  }
  if (header_count && out.size() + 5 < header_count && out.size() < header_count / 2)
    return false;  // count mismatch too bad
  return out.size() >= 2;
}

}  // namespace

bool load_path_file(const std::string& path, std::vector<Waypoint>& out) {
  out.clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  auto sz = f.tellg();
  if (sz <= 0 || sz > 50 * 1024 * 1024) return false;
  f.seekg(0);
  std::vector<uint8_t> data(static_cast<size_t>(sz));
  f.read(reinterpret_cast<char*>(data.data()), sz);
  if (!f) return false;
  if (try_simple_24(data, out)) return true;
  return try_variable(data, out);
}

std::vector<std::vector<Waypoint>> segment_path(const std::vector<Waypoint>& pts, size_t per_segment) {
  std::vector<std::vector<Waypoint>> segs;
  if (pts.empty() || per_segment == 0) return segs;
  for (size_t i = 0; i < pts.size(); i += per_segment) {
    size_t end = std::min(i + per_segment, pts.size());
    if (end - i < 2) break;
    segs.emplace_back(pts.begin() + static_cast<std::ptrdiff_t>(i),
                      pts.begin() + static_cast<std::ptrdiff_t>(end));
  }
  return segs;
}

}  // namespace nineyin::scene
