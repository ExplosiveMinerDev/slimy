#pragma once
#include "physics/Body.h"
#include "physics/SoftBody.h"
#include "physics/Collision.h"
#include "physics/Broadphase.h"
#include <memory>
#include <vector>

namespace pe {

class World {
public:
    Vec2 gravity{0.f, 30.f}; // y down (screen-style); change for game
    int velocityIterations = 28; // bumped: no warm-starting yet
    int positionIterations = 10;

    World();

    Body* createBody(Shape shape, Vec2 pos, BodyType type = BodyType::Dynamic, float density = 1.f);
    SoftBody* addSoftBody(SoftBody sb);
    void clear();

    void step(float dt);

    /// When a tagged blob is still connected but several springs are broken, split it into two hull bodies.
    void tryBinarySplitDamagedBlob(int tag);
    /// Fuse all soft bodies with `tag` into one convex blob (e.g. hold Enter to re-merge fragments).
    void mergeSoftBodiesWithTag(int tag, float pressureTargetHint);
    /// Remove every soft body using `tag` (e.g. player disconnected).
    void removeSoftBodiesWithTag(int tag);
    /// Cut the largest tagged blob in half along a line through its centroid. `axisDir`
    /// is the direction the cut runs; default {1,0} splits top-half / bottom-half.
    bool splitLargestBlobWithTag(int tag, Vec2 axisDir = {0.f, 1.f});

    const std::vector<std::unique_ptr<Body>>& bodies() const { return bodies_; }
    const std::vector<std::unique_ptr<SoftBody>>& softBodies() const { return softBodies_; }

private:
    std::vector<std::unique_ptr<Body>> bodies_;
    std::vector<std::unique_ptr<SoftBody>> softBodies_;
    Broadphase broadphase_{4.f};

    std::vector<Manifold> manifolds_;

    uint32_t nextId_ = 1;

    void processSoftBodyConnectivity();
    void removeSoftBodyAt(size_t index);

    void integrateVelocities(float dt);
    void integratePositions(float dt);
    void detectCollisions();
    void prepareContacts(float dt);
    void warmStart();
    void solveVelocities();
    void solvePositions();
};

} // namespace pe
