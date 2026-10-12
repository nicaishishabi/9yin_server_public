#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nineyin::scene {

struct Waypoint {
  float x = 0, y = 0, z = 0;
  float orient = 0;
  int wait_ms = 200;  // stand time at point (mappath field 6)
};

// Parse res/map/path/<scene>_main.path
// - Simple format: strict 24 bytes/waypoint (x,y,z as float32 LE + pad/orient)
// - Variable format: scan for plausible float runs + header count check
bool load_path_file(const std::string& path, std::vector<Waypoint>& out);

// Split into segments of `per_segment` waypoints (default 12).
std::vector<std::vector<Waypoint>> segment_path(const std::vector<Waypoint>& pts, size_t per_segment = 12);

}  // namespace nineyin::scene
