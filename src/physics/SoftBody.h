#pragma once
#include "math/Vec2.h"
#include <cstdint>
#include <vector>

namespace pe {

struct Body;
class World;

/// Player / networked slime tags use `Slime::playerTag` and `networkedPlayerBlobTag`.
inline bool isPlayerSlimeSoftBodyTag(int t) {
    if (t < 1) return false;
    return (t - 1) % 100 == 0;
}

struct PointMass {
    Vec2 pos;
    Vec2 prev;       // for Verlet
    Vec2 vel;
    Vec2 force;
    float mass = 1.f;
    float invMass = 1.f;
    bool pinned = false;
};

struct Spring {
    int a = 0, b = 0;
    float restLength = 0.f;
    float stiffness = 800.f;
    float damping = 20.f;
    bool broken = false;
    bool isBrace = false;
};

// Pressurized mass-spring soft body — gas model (PV = nRT)
// Suitable for slime, jelly, balloons.
class SoftBody {
public:
    std::vector<PointMass> points;
    std::vector<Spring> springs;     // perimeter (CCW)

    float pressureTarget = 8.f;      // nRT (units of energy)
    float pressureK = 1.f;           // pressure scaling
    float restitution = 0.1f;
    float friction = 0.58f;

    /// Tension-only tear thresholds (length / restLength). 0 disables tearing.
    /// Spring breaks if it's stretched (not compressed) past the ratio. Compression
    /// (squashing) never tears, so the blob keeps its shape under gravity / impact.
    float perimeterTearRatio = 0.f;
    float braceTearRatio = 0.f;

    int tag = 0;

    // Build a circular blob with N points
    static SoftBody makeCircle(const Vec2& center, float radius, int segments,
                               float massTotal, float stiffness = 1200.f, float damping = 25.f);

    Vec2 centroid() const;
    Vec2 averageVelocity() const;
    float area() const; // signed; should be positive for CCW
    /// Robust occupied area (convex hull); use for pressure when topology is damaged.
    float convexHullArea() const;
    AABB aabb() const;

    void applyForce(const Vec2& f);                      // distributed
    void applyForceAtCentroid(const Vec2& f);            // total force, push by mass
    void translate(const Vec2& d);

    // accumulate forces (gravity, springs, pressure, damping, optional static adhesion) into points
    void accumulateForces(const Vec2& gravity, const std::vector<Body*>& rigids);

    // integrate positions (semi-implicit Euler)
    void integrate(float dt);

    // collide each point against rigid bodies; resolve impulse
    void resolveCollisions(std::vector<Body*>& rigids);

    // self-clamp: prevent inversions by pulling crossing edges (light)
    void postStep();

    /// Widen very thin silhouettes (needle / drip) toward a max aspect ratio & min thickness.
    void clampNeedleLikeShape(float maxAspect = 3.5f, float minThin = 0.33f);
};

} // namespace pe
