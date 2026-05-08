#pragma once
#include "physics/Body.h"

namespace pe {

struct Contact {
    Vec2 point;       // world contact point
    Vec2 normal;      // points from A to B
    float penetration = 0.f;

    // accumulated impulses (for warm starting)
    float normalImpulse = 0.f;
    float tangentImpulse = 0.f;

    // mass terms (cached per step)
    float normalMass = 0.f;
    float tangentMass = 0.f;
    float velocityBias = 0.f;
};

struct Manifold {
    Body* a = nullptr;
    Body* b = nullptr;
    int count = 0;
    Contact contacts[2];
    Vec2 normal{0, 0};
    float restitution = 0.f;
    float friction = 0.f;
};

bool collide(Body& a, Body& b, Manifold& out);

// point-vs-shape (for soft body)
bool pointVsBody(const Vec2& p, Body& b, Vec2& outNormal, float& outDepth, Vec2& outClosest);

/// Point vs convex polygon: overlap uses SAT (inside hull). Correction is the Euclidean
/// translation to the closest point on the hull boundary — stable at corners and when a
/// soft-body point sits in a wedge between two faces. `prev` is unused for polygons (circles
/// still use `pointVsBody(pos)`).
bool pointVsBodyWithEntry(const Vec2& prev, const Vec2& pos, Body& b,
                          Vec2& outNormal, float& outDepth, Vec2& outClosest);

/// Closest point on `b`'s surface to `p` (circle or convex polygon). `outSignedDist` is
/// negative if `p` is inside the solid, positive if outside; magnitude = distance to boundary.
/// `outDirToSurface` is unit-ish direction from `p` toward the boundary (pull slime onto surface).
bool closestSurfacePointToBody(const Vec2& p, const Body& b, Vec2& outClosest, float& outSignedDist,
                               Vec2& outDirToSurface);

} // namespace pe
