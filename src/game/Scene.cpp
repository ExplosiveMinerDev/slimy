#include "game/Scene.h"
#include "game/MapIO.h"
#include "game/Slime.h"
#include "physics/Body.h"
#include "physics/World.h"

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
    appendSolidMapEntries(world, entries);
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
