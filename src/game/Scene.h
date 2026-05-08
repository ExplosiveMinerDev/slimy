#pragma once
#include "game/MapIO.h"
#include "math/Vec2.h"
#include "net/Protocol.h"

namespace pe {
class World;

/// Standard playground: bounds, ground, platforms, spikes, crates, balls (no `.sjmap` file).
void buildSceneCore(World& world);

/// Append static boxes from a list (map editor + fichier défaut).
void appendSolidMapEntries(World& world, const std::vector<SolidMapEntry>& entries);

/// Full scene: `buildSceneCore` puis lignes supplémentaires de `maps/default.sjmap`
/// (les boîtes déjà présentes dans le core sont ignorées pour éviter les doublons).
void buildScene(World& world);

/// Spawn position for a given player slot (0..net::kMaxPlayers-1). Out-of-range
/// inputs wrap into the table.
Vec2 spawnPosForSlot(int slot);

extern const Vec2 kSpawnPos;

} // namespace pe
