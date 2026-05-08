#include "physics/Body.h"
#include <limits>

namespace pe {

Shape Shape::circle(float r) {
    Shape s;
    s.type = ShapeType::Circle;
    s.radius = r;
    return s;
}

Shape Shape::box(float hw, float hh) {
    return polygon({
        {-hw, -hh}, { hw, -hh}, { hw,  hh}, {-hw,  hh}
    });
}

Shape Shape::polygon(std::vector<Vec2> verts) {
    Shape s;
    s.type = ShapeType::Polygon;
    s.vertices = std::move(verts);
    s.normals.clear();
    s.normals.reserve(s.vertices.size());
    for (size_t i = 0; i < s.vertices.size(); ++i) {
        Vec2 a = s.vertices[i];
        Vec2 b = s.vertices[(i + 1) % s.vertices.size()];
        Vec2 e = b - a;
        s.normals.push_back(Vec2{e.y, -e.x}.normalized()); // CCW outward
    }
    // bounding radius
    float r2 = 0.f;
    for (auto& v : s.vertices) r2 = std::max(r2, v.lenSq());
    s.radius = std::sqrt(r2);
    return s;
}

void Body::setMass(float m) {
    if (m <= 0.f) { makeStatic(); return; }
    mass = m;
    invMass = 1.f / m;
    // approximate inertia
    if (shape.type == ShapeType::Circle) {
        inertia = 0.5f * m * shape.radius * shape.radius;
    } else {
        // polygon inertia about centroid (assume centroid at origin)
        float num = 0.f, den = 0.f;
        for (size_t i = 0; i < shape.vertices.size(); ++i) {
            Vec2 a = shape.vertices[i];
            Vec2 b = shape.vertices[(i + 1) % shape.vertices.size()];
            float c = std::abs(cross(a, b));
            num += c * (dot(a, a) + dot(a, b) + dot(b, b));
            den += c;
        }
        inertia = den > 1e-8f ? (m * num) / (6.f * den) : m;
    }
    invInertia = 1.f / inertia;
}

void Body::makeStatic() {
    type = BodyType::Static;
    mass = 0.f; invMass = 0.f;
    inertia = 0.f; invInertia = 0.f;
    vel = {0, 0}; angVel = 0.f;
}

AABB Body::aabb() const {
    AABB box;
    box.min = box.max = pos;
    if (shape.type == ShapeType::Circle) {
        box.min = pos - Vec2{shape.radius, shape.radius};
        box.max = pos + Vec2{shape.radius, shape.radius};
    } else {
        Mat2 R(rot);
        box.min = box.max = pos + R.mul(shape.vertices[0]);
        for (size_t i = 1; i < shape.vertices.size(); ++i) {
            Vec2 w = pos + R.mul(shape.vertices[i]);
            box.expand(w);
        }
    }
    return box;
}

Vec2 Body::localToWorld(const Vec2& local) const {
    return pos + Mat2(rot).mul(local);
}

Vec2 Body::worldToLocal(const Vec2& world) const {
    return Mat2(rot).mulT(world - pos);
}

} // namespace pe
