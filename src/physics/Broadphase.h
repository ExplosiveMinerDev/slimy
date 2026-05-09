#pragma once
#include "math/Vec2.h"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pe {

struct Body;

// Spatial hash grid broadphase
class Broadphase {
public:
    explicit Broadphase(float cellSize = 4.f) : cellSize_(cellSize) {}

    void clear();
    void insert(Body* b);

    /// Fills `out` (cleared first); reuses storage to avoid per-frame allocations on long runs.
    void queryPairs(std::vector<std::pair<Body*, Body*>>& out);

private:
    float cellSize_;
    struct Cell { std::vector<Body*> bodies; };
    std::unordered_map<uint64_t, Cell> cells_;
    std::vector<Body*> all_;
    std::unordered_set<uint64_t> pairSeenScratch_;

    static uint64_t key(int x, int y) {
        return (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32);
    }
};

} // namespace pe
