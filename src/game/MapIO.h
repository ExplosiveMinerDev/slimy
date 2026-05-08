#pragma once
#include "math/Vec2.h"
#include <string>
#include <vector>

namespace pe {

/// One axis-aligned static box (center + half extents), same convention as `Shape::box`.
struct SolidMapEntry {
    Vec2 pos{};
    float halfW = 0.5f;
    float halfH = 0.5f;
    int tag = 0;
};

/// Parse `.sjmap` text into entries (does not touch `World`).
bool parseSolidMapFile(const std::string& path, std::vector<SolidMapEntry>& out, std::string* errOut);

/// Write entries to `.sjmap` (UTF-8, LF; overwrites file).
bool writeSolidMapFile(const std::string& path, const std::vector<SolidMapEntry>& in, std::string* errOut);

} // namespace pe
