#include "physics/Collision.h"
#include <algorithm>
#include <limits>

namespace pe {

namespace {

bool circleCircle(Body& A, Body& B, Manifold& m) {
    Vec2 d = B.pos - A.pos;
    float r = A.shape.radius + B.shape.radius;
    float d2 = d.lenSq();
    if (d2 >= r * r) return false;
    float dist = std::sqrt(d2);
    Vec2 n = dist > 1e-8f ? d / dist : Vec2{1, 0};
    m.normal = n;
    m.count = 1;
    m.contacts[0].normal = n;
    m.contacts[0].penetration = r - dist;
    m.contacts[0].point = A.pos + n * A.shape.radius;
    return true;
}

// Find support point of polygon B (in world) along direction n
Vec2 polySupport(const Body& B, const Vec2& n) {
    Mat2 R(B.rot);
    Vec2 localN = R.mulT(n);
    int best = 0;
    float bestProj = dot(B.shape.vertices[0], localN);
    for (size_t i = 1; i < B.shape.vertices.size(); ++i) {
        float p = dot(B.shape.vertices[i], localN);
        if (p > bestProj) { bestProj = p; best = (int)i; }
    }
    return B.pos + R.mul(B.shape.vertices[best]);
}

// SAT: find axis with max separation. Returns axis index (-1 if overlap).
struct AxisResult { float separation; int index; Vec2 normal; };

AxisResult findMaxSeparation(const Body& A, const Body& B) {
    AxisResult best{-std::numeric_limits<float>::infinity(), -1, {0, 0}};
    Mat2 RA(A.rot);
    for (size_t i = 0; i < A.shape.normals.size(); ++i) {
        Vec2 nWorld = RA.mul(A.shape.normals[i]);
        Vec2 sup = polySupport(B, -nWorld);
        Vec2 vA = A.pos + RA.mul(A.shape.vertices[i]);
        float sep = dot(nWorld, sup - vA);
        if (sep > best.separation) {
            best.separation = sep;
            best.index = (int)i;
            best.normal = nWorld;
        }
    }
    return best;
}

// Find incident edge on B against reference normal n (world)
void findIncidentEdge(const Body& B, const Vec2& nRef, Vec2 out[2]) {
    Mat2 R(B.rot);
    Vec2 nLocal = R.mulT(nRef);
    int incident = 0;
    float minDot = dot(B.shape.normals[0], nLocal);
    for (size_t i = 1; i < B.shape.normals.size(); ++i) {
        float d = dot(B.shape.normals[i], nLocal);
        if (d < minDot) { minDot = d; incident = (int)i; }
    }
    int i1 = incident;
    int i2 = (incident + 1) % (int)B.shape.vertices.size();
    out[0] = B.pos + R.mul(B.shape.vertices[i1]);
    out[1] = B.pos + R.mul(B.shape.vertices[i2]);
}

// Clip line segment to side plane. Returns count of points kept.
int clipSegmentToLine(Vec2 vIn[2], Vec2 vOut[2], const Vec2& n, float offset) {
    int count = 0;
    float d0 = dot(n, vIn[0]) - offset;
    float d1 = dot(n, vIn[1]) - offset;
    if (d0 <= 0.f) vOut[count++] = vIn[0];
    if (d1 <= 0.f) vOut[count++] = vIn[1];
    if (d0 * d1 < 0.f) {
        float t = d0 / (d0 - d1);
        vOut[count++] = vIn[0] + (vIn[1] - vIn[0]) * t;
    }
    return count;
}

bool polyPoly(Body& A, Body& B, Manifold& m) {
    AxisResult sa = findMaxSeparation(A, B);
    if (sa.separation > 0.f) return false;
    AxisResult sb = findMaxSeparation(B, A);
    if (sb.separation > 0.f) return false;

    // pick reference body (less negative separation)
    Body* ref;
    Body* inc;
    int refIdx;
    Vec2 refNormal;
    bool flip = false;
    const float bias = 0.005f;
    if (sa.separation > sb.separation - bias) {
        ref = &A; inc = &B; refIdx = sa.index; refNormal = sa.normal;
    } else {
        ref = &B; inc = &A; refIdx = sb.index; refNormal = sb.normal;
        flip = true;
    }

    Vec2 incidentEdge[2];
    findIncidentEdge(*inc, refNormal, incidentEdge);

    // reference edge
    Mat2 RR(ref->rot);
    Vec2 v1 = ref->pos + RR.mul(ref->shape.vertices[refIdx]);
    Vec2 v2 = ref->pos + RR.mul(ref->shape.vertices[(refIdx + 1) % ref->shape.vertices.size()]);
    Vec2 sideNormal = (v2 - v1).normalized();
    Vec2 frontNormal = {sideNormal.y, -sideNormal.x};
    float frontOffset = dot(frontNormal, v1);
    float sideOffset1 = -dot(sideNormal, v1);
    float sideOffset2 = dot(sideNormal, v2);

    Vec2 clip1[2], clip2[2];
    if (clipSegmentToLine(incidentEdge, clip1, -sideNormal, sideOffset1) < 2) return false;
    if (clipSegmentToLine(clip1, clip2, sideNormal, sideOffset2) < 2) return false;

    Vec2 nOut = flip ? -frontNormal : frontNormal;
    m.normal = nOut;
    m.count = 0;
    for (int i = 0; i < 2; ++i) {
        float sep = dot(frontNormal, clip2[i]) - frontOffset;
        if (sep <= 0.f) {
            Contact& c = m.contacts[m.count++];
            c.point = clip2[i];
            c.normal = nOut;
            c.penetration = -sep;
        }
    }
    return m.count > 0;
}

bool circlePoly(Body& C, Body& P, Manifold& m, bool flip) {
    Mat2 R(P.rot);
    Vec2 cLocal = R.mulT(C.pos - P.pos);

    // find face with max separation
    int best = 0;
    float maxSep = -std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < P.shape.normals.size(); ++i) {
        float s = dot(P.shape.normals[i], cLocal - P.shape.vertices[i]);
        if (s > C.shape.radius) return false;
        if (s > maxSep) { maxSep = s; best = (int)i; }
    }

    Vec2 v1 = P.shape.vertices[best];
    Vec2 v2 = P.shape.vertices[(best + 1) % P.shape.vertices.size()];

    Vec2 contactLocal;
    Vec2 nLocal;
    float pen;
    if (maxSep < 1e-4f) {
        // deep inside — use face normal
        nLocal = P.shape.normals[best];
        contactLocal = cLocal - nLocal * C.shape.radius;
        pen = C.shape.radius - maxSep;
    } else {
        // Voronoi region
        float u1 = dot(cLocal - v1, v2 - v1);
        float u2 = dot(cLocal - v2, v1 - v2);
        Vec2 closest;
        if (u1 <= 0.f)      closest = v1;
        else if (u2 <= 0.f) closest = v2;
        else {
            float t = u1 / dot(v2 - v1, v2 - v1);
            closest = v1 + (v2 - v1) * t;
        }
        Vec2 d = cLocal - closest;
        float distSq = d.lenSq();
        if (distSq > C.shape.radius * C.shape.radius) return false;
        float dist = std::sqrt(distSq);
        nLocal = dist > 1e-8f ? d / dist : P.shape.normals[best];
        contactLocal = closest;
        pen = C.shape.radius - dist;
    }

    Vec2 nWorld = R.mul(nLocal);
    Vec2 cpWorld = P.pos + R.mul(contactLocal);
    // normal A->B convention: from C to P
    Vec2 finalNormal = flip ? nWorld : -nWorld;
    m.normal = finalNormal;
    m.count = 1;
    m.contacts[0].point = cpWorld;
    m.contacts[0].normal = finalNormal;
    m.contacts[0].penetration = pen;
    return true;
}

} // anon

bool collide(Body& A, Body& B, Manifold& m) {
    m.a = &A; m.b = &B; m.count = 0;
    auto effRest = [](const Body& b) {
        return (b.slimeGlueTimer > 1e-4f) ? std::min(b.restitution, 0.018f) : b.restitution;
    };
    auto effFric = [](const Body& b) {
        return (b.slimeGlueTimer > 1e-4f) ? std::max(b.friction, 0.992f) : b.friction;
    };
    m.restitution = std::min(effRest(A), effRest(B));
    m.friction = std::sqrt(effFric(A) * effFric(B));

    if (A.shape.type == ShapeType::Circle && B.shape.type == ShapeType::Circle) {
        return circleCircle(A, B, m);
    }
    if (A.shape.type == ShapeType::Polygon && B.shape.type == ShapeType::Polygon) {
        return polyPoly(A, B, m);
    }
    if (A.shape.type == ShapeType::Circle && B.shape.type == ShapeType::Polygon) {
        return circlePoly(A, B, m, false);
    }
    // polygon vs circle: swap
    m.a = &A; m.b = &B;
    return circlePoly(B, A, m, true);
}

bool pointVsBody(const Vec2& p, Body& b, Vec2& outNormal, float& outDepth, Vec2& outClosest) {
    if (b.shape.type == ShapeType::Circle) {
        Vec2 d = p - b.pos;
        float dl = d.len();
        if (dl >= b.shape.radius) return false;
        outNormal = dl > 1e-8f ? d / dl : Vec2{0, 1};
        outDepth = b.shape.radius - dl;
        outClosest = b.pos + outNormal * b.shape.radius;
        return true;
    }
    // polygon: check inside via face separations
    Mat2 R(b.rot);
    Vec2 pLocal = R.mulT(p - b.pos);
    float minSep = -std::numeric_limits<float>::infinity();
    int bestFace = -1;
    for (size_t i = 0; i < b.shape.normals.size(); ++i) {
        float s = dot(b.shape.normals[i], pLocal - b.shape.vertices[i]);
        if (s > 0.f) return false; // outside
        if (s > minSep) { minSep = s; bestFace = (int)i; }
    }
    if (bestFace < 0) return false;
    Vec2 nLocal = b.shape.normals[bestFace];
    Vec2 nWorld = R.mul(nLocal);
    outNormal = nWorld;
    outDepth = -minSep;
    outClosest = p + nWorld * outDepth;
    return true;
}

bool closestSurfacePointToBody(const Vec2& p, const Body& b, Vec2& outClosest, float& outSignedDist,
                               Vec2& outDirToSurface) {
    outSignedDist = 0.f;
    outClosest = p;
    outDirToSurface = {0.f, 1.f};
    if (b.shape.type == ShapeType::Circle) {
        Vec2 d = p - b.pos;
        float dl = d.len();
        const float r = b.shape.radius;
        Vec2 radial = dl > 1e-8f ? d * (1.f / dl) : Vec2{0.f, 1.f};
        outClosest = b.pos + radial * r;
        outSignedDist = dl - r;
        Vec2 dir = outClosest - p;
        float L = dir.len();
        outDirToSurface = L > 1e-8f ? dir * (1.f / L) : radial;
        return true;
    }
    Mat2 R(b.rot);
    Vec2 pLocal = R.mulT(p - b.pos);
    const auto& V = b.shape.vertices;
    const size_t nv = V.size();
    if (nv < 2) return false;

    Vec2 bestLocal = V[0];
    float bestD2 = std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < nv; ++i) {
        const Vec2& a = V[i];
        const Vec2& e = V[(i + 1) % nv];
        Vec2 ab = e - a;
        float ab2 = ab.lenSq();
        if (ab2 < 1e-14f) continue;
        float t = std::clamp(dot(pLocal - a, ab) / ab2, 0.f, 1.f);
        Vec2 cl = a + ab * t;
        float d2 = (pLocal - cl).lenSq();
        if (d2 < bestD2) {
            bestD2 = d2;
            bestLocal = cl;
        }
    }
    Vec2 closestWorld = b.pos + R.mul(bestLocal);
    float dist = std::sqrt(std::max(0.f, bestD2));

    bool inside = true;
    for (size_t i = 0; i < b.shape.normals.size(); ++i) {
        if (dot(b.shape.normals[i], pLocal - V[i]) > 0.f) {
            inside = false;
            break;
        }
    }
    outClosest = closestWorld;
    outSignedDist = inside ? -dist : dist;
    Vec2 dirW = closestWorld - p;
    float Lw = dirW.len();
    outDirToSurface = Lw > 1e-8f ? dirW * (1.f / Lw) : R.mul(b.shape.normals[0]);
    return true;
}

bool pointVsBodyWithEntry(const Vec2& prev, const Vec2& pos, Body& b,
                          Vec2& outNormal, float& outDepth, Vec2& outClosest) {
    if (b.shape.type == ShapeType::Circle)
        return pointVsBody(pos, b, outNormal, outDepth, outClosest);

    Mat2 R(b.rot);
    Vec2 prevLocal = R.mulT(prev - b.pos);
    Vec2 posLocal = R.mulT(pos - b.pos);
    const auto& V = b.shape.vertices;
    const auto& N = b.shape.normals;

    int entryFace = -1;
    float bestPrevSep = -std::numeric_limits<float>::infinity();
    float entryDepth = 0.f;
    for (size_t i = 0; i < N.size(); ++i) {
        const float nowSep = dot(N[i], posLocal - V[i]);
        if (nowSep > 0.f)
            return false;
        const float prevSep = dot(N[i], prevLocal - V[i]);
        if (prevSep > 0.f && prevSep > bestPrevSep) {
            bestPrevSep = prevSep;
            entryFace = (int)i;
            entryDepth = -nowSep;
        }
    }

    constexpr float kMinPen = 5e-6f;
    if (entryFace >= 0 && entryDepth > kMinPen) {
        outNormal = R.mul(N[(size_t)entryFace]);
        outDepth = entryDepth;
        outClosest = pos + outNormal * outDepth;
        return true;
    }

    Vec2 geoClosest;
    float sd = 0.f;
    Vec2 geoDir;
    if (!closestSurfacePointToBody(pos, b, geoClosest, sd, geoDir))
        return false;

    float depth = -sd;
    if (depth <= kMinPen)
        return false;

    outNormal = geoDir;
    outDepth = depth;
    outClosest = geoClosest;
    return true;
}

} // namespace pe
