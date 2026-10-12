#include "9yin/scene/path_file.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace nineyin::scene {
namespace {

bool finite_f(float v) { return std::isfinite(v); }

bool plausible(float x, float y, float z) {
  if (!finite_f(x) || !finite_f(y) || !finite_f(z)) return false;
  if (std::fabs(x) > 20000.f || std::fabs(z) > 20000.f) return false;
  if (std::fabs(y) > 5000.f) return false;
  // reject pure zeros
  if (std::fabs(x) + std::fabs(z) < 1.f) return false;
  return true;
}

}  // namespace

bool load_path_file(const std::string& path, std::vector<Waypoint>& out) {
  out.clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  const auto sz = static_cast<size_t>(f.tellg());
  f.seekg(0, std::ios::beg);
  if (sz < 12 || sz > 50 * 1024 * 1024) return false;
  std::vector<uint8_t> data(sz);
  f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));
  if (!f) return false;

  // Scan float stream for consecutive plausible (x,y,z); step ~8 floats between hits
  // (matches born03_main.path layout observed in production data).
  const size_t nfloat = data.size() / 4;
  std::vector<float> floats(nfloat);
  for (size_t i = 0; i < nfloat; ++i) {
    std::memcpy(&floats[i], data.data() + i * 4, 4);
  }

  for (size_t i = 0; i + 2 < nfloat;) {
    float x = floats[i], y = floats[i + 1], z = floats[i + 2];
    if (plausible(x, y, z)) {
      if (out.empty() ||
          (x - out.back().x) * (x - out.back().x) + (z - out.back().z) * (z - out.back().z) > 0.5f) {
        Waypoint w;
        w.x = x;
        w.y = y;
        w.z = z;
        w.orient = 0.f;
        out.push_back(w);
        i += 8;  // typical spacing between waypoints in main.path
        continue;
      }
    }
    ++i;
  }

  // Fallback: strict 24-byte records
  if (out.size() < 8 && data.size() % 24 == 0) {
    out.clear();
    const size_t n = data.size() / 24;
    for (size_t i = 0; i < n; ++i) {
      float vals[6];
      std::memcpy(vals, data.data() + i * 24, 24);
      if (!plausible(vals[0], vals[1], vals[2])) continue;
      Waypoint w{vals[0], vals[1], vals[2], 0.f};
      if (finite_f(vals[3]) && std::fabs(vals[3]) <= 20.f) w.orient = vals[3];
      out.push_back(w);
    }
  }
  return out.size() >= 2;
}

std::vector<std::vector<Waypoint>> segment_path(const std::vector<Waypoint>& pts, size_t per_segment) {
  std::vector<std::vector<Waypoint>> segs;
  if (pts.empty() || per_segment < 2) return segs;
  for (size_t i = 0; i + per_segment <= pts.size(); i += per_segment) {
    segs.emplace_back(pts.begin() + static_cast<std::ptrdiff_t>(i),
                      pts.begin() + static_cast<std::ptrdiff_t>(i + per_segment));
  }
  // remainder as last segment if long enough
  size_t rem = pts.size() % per_segment;
  if (rem >= 4) {
    segs.emplace_back(pts.end() - static_cast<std::ptrdiff_t>(rem), pts.end());
  }
  return segs;
}

}  // namespace nineyin::scene
