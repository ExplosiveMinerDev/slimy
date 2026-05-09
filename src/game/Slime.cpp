#include "game/Slime.h"
#include "physics/World.h"
#include "physics/SoftBody.h"
#include "physics/Body.h"
#include "physics/Collision.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace pe {

namespace {

/// Spike never counts. Static = any solid. Dynamic = only crates/balls (stand on top).
bool footOnBodySupport(const Vec2& footPos, Body& b) {
    if (b.tag == Slime::spikeHazardTag) return false;
    Vec2 n, closest;
    float depth;
    Vec2 probe = footPos + Vec2{0.f, 0.055f};
    if (!pointVsBody(probe, b, n, depth, closest)) return false;
    if (n.y > -0.38f) return false;
    if (b.type == BodyType::Static) return true;
    if (b.type == BodyType::Dynamic)
        return b.tag == Slime::crateTag || b.tag == Slime::ballTag;
    return false;
}

bool blobOnSupport(const SoftBody& sb, const World& world) {
    for (auto& p : sb.points) {
        for (auto& b : world.bodies()) {
            if (footOnBodySupport(p.pos, *b)) return true;
        }
    }
    return false;
}

/// True when the blob penetrates a static solid on a non-floor face (wall, underside of platform,
/// overhang, slope side). Excludes spikes and walkable top faces (`blobOnSupport`).
bool blobWallBrace(const SoftBody& sb, const World& world) {
    constexpr float kMinPen = 0.011f;
    constexpr float kFloorNyCutoff = -0.36f;
    for (auto& p : sb.points) {
        if (p.pinned) continue;
        for (auto& b : world.bodies()) {
            if (b->tag == Slime::spikeHazardTag) continue;
            if (b->type != BodyType::Static) continue;
            Vec2 n, closest;
            float depth;
            if (!pointVsBody(p.pos, *b, n, depth, closest)) continue;
            if (depth < kMinPen) continue;
            // Include underside contacts (normal → downward): previously skipped as "ceiling", which
            // blocked jumping while glued under floating platforms.
            if (n.y <= kFloorNyCutoff) continue;
            return true;
        }
    }
    return false;
}

/// Adhesion pulls points that stay slightly *outside* the rigid hull (`signedDist` > 0). Those often
/// miss `pointVsBody`, so `blobWallBrace` never fires — jump logic thinks you're in the air. Uses the
/// same closest-surface query as the sticky forces so hanging under platforms stays playable.
bool blobGlueSurfaceBrace(const SoftBody& sb, const World& world) {
    constexpr float kMaxOutside = 0.36f;
    constexpr float kMinSd = -0.06f;
    constexpr float kWalkableTopOutwardNy = -0.40f;
    for (auto& p : sb.points) {
        if (p.pinned) continue;
        for (auto& bptr : world.bodies()) {
            Body& b = *bptr;
            if (b.tag == Slime::spikeHazardTag) continue;
            if (b.type != BodyType::Static) continue;
            Vec2 cl, dirToSurf;
            float sd = 0.f;
            if (!closestSurfacePointToBody(p.pos, b, cl, sd, dirToSurf)) continue;
            if (sd > kMaxOutside || sd < kMinSd) continue;
            Vec2 outward{-dirToSurf.x, -dirToSurf.y};
            const float oLen = outward.len();
            if (oLen < 1e-6f) continue;
            outward = outward * (1.f / oLen);
            if (outward.y <= kWalkableTopOutwardNy) continue;
            return true;
        }
    }
    return false;
}

static thread_local uint32_t s_rng = 0x9E3779B9u;
inline float rnd11() {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return ((s_rng & 0xFFFFFF) * (1.f / float(0xFFFFFF))) * 2.f - 1.f;
}
inline float rnd01() { return rnd11() * 0.5f + 0.5f; }

} // namespace

void Slime::spawn(World& world, const Vec2& pos, float radius, int segments, int tag) {
    myTag_ = tag;
    SoftBody sb = SoftBody::makeCircle(pos, radius, segments,
                                       /*massTotal*/ 4.0f,
                                       /*stiffness*/ 720.f,
                                       /*damping*/   20.f);   // was 16 — settles faster
    sb.pressureK = 1.0f;
    sb.restitution = 0.28f;   // was 0.35 — calmer impacts; pairs with the 20% momentum cut
    sb.friction = 0.58f;      // moderate Coulomb friction; wall/ground stick uses adhesion in SoftBody
    sb.perimeterTearRatio = 0.f;
    sb.braceTearRatio = 0.f;
    sb.tag = myTag_;
    SoftBody* blob = world.addSoftBody(std::move(sb));
    basePressure_ = blob->pressureTarget;

    grounded_ = false;
    charging_ = false;
    chargeTimer_ = 0.f;
    coyoteTimer_ = 0.f;
    chargeBufferTimer_ = 0.f;
    puddles_.clear();
    spikeDwell_ = 0.f;
    spikeSplitCd_ = 0.f;
    spikeSliceHint_ = {0.f, 0.f};
    embeddedSpikes_.clear();
    spikeStickTimer_ = 0.f;
    grabBodyId_ = 0;
    wasGrabHeld_ = false;
    trailEmitCd_ = 0.f;
    jumpCooldownRemaining_ = 0.f;
}

Vec2 Slime::playerMassCentroid(const World& world, int tag) {
    Vec2 c{0, 0};
    float m = 0.f;
    for (auto& sb : world.softBodies()) {
        if (sb->tag != tag) continue;
        float tm = 0.f;
        for (auto& p : sb->points) tm += p.mass;
        if (tm < 1e-8f) continue;
        c += sb->centroid() * tm;
        m += tm;
    }
    return m > 1e-8f ? c / m : Vec2{0, 0};
}

int Slime::playerBlobCount(const World& world, int tag) {
    int n = 0;
    for (auto& sb : world.softBodies())
        if (sb->tag == tag) ++n;
    return n;
}

void Slime::applyFragmentGather(float dt, World& world, bool gatherHeld) {
    if (!gatherHeld) return;
    if (playerBlobCount(world, myTag_) <= 1) return;
    Vec2 c = playerMassCentroid(world, myTag_);
    constexpr float kGather = 92.f;
    constexpr float kFarCap = 8.f;
    for (auto& sbPtr : world.softBodies()) {
        SoftBody& sb = *sbPtr;
        if (sb.tag != myTag_) continue;
        for (auto& p : sb.points) {
            Vec2 d = c - p.pos;
            float len = d.len();
            if (len < 0.018f) continue;
            Vec2 dir = d * (1.f / len);
            float w = std::min(len, kFarCap);
            p.vel += dir * (kGather * dt * w);
        }
    }
}

void Slime::applySpikeHazard(float dt, World& world) {
    constexpr float kMinPen = 0.019f;
    constexpr int kMaxEmbedded = 8;
    constexpr float kSpikeAddInterval = 0.42f;
    constexpr float kDeepEnough = 0.031f;
    Vec2 nAcc{0, 0};
    bool touching = false;
    float deepest = 0.f;

    float vr = visualRadius_;
    for (auto& sb : world.softBodies()) {
        if (sb->tag != myTag_) continue;
        Vec2 cc = sb->centroid();
        for (auto& q : sb->points) vr = std::max(vr, distance(q.pos, cc));
    }

    for (auto& sb : world.softBodies()) {
        if (sb->tag != myTag_) continue;
        for (auto& pt : sb->points) {
            if (pt.pinned) continue;
            for (auto& hb : world.bodies()) {
                if (hb->tag != spikeHazardTag) continue;
                Vec2 n, closest;
                float depth;
                if (!pointVsBody(pt.pos, *hb, n, depth, closest)) continue;
                if (depth < kMinPen) continue;
                touching = true;
                deepest = std::max(deepest, depth);
                nAcc += n;
                float vn = dot(pt.vel, n);
                if (vn < -0.04f)
                    pt.vel -= n * vn * std::min(1.f, 14.f * dt);
                pt.vel *= std::max(0.f, 1.f - 2.4f * dt);
                float push = std::min(depth, 0.11f) * dt * 3.4f;
                pt.pos += n * push;
                pt.prev += n * push;
            }
        }
    }

    if (touching && embeddedSpikes_.size() < (size_t)kMaxEmbedded && deepest >= kDeepEnough) {
        spikeStickTimer_ += dt;
        while (spikeStickTimer_ >= kSpikeAddInterval &&
               embeddedSpikes_.size() < (size_t)kMaxEmbedded) {
            spikeStickTimer_ -= kSpikeAddInterval;
            float ang = (float)embeddedSpikes_.size() * 2.5132742f + rnd01() * 0.28f;
            Vec2 dir{std::cos(ang), std::sin(ang)};
            float rad = vr * (0.72f + rnd01() * 0.16f);
            Vec2 radial = dir * rad;
            bool dup = false;
            for (const auto& es : embeddedSpikes_) {
                if ((es.radial - radial).lenSq() < 0.048f) {
                    dup = true;
                    break;
                }
            }
            if (!dup)
                embeddedSpikes_.push_back({radial, rnd01() * 6.2831853f});
        }
    } else {
        spikeStickTimer_ = 0.f;
    }

    const float prevDwell = spikeDwell_;
    if (touching) {
        spikeDwell_ += dt;
        if (nAcc.lenSq() > 1e-12f)
            spikeSliceHint_ = nAcc;
    } else {
        if (prevDwell >= 0.17f && spikeSplitCd_ <= 0.f && spikeSliceHint_.lenSq() > 1e-10f) {
            Vec2 hint = spikeSliceHint_.normalized();
            Vec2 axis{-hint.y, hint.x};
            if (world.splitLargestBlobWithTag(myTag_, axis)) {
                spikeSplitCd_ = 2.6f;
                embeddedSpikes_.clear();
            }
        }
        spikeDwell_ = 0.f;
        spikeSliceHint_ = {0.f, 0.f};
    }
}

void Slime::syncEmbeddedSpikes(float dt, const World& world) {
    if (embeddedSpikes_.empty()) return;
    const float targetR = visualRadius_ * 0.90f;
    for (auto& e : embeddedSpikes_) {
        e.phase += dt * 3.8f;
        float L = e.radial.len();
        if (L < 1e-4f) continue;
        Vec2 dir = e.radial * (1.f / L);
        float newL = L + (targetR - L) * std::min(1.f, dt * 5.f);
        Vec2 tang{-dir.y, dir.x};
        e.radial = dir * newL +
                   tang * (0.022f * visualRadius_ * std::sin(e.phase * 2.8f));
    }
    (void)world;
}

void Slime::embeddedSpikeDrawOffsets(const World& world, std::vector<Vec2>& outOffsets) const {
    outOffsets.clear();
    outOffsets.reserve(embeddedSpikes_.size());
    for (const auto& e : embeddedSpikes_) {
        Vec2 r = e.radial;
        float L = r.len();
        if (L < 1e-4f) continue;
        Vec2 dir = r * (1.f / L);
        float w = std::sin(e.phase * 1.65f) * 0.07f * visualRadius_;
        outOffsets.push_back(dir * L + Vec2{-dir.y, dir.x} * w);
    }
    (void)world;
}

void Slime::emitLandingTrail(World& world, float impactSpeed) {
    if (impactSpeed < 1.6f) return;

    // Collect slime points on upward-facing support (static terrain or crates/balls).
    struct Contact { Vec2 surface; };
    std::vector<Contact> contacts;
    contacts.reserve(32);
    for (auto& sb : world.softBodies()) {
        if (sb->tag != myTag_) continue;
        for (auto& pt : sb->points) {
            for (auto& b : world.bodies()) {
                if (b->tag == spikeHazardTag) continue;
                Vec2 n, closest;
                float depth;
                Vec2 probe = pt.pos + Vec2{0.f, 0.06f};
                if (!pointVsBody(probe, *b, n, depth, closest)) continue;
                if (n.y > -0.42f) continue;
                const bool support = b->type == BodyType::Static ||
                    (b->type == BodyType::Dynamic &&
                     (b->tag == crateTag || b->tag == ballTag));
                if (!support) continue;
                contacts.push_back({closest});
                break;
            }
        }
    }
    if (contacts.empty()) return;

    // Drop a vertical probe at world-x `x`, starting at `yFrom` and scanning
    // downward (y increases) until it hits the top of any STATIC body. Static
    // only — dynamic props move and leave floaty drops behind. Returns the
    // surface y, or nullopt if nothing within reach.
    auto findGroundY = [&](float x, float yFrom) -> std::optional<float> {
        constexpr float kMaxScan = 0.9f;
        constexpr float kStep = 0.04f;
        for (float dy = -0.05f; dy <= kMaxScan; dy += kStep) {
            Vec2 probe{x, yFrom + dy};
            for (auto& b : world.bodies()) {
                if (b->tag == spikeHazardTag) continue;
                if (b->type != BodyType::Static) continue;
                Vec2 n, closest;
                float depth;
                if (!pointVsBody(probe, *b, n, depth, closest)) continue;
                if (n.y > -0.55f) continue;
                return closest.y;
            }
        }
        return std::nullopt;
    };

    // Big splat first — averaged contact point, scaled by impact speed.
    Vec2 avg{0, 0};
    for (auto& cp : contacts) avg += cp.surface;
    avg = avg * (1.f / (float)contacts.size());
    SlimePuddle splat;
    splat.pos = avg + Vec2{0.f, -0.02f};
    splat.radius = std::clamp(0.13f + 0.025f * impactSpeed, 0.13f, 0.42f);
    splat.alpha = 1.0f;
    puddles_.push_back(splat);

    // Drops scattered around real contact points — random radii, random angles.
    int count = std::clamp((int)(impactSpeed * 2.2f), 5, 18);
    for (int i = 0; i < count; ++i) {
        const Vec2& cp = contacts[(size_t)((int)(rnd01() * contacts.size())
                                            % (int)contacts.size())].surface;
        float px = cp.x + rnd11() * 0.25f;
        auto sy = findGroundY(px, cp.y - 0.2f);
        if (!sy) continue;
        SlimePuddle d;
        d.pos.x = px;
        d.pos.y = *sy - 0.01f - rnd01() * 0.04f;
        d.radius = 0.06f + 0.10f * rnd01();
        d.alpha = 0.85f + 0.15f * rnd01();
        puddles_.push_back(d);
    }

    // Tiny satellite dots ringing the splat — extra "splatter" detail.
    int sat = std::clamp((int)(impactSpeed * 0.8f), 2, 8);
    for (int i = 0; i < sat; ++i) {
        float ang = rnd01() * 6.2832f;
        float dist = splat.radius * (1.05f + rnd01() * 0.6f);
        float px = avg.x + std::cos(ang) * dist;
        auto sy = findGroundY(px, avg.y - 0.2f);
        if (!sy) continue;
        SlimePuddle d;
        d.pos.x = px;
        d.pos.y = *sy - 0.012f - rnd01() * 0.03f;
        d.radius = 0.03f + 0.04f * rnd01();
        d.alpha = 0.7f + 0.25f * rnd01();
        puddles_.push_back(d);
    }
}

void Slime::emitContinuousTrail(float dt, World& world) {
    trailEmitCd_ = std::max(0.f, trailEmitCd_ - dt);
    if (trailEmitCd_ > 0.f) return;

    // Drop only on static terrain. Dynamic props (crates/balls) move out from
    // under their drops and leave them floating mid-air, so they're excluded.
    float bestY = -1e30f;
    Vec2 bestSurface{0.f, 0.f};
    bool found = false;
    for (auto& sb : world.softBodies()) {
        if (sb->tag != myTag_) continue;
        for (auto& pt : sb->points) {
            for (auto& b : world.bodies()) {
                if (b->tag == spikeHazardTag) continue;
                if (b->type != BodyType::Static) continue;
                Vec2 n, closest;
                float depth;
                Vec2 probe = pt.pos + Vec2{0.f, 0.06f};
                if (!pointVsBody(probe, *b, n, depth, closest)) continue;
                if (n.y > -0.55f) continue;     // stricter "upward-facing" gate
                // Distance from the slime point down to that surface — if the
                // surface is far below the point, the point isn't actually on
                // it (probe just clipped a passing body); skip.
                if (closest.y - pt.pos.y > 0.18f) continue;
                if (pt.pos.y > bestY) {
                    bestY = pt.pos.y;
                    bestSurface = closest;
                    found = true;
                }
                break;
            }
        }
    }
    if (!found) return;

    SlimePuddle d;
    d.pos.x = bestSurface.x + rnd11() * 0.045f;
    d.pos.y = bestSurface.y - 0.012f;
    d.radius = 0.055f + 0.034f * rnd01();
    d.alpha = 0.68f + 0.28f * rnd01();
    puddles_.push_back(d);

    // Emission rate: denser drip so the streak reads clearly; charging still
    // slows ooze a bit. Fast travel tightens cooldown for continuity.
    float speed = playerVel_.len();
    float baseCd = charging_ ? 0.19f : 0.095f;
    float speedScale = std::clamp(1.f - speed * 0.045f, 0.42f, 1.f);
    trailEmitCd_ = baseCd * speedScale;
}

void Slime::applyTrailGlue(World& world) {
    // Any dynamic crate / ball whose centre passes near a puddle gets coated:
    // the slimeGlueTimer makes its next contacts low-bounce + high-friction
    // (already wired in Body / SoftBody / Collision). This is what makes the
    // trail "sticky to objects" — props that roll through the goo lose energy
    // and stick instead of bouncing freely.
    if (puddles_.empty()) return;
    constexpr float kCoatPad = 0.30f;
    constexpr float kCoatDuration = 2.0f;
    for (auto& bp : world.bodies()) {
        Body& B = *bp;
        if (B.type != BodyType::Dynamic) continue;
        if (B.tag != crateTag && B.tag != ballTag) continue;
        float bodyR = (B.shape.type == ShapeType::Circle)
                          ? B.shape.radius
                          : B.shape.radius * 0.85f;
        for (const auto& pud : puddles_) {
            if (pud.alpha < 0.12f) continue;
            float reach = pud.radius + bodyR + kCoatPad;
            if ((B.pos - pud.pos).lenSq() <= reach * reach) {
                if (B.slimeGlueTimer < kCoatDuration)
                    B.slimeGlueTimer = kCoatDuration;
                break;
            }
        }
    }
}

void Slime::fadeTrail(float dt) {
    const float fadeRate = 0.095f;  // alpha / sec — goo lingers longer on terrain
    for (size_t i = 0; i < puddles_.size();) {
        puddles_[i].alpha -= dt * fadeRate;
        if (puddles_[i].alpha <= 0.f) {
            puddles_[i] = puddles_.back();
            puddles_.pop_back();
        } else {
            ++i;
        }
    }
    if (puddles_.size() > 320) {
        puddles_.erase(puddles_.begin(), puddles_.begin() + (puddles_.size() - 320));
    }
}

void Slime::launch(World& world, const Vec2& aimDir) {
    float t = std::clamp(chargeTimer_ / chargeMaxTime, 0.f, 1.f);
    // Ease-out cubic on the charge curve — most of the force is in the early
    // half-second, then the curve flattens. Feels like a real charge "ramping
    // up": light press → quick small hops, full hold → bigger but not 5× bigger.
    float te = 1.f - (1.f - t) * (1.f - t) * (1.f - t);

    if (!embeddedSpikes_.empty()) {
        const float t01 = std::clamp(te, 0.f, 1.f);
        size_t want = (size_t)std::floor((1.f - t01) * (float)embeddedSpikes_.size());
        if (t01 >= 0.98f) want = 0;
        while (embeddedSpikes_.size() > want)
            embeddedSpikes_.pop_back();
    }

    float forceMag = jumpForceMin + (jumpForceMax - jumpForceMin) * te;

    // Jitter: random angle + magnitude → less predictable arc each jump.
    float angle = std::atan2(aimDir.y, aimDir.x) + rnd11() * angleJitterRad;
    float magScale = 1.f + rnd11() * forceJitterFrac;
    Vec2 vel{std::cos(angle) * forceMag * magScale,
             std::sin(angle) * forceMag * magScale};

    // Off-centre impulse: pick a random sideways offset → blob spins in flight.
    float spinSign = (rnd11() >= 0.f) ? 1.f : -1.f;
    float spinMag = launchSpinMax * (0.4f + 0.6f * t);

    for (auto& sb : world.softBodies()) {
        if (sb->tag != myTag_) continue;
        Vec2 cc = sb->centroid();
        for (auto& p : sb->points) {
            // Linear: every point gets the launch velocity.
            p.vel += vel;
            // Angular: tangential kick perpendicular to (p − centroid).
            Vec2 r = p.pos - cc;
            // perp(r) = (−r.y, r.x); rotate by spinSign to flip direction.
            Vec2 tangent{-r.y * spinSign, r.x * spinSign};
            p.vel += tangent * spinMag;
        }
        sb->pressureTarget = basePressure_ * launchPuffFactor;
    }
    charging_ = false;
    chargeTimer_ = 0.f;
    coyoteTimer_ = 0.f;
    chargeBufferTimer_ = 0.f;
    jumpCooldownRemaining_ = jumpCooldownDuration;
}

void Slime::updateGrabThrow(float dt, World& world, bool grabHeld) {
    Vec2 c = playerMassCentroid(world, myTag_);
    auto findBody = [&](uint32_t id) -> Body* {
        for (auto& bp : world.bodies()) {
            if (bp->id == id && bp->type == BodyType::Dynamic)
                return bp.get();
        }
        return nullptr;
    };

    Vec2 slimeVel{0.f, 0.f};
    float slimeMass = 0.f;
    for (auto& sb : world.softBodies()) {
        if (sb->tag != myTag_) continue;
        for (auto& p : sb->points) {
            slimeVel += p.vel * p.mass;
            slimeMass += p.mass;
        }
    }
    if (slimeMass > 1e-8f) slimeVel *= (1.f / slimeMass);

    constexpr float kGrabReach = 5.4f;
    constexpr float kHoldBreak = 9.5f;
    constexpr float kPull = 230.f;
    constexpr float kDamp = 44.f;
    constexpr float kThrowVel = 14.5f;
    constexpr float kThrowSpin = 4.2f;
    constexpr float kGlueSeconds = 2.35f;
    constexpr float kGrabForceMax = 4200.f;
    constexpr float kAimBonus = 1.4f;     // forward-of-aim weighting in selection
    // Hold the prop just outside the slime silhouette (no more clipping into the body).
    const float holdDist = std::max(0.32f, visualRadius_ + 0.22f);

    if (!grabHeld) {
        if (grabBodyId_ != 0) {
            Body* b = findBody(grabBodyId_);
            if (b && (b->tag == crateTag || b->tag == ballTag)) {
                Vec2 d = aimDir_;
                if (d.lenSq() < 1e-6f) d = {0.f, -1.f};
                else d = d.normalized();
                // Carry slime motion so the throw feels attached, then slime-coating on impact.
                b->vel += slimeVel * 0.65f;
                b->applyImpulse(d * kThrowVel);
                b->applyImpulseAt(Vec2{-d.y, d.x} * (kThrowSpin * 0.28f), c);
                b->slimeGlueTimer = kGlueSeconds;
            }
            grabBodyId_ = 0;
        }
        wasGrabHeld_ = false;
        return;
    }

    Body* held = findBody(grabBodyId_);
    if (!wasGrabHeld_ || !held) {
        grabBodyId_ = 0;
        // Score = squared distance − aim-cone bonus. Picks the prop the player
        // is *aiming at*, not just the geometrically nearest one — fixes E
        // randomly grabbing a crate behind you when several are in reach.
        float best = std::numeric_limits<float>::infinity();
        Body* pick = nullptr;
        for (auto& bp : world.bodies()) {
            Body& B = *bp;
            if (B.type != BodyType::Dynamic) continue;
            if (B.tag != crateTag && B.tag != ballTag) continue;
            if (B.grabOwnerTag != 0 && B.grabOwnerTag != myTag_) continue;
            Vec2 toB = B.pos - c;
            float d2 = toB.lenSq();
            if (d2 > kGrabReach * kGrabReach) continue;
            float aimDot = 0.f;
            float L = std::sqrt(d2);
            if (L > 1e-4f) aimDot = dot(toB / L, aimDir_);
            float score = d2 - kAimBonus * std::max(0.f, aimDot) * (kGrabReach * kGrabReach);
            if (score < best) {
                best = score;
                pick = &B;
            }
        }
        if (pick) {
            pick->slimeGlueTimer = 0.f;
            grabBodyId_ = pick->id;
        }
        held = findBody(grabBodyId_);
    }
    wasGrabHeld_ = true;
    if (!held) {
        grabBodyId_ = 0;
        return;
    }
    if ((held->pos - c).lenSq() > kHoldBreak * kHoldBreak) {
        grabBodyId_ = 0;
        return;
    }

    // Hold point sits just outside the blob along aimDir → object visibly
    // "leads" with the slime instead of clipping into it. Spring chases it.
    Vec2 holdTarget = c + aimDir_ * holdDist;
    Vec2 err = holdTarget - held->pos;
    Vec2 relV = held->vel - slimeVel;
    Vec2 F = err * kPull - relV * kDamp;
    float f2 = F.lenSq();
    if (f2 > kGrabForceMax * kGrabForceMax) F *= kGrabForceMax / std::sqrt(f2);
    held->applyForce(F);
    // Stronger angular brake so heavy crates settle quickly when held.
    held->angVel *= std::exp(-12.f * dt);
    held->grabOwnerTag = myTag_;
}

void Slime::update(float dt, World& world, const Vec2& aimWorld, bool jumpHeld, bool grabHeld,
                   bool gatherHeld, bool shiftSplitClick) {
    spikeSplitCd_ = std::max(0.f, spikeSplitCd_ - dt);
    jumpCooldownRemaining_ = std::max(0.f, jumpCooldownRemaining_ - dt);

    if (shiftSplitClick && playerBlobCount(world, myTag_) > 0) {
        Vec2 c = playerMassCentroid(world, myTag_);
        world.playerSplitLargestBlobWithTag(myTag_, aimWorld - c);
    }

    applyFragmentGather(dt, world, gatherHeld);
    applySpikeHazard(dt, world);
    syncEmbeddedSpikes(dt, world);

    // Compute aim direction from current player centroid → mouse.
    {
        Vec2 c = playerMassCentroid(world, myTag_);
        Vec2 d = aimWorld - c;
        float L = d.len();
        aimDir_ = (L > 1e-3f) ? d / L : Vec2{0.f, -1.f};
    }

    updateGrabThrow(dt, world, grabHeld);

    // Track fall speed each frame — used for landing-impact trail intensity.
    {
        float maxVy = 0.f;
        for (auto& sb : world.softBodies()) {
            if (sb->tag != myTag_) continue;
            for (auto& p : sb->points) maxVy = std::max(maxVy, p.vel.y);
        }
        if (!grounded_) lastFallSpeed_ = std::max(lastFallSpeed_, maxVy);
    }

    // Ground / wall contact: feet on a flat top, or braced against a static wall/slope (not ceiling).
    bool footGrounded = false;
    bool wallBraced = false;
    for (auto& sb : world.softBodies()) {
        if (sb->tag != myTag_) continue;
        if (blobOnSupport(*sb, world)) footGrounded = true;
        if (blobWallBrace(*sb, world) || blobGlueSurfaceBrace(*sb, world)) wallBraced = true;
        if (footGrounded && wallBraced) break;
    }
    const bool braced = footGrounded || wallBraced;
    // Landing transition — emit splat trail proportional to fall speed (floor contacts only).
    if (footGrounded && !wasFootGrounded_) {
        emitLandingTrail(world, lastFallSpeed_);
        lastFallSpeed_ = 0.f;
    }
    wasFootGrounded_ = footGrounded;

    grounded_ = braced;
    if (footGrounded) coyoteTimer_ = coyoteTime;
    else coyoteTimer_ = std::max(0.f, coyoteTimer_ - dt);

    // Continuous slime drip (wall or floor), and stick-coating for props in the trail.
    if (braced) emitContinuousTrail(dt, world);
    else trailEmitCd_ = std::max(trailEmitCd_, 0.05f);
    applyTrailGlue(world);

    fadeTrail(dt);

    const bool canCharge = braced || coyoteTimer_ > 0.f;

    // Buffer: pressing in the air → start charge as soon as you land / brace.
    if (jumpHeld && !canCharge) {
        chargeBufferTimer_ = chargeBufferTime;
    } else {
        chargeBufferTimer_ = std::max(0.f, chargeBufferTimer_ - dt);
    }

    const bool jumpReady = jumpCooldownRemaining_ <= 0.f;

    // Start charging when held + grounded (or buffered hold landing).
    if (jumpHeld && canCharge && jumpReady && !charging_) {
        charging_ = true;
        chargeTimer_ = 0.f;
    }

    // While charging: build up timer and squash the blob (pressure dip).
    if (charging_) {
        chargeTimer_ = std::min(chargeTimer_ + dt, chargeMaxTime);
        float t = chargeTimer_ / chargeMaxTime;
        for (auto& sb : world.softBodies()) {
            if (sb->tag != myTag_) continue;
            sb->pressureTarget = basePressure_ * (1.f - chargeSquashAmount * t);
        }
        // Auto-launch if max charge reached (stops infinite hold).
        if (chargeTimer_ >= chargeMaxTime - 1e-4f && jumpHeld) {
            launch(world, aimDir_);
        }
        // Manual launch on release.
        else if (!jumpHeld) {
            launch(world, aimDir_);
        }
        // Walked off ledge / lost wall contact while charging → cancel cleanly (no launch).
        else if (!canCharge) {
            charging_ = false;
            chargeTimer_ = 0.f;
            for (auto& sb : world.softBodies()) {
                if (sb->tag != myTag_) continue;
                sb->pressureTarget = basePressure_;
            }
        }
    }

    // Pressure relaxes back to base whenever not charging.
    if (!charging_) {
        for (auto& sb : world.softBodies()) {
            if (sb->tag != myTag_) continue;
            float k = std::min(1.f, dt * 4.f);
            sb->pressureTarget += (basePressure_ - sb->pressureTarget) * k;
        }
    }

    // ----- Eye physics: virtual point-masses pulled toward blob anchors. -----
    SoftBody* blob = nullptr;
    for (auto& sb : world.softBodies()) {
        if (sb->tag == myTag_) { blob = sb.get(); break; }
    }
    if (blob && (int)blob->points.size() > 4) {
        const int n = (int)blob->points.size();
        const int rIdx = std::max(1, n / 8);
        const int lIdx = ((3 * n) / 8) % n;
        const Vec2 cc = blob->centroid();

        float maxR = 0.f;
        for (auto& p : blob->points) maxR = std::max(maxR, distance(p.pos, cc));
        visualRadius_ = maxR;

        Vec2 vSum{0, 0}; float mSum = 0.f;
        for (auto& p : blob->points) { vSum += p.vel * p.mass; mSum += p.mass; }
        playerVel_ = (mSum > 1e-8f) ? vSum * (1.f / mSum) : Vec2{0, 0};

        Vec2 lTarget = cc + (blob->points[(size_t)lIdx].pos - cc) * 0.55f;
        Vec2 rTarget = cc + (blob->points[(size_t)rIdx].pos - cc) * 0.55f;

        if (!eyesInit_) {
            leftEyePos_ = lTarget;
            rightEyePos_ = rTarget;
            leftEyeVel_ = {0, 0};
            rightEyeVel_ = {0, 0};
            eyesInit_ = true;
        } else {
            // Spring + damping → eyes lag the blob, overshoot on direction
            // changes, then settle. k high = snappy, damp tuned ~ critical.
            const float k = 240.f;
            const float damp = 24.f;
            Vec2 lF = (lTarget - leftEyePos_) * k - leftEyeVel_ * damp;
            Vec2 rF = (rTarget - rightEyePos_) * k - rightEyeVel_ * damp;
            leftEyeVel_ += lF * dt;
            rightEyeVel_ += rF * dt;
            leftEyePos_ += leftEyeVel_ * dt;
            rightEyePos_ += rightEyeVel_ * dt;

            // Keep eyes inside the blob so overshoot can't escape the silhouette.
            auto clampInside = [&](Vec2& ep, Vec2& ev) {
                Vec2 d = ep - cc;
                float L = d.len();
                float maxLen = visualRadius_ * 0.62f;
                if (L > maxLen && L > 1e-5f) {
                    Vec2 dir = d / L;
                    ep = cc + dir * maxLen;
                    float vRad = dot(ev, dir);
                    if (vRad > 0.f) ev -= dir * vRad; // bleed outward velocity
                }
            };
            clampInside(leftEyePos_, leftEyeVel_);
            clampInside(rightEyePos_, rightEyeVel_);
        }
    } else {
        eyesInit_ = false;
    }
}

} // namespace pe
