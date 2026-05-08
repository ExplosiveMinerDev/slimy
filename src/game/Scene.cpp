#include "game/Scene.h"
#include "game/MapIO.h"
#include "game/Slime.h"
#include "physics/Body.h"
#include "physics/World.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace pe {

void appendSolidMapEntries(World& world, const std::vector<SolidMapEntry>& entries);

namespace {

constexpr Vec2 kSpawnPoints[net::kMaxPlayers] = {
    {-12.f,  8.7f},
    { 12.f,  8.7f},
    {  0.f,  3.0f},
    {-22.f,  8.7f},
    { 22.f,  8.7f},
    {-30.f,  8.7f},
    { 30.f,  8.7f},
    {  0.f,  8.7f},
};

/// True if a static AABB box already exists (same centre / half extents / tag) — avoids
/// duplicate solids when maps/default.sjmap repeats geometry already in buildSceneCore
/// (phantom platforms: drawn twice or client/server mismatch vs cwd).
bool nearlySameBoxStatic(const World& world, const SolidMapEntry& e) {
    constexpr float epsC = 0.06f;
    constexpr float epsH = 0.04f;
    for (const auto& bp : world.bodies()) {
        const Body& b = *bp;
        if (b.type != BodyType::Static) continue;
        if (b.shape.type != ShapeType::Polygon) continue;
        if (b.shape.vertices.size() != 4) continue;
        if (std::abs(b.rot) > 1e-3f) continue;
        AABB box = b.aabb();
        const float cx = (box.min.x + box.max.x) * 0.5f;
        const float cy = (box.min.y + box.max.y) * 0.5f;
        const float hx = (box.max.x - box.min.x) * 0.5f;
        const float hy = (box.max.y - box.min.y) * 0.5f;
        if (std::abs(cx - e.pos.x) > epsC || std::abs(cy - e.pos.y) > epsC) continue;
        if (std::abs(hx - e.halfW) > epsH || std::abs(hy - e.halfH) > epsH) continue;
        if (b.tag != e.tag) continue;
        return true;
    }
    return false;
}

void tryAppendDefaultSolidMap(World& world) {
    const char* path = "maps/default.sjmap";
    std::ifstream probe(path);
    if (!probe.good()) return;
    probe.close();

    std::vector<SolidMapEntry> entries;
    std::string err;
    if (!parseSolidMapFile(path, entries, &err)) {
        std::fprintf(stderr, "[map] %s: %s\n", path, err.c_str());
        return;
    }
    std::vector<SolidMapEntry> filtered;
    filtered.reserve(entries.size());
    for (const auto& e : entries) {
        if (nearlySameBoxStatic(world, e)) continue;
        filtered.push_back(e);
    }
    if (filtered.empty()) return;
    appendSolidMapEntries(world, filtered);
}

} // namespace

const Vec2 kSpawnPos = kSpawnPoints[0];

Vec2 spawnPosForSlot(int slot) {
    if (slot < 0) slot = 0;
    if (slot >= net::kMaxPlayers) slot = slot % net::kMaxPlayers;
    return kSpawnPoints[slot];
}

void appendSolidMapEntries(World& world, const std::vector<SolidMapEntry>& entries) {
    auto frictionFor = [](int tag) -> float {
        if (tag == Slime::spikeHazardTag) return 0.4f;
        return 0.92f;
    };
    for (const auto& e : entries) {
        Body* b = world.createBody(Shape::box(e.halfW, e.halfH), e.pos, BodyType::Static);
        b->tag = e.tag;
        b->friction = frictionFor(e.tag);
    }
}

void buildSceneCore(World& world) {
    auto addStatic = [&](Shape sh, Vec2 pos, int tag, float friction = 0.92f) {
        Body* b = world.createBody(std::move(sh), pos, BodyType::Static);
        b->tag = tag;
        b->friction = friction;
        return b;
    };
    auto addDynamic = [&](Shape sh, Vec2 pos, int tag, float density = 1.f) {
        Body* b = world.createBody(std::move(sh), pos, BodyType::Dynamic, density);
        b->tag = tag;
        return b;
    };
    auto spikeTri = []() {
        std::vector<Vec2> v{{0.f, -0.42f}, {0.30f, 0.f}, {-0.30f, 0.f}};
        return Shape::polygon(v);
    };

    // === Bounds ===
    addStatic(Shape::box(0.55f, 28.f), {-44.f, -2.f}, Slime::stoneTag);
    addStatic(Shape::box(0.55f, 28.f), { 44.f, -2.f}, Slime::stoneTag);

    // === Main ground — flat grass strip ===
    addStatic(Shape::box(40.f, 0.6f), {0.f, 10.5f}, Slime::grassTag);

    // === Floating wooden platforms ===
    addStatic(Shape::box(2.6f, 0.30f), {-18.f, 6.6f}, Slime::platformTag);
    addStatic(Shape::box(2.6f, 0.30f), {  0.f, 4.5f}, Slime::platformTag);
    addStatic(Shape::box(2.6f, 0.30f), { 18.f, 6.6f}, Slime::platformTag);

    // === Platforms mirrored from maps/default.sjmap — inlined so client/server match even when
    //     maps/default.sjmap is missing next to the Windows exe (GCP cwd loads the file). ===
    addStatic(Shape::box(3.f, 0.25f), {-10.f, 5.5f}, Slime::platformTag);
    addStatic(Shape::box(3.f, 0.25f), { 10.f, 5.5f}, Slime::platformTag);
    addStatic(Shape::box(5.f, 0.2f), { 0.f, 3.f}, Slime::platformTag);

    // === Spike strip on the right side of the main ground ===
    for (float x = 22.f; x <= 28.f; x += 0.7f)
        addStatic(spikeTri(), {x, 9.78f}, Slime::spikeHazardTag, 0.4f);

    // === Light playground props ===
    for (int i = 0; i < 3; ++i) {
        Body* c = addDynamic(Shape::box(0.42f, 0.42f),
                             {-30.f + (float)i * 1.0f, 9.55f},
                             Slime::crateTag, 0.6f);
        c->friction = 0.55f;
        c->restitution = 0.08f;
    }
    {
        Body* ball = addDynamic(Shape::circle(0.55f), {30.f, 9.0f}, Slime::ballTag, 1.2f);
        ball->friction = 0.4f;
        ball->restitution = 0.45f;
    }
    {
        Body* ball = addDynamic(Shape::circle(0.4f), {6.f, 8.5f}, Slime::ballTag, 1.0f);
        ball->friction = 0.4f;
        ball->restitution = 0.45f;
    }
}

void buildScene(World& world) {
    buildSceneCore(world);
    tryAppendDefaultSolidMap(world);
}

} // namespace pe
