#include "physics/SoftBody.h"
#include "physics/Body.h"
#include "physics/Collision.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace pe {

namespace {
// Spring force is force-limited (avoids explosions in stiff configs); springs are NEVER
// auto-broken by strain — spike hazards no longer bulk-break springs each frame (see Slime::applySpikeHazard).
constexpr float kMaxSpringForce = 8000.f;
constexpr float kPenetrationCap = 0.5f;
constexpr float kMaxPressure = 1200.f;
} // namespace

SoftBody SoftBody::makeCircle(const Vec2& center, float radius, int segments,
                              float massTotal, float stiffness, float damping) {
    SoftBody sb;
    sb.points.reserve((size_t)segments);
    float perPointMass = massTotal / (float)segments;
    // Wind CCW so polygon area() is positive and pressure normal points outward.
    for (int i = 0; i < segments; ++i) {
        float a = -(float)i / (float)segments * 6.2831853f;
        Vec2 p{center.x + std::cos(a) * radius, center.y + std::sin(a) * radius};
        PointMass pm;
        pm.pos = p;
        pm.prev = p;
        pm.mass = perPointMass;
        pm.invMass = 1.f / perPointMass;
        sb.points.push_back(pm);
    }
    // Perimeter springs.
    for (int i = 0; i < segments; ++i) {
        Spring s;
        s.a = i;
        s.b = (i + 1) % segments;
        s.restLength = (sb.points[(size_t)s.a].pos - sb.points[(size_t)s.b].pos).len();
        s.stiffness = stiffness;
        s.damping = damping;
        s.isBrace = false;
        sb.springs.push_back(s);
    }
    // Sparse internal bracing — only diametric chords. Just enough to prevent the
    // perimeter from collapsing radially; lets the blob squash + jiggle visibly.
    auto addBrace = [&](int a, int b, float k, float d) {
        if (a == b) return;
        Spring s;
        s.a = a;
        s.b = b;
        s.restLength = (sb.points[(size_t)a].pos - sb.points[(size_t)b].pos).len();
        s.stiffness = k;
        s.damping = d;
        s.isBrace = true;
        sb.springs.push_back(s);
    };
    int half = segments / 2;
    for (int i = 0; i < half; ++i) {
        // Soft diametric chord — keeps the blob from collapsing along any axis but
        // still allows visible jiggle / squash. Slow damping → oscillation lingers.
        addBrace(i, (i + half) % segments, stiffness * 0.15f, damping * 0.35f);
    }
    // Sparse 1-step neighbour chord — controls perimeter kinking without making the
    // outline rigid (lets squash bulge outward smoothly).
    for (int i = 0; i < segments; ++i) {
        addBrace(i, (i + 2) % segments, stiffness * 0.30f, damping * 0.55f);
    }

    // Pressure target balances against gravity (≈ 22 in main.cpp): blob keeps a
    // mostly-round shape but visibly squashes on hard impact.
    float baseArea = 3.14159265f * radius * radius;
    sb.pressureTarget = baseArea * 24.f;
    return sb;
}

Vec2 SoftBody::centroid() const {
    Vec2 c{0, 0};
    float m = 0.f;
    for (auto& p : points) { c += p.pos * p.mass; m += p.mass; }
    return m > 1e-8f ? c / m : Vec2{0, 0};
}

Vec2 SoftBody::averageVelocity() const {
    Vec2 v{0, 0};
    float m = 0.f;
    for (auto& p : points) { v += p.vel * p.mass; m += p.mass; }
    return m > 1e-8f ? v / m : Vec2{0, 0};
}

float SoftBody::area() const {
    float a = 0.f;
    int n = (int)points.size();
    for (int i = 0; i < n; ++i) {
        const Vec2& p0 = points[(size_t)i].pos;
        const Vec2& p1 = points[(size_t)((i + 1) % n)].pos;
        a += cross(p0, p1);
    }
    return a * 0.5f;
}

float SoftBody::convexHullArea() const {
    const int m = (int)points.size();
    if (m < 3) return 1e-4f;
    std::vector<Vec2> pts((size_t)m);
    for (int i = 0; i < m; ++i) pts[(size_t)i] = points[(size_t)i].pos;

    std::vector<int> ord((size_t)m);
    std::iota(ord.begin(), ord.end(), 0);
    std::sort(ord.begin(), ord.end(), [&](int a, int b) {
        return pts[(size_t)a].x < pts[(size_t)b].x ||
               (pts[(size_t)a].x == pts[(size_t)b].x && pts[(size_t)a].y < pts[(size_t)b].y);
    });

    auto orient = [&](int o, int a, int b) {
        return cross(pts[(size_t)a] - pts[(size_t)o], pts[(size_t)b] - pts[(size_t)o]);
    };

    std::vector<int> lower;
    for (int idx : ord) {
        while ((int)lower.size() >= 2 &&
               orient(lower[(size_t)lower.size() - 2], lower.back(), idx) <= 0)
            lower.pop_back();
        lower.push_back(idx);
    }
    std::vector<int> upper;
    for (int i = m - 1; i >= 0; --i) {
        int idx = ord[(size_t)i];
        while ((int)upper.size() >= 2 &&
               orient(upper[(size_t)upper.size() - 2], upper.back(), idx) <= 0)
            upper.pop_back();
        upper.push_back(idx);
    }
    if (!lower.empty()) lower.pop_back();
    if (!upper.empty()) upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());

    const int hn = (int)lower.size();
    if (hn < 3) return 1e-4f;
    float twice = 0.f;
    for (int i = 0; i < hn; ++i) {
        const Vec2& p0 = pts[(size_t)lower[(size_t)i]];
        const Vec2& p1 = pts[(size_t)lower[(size_t)((i + 1) % hn)]];
        twice += cross(p0, p1);
    }
    return std::max(1e-4f, std::abs(twice * 0.5f));
}

AABB SoftBody::aabb() const {
    AABB box;
    box.min = box.max = points[0].pos;
    for (auto& p : points) box.expand(p.pos);
    return box;
}

void SoftBody::applyForce(const Vec2& f) {
    Vec2 per = f * (1.f / (float)points.size());
    for (auto& p : points) p.force += per;
}

void SoftBody::applyForceAtCentroid(const Vec2& f) {
    float totalMass = 0.f;
    for (auto& p : points) totalMass += p.mass;
    if (totalMass <= 1e-8f) return;
    for (auto& p : points) p.force += f * (p.mass / totalMass);
}

void SoftBody::translate(const Vec2& d) {
    for (auto& p : points) { p.pos += d; p.prev += d; }
}

void SoftBody::clampNeedleLikeShape(float maxAspect, float minThin) {
    const int n = (int)points.size();
    if (n < 3) return;

    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    for (auto& p : points) {
        minX = std::min(minX, p.pos.x);
        maxX = std::max(maxX, p.pos.x);
        minY = std::min(minY, p.pos.y);
        maxY = std::max(maxY, p.pos.y);
    }
    float w = std::max(1e-4f, maxX - minX);
    float h = std::max(1e-4f, maxY - minY);

    Vec2 cc{0, 0};
    float M = 0.f;
    for (auto& p : points) {
        cc += p.pos * p.mass;
        M += p.mass;
    }
    if (M < 1e-8f) return;
    cc *= (1.f / M);

    bool changed = false;
    bool scaledX = false, scaledY = false;
    if (w < minThin) {
        float sx = minThin / w;
        for (auto& p : points) p.pos.x = cc.x + (p.pos.x - cc.x) * sx;
        changed = true;
        scaledX = true;
    }
    if (h < minThin) {
        float sy = minThin / h;
        for (auto& p : points) p.pos.y = cc.y + (p.pos.y - cc.y) * sy;
        changed = true;
        scaledY = true;
    }

    minX = 1e30f; maxX = -1e30f; minY = 1e30f; maxY = -1e30f;
    for (auto& p : points) {
        minX = std::min(minX, p.pos.x);
        maxX = std::max(maxX, p.pos.x);
        minY = std::min(minY, p.pos.y);
        maxY = std::max(maxY, p.pos.y);
    }
    w = std::max(1e-4f, maxX - minX);
    h = std::max(1e-4f, maxY - minY);
    cc = {0, 0};
    M = 0.f;
    for (auto& p : points) { cc += p.pos * p.mass; M += p.mass; }
    if (M > 1e-8f) cc *= (1.f / M);

    if (w > h * maxAspect) {
        float targetH = w / maxAspect * 0.97f;
        if (targetH > h) {
            float sy = targetH / h;
            for (auto& p : points) p.pos.y = cc.y + (p.pos.y - cc.y) * sy;
            changed = true;
            scaledY = true;
        }
    } else if (h > w * maxAspect) {
        float targetW = h / maxAspect * 0.97f;
        if (targetW > w) {
            float sx = targetW / w;
            for (auto& p : points) p.pos.x = cc.x + (p.pos.x - cc.x) * sx;
            changed = true;
            scaledX = true;
        }
    }

    if (changed) {
        // Kill velocity along any warped axis so the asymmetric ground
        // contact (one side pinned, the other free) can't turn this
        // positional fix into a per-frame upward impulse pump.
        for (auto& p : points) {
            if (scaledX) p.vel.x = 0.f;
            if (scaledY) p.vel.y = 0.f;
            p.prev = p.pos;
        }
    }
}

void SoftBody::accumulateForces(const Vec2& gravity, const std::vector<Body*>& rigids) {
    for (auto& p : points) {
        if (!p.pinned) p.force += gravity * p.mass;
    }

    // Springs: Hooke + relative-velocity damping. Force magnitude clamped (anti-blow-up).
    // Tension-only tearing: a stretched spring past `*TearRatio` breaks. Compression
    // is never enough to tear — squashing under gravity/impact stays intact.
    for (auto& s : springs) {
        if (s.broken) continue;
        Vec2 d = points[(size_t)s.b].pos - points[(size_t)s.a].pos;
        float dist = d.len();
        if (dist < 1e-8f) continue;
        Vec2 dir = d / dist;
        float ext = dist - s.restLength;
        float tearR = s.isBrace ? braceTearRatio : perimeterTearRatio;
        if (tearR > 0.f && s.restLength > 1e-4f && dist > s.restLength * tearR) {
            s.broken = true;
            continue;
        }
        float relV = dot(points[(size_t)s.b].vel - points[(size_t)s.a].vel, dir);
        float forceMag = s.stiffness * ext + s.damping * relV;
        forceMag = clamp(forceMag, -kMaxSpringForce, kMaxSpringForce);
        Vec2 f = dir * forceMag;
        if (!points[(size_t)s.a].pinned) points[(size_t)s.a].force += f;
        if (!points[(size_t)s.b].pinned) points[(size_t)s.b].force -= f;
    }

    // Pressure (gas model): each intact perimeter edge pushes outward with magnitude
    // p·L, split equally between its endpoints. Uses convex-hull area so a torn blob
    // doesn't get fictitious pressure spikes from a tangled signed-area.
    float A = convexHullArea();
    if (A < 1e-4f) A = 1e-4f;
    float pressure = pressureK * (pressureTarget / A);
    pressure = std::min(pressure, kMaxPressure);
    Vec2 c = centroid();
    for (const Spring& s : springs) {
        if (s.broken || s.isBrace) continue;
        const Vec2& p0 = points[(size_t)s.a].pos;
        const Vec2& p1 = points[(size_t)s.b].pos;
        Vec2 edge = p1 - p0;
        float edgeLen = edge.len();
        if (edgeLen < 1e-8f) continue;
        Vec2 outward = Vec2{edge.y, -edge.x} / edgeLen;
        Vec2 mid = (p0 + p1) * 0.5f;
        if (dot(outward, mid - c) < 0.f) outward = -outward;
        Vec2 f = outward * (pressure * edgeLen * 0.5f);
        if (!points[(size_t)s.a].pinned) points[(size_t)s.a].force += f;
        if (!points[(size_t)s.b].pinned) points[(size_t)s.b].force += f;
    }

    // Static-surface adhesion: pull perimeter particles toward the nearest boundary even when
    // they sit just outside the collision volume (pointVsBody inside-only would miss them).
    if (isPlayerSlimeSoftBodyTag(tag)) {
        constexpr float kReach = 0.34f;
        constexpr float kPull = 300.f;
        constexpr float kInsideBoost = 4.0f;
        constexpr float kTangentDamp = 62.f;
        constexpr float kFmaxPerMass = 480.f;
        constexpr int kSpikeHazardTag = 2;
        for (auto& p : points) {
            if (p.pinned) continue;
            for (Body* bp : rigids) {
                if (!bp || bp->type != BodyType::Static || bp->tag == kSpikeHazardTag) continue;
                Vec2 cl, dir;
                float sd = 0.f;
                if (!closestSurfacePointToBody(p.pos, *bp, cl, sd, dir)) continue;
                if (sd > kReach) continue;

                float w = (sd <= 0.f)
                              ? (1.f + std::min(-sd, 0.42f) * kInsideBoost)
                              : (1.f - sd / kReach);
                w = clamp(w, 0.f, 3.5f);

                // Surfaces whose closest-direction is mostly horizontal are walls /
                // platform sides. Pulling hard onto them and damping their (vertical)
                // tangent fights gravity and hangs the slime on a corner. Attenuate.
                const bool wallish = std::abs(dir.x) > 0.56f;
                const float pullScale = wallish ? 0.35f : 1.f;

                Vec2 f = dir * (kPull * w * p.mass * pullScale);
                float maxMag = kFmaxPerMass * p.mass;
                float L2 = f.lenSq();
                if (L2 > maxMag * maxMag)
                    f = f.normalized() * maxMag;
                p.force += f;

                if (!wallish) {
                    Vec2 tang{-dir.y, dir.x};
                    float vt = dot(p.vel, tang);
                    float damp = std::clamp(w, 0.f, 1.f);
                    p.force -= tang * (vt * kTangentDamp * p.mass * damp);
                }
            }
        }
    }
}

void SoftBody::integrate(float dt) {
    // Very light global drag — tames residual jitter without bleeding horizontal
    // momentum during airborne flight. Spring damping (per-spring) handles the
    // real oscillation killing.
    const float globalDamping = 0.0004f;
    const float maxSpeed = 50.f;
    // Air-drag style terminal velocity on +y only → free-fall plateaus, so the
    // slime never builds a back-breaking landing speed. Diagonal motion above
    // the fall line is untouched.
    const float maxFallVel = 11.f;
    for (auto& p : points) {
        if (p.pinned) { p.force = {0, 0}; continue; }
        Vec2 acc = p.force * p.invMass;
        p.vel += acc * dt;
        p.vel *= (1.f - globalDamping);
        if (p.vel.y > maxFallVel) {
            // Soft easing toward terminal — gentle, not a hard clip.
            p.vel.y = maxFallVel + (p.vel.y - maxFallVel) * 0.94f;
        }
        float v2 = p.vel.lenSq();
        if (v2 > maxSpeed * maxSpeed) p.vel = p.vel * (maxSpeed / std::sqrt(v2));
        p.prev = p.pos;
        p.pos += p.vel * dt;
        p.force = {0, 0};
    }
}

void SoftBody::resolveCollisions(std::vector<Body*>& rigids) {
    for (auto& p : points) {
        if (p.pinned) continue;
        for (Body* b : rigids) {
            Vec2 n, closest;
            float depth;
            // Use the entry-aware variant so a perimeter point that punched
            // through a thin platform gets pushed back out the entry face,
            // not ejected through the bottom.
            if (!pointVsBodyWithEntry(p.prev, p.pos, *b, n, depth, closest)) continue;

            float useDepth = std::min(depth, kPenetrationCap);

            float wA = p.invMass;
            float wB = b->invMass;
            float w = wA + wB;
            if (w < 1e-8f) continue;

            float bodyRest = b->restitution;
            if (b->slimeGlueTimer > 1e-4f) bodyRest = std::min(bodyRest, 0.022f);

            Vec2 corr = n * useDepth;
            float ratioA = wA / w;
            float ratioB = wB / w;
            p.pos += corr * ratioA;
            if (b->type == BodyType::Dynamic) b->pos -= corr * ratioB;

            Vec2 r = closest - b->pos;
            Vec2 vRigid = b->vel + cross(b->angVel, r);
            Vec2 vRel = p.vel - vRigid;
            float vn = dot(vRel, n);
            if (vn < 0.f) {
                // Bouncy on terrain (static), gentle on stuff that can move.
                float e = (b->type == BodyType::Static)
                              ? std::max(restitution, bodyRest)
                              : std::min(restitution, bodyRest);
                // Velocity gate: only restitute on real impacts, otherwise the
                // blob jitters / "flies" — slow contact = quiet rest.
                constexpr float kRestVelThreshold = 1.6f;
                if (-vn < kRestVelThreshold) e = 0.f;
                float jn = -(1.f + e) * vn;
                float rxn = cross(r, n);
                float kn = wA + wB + rxn * rxn * b->invInertia;
                if (kn < 1e-8f) continue;
                jn /= kn;
                Vec2 J = n * jn;
                p.vel += J * wA;
                if (b->type == BodyType::Dynamic) {
                    b->vel -= J * wB;
                    b->angVel -= rxn * jn * b->invInertia;
                }

                Vec2 t{-n.y, n.x};
                float vt = dot(p.vel - (b->vel + cross(b->angVel, r)), t);
                float rxt = cross(r, t);
                float kt = wA + wB + rxt * rxt * b->invInertia;
                if (kt > 1e-8f) {
                    float jt = -vt / kt;
                    float bodyMu = b->friction;
                    if (b->slimeGlueTimer > 1e-4f) bodyMu = std::max(bodyMu, 0.992f);
                    float mu = std::sqrt(friction * bodyMu);
                    float maxFric = std::abs(jn) * mu;
                    jt = clamp(jt, -maxFric, maxFric);
                    Vec2 Jt = t * jt;
                    p.vel += Jt * wA;
                    if (b->type == BodyType::Dynamic) {
                        b->vel -= Jt * wB;
                        b->angVel -= rxt * jt * b->invInertia;
                    }
                }
            }
            // Tangential stick on static terrain for player slime (extra frame passes).
            if (isPlayerSlimeSoftBodyTag(tag) && b->type == BodyType::Static && b->tag != 2 &&
                useDepth > 0.006f) {
                Vec2 t{-n.y, n.x};
                float vt = dot(p.vel - vRigid, t);
                float damp = (std::abs(n.y) > 0.55f) ? 0.38f : (std::abs(n.x) > 0.45f ? 0.62f : 0.28f);
                p.vel -= t * (vt * damp);
            } else if (b->type == BodyType::Static && b->tag != 2 && useDepth > 0.012f) {
                const float ax = std::abs(n.x);
                const float ay = std::abs(n.y);
                if (ax >= 0.46f && ax >= ay + 0.05f) {
                    Vec2 t{-n.y, n.x};
                    float vt = dot(p.vel - vRigid, t);
                    constexpr float kWallSlipDamp = 0.024f;
                    p.vel -= t * (vt * kWallSlipDamp);
                } else if (n.y <= -0.50f) {
                    Vec2 t{-n.y, n.x};
                    float vt = dot(p.vel - vRigid, t);
                    constexpr float kFloorSlipDamp = 0.016f;
                    p.vel -= t * (vt * kFloorSlipDamp);
                }
            }
        }
    }
}

void SoftBody::postStep() {
    // Self-clamp: pull any edge that has crossed the centroid back outside.
    // makeCircle winds CW (math), so the naive perpendicular `(edge.y,-edge.x)`
    // points inward. We compute the polygon's signed area once to know which
    // side is "outward" — then only fix edges actually inverted relative to it,
    // not every edge of a CW blob (that bug inflated the slime each substep).
    const int n = (int)points.size();
    if (n < 3) return;
    Vec2 c = centroid();
    float twice = 0.f;
    for (int i = 0; i < n; ++i) {
        twice += cross(points[(size_t)i].pos,
                       points[(size_t)((i + 1) % n)].pos);
    }
    const float orient = (twice >= 0.f) ? 1.f : -1.f; // +1 CCW, -1 CW
    for (auto& s : springs) {
        if (s.broken || s.isBrace) continue;
        Vec2 mid = (points[(size_t)s.a].pos + points[(size_t)s.b].pos) * 0.5f;
        Vec2 edge = points[(size_t)s.b].pos - points[(size_t)s.a].pos;
        Vec2 outward = Vec2{edge.y, -edge.x} * orient;
        if (dot(outward, mid - c) < 0.f) {
            Vec2 fix = (c - mid).normalized() * 0.02f;
            if (!points[(size_t)s.a].pinned) {
                points[(size_t)s.a].pos -= fix;
                points[(size_t)s.a].prev -= fix;
            }
            if (!points[(size_t)s.b].pinned) {
                points[(size_t)s.b].pos -= fix;
                points[(size_t)s.b].prev -= fix;
            }
        }
    }
}

} // namespace pe
