#pragma once
#include "math/Vec2.h"
#include <vector>
#include <cstdint>

namespace pe {

enum class ShapeType { Circle, Polygon };

struct Shape {
    ShapeType type = ShapeType::Circle;
    float radius = 1.f;
    std::vector<Vec2> vertices; // local space, CCW
    std::vector<Vec2> normals;  // edge normals (i -> i+1)

    static Shape circle(float r);
    static Shape box(float hw, float hh);
    static Shape polygon(std::vector<Vec2> verts); // CCW, convex
};

enum class BodyType { Static, Dynamic };

struct Body {
    uint32_t id = 0;
    BodyType type = BodyType::Dynamic;

    Vec2 pos{0, 0};
    float rot = 0.f;
    Vec2 vel{0, 0};
    float angVel = 0.f;

    Vec2 force{0, 0};
    float torque = 0.f;

    float mass = 1.f;
    float invMass = 1.f;
    float inertia = 1.f;
    float invInertia = 1.f;

    float restitution = 0.2f;
    float friction = 0.5f;
    float linearDamping = 0.01f;
    float angularDamping = 0.05f;

    Shape shape;

    // user data
    int tag = 0;
    /// Cleared each physics tick before `Slime::update`; while held, set to that slime's `myTag()`.
    int grabOwnerTag = 0;
    /// After a slime throw: brief low-bounce / high-friction contacts (seconds, decayed in `World::step`).
    float slimeGlueTimer = 0.f;

    void setMass(float m);
    void makeStatic();

    AABB aabb() const;
    Vec2 localToWorld(const Vec2& local) const;
    Vec2 worldToLocal(const Vec2& world) const;

    void applyForce(const Vec2& f) { force += f; }
    void applyForceAt(const Vec2& f, const Vec2& worldPoint) {
        force += f;
        torque += cross(worldPoint - pos, f);
    }
    void applyImpulse(const Vec2& imp) { vel += imp * invMass; }
    void applyImpulseAt(const Vec2& imp, const Vec2& worldPoint) {
        vel += imp * invMass;
        angVel += invInertia * cross(worldPoint - pos, imp);
    }
};

} // namespace pe
