#include "game/Scene.h"
#include "game/MapIO.h"
#include "game/Slime.h"
#include "physics/Body.h"
#include "physics/World.h"
#include "physics/SoftBody.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <string>
#include <vector>

namespace pe {

void appendSolidMapEntries(World& world, const std::vector<SolidMapEntry>& entries);

namespace {

constexpr Vec2 kSpawnPoints[net::kMaxPlayers] = {
    {-42.8f, 10.05f}, {-41.4f, 10.05f}, {-40.1f, 10.05f}, {-42.5f, 9.45f},
    {-41.0f, 9.45f}, {-39.7f, 9.45f}, {-43.0f, 8.85f}, {-39.5f, 8.85f},
};

/// True if a static AABB box already exists (same centre / half extents / tag) — avoids
/// duplicate solids when maps/default.sjmap repeats geometry already in buildSceneCore
/// (phantom platforms: drawn twice or client/server mismatch vs cwd).
bool nearlySameBoxStatic(const World& world, const SolidMapEntry& e) {
    /// Tolérance plus large : évite doublons carte/code (.sjmap vs Scene) à cause du flottant.
    constexpr float epsC = 0.12f;
    constexpr float epsH = 0.08f;
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
        if (tag == Slime::airVentTag) return 0.35f;
        return 0.92f;
    };
    for (const auto& e : entries) {
        Body* b = world.createBody(Shape::box(e.halfW, e.halfH), e.pos, BodyType::Static);
        b->tag = e.tag;
        b->friction = frictionFor(e.tag);
        if (e.tag == Slime::airVentTag) b->restitution = 0.02f;
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
    auto addAirVent = [&](Vec2 pos, float halfW = 0.82f) {
        Body* b = addStatic(Shape::box(halfW, 0.12f), pos, Slime::airVentTag, 0.35f);
        b->restitution = 0.02f;
        return b;
    };

    // === Bounds ===
    addStatic(Shape::box(0.55f, 28.f), {-44.f, -2.f}, Slime::stoneTag);
    addStatic(Shape::box(0.55f, 28.f), { 44.f, -2.f}, Slime::stoneTag);

    // === Main ground — flat grass strip (walkable top + dirt body drawn as one solid) ===
    addStatic(Shape::box(40.f, 0.6f), {0.f, 10.5f}, Slime::grassTag);
    /// Bedrock under dirt/grass: same width, stacked directly below the grass box (see cy/hh math).
    constexpr float kGrassHalfH = 0.6f;
    constexpr float kStoneUnderHalfH = 2.8f;
    const float kGrassBottomY = 10.5f + kGrassHalfH;
    const float kStoneUnderCy = kGrassBottomY + kStoneUnderHalfH;
    addStatic(Shape::box(40.f, kStoneUnderHalfH), {0.f, kStoneUnderCy}, Slime::stoneTag);

    // === Left edge: floor continuation + small starter room (door + 2 pressure plates) ===
    addStatic(Shape::box(3.f, 0.6f), {-43.f, 10.5f}, Slime::grassTag);
    addStatic(Shape::box(0.38f, 2.9f), {-45.78f, 11.35f}, Slime::stoneTag);
    addStatic(Shape::box(5.4f, 0.22f), {-41.4f, 12.85f}, Slime::stoneTag);
    addStatic(Shape::box(0.42f, 2.45f), {-38.48f, 11.15f}, Slime::stoneTag);
    addStatic(Shape::box(0.42f, 2.45f), {-37.05f, 11.15f}, Slime::stoneTag);
    addStatic(Shape::box(1.55f, 0.22f), {-37.75f, 12.48f}, Slime::stoneTag);

    auto mkKinematic = [](Body* b) {
        b->kinematic = true;
        b->invMass = 0.f;
        b->invInertia = 0.f;
    };
    // Door + plates must be Dynamic so snapshots replicate positions online.
    {
        Body* door =
            addDynamic(Shape::box(0.14f, 1.22f), {-37.68f, 10.92f}, Slime::doorRoomDoorTag, 800.f);
        door->friction = 0.88f;
        door->restitution = 0.02f;
        mkKinematic(door);
        Body* outer =
            addDynamic(Shape::box(0.52f, 0.07f), {-36.62f, 9.97f}, Slime::pressurePlateDoorOuterTag,
                       120.f);
        outer->friction = 0.95f;
        mkKinematic(outer);
        Body* inner =
            addDynamic(Shape::box(0.52f, 0.07f), {-41.05f, 9.97f}, Slime::pressurePlateDoorHoldTag,
                       120.f);
        inner->friction = 0.95f;
        mkKinematic(inner);
    }

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

    /// Surface « marchable » du sol principal (grass box cy=10.5, halfH=0.6).
    constexpr float kGrassTopY = 10.5f - 0.6f;

    // === Gentle movement props ===
    addAirVent({-4.7f, kGrassTopY - 0.08f}, 0.82f);
    addAirVent({16.6f, 6.28f}, 0.68f);

    // === Light playground props ===
    for (int i = 0; i < 3; ++i) {
        Body* c = addDynamic(Shape::box(0.42f, 0.42f),
                             {-30.f + (float)i * 1.0f, kGrassTopY + 0.42f},
                             Slime::crateTag, 0.6f);
        c->friction = 0.55f;
        c->restitution = 0.08f;
    }
    {
        Body* ball = addDynamic(Shape::circle(0.55f), {30.f, kGrassTopY + 0.55f}, Slime::ballTag, 1.2f);
        ball->friction = 0.4f;
        ball->restitution = 0.45f;
    }
    {
        Body* ball = addDynamic(Shape::circle(0.4f), {6.f, kGrassTopY + 0.4f}, Slime::ballTag, 1.0f);
        ball->friction = 0.4f;
        ball->restitution = 0.45f;
    }

    // === More dynamic props (grab / roll / stack) ===
    constexpr float kStackHalf = 0.36f;
    constexpr float kStackGap = 0.74f; // ≥ 2×half + marge pour éviter spawn coincé
    {
        const float yBottom = kGrassTopY + kStackHalf;
        for (int stack = 0; stack < 3; ++stack) {
            const float y = yBottom - (float)stack * kStackGap;
            Body* c = addDynamic(Shape::box(kStackHalf, kStackHalf),
                                 {-14.f + (float)stack * 0.015f, y}, Slime::crateTag, 0.55f);
            c->friction = 0.52f;
            c->restitution = 0.1f;
        }
    }
    // Plateformes « médianes » (-10,5.5) et (10,5.5) : surface haute y = 5.25 caisse half 0.16 → cy ≈ 5.41
    for (float x : {-8.f, 8.f}) {
        Body* c = addDynamic(Shape::box(0.32f, 0.32f), {x + 0.12f, 5.41f}, Slime::crateTag, 0.45f);
        c->friction = 0.5f;
        c->restitution = 0.06f;
    }
    // Balles sur plateforme centrale (0,4.5) surface y = 4.2 — x dans [-2.6,2.6]
    {
        Body* b = addDynamic(Shape::circle(0.28f), {-2.f, 4.2f + 0.28f}, Slime::ballTag, 0.55f);
        b->friction = 0.35f;
        b->restitution = 0.52f;
    }
    {
        Body* b = addDynamic(Shape::circle(0.32f), {2.f, 4.2f + 0.32f}, Slime::ballTag, 0.62f);
        b->friction = 0.38f;
        b->restitution = 0.48f;
    }
    // Pont étroit (0,3) surface y = 2.8
    {
        Body* b = addDynamic(Shape::circle(0.22f), {-0.8f, 2.8f + 0.22f}, Slime::ballTag, 0.5f);
        b->friction = 0.32f;
        b->restitution = 0.58f;
    }

    Body* wedge = addDynamic(Shape::box(0.34f, 0.52f), {18.f, kGrassTopY + 0.52f}, Slime::crateTag,
                             0.65f);
    wedge->rot = -0.22f;
    wedge->friction = 0.48f;

    Body* tumble =
        addDynamic(Shape::box(0.3f, 0.62f), {24.f, kGrassTopY + 0.62f}, Slime::crateTag, 0.58f);
    tumble->rot = 0.35f;
    tumble->friction = 0.5f;

    {
        Body* b = addDynamic(Shape::circle(0.26f), {-35.f, kGrassTopY + 0.26f}, Slime::ballTag, 0.55f);
        b->friction = 0.4f;
        b->restitution = 0.5f;
    }
    {
        Body* b = addDynamic(Shape::circle(0.3f), {32.f, kGrassTopY + 0.3f}, Slime::ballTag, 0.75f);
        b->friction = 0.42f;
        b->restitution = 0.42f;
    }

    // === Map update test (see maps/default.sjmap): caillou rouge sur le sol ===
    addStatic(Shape::box(0.5f, 0.45f), {4.f, 10.35f}, Slime::mapTestRockTag);
}

void buildScene(World& world) {
    buildSceneCore(world);
    tryAppendDefaultSolidMap(world);
}

namespace {

AABB plateSensorZone(const Body& plate) {
    AABB z = plate.aabb();
    z.min.x -= 0.18f;
    z.max.x += 0.18f;
    z.min.y -= 0.08f;
    z.max.y += 1.05f;
    return z;
}

bool anyPlayerBlobInZone(World& world, const AABB& zone) {
    for (const auto& sb : world.softBodies()) {
        if (!isPlayerSlimeSoftBodyTag(sb->tag)) continue;
        for (const auto& pt : sb->points) {
            if (zone.contains(pt.pos)) return true;
        }
    }
    return false;
}

} // namespace

void updateDoorRoom(World& world, float dt) {
    Body* door = nullptr;
    Body* outer = nullptr;
    Body* inner = nullptr;
    for (auto& bp : world.bodies()) {
        Body& B = *bp;
        if (B.tag == Slime::doorRoomDoorTag) door = &B;
        else if (B.tag == Slime::pressurePlateDoorOuterTag) outer = &B;
        else if (B.tag == Slime::pressurePlateDoorHoldTag) inner = &B;
    }
    if (!door || !outer || !inner) return;

    static bool inited = false;
    static float yDoorClosed, yOuter0, yInner0;
    if (!inited) {
        yDoorClosed = door->pos.y;
        yOuter0 = outer->pos.y;
        yInner0 = inner->pos.y;
        inited = true;
    }

    constexpr float kDrop = 0.11f;
    constexpr float kDoorLift = 2.15f;
    const float yDoorOpen = yDoorClosed - kDoorLift;

    const bool onOuter = anyPlayerBlobInZone(world, plateSensorZone(*outer));
    const bool onInner = anyPlayerBlobInZone(world, plateSensorZone(*inner));
    const bool doorOpen = onOuter || onInner;

    const float outerTarget = yOuter0 - (onOuter ? kDrop : 0.f);
    const float innerTarget = yInner0 - (onInner ? kDrop : 0.f);
    const float doorTarget = doorOpen ? yDoorOpen : yDoorClosed;

    const float kPlate = std::min(1.f, dt * 18.f);
    const float kDoor = std::min(1.f, dt * 12.f);
    outer->pos.y += (outerTarget - outer->pos.y) * kPlate;
    inner->pos.y += (innerTarget - inner->pos.y) * kPlate;
    door->pos.y += (doorTarget - door->pos.y) * kDoor;

    outer->vel = {};
    inner->vel = {};
    door->vel = {};
    outer->angVel = 0.f;
    inner->angVel = 0.f;
    door->angVel = 0.f;
}

} // namespace pe
