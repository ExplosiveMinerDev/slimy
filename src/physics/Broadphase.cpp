#include "physics/Broadphase.h"
#include "physics/Body.h"
#include <cmath>

namespace pe {

void Broadphase::clear() {
    cells_.clear();
    all_.clear();
}

void Broadphase::insert(Body* b) {
    all_.push_back(b);
    AABB box = b->aabb();
    int x0 = (int)std::floor(box.min.x / cellSize_);
    int y0 = (int)std::floor(box.min.y / cellSize_);
    int x1 = (int)std::floor(box.max.x / cellSize_);
    int y1 = (int)std::floor(box.max.y / cellSize_);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            cells_[key(x, y)].bodies.push_back(b);
}

void Broadphase::queryPairs(std::vector<std::pair<Body*, Body*>>& out) {
    out.clear();
    pairSeenScratch_.clear();
    for (auto& [k, cell] : cells_) {
        for (size_t i = 0; i < cell.bodies.size(); ++i) {
            for (size_t j = i + 1; j < cell.bodies.size(); ++j) {
                Body* a = cell.bodies[i];
                Body* b = cell.bodies[j];
                if (a->type == BodyType::Static && b->type == BodyType::Static) continue;
                Body* lo = a < b ? a : b;
                Body* hi = a < b ? b : a;
                uint64_t pk = (uint64_t)lo->id | ((uint64_t)hi->id << 32);
                if (!pairSeenScratch_.insert(pk).second) continue;
                if (!lo->aabb().overlaps(hi->aabb())) continue;
                out.emplace_back(lo, hi);
            }
        }
    }
}

} // namespace pe
