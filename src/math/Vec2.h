#pragma once
#include <cmath>
#include <algorithm>

namespace pe {

struct Vec2 {
    float x = 0.f, y = 0.f;

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2 operator/(float s) const { if (std::abs(s) < 1e-12f) return {0,0}; return {x / s, y / s}; }
    Vec2 operator-() const { return {-x, -y}; }

    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
    Vec2& operator/=(float s) { if (std::abs(s) < 1e-12f) { x=0; y=0; return *this; } x /= s; y /= s; return *this; }

    float lenSq() const { return x * x + y * y; }
    float len() const { return std::sqrt(lenSq()); }

    Vec2 normalized() const {
        float l = len();
        return l > 1e-8f ? Vec2{x / l, y / l} : Vec2{0, 0};
    }

    Vec2 perp() const { return {-y, x}; } // 90° CCW
};

inline Vec2 operator*(float s, const Vec2& v) { return v * s; }

inline float dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline float cross(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }
inline Vec2 cross(float s, const Vec2& v) { return {-s * v.y, s * v.x}; }
inline Vec2 cross(const Vec2& v, float s) { return {s * v.y, -s * v.x}; }

inline float distance(const Vec2& a, const Vec2& b) { return (a - b).len(); }
inline float distanceSq(const Vec2& a, const Vec2& b) { return (a - b).lenSq(); }

struct Mat2 {
    float c = 1, s = 0; // cos, sin

    Mat2() = default;
    explicit Mat2(float angle) : c(std::cos(angle)), s(std::sin(angle)) {}

    Vec2 mul(const Vec2& v) const { return {c * v.x - s * v.y, s * v.x + c * v.y}; }
    Vec2 mulT(const Vec2& v) const { return {c * v.x + s * v.y, -s * v.x + c * v.y}; }
};

struct AABB {
    Vec2 min, max;
    bool overlaps(const AABB& o) const {
        return !(max.x < o.min.x || min.x > o.max.x ||
                 max.y < o.min.y || min.y > o.max.y);
    }
    void expand(const Vec2& p) {
        min.x = std::min(min.x, p.x); min.y = std::min(min.y, p.y);
        max.x = std::max(max.x, p.x); max.y = std::max(max.y, p.y);
    }
    Vec2 center() const { return (min + max) * 0.5f; }
    Vec2 size() const { return max - min; }
};

inline float clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

} // namespace pe
