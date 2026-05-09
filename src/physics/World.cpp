#include "physics/World.h"
#include "net/Protocol.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <numeric>

namespace pe {

namespace {

thread_local std::vector<std::vector<int>> tlsConnComps;

/// Fill `out` with vertex indices per connected component (spring graph). Reuses TLS scratch;
/// avoids allocating fresh vectors every physics step (idle servers still ran this for each blob).
void connectedComponentsInto(const SoftBody& sb, std::vector<std::vector<int>>& out) {
    out.clear();
    const int n = (int)sb.points.size();
    if (n <= 0) return;

    thread_local std::vector<std::vector<int>> adjRows;
    thread_local std::vector<char> vis;
    thread_local std::vector<int> comp;
    thread_local std::deque<int> q;

    if ((int)adjRows.size() < n) adjRows.resize((size_t)n);
    for (int i = 0; i < n; ++i) adjRows[(size_t)i].clear();

    for (const Spring& sp : sb.springs) {
        if (sp.broken) continue;
        adjRows[(size_t)sp.a].push_back(sp.b);
        adjRows[(size_t)sp.b].push_back(sp.a);
    }

    vis.assign((size_t)n, 0);

    for (int i = 0; i < n; ++i) {
        if (vis[(size_t)i]) continue;
        comp.clear();
        q.clear();
        q.push_back(i);
        vis[(size_t)i] = 1;
        while (!q.empty()) {
            int u = q.front();
            q.pop_front();
            comp.push_back(u);
            for (int v : adjRows[(size_t)u]) {
                if (!vis[(size_t)v]) {
                    vis[(size_t)v] = 1;
                    q.push_back(v);
                }
            }
        }
        out.emplace_back();
        out.back().swap(comp);
    }
}

/// Indices into `comp` (local 0..m-1), monotone chain; excludes duplicate endpoints.
std::vector<int> convexHullLocalIndices(const std::vector<int>& comp, const SoftBody& orig) {
    const int m = (int)comp.size();
    if (m <= 2) {
        std::vector<int> id(m);
        std::iota(id.begin(), id.end(), 0);
        return id;
    }
    std::vector<Vec2> pts(m);
    for (int i = 0; i < m; ++i) pts[i] = orig.points[comp[i]].pos;

    std::vector<int> ord(m);
    std::iota(ord.begin(), ord.end(), 0);
    std::sort(ord.begin(), ord.end(), [&](int a, int b) {
        return pts[a].x < pts[b].x || (pts[a].x == pts[b].x && pts[a].y < pts[b].y);
    });

    auto orient = [&](int o, int a, int b) { return cross(pts[a] - pts[o], pts[b] - pts[o]); };

    std::vector<int> lower;
    for (int idx : ord) {
        while ((int)lower.size() >= 2 && orient(lower[lower.size() - 2], lower.back(), idx) <= 0)
            lower.pop_back();
        lower.push_back(idx);
    }
    std::vector<int> upper;
    for (int i = m - 1; i >= 0; --i) {
        int idx = ord[i];
        while ((int)upper.size() >= 2 && orient(upper[upper.size() - 2], upper.back(), idx) <= 0)
            upper.pop_back();
        upper.push_back(idx);
    }
    if (!lower.empty()) lower.pop_back();
    if (!upper.empty()) upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());
    return lower;
}

std::vector<int> hullGlobalOrder(const std::vector<int>& comp, const SoftBody& orig) {
    std::vector<int> local = convexHullLocalIndices(comp, orig);
    std::vector<int> hull;
    hull.reserve(local.size());
    for (int li : local) hull.push_back(comp[li]);

    const int hn = (int)hull.size();
    if (hn >= 3) {
        float twice = 0.f;
        for (int i = 0; i < hn; ++i) {
            const Vec2& p0 = orig.points[hull[i]].pos;
            const Vec2& p1 = orig.points[hull[(i + 1) % hn]].pos;
            twice += cross(p0, p1);
        }
        if (twice < 0.f) std::reverse(hull.begin(), hull.end());
    }
    return hull;
}

/// After graph splits, hull vertices can be nearly collinear on the floor → tiny area,
/// huge pressure, pancake blob. Re-expand around COM before rebuilding springs.
static void inflateReconstructedBlob(SoftBody& nb) {
    const float kMinRadial = 0.38f;       // min max distance COM → vertex
    const float kMinHalfExtent = 0.255f;    // min half-width / half-height of hull AABB
    const float kMaxScale = 5.5f;

    const int hn = (int)nb.points.size();
    if (hn < 3) return;

    Vec2 c{0, 0};
    float M = 0.f;
    for (auto& p : nb.points) {
        c += p.pos * p.mass;
        M += p.mass;
    }
    if (M < 1e-8f) return;
    c *= (1.f / M);

    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    float maxR = 0.f;
    for (auto& p : nb.points) {
        minX = std::min(minX, p.pos.x);
        maxX = std::max(maxX, p.pos.x);
        minY = std::min(minY, p.pos.y);
        maxY = std::max(maxY, p.pos.y);
        maxR = std::max(maxR, distance(p.pos, c));
    }

    const float hx = 0.5f * std::max(1e-5f, maxX - minX);
    const float hy = 0.5f * std::max(1e-5f, maxY - minY);
    const float minHalf = std::min(hx, hy);

    float s = 1.f;
    if (maxR < kMinRadial) s = std::max(s, kMinRadial / maxR);
    if (minHalf < kMinHalfExtent) s = std::max(s, kMinHalfExtent / minHalf);
    s = std::min(s, kMaxScale);

    if (s > 1.001f) {
        for (auto& p : nb.points) p.pos = c + (p.pos - c) * s;
    }

    nb.clampNeedleLikeShape(1.92f, 0.38f);

    for (auto& p : nb.points) p.prev = p.pos;
}

static SoftBody buildMergedRoundedSoftBody(std::vector<PointMass> src, float stiff, float damp, float pk,
                                           float pressureTargetHint, float restitution, float friction,
                                           float perimTear, float braceTear,
                                           int tag, uint8_t colorIndex) {
    SoftBody nb;
    const int m = (int)src.size();
    if (m < 3) return nb;

    float totalM = 0.f;
    Vec2 center{0.f, 0.f};
    Vec2 velocity{0.f, 0.f};
    for (const auto& p : src) {
        totalM += p.mass;
        center += p.pos * p.mass;
        velocity += p.vel * p.mass;
    }
    if (totalM < 1e-8f) return nb;
    center *= (1.f / totalM);
    velocity *= (1.f / totalM);

    constexpr float kPi = 3.14159265f;
    constexpr float kPressurePerArea = 24.f;
    constexpr int kMergedSegments = 22;
    const float hintedArea = pressureTargetHint > 1e-4f
        ? pressureTargetHint / kPressurePerArea
        : kPi * 0.9f * 0.9f;
    const float radius = std::clamp(std::sqrt(std::max(0.14f, hintedArea) / kPi), 0.38f, 1.25f);

    nb = SoftBody::makeCircle(center, radius, kMergedSegments, totalM, stiff, damp);
    for (auto& p : nb.points) {
        p.vel = velocity;
        p.prev = p.pos;
    }

    nb.pressureTarget = std::max(pressureTargetHint * 0.96f,
                                 kPi * radius * radius * kPressurePerArea);
    nb.pressureK = pk;
    nb.restitution = restitution;
    nb.friction = friction;
    nb.perimeterTearRatio = perimTear;
    nb.braceTearRatio = braceTear;
    nb.tag = tag;
    nb.colorIndex = colorIndex;
    nb.playerControlled = true;
    return nb;
}

SoftBody rebuildConvexFragment(const SoftBody& orig, const std::vector<int>& comp) {
    std::vector<int> order = hullGlobalOrder(comp, orig);
    const int hn = (int)order.size();
    SoftBody nb;
    if (hn < 3) return nb;

    float totalMass = 0.f;
    for (int gi : comp) totalMass += orig.points[gi].mass;
    const float perM = totalMass / (float)hn;

    float stiff = 1200.f, damp = 25.f;
    for (const Spring& sp : orig.springs) {
        if (!sp.broken && !sp.isBrace) {
            stiff = sp.stiffness;
            damp = sp.damping;
            break;
        }
    }

    nb.points.reserve(hn);
    for (int gi : order) {
        PointMass pm = orig.points[gi];
        pm.mass = perM;
        pm.invMass = 1.f / perM;
        nb.points.push_back(pm);
    }

    // Keep CCW winding so rendering/pressure stay consistent after split.
    if (hn >= 3) {
        float twice = 0.f;
        for (int i = 0; i < hn; ++i) {
            twice += cross(nb.points[(size_t)i].pos,
                           nb.points[(size_t)((i + 1) % hn)].pos);
        }
        if (twice < 0.f) std::reverse(nb.points.begin(), nb.points.end());
    }

    inflateReconstructedBlob(nb);

    for (int i = 0; i < hn; ++i) {
        Spring s;
        s.a = i;
        s.b = (i + 1) % hn;
        s.restLength = (nb.points[s.a].pos - nb.points[s.b].pos).len();
        s.stiffness = stiff;
        s.damping = damp;
        s.isBrace = false;
        nb.springs.push_back(s);
    }
    const int segments = hn;
    const int step = std::max(2, segments / 8);
    for (int i = 0; i < segments; i += step) {
        Spring s;
        s.a = i;
        s.b = (i + segments / 2) % segments;
        s.restLength = (nb.points[s.a].pos - nb.points[s.b].pos).len();
        s.stiffness = stiff * 0.25f;
        s.damping = damp * 0.5f;
        s.isBrace = true;
        nb.springs.push_back(s);
    }

    const float newA = std::max(1e-4f, nb.convexHullArea());
    // Each fragment is its own balloon: nRT = newArea * basePressureScale, calibrated to
    // gravity = 30 / mass-density used in Slime::spawn. Avoids deriving nRT from a flat /
    // tangled signed area on the parent.
    nb.pressureTarget = newA * 22.f;
    nb.pressureK = orig.pressureK;
    nb.restitution = orig.restitution;
    nb.friction = orig.friction;
    nb.perimeterTearRatio = orig.perimeterTearRatio;
    nb.braceTearRatio = orig.braceTearRatio;
    nb.tag = orig.tag;
    nb.colorIndex = orig.colorIndex;
    nb.playerControlled = orig.playerControlled;
    return nb;
}

float pointMassTotal(const SoftBody& sb) {
    float total = 0.f;
    for (const auto& p : sb.points) total += p.mass;
    return total;
}

float pointMassTotal(const SoftBody& sb, const std::vector<int>& ids) {
    float total = 0.f;
    for (int id : ids) total += sb.points[(size_t)id].mass;
    return total;
}

Vec2 pointMassCentroid(const SoftBody& sb, const std::vector<int>& ids) {
    Vec2 c{0.f, 0.f};
    float total = 0.f;
    for (int id : ids) {
        const PointMass& p = sb.points[(size_t)id];
        c += p.pos * p.mass;
        total += p.mass;
    }
    return total > 1e-8f ? c * (1.f / total) : sb.centroid();
}

Vec2 pointMassVelocity(const SoftBody& sb, const std::vector<int>& ids) {
    Vec2 v{0.f, 0.f};
    float total = 0.f;
    for (int id : ids) {
        const PointMass& p = sb.points[(size_t)id];
        v += p.vel * p.mass;
        total += p.mass;
    }
    return total > 1e-8f ? v * (1.f / total) : sb.averageVelocity();
}

void springTuningFromSoftBody(const SoftBody& orig, float& stiff, float& damp) {
    stiff = 1200.f;
    damp = 25.f;
    for (const Spring& sp : orig.springs) {
        if (sp.broken || sp.isBrace) continue;
        stiff = sp.stiffness;
        damp = sp.damping;
        return;
    }
}

SoftBody makeRoundedSplitFragment(const SoftBody& orig, const std::vector<int>& ids,
                                  Vec2 center, float radius, int segments, Vec2 extraVel) {
    float stiff = 1200.f;
    float damp = 25.f;
    springTuningFromSoftBody(orig, stiff, damp);

    const float mass = std::max(0.001f, pointMassTotal(orig, ids));
    SoftBody nb = SoftBody::makeCircle(center, radius, segments, mass, stiff, damp);
    nb.pressureK = orig.pressureK;
    nb.restitution = orig.restitution;
    nb.friction = orig.friction;
    nb.perimeterTearRatio = orig.perimeterTearRatio;
    nb.braceTearRatio = orig.braceTearRatio;
    nb.tag = orig.tag;
    nb.colorIndex = orig.colorIndex;
    nb.playerControlled = orig.playerControlled;
    nb.pressureTarget = std::max(1e-4f, 3.14159265f * radius * radius * 24.f);

    const Vec2 v = pointMassVelocity(orig, ids) + extraVel;
    for (auto& p : nb.points) {
        p.vel = v;
        p.prev = p.pos;
    }
    return nb;
}

} // namespace

namespace {

/// Same convention as `Slime::networkedPlayerBlobTag` / needle clamp in `World::step`.
inline bool isNetworkedPlayerSlimeTag(int t) {
    constexpr int base = 1;
    constexpr int stride = 100;
    if (t < base) return false;
    return (t - base) % stride == 0;
}

inline float blobApproxRadius(const SoftBody& sb) {
    Vec2 c = sb.centroid();
    float r = 0.f;
    for (const auto& p : sb.points)
        r = std::max(r, (p.pos - c).len());
    return std::max(r, 0.1f);
}

/// Push `A`'s points out of `B`'s closed polygon edges (CCW point order).
inline void depenetratePointsVsPolygon(SoftBody& A, const SoftBody& B, float skin,
                                       float push) {
    const int nB = (int)B.points.size();
    if (nB < 2) return;
    for (auto& pa : A.points) {
        if (pa.pinned) continue;
        for (int e = 0; e < nB; ++e) {
            const Vec2& a = B.points[(size_t)e].pos;
            const Vec2& b = B.points[(size_t)((e + 1) % nB)].pos;
            Vec2 ab = b - a;
            float ab2 = ab.lenSq();
            if (ab2 < 1e-12f) continue;
            float t = std::clamp(dot(pa.pos - a, ab) / ab2, 0.f, 1.f);
            Vec2 closest = a + ab * t;
            Vec2 d = pa.pos - closest;
            float l2 = d.lenSq();
            if (l2 >= skin * skin || l2 < 1e-14f) continue;
            float len = std::sqrt(l2);
            Vec2 n = d * (1.f / len);
            Vec2 delta = n * ((skin - len) * push);
            pa.pos += delta;
            pa.prev += delta;
        }
    }
}

/// Even–odd ray test — works for simple (non self-intersecting) closed polylines.
inline bool pointInPolygonEvenOdd(const std::vector<PointMass>& poly, Vec2 p) {
    const int n = (int)poly.size();
    if (n < 3) return false;
    bool c = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const Vec2& a = poly[(size_t)i].pos;
        const Vec2& b = poly[(size_t)j].pos;
        if ((a.y > p.y) != (b.y > p.y)) {
            float inv = (b.y - a.y);
            if (std::abs(inv) < 1e-8f) continue;
            float xInt = (b.x - a.x) * (p.y - a.y) / inv + a.x;
            if (p.x < xInt) c = !c;
        }
    }
    return c;
}

/// If `who`'s centroid lies inside `shell`'s polygon, nudge all points along the escape axis.
/// Gated by COM distance so a damaged / self-crossing outline cannot spuriously fire every tick.
inline void escapeCentroidFromPolygon(SoftBody& who, const SoftBody& shell, float step,
                                      float whoRad, float shellRad) {
    Vec2 cW = who.centroid();
    Vec2 cS = shell.centroid();
    if ((cW - cS).len() > (whoRad + shellRad) * 1.05f + 0.18f) return;
    if (!pointInPolygonEvenOdd(shell.points, cW)) return;
    Vec2 axis = cW - cS;
    if (axis.lenSq() < 1e-8f) axis = {1.f, 0.f};
    axis = axis.normalized();
    Vec2 d = axis * step;
    for (auto& p : who.points) {
        if (p.pinned) continue;
        p.pos += d;
        p.prev += d;
    }
}

inline float totalMass(const SoftBody& sb) {
    float m = 0.f;
    for (const auto& p : sb.points) m += p.mass;
    return m;
}

/// Same linear delta-velocity on every point → changes COM motion only (keeps internal jiggle).
inline void addUniformDeltaVel(SoftBody& sb, const Vec2& dv) {
    for (auto& p : sb.points) {
        if (!p.pinned) p.vel += dv;
    }
}

/// One-pass point-vs-edge contact resolution between two soft bodies, treating
/// Y's perimeter as a rigid-ish polygon for X's points. Bounded Baumgarte
/// position correction + capped restitution impulse + Coulomb friction.
/// All caps exist because, unlike the rigid pipeline, soft-soft contacts are
/// not cached / warm-started across passes — every pass recomputes contacts
/// from scratch, so unbounded impulses + 6 passes used to pump enough energy
/// to literally explode the blobs apart.
inline void resolveSoftPair(SoftBody& X, SoftBody& Y, float restitution, float friction) {
    const int nY = (int)Y.points.size();
    if (nY < 3) return;
    Vec2 cY = Y.centroid();

    constexpr float kRestVelGate    = 1.6f;
    constexpr float kSlop           = 0.004f;
    constexpr float kBaumgarte      = 0.32f;
    constexpr float kMaxPosCorrPass = 0.05f;   // m / contact / pass
    constexpr float kMaxImpulseDv   = 4.5f;    // m·s^-1 / contact / pass

    for (auto& pa : X.points) {
        if (pa.pinned) continue;

        // Even-odd ray test: is pa.pos inside Y's perimeter?
        bool inside = false;
        for (int i = 0, j = nY - 1; i < nY; j = i++) {
            const Vec2& a = Y.points[(size_t)i].pos;
            const Vec2& b = Y.points[(size_t)j].pos;
            if ((a.y > pa.pos.y) != (b.y > pa.pos.y)) {
                float inv = (b.y - a.y);
                if (std::abs(inv) < 1e-8f) continue;
                float xInt = (b.x - a.x) * (pa.pos.y - a.y) / inv + a.x;
                if (pa.pos.x < xInt) inside = !inside;
            }
        }
        if (!inside) continue;

        // Closest edge of Y to pa.
        float bestD2 = 1e30f;
        int bestEdge = -1;
        float bestT = 0.f;
        for (int e = 0; e < nY; ++e) {
            const Vec2& ea = Y.points[(size_t)e].pos;
            const Vec2& eb = Y.points[(size_t)((e + 1) % nY)].pos;
            Vec2 ab = eb - ea;
            float ab2 = ab.lenSq();
            if (ab2 < 1e-12f) continue;
            float t = std::clamp(dot(pa.pos - ea, ab) / ab2, 0.f, 1.f);
            Vec2 cl = ea + ab * t;
            float d2 = (pa.pos - cl).lenSq();
            if (d2 < bestD2) {
                bestD2 = d2;
                bestEdge = e;
                bestT = t;
            }
        }
        if (bestEdge < 0) continue;

        const int yi0 = bestEdge;
        const int yi1 = (bestEdge + 1) % nY;
        PointMass& py0 = Y.points[(size_t)yi0];
        PointMass& py1 = Y.points[(size_t)yi1];

        Vec2 edge = py1.pos - py0.pos;
        Vec2 outward{edge.y, -edge.x};
        float ol = outward.len();
        if (ol < 1e-8f) continue;
        Vec2 n = outward / ol;
        Vec2 mid = (py0.pos + py1.pos) * 0.5f;
        if (dot(n, mid - cY) < 0.f) n = -n;

        const float depth = std::sqrt(bestD2);
        const float t  = bestT;
        const float w0 = py0.pinned ? 0.f : py0.invMass;
        const float w1 = py1.pinned ? 0.f : py1.invMass;
        const float wA = pa.invMass;
        const float wY = (1.f - t) * (1.f - t) * w0 + t * t * w1;
        const float wTot = wA + wY;
        if (wTot < 1e-8f) continue;

        // Position correction (NGS-style, bounded so it can't teleport points).
        float corrMag = std::clamp(kBaumgarte * (depth - kSlop), 0.f, kMaxPosCorrPass) / wTot;
        if (corrMag > 0.f) {
            Vec2 corr = n * corrMag;
            pa.pos  += corr * wA;
            pa.prev += corr * wA;
            if (!py0.pinned) {
                Vec2 d0 = corr * (1.f - t) * w0;
                py0.pos  -= d0;
                py0.prev -= d0;
            }
            if (!py1.pinned) {
                Vec2 d1 = corr * t * w1;
                py1.pos  -= d1;
                py1.prev -= d1;
            }
        }

        Vec2 vY = py0.vel * (1.f - t) + py1.vel * t;
        Vec2 vRel = pa.vel - vY;
        float vn = dot(vRel, n);
        if (vn >= 0.f) continue;

        float e = restitution;
        if (-vn < kRestVelGate) e = 0.f;
        float jn = -(1.f + e) * vn / wTot;
        // Hard cap: per-pass dv on the slime point can't exceed kMaxImpulseDv.
        // Without this, a single deep-penetration step blows the blobs apart.
        float dvA = jn * wA;
        if (std::abs(dvA) > kMaxImpulseDv) {
            jn *= kMaxImpulseDv / std::abs(dvA);
        }
        Vec2 Jn = n * jn;
        pa.vel += Jn * wA;
        if (!py0.pinned) py0.vel -= Jn * (1.f - t) * w0;
        if (!py1.pinned) py1.vel -= Jn * t * w1;

        // Coulomb friction along tangent.
        Vec2 tax{-n.y, n.x};
        Vec2 vY2 = py0.vel * (1.f - t) + py1.vel * t;
        float vt = dot(pa.vel - vY2, tax);
        float jt = -vt / wTot;
        float maxFric = std::abs(jn) * friction;
        jt = std::clamp(jt, -maxFric, maxFric);
        Vec2 Jt = tax * jt;
        pa.vel += Jt * wA;
        if (!py0.pinned) py0.vel -= Jt * (1.f - t) * w0;
        if (!py1.pinned) py1.vel -= Jt * t * w1;
    }
}

/// Soft-soft separation between player slimes and same-player fragments. Three passes are enough:
/// each contact resolution is bounded, so additional passes only refine
/// (no energy pumping). Replaces the old COM-shove model.
void resolvePlayerSlimesMutualRepulsion(std::vector<std::unique_ptr<SoftBody>>& softBodies) {
    const size_t n = softBodies.size();

    constexpr int   kPasses    = 3;
    constexpr float kPairRest  = 0.12f;
    constexpr float kPairFric  = 0.55f;

#ifdef PE_HEADLESS_SERVER
    // Dedicated server: cross-slot pairs stay cheap (only different players). Same-slot
    // fragments still need resolveSoftPair so split blobs don't interpenetrate.
    std::array<std::vector<size_t>, pe::net::kMaxPlayers> bySlot{};
    for (size_t idx = 0; idx < n; ++idx) {
        const int t = softBodies[idx]->tag;
        if (!isNetworkedPlayerSlimeTag(t)) continue;
        const int slot = (t - 1) / 100;
        if (slot >= 0 && slot < pe::net::kMaxPlayers)
            bySlot[(size_t)slot].push_back(idx);
    }
    int activeSlots = 0;
    for (auto& v : bySlot)
        if (!v.empty()) ++activeSlots;

    for (int pass = 0; pass < kPasses; ++pass) {
        if (activeSlots >= 2) {
            for (int sa = 0; sa < pe::net::kMaxPlayers; ++sa) {
                if (bySlot[(size_t)sa].empty()) continue;
                for (int sb = sa + 1; sb < pe::net::kMaxPlayers; ++sb) {
                    if (bySlot[(size_t)sb].empty()) continue;
                    for (size_t ia : bySlot[(size_t)sa]) {
                        SoftBody& A = *softBodies[ia];
                        for (size_t ib : bySlot[(size_t)sb]) {
                            SoftBody& B = *softBodies[ib];
                            if (!A.aabb().overlaps(B.aabb())) continue;
                            resolveSoftPair(A, B, kPairRest, kPairFric);
                            resolveSoftPair(B, A, kPairRest, kPairFric);
                        }
                    }
                }
            }
        }
        for (int sa = 0; sa < pe::net::kMaxPlayers; ++sa) {
            const std::vector<size_t>& slotIdx = bySlot[(size_t)sa];
            const size_t m = slotIdx.size();
            if (m < 2) continue;
            for (size_t a = 0; a < m; ++a) {
                SoftBody& A = *softBodies[slotIdx[a]];
                for (size_t b = a + 1; b < m; ++b) {
                    SoftBody& B = *softBodies[slotIdx[b]];
                    if (!A.aabb().overlaps(B.aabb())) continue;
                    resolveSoftPair(A, B, kPairRest, kPairFric);
                    resolveSoftPair(B, A, kPairRest, kPairFric);
                }
            }
        }
    }
#else
    int nPlayer = 0;
    for (size_t idx = 0; idx < n; ++idx) {
        if (isNetworkedPlayerSlimeTag(softBodies[idx]->tag))
            ++nPlayer;
    }
    if (nPlayer < 2) return;

    for (int pass = 0; pass < kPasses; ++pass) {
        for (size_t i = 0; i < n; ++i) {
            SoftBody& A = *softBodies[i];
            if (!isNetworkedPlayerSlimeTag(A.tag)) continue;
            for (size_t j = i + 1; j < n; ++j) {
                SoftBody& B = *softBodies[j];
                if (!isNetworkedPlayerSlimeTag(B.tag)) continue;
                if (!A.aabb().overlaps(B.aabb())) continue;
                resolveSoftPair(A, B, kPairRest, kPairFric);
                resolveSoftPair(B, A, kPairRest, kPairFric);
            }
        }
    }
#endif
}

} // namespace

void World::removeSoftBodiesWithTag(int tag) {
    for (int i = (int)softBodies_.size() - 1; i >= 0; --i) {
        if (softBodies_[(size_t)i]->tag == tag)
            removeSoftBodyAt((size_t)i);
    }
}

void World::mergeSoftBodiesWithTag(int tag, float pressureTargetHint) {
    std::vector<PointMass> all;
    std::vector<size_t> rm;
    float stiff = 1480.f, damp = 31.f;
    float pk = 1.f, rest = 0.05f, fric = 0.85f;
    bool gotStiff = false;

    for (size_t i = 0; i < softBodies_.size(); ++i) {
        SoftBody& sb = *softBodies_[i];
        if (sb.tag != tag) continue;
        rm.push_back(i);
        if (!gotStiff) {
            for (auto& sp : sb.springs) {
                if (!sp.isBrace && !sp.broken) {
                    stiff = sp.stiffness;
                    damp = sp.damping;
                    gotStiff = true;
                    break;
                }
            }
            pk = sb.pressureK;
            rest = sb.restitution;
            fric = sb.friction;
        }
        for (auto& p : sb.points) all.push_back(p);
    }
    if (rm.size() <= 1 || all.size() < 3) return;

    float perimTear = 0.f, braceTear = 0.f;
    uint8_t colorIndex = 0;
    for (auto& sb : softBodies_) {
        if (sb->tag == tag) {
            perimTear = sb->perimeterTearRatio;
            braceTear = sb->braceTearRatio;
            colorIndex = sb->colorIndex;
            break;
        }
    }
    SoftBody merged = buildMergedRoundedSoftBody(std::move(all), stiff, damp, pk, pressureTargetHint, rest,
                                                 fric, perimTear, braceTear, tag, colorIndex);
    if (merged.points.size() < 3) return;

    std::sort(rm.begin(), rm.end(), std::greater<size_t>());
    for (size_t idx : rm) removeSoftBodyAt(idx);
    addSoftBody(std::move(merged));
}

bool World::splitLargestBlobWithTag(int tag, Vec2 axisDir) {
    int bestIdx = -1;
    int bestCount = 0;
    for (size_t i = 0; i < softBodies_.size(); ++i) {
        SoftBody& sb = *softBodies_[i];
        if (sb.tag != tag) continue;
        if (!sb.playerControlled) continue;
        if ((int)sb.points.size() > bestCount) {
            bestCount = (int)sb.points.size();
            bestIdx = (int)i;
        }
    }
    if (bestIdx < 0 || bestCount < 8) return false;

    SoftBody& sb = *softBodies_[(size_t)bestIdx];
    if (sb.convexHullArea() < pe::net::kMinSlimeConvexAreaToSplit)
        return false;
    if (axisDir.lenSq() < 1e-8f) axisDir = {0.f, 1.f};
    Vec2 dir = axisDir.normalized();
    Vec2 cutNormal{-dir.y, dir.x};
    Vec2 c = sb.centroid();

    auto sideOf = [&](const Vec2& p) {
        float d = dot(p - c, cutNormal);
        return d >= 0.f ? 1 : -1;
    };

    int cutsMade = 0;
    for (auto& sp : sb.springs) {
        if (sp.broken) continue;
        const Vec2& pa = sb.points[(size_t)sp.a].pos;
        const Vec2& pb = sb.points[(size_t)sp.b].pos;
        if (sideOf(pa) != sideOf(pb)) {
            sp.broken = true;
            ++cutsMade;
        }
    }
    if (cutsMade == 0) return false;

    // Add a small lateral kick so the halves visibly separate.
    const float kick = 1.4f;
    for (auto& p : sb.points) {
        int s = sideOf(p.pos);
        p.vel += cutNormal * (kick * (float)s);
    }

    processSoftBodyConnectivity();
    return true;
}

bool World::angularBisectLargestBlobWithTag(int tag, Vec2 axisDir) {
    int bestIdx = -1;
    int bestCount = 0;
    for (size_t i = 0; i < softBodies_.size(); ++i) {
        SoftBody& sb = *softBodies_[i];
        if (sb.tag != tag) continue;
        if (!sb.playerControlled) continue;
        if ((int)sb.points.size() > bestCount) {
            bestCount = (int)sb.points.size();
            bestIdx = (int)i;
        }
    }
    if (bestIdx < 0) return false;
    SoftBody& sb = *softBodies_[(size_t)bestIdx];
    const int n = (int)sb.points.size();
    if (n < 6) return false;
    if (sb.convexHullArea() < pe::net::kMinSlimeConvexAreaToSplit)
        return false;

    Vec2 c = sb.centroid();
    if (axisDir.lenSq() < 1e-8f) axisDir = {1.f, 0.f};
    Vec2 dir = axisDir.normalized();
    Vec2 cutNormal = dir;

    std::vector<std::pair<float, int>> bySide;
    bySide.reserve((size_t)n);
    for (int k = 0; k < n; ++k) {
        bySide.push_back({dot(sb.points[(size_t)k].pos - c, cutNormal), k});
    }
    std::sort(bySide.begin(), bySide.end(),
              [](const std::pair<float, int>& a, const std::pair<float, int>& b) {
                  return a.first < b.first;
              });

    std::vector<int> A, B;
    const int half = (int)clamp((float)(n / 2), 3.f, (float)(n - 3));
    A.reserve((size_t)half);
    B.reserve((size_t)(n - half));
    for (int i = 0; i < half; ++i) A.push_back(bySide[(size_t)i].second);
    for (int i = half; i < n; ++i) B.push_back(bySide[(size_t)i].second);
    if ((int)A.size() < 3 || (int)B.size() < 3) return false;

    Vec2 ca = pointMassCentroid(sb, A);
    Vec2 cb = pointMassCentroid(sb, B);
    Vec2 sep = cb - ca;
    if (dot(sep, cutNormal) < 0.f) sep = -sep;
    if (sep.lenSq() < 1e-8f) sep = cutNormal;
    sep = sep.normalized();

    const float totalMass = std::max(0.001f, pointMassTotal(sb));
    const float massA = pointMassTotal(sb, A);
    const float massB = pointMassTotal(sb, B);
    const float area = std::max(0.14f, sb.convexHullArea());
    constexpr float kPi = 3.14159265f;
    float radiusA = std::sqrt(std::max(0.045f, area * (massA / totalMass)) / kPi);
    float radiusB = std::sqrt(std::max(0.045f, area * (massB / totalMass)) / kPi);
    radiusA = clamp(radiusA, 0.28f, 1.1f);
    radiusB = clamp(radiusB, 0.28f, 1.1f);

    const float wantedSep = (radiusA + radiusB) * 1.08f;
    const float haveSep = (cb - ca).len();
    if (haveSep < wantedSep) {
        const float push = (wantedSep - haveSep) * 0.5f;
        ca -= sep * push;
        cb += sep * push;
    }

    const int segA = (int)clamp((float)A.size() + 2.f, 10.f, 18.f);
    const int segB = (int)clamp((float)B.size() + 2.f, 10.f, 18.f);
    const float mainNudge = 0.55f;
    const float throwSpeed = 10.5f;
    SoftBody na = makeRoundedSplitFragment(sb, A, ca, radiusA, segA, -sep * mainNudge);
    SoftBody nb = makeRoundedSplitFragment(sb, B, cb, radiusB, segB, dir * throwSpeed);
    na.playerControlled = true;
    nb.playerControlled = false;

    removeSoftBodyAt((size_t)bestIdx);
    addSoftBody(std::move(na));
    addSoftBody(std::move(nb));
    return true;
}

bool World::playerSplitLargestBlobWithTag(int tag, Vec2 axisDir) {
    // Player-triggered split should be deterministic and binary: exactly two
    // stable child blobs. Reusing the old outline creates needle/triangle shards
    // when the slime is flattened against terrain, so rebuild rounded pieces.
    return angularBisectLargestBlobWithTag(tag, axisDir);
}

void World::tryBinarySplitDamagedBlob(int tag) {
    int tagCount = 0;
    for (const auto& sb : softBodies_) {
        if (sb->tag == tag) ++tagCount;
    }
    // Split only while exactly one blob carries this player tag. Otherwise each
    // physics step can split again and fragment count grows without bound (server
    // lag after tens of seconds). After damage-split into two pieces, merge must
    // recombine before another split can occur.
    if (tagCount != 1) return;

    for (size_t i = 0; i < softBodies_.size(); ++i) {
        SoftBody& sb = *softBodies_[i];
        if (sb.tag != tag) continue;

        connectedComponentsInto(sb, tlsConnComps);
        if (tlsConnComps.size() != 1) continue;

        int brk = 0;
        for (auto& sp : sb.springs)
            if (sp.broken) ++brk;
        if (brk < 2) continue;

        const int n = (int)sb.points.size();
        if (n < 10) continue;
        if (sb.convexHullArea() < pe::net::kMinSlimeConvexAreaToSplit)
            continue;

        Vec2 c = sb.centroid();
        std::vector<std::pair<float, int>> ord;
        ord.reserve((size_t)n);
        for (int k = 0; k < n; ++k) {
            Vec2 d = sb.points[(size_t)k].pos - c;
            ord.push_back({std::atan2(d.y, d.x), k});
        }
        std::sort(ord.begin(), ord.end(),
                  [](const std::pair<float, int>& a, const std::pair<float, int>& b) {
                      return a.first < b.first;
                  });

        int half = n / 2;
        half = (int)clamp((float)half, 3.f, (float)(n - 3));

        std::vector<int> A, B;
        A.reserve((size_t)half);
        for (int k = 0; k < half; ++k) A.push_back(ord[(size_t)k].second);
        for (int k = half; k < n; ++k) B.push_back(ord[(size_t)k].second);

        if ((int)A.size() < 3 || (int)B.size() < 3) continue;

        SoftBody na = rebuildConvexFragment(sb, A);
        SoftBody nb = rebuildConvexFragment(sb, B);
        if (na.points.size() < 3 || nb.points.size() < 3) continue;

        removeSoftBodyAt(i);
        addSoftBody(std::move(na));
        addSoftBody(std::move(nb));
        return;
    }
}

World::World() = default;

Body* World::createBody(Shape shape, Vec2 pos, BodyType type, float density) {
    auto b = std::make_unique<Body>();
    b->id = nextId_++;
    b->shape = std::move(shape);
    b->pos = pos;
    b->type = type;
    if (type == BodyType::Static) {
        b->makeStatic();
    } else {
        // mass from area * density
        float area;
        if (b->shape.type == ShapeType::Circle) {
            area = 3.14159265f * b->shape.radius * b->shape.radius;
        } else {
            float a = 0.f;
            for (size_t i = 0; i < b->shape.vertices.size(); ++i) {
                a += cross(b->shape.vertices[i],
                           b->shape.vertices[(i + 1) % b->shape.vertices.size()]);
            }
            area = std::abs(a) * 0.5f;
        }
        b->setMass(std::max(0.001f, area * density));
    }
    Body* ptr = b.get();
    bodies_.push_back(std::move(b));
    return ptr;
}

void World::clear() {
    bodies_.clear();
    softBodies_.clear();
    manifolds_.clear();
    nextId_ = 1;
}

SoftBody* World::addSoftBody(SoftBody sb) {
    auto p = std::make_unique<SoftBody>(std::move(sb));
    SoftBody* ptr = p.get();
    softBodies_.push_back(std::move(p));
    return ptr;
}

void World::step(float dt) {
    if (dt <= 0.f) return;

    integrateVelocities(dt);

    // 2. broadphase + narrow phase for rigids
    detectCollisions();

    // 3. prepare contacts (mass terms, restitution bias)
    prepareContacts(dt);

    // 4. warm start
    // warmStart(); // disabled: impulses are zeroed each frame (no cross-frame cache)

    // 5. velocity iterations
    for (int i = 0; i < velocityIterations; ++i) solveVelocities();

    // 6. integrate positions
    integratePositions(dt);

    // 7. position iterations (Baumgarte / NGS)
    for (int i = 0; i < positionIterations; ++i) solvePositions();

    // 8. soft bodies
    rigidsStepScratch_.clear();
    rigidsStepScratch_.reserve(bodies_.size());
    for (auto& b : bodies_) rigidsStepScratch_.push_back(b.get());

    // Stiff mass-springs need smaller dt than the rigid solver — sub-step soft integration.
#ifdef PE_HEADLESS_SERVER
    const int softSubsteps = 4;
#else
    const int softSubsteps = 10;
#endif
    float softH = dt / (float)softSubsteps;
    for (int k = 0; k < softSubsteps; ++k) {
        for (auto& sb : softBodies_) {
            sb->accumulateForces(gravity, rigidsStepScratch_);
            sb->integrate(softH);
        }
        for (auto& sb : softBodies_) {
#ifdef PE_HEADLESS_SERVER
            for (int it = 0; it < 4; ++it)
#else
            for (int it = 0; it < 8; ++it)
#endif
                sb->resolveCollisions(rigidsStepScratch_);
        }
#ifdef PE_HEADLESS_SERVER
        if (k == softSubsteps - 1)
#endif
            resolvePlayerSlimesMutualRepulsion(softBodies_);
        for (auto& sb : softBodies_) {
            sb->postStep();
        }
        // No needle clamp inside substeps: scaling positions around centroid
        // while a side is pinned by collision pumps energy (blob walks upward
        // each substep). End-of-step clamp below is sufficient.
    }

    // Final squeeze: pressure + springs fight player-vs-player separation during substeps.
#ifdef PE_HEADLESS_SERVER
    resolvePlayerSlimesMutualRepulsion(softBodies_);
#else
    for (int z = 0; z < 2; ++z)
        resolvePlayerSlimesMutualRepulsion(softBodies_);
#endif
    // Player slimes only (solo tag 1 + networked tags 101, 201, …): needle clamp.
    // Keep stride in sync with Slime::networkedPlayerBlobTag / Slime::playerTag.
    constexpr int kPlayerTagBase = 1;
    constexpr int kPlayerTagStride = 100;
    for (auto& sb : softBodies_) {
        const int t = sb->tag;
        if (t < kPlayerTagBase) continue;
        if ((t - kPlayerTagBase) % kPlayerTagStride != 0) continue;
        // Aspect cap loose enough to allow a real squash (blob can flatten on
        // landing); only triggers when shape is actually needle-like.
        sb->clampNeedleLikeShape(3.5f, 0.32f);
    }

    for (auto& bp : bodies_) {
        if (bp->type != BodyType::Dynamic) continue;
        if (bp->slimeGlueTimer > 0.f)
            bp->slimeGlueTimer = std::max(0.f, bp->slimeGlueTimer - dt);
    }

    processSoftBodyConnectivity();
}

void World::removeSoftBodyAt(size_t index) {
    if (index >= softBodies_.size()) return;
    softBodies_.erase(softBodies_.begin() + (ptrdiff_t)index);
}

void World::processSoftBodyConnectivity() {
    std::vector<size_t> eraseDesc;
    std::vector<SoftBody> newOnes;
    eraseDesc.reserve(4);
    newOnes.reserve(8);

    for (size_t i = 0; i < softBodies_.size(); ++i) {
        SoftBody& sb = *softBodies_[i];
        bool anyBrokenSpring = false;
        for (const Spring& sp : sb.springs) {
            if (sp.broken) {
                anyBrokenSpring = true;
                break;
            }
        }
        if (!anyBrokenSpring) continue;

        connectedComponentsInto(sb, tlsConnComps);
        if (tlsConnComps.size() <= 1) continue;

        std::vector<SoftBody> frags;
        for (auto& c : tlsConnComps) {
            if (c.size() < 3) continue;
            SoftBody piece = rebuildConvexFragment(sb, c);
            if (piece.points.size() >= 3) frags.push_back(std::move(piece));
        }
        eraseDesc.push_back(i);
        for (auto& f : frags) newOnes.push_back(std::move(f));
    }

    std::sort(eraseDesc.begin(), eraseDesc.end(), std::greater<size_t>());
    for (size_t idx : eraseDesc) removeSoftBodyAt(idx);
    for (auto& f : newOnes) addSoftBody(std::move(f));
}

void World::integrateVelocities(float dt) {
    for (auto& b : bodies_) {
        if (b->type != BodyType::Dynamic) continue;
        b->vel += (gravity + b->force * b->invMass) * dt;
        b->angVel += b->torque * b->invInertia * dt;
        b->vel *= (1.f - b->linearDamping * dt);
        b->angVel *= (1.f - b->angularDamping * dt);
        b->force = {0, 0};
        b->torque = 0.f;
    }
}

void World::integratePositions(float dt) {
    for (auto& b : bodies_) {
        if (b->type != BodyType::Dynamic) continue;
        b->pos += b->vel * dt;
        b->rot += b->angVel * dt;
    }
}

void World::detectCollisions() {
    manifolds_.clear();
    broadphase_.clear();
    for (auto& b : bodies_) broadphase_.insert(b.get());

    broadphase_.queryPairs(broadpairScratch_);
    for (auto& [a, b] : broadpairScratch_) {
        Manifold m;
        if (collide(*a, *b, m)) {
            m.a = a; m.b = b;
            manifolds_.push_back(m);
        }
    }
}

void World::prepareContacts(float dt) {
    const float slop = 0.0035f;
    const float biasFactor = 0.24f;
    for (auto& m : manifolds_) {
        Body* a = m.a; Body* b = m.b;
        for (int i = 0; i < m.count; ++i) {
            Contact& c = m.contacts[i];
            Vec2 ra = c.point - a->pos;
            Vec2 rb = c.point - b->pos;

            float rnA = cross(ra, c.normal);
            float rnB = cross(rb, c.normal);
            float kn = a->invMass + b->invMass + rnA * rnA * a->invInertia + rnB * rnB * b->invInertia;
            c.normalMass = kn > 0.f ? 1.f / kn : 0.f;

            Vec2 tangent{-c.normal.y, c.normal.x};
            float rtA = cross(ra, tangent);
            float rtB = cross(rb, tangent);
            float kt = a->invMass + b->invMass + rtA * rtA * a->invInertia + rtB * rtB * b->invInertia;
            c.tangentMass = kt > 0.f ? 1.f / kt : 0.f;

            // restitution (only if approaching fast enough)
            Vec2 va = a->vel + cross(a->angVel, ra);
            Vec2 vb = b->vel + cross(b->angVel, rb);
            float vn = dot(vb - va, c.normal);
            const float velThreshold = 1.f;
            float e = m.restitution;
            float restitutionBias = (vn < -velThreshold) ? -e * vn : 0.f;
            float positionBias = -biasFactor / dt * std::max(0.f, c.penetration - slop);
            c.velocityBias = restitutionBias + positionBias;

            // TODO: implement manifold caching across frames for proper warm-starting.
            c.normalImpulse = 0.f;
            c.tangentImpulse = 0.f;
        }
    }
}

void World::warmStart() {
    for (auto& m : manifolds_) {
        Body* a = m.a; Body* b = m.b;
        for (int i = 0; i < m.count; ++i) {
            Contact& c = m.contacts[i];
            Vec2 tangent{-c.normal.y, c.normal.x};
            Vec2 P = c.normal * c.normalImpulse + tangent * c.tangentImpulse;
            Vec2 ra = c.point - a->pos;
            Vec2 rb = c.point - b->pos;
            a->vel -= P * a->invMass;
            a->angVel -= cross(ra, P) * a->invInertia;
            b->vel += P * b->invMass;
            b->angVel += cross(rb, P) * b->invInertia;
        }
    }
}

void World::solveVelocities() {
    for (auto& m : manifolds_) {
        Body* a = m.a; Body* b = m.b;
        for (int i = 0; i < m.count; ++i) {
            Contact& c = m.contacts[i];
            Vec2 ra = c.point - a->pos;
            Vec2 rb = c.point - b->pos;

            // tangent (friction) first
            Vec2 tangent{-c.normal.y, c.normal.x};
            Vec2 va = a->vel + cross(a->angVel, ra);
            Vec2 vb = b->vel + cross(b->angVel, rb);
            float vt = dot(vb - va, tangent);
            float lambdaT = -vt * c.tangentMass;
            float maxFric = m.friction * c.normalImpulse;
            float oldTan = c.tangentImpulse;
            c.tangentImpulse = clamp(oldTan + lambdaT, -maxFric, maxFric);
            lambdaT = c.tangentImpulse - oldTan;
            Vec2 Pt = tangent * lambdaT;
            a->vel -= Pt * a->invMass;
            a->angVel -= cross(ra, Pt) * a->invInertia;
            b->vel += Pt * b->invMass;
            b->angVel += cross(rb, Pt) * b->invInertia;

            // normal
            va = a->vel + cross(a->angVel, ra);
            vb = b->vel + cross(b->angVel, rb);
            float vn = dot(vb - va, c.normal);
            float lambdaN = (-vn + c.velocityBias) * c.normalMass;
            float oldN = c.normalImpulse;
            c.normalImpulse = std::max(oldN + lambdaN, 0.f);
            lambdaN = c.normalImpulse - oldN;
            Vec2 Pn = c.normal * lambdaN;
            a->vel -= Pn * a->invMass;
            a->angVel -= cross(ra, Pn) * a->invInertia;
            b->vel += Pn * b->invMass;
            b->angVel += cross(rb, Pn) * b->invInertia;
        }
    }
}

void World::solvePositions() {
    const float slop = 0.0035f;
    const float maxCorrection = 0.34f;
    const float scale = 0.26f;
    for (auto& m : manifolds_) {
        Body* a = m.a; Body* b = m.b;
        for (int i = 0; i < m.count; ++i) {
            Contact& c = m.contacts[i];
            Vec2 ra = c.point - a->pos;
            Vec2 rb = c.point - b->pos;
            float rnA = cross(ra, c.normal);
            float rnB = cross(rb, c.normal);
            float k = a->invMass + b->invMass + rnA * rnA * a->invInertia + rnB * rnB * b->invInertia;
            if (k <= 0.f) continue;
            float corr = clamp(scale * (c.penetration - slop), 0.f, maxCorrection);
            float lambda = corr / k;
            Vec2 P = c.normal * lambda;
            a->pos -= P * a->invMass;
            a->rot -= cross(ra, P) * a->invInertia;
            b->pos += P * b->invMass;
            b->rot += cross(rb, P) * b->invInertia;
        }
    }
}

} // namespace pe
