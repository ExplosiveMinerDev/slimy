#pragma once
#include "math/Vec2.h"
#include <cstdint>
#include <vector>

namespace pe {

class World;
class SoftBody;

/// Decal left on the ground while the slime moves (visual only).
struct SlimePuddle {
    Vec2 pos{};
    float radius = 0.16f;
    float alpha = 1.f;
};

/// Player slime: charge-based jump movement (no walking).
/// Hold space to charge → release to launch. A/D held during release sets
/// horizontal launch direction. Force scales linearly from `min` to `max`
/// over `chargeMaxTime` seconds of holding.
class Slime {
public:
    static constexpr int playerTag = 1;
    /// Server gives each connected player a distinct soft-body tag: `playerTag + slot * stride`.
    static constexpr int playerBlobTagStride = 100;
    static constexpr int networkedPlayerBlobTag(int slot) {
        return playerTag + slot * playerBlobTagStride;
    }

    static constexpr int spikeHazardTag = 2;
    static constexpr int grassTag = 3;
    static constexpr int stoneTag = 4;
    static constexpr int platformTag = 5;
    static constexpr int crateTag = 6;
    static constexpr int ballTag = 7;
    /// Solides décor / tests carte (ex. caillou rouge dans default.sjmap).
    static constexpr int mapTestRockTag = 8;
    static constexpr int airVentTag = 9;

    /// Launch velocity magnitudes (world units / s) — applied along the aim direction.
    /// (Tuned at ~80% of the original feel — momentum reduced by 20%.)
    float jumpForceMin = 6.2f;
    float jumpForceMax = 18.5f;
    /// Seconds held to reach max charge.
    float chargeMaxTime = 0.6f;
    /// While charging, pressure dips this fraction → visible pre-jump squash.
    float chargeSquashAmount = 0.55f;
    /// Brief pressure spike on launch → visible mid-air inflate.
    float launchPuffFactor = 1.35f;
    /// Launch unpredictability — random angle (radians) and force scale jitter.
    float angleJitterRad = 0.14f;       // ≈ ±8° (was 10°)
    float forceJitterFrac = 0.12f;      // ±12% magnitude (was 16%)
    /// Off-centre impulse: applies a torque-like twist to the blob on launch (rad/s).
    /// Reduced 20% along with the linear forces above.
    float launchSpinMax = 7.6f;

    /// Coyote / buffer windows for forgiving timing on edges.
    float coyoteTime = 0.10f;
    float chargeBufferTime = 0.12f;
    /// Seconds after a launch before a new charge can start (anti spam-jump).
    float jumpCooldownDuration = 1.f;

    /// Re-merge fragments handler reads this externally.
    float basePressureTarget() const { return basePressure_; }
    uint8_t colorIndex() const { return colorIndex_; }
    void setColorIndex(World& world, uint8_t colorIndex);
    void cycleColor(World& world);
    bool cycleControlledFragment(World& world, int dir = 1);

    /// `tag` lets multi-player setups give each Slime instance a unique blob tag.
    void spawn(World& world, const Vec2& pos, float radius = 0.9f, int segments = 22,
               int tag = playerTag);
    /// aimWorld: world-space target the slime should jump toward (typically the mouse
    /// cursor position). Direction = (aimWorld − slime centroid). Up == −y.
    /// jumpHeld: charge while held, launch on release.
    /// gatherHeld: Ctrl — pull fragments toward mass centroid (multi-blob only).
    /// shiftSplitClick: Shift+LMB edge — slice largest blob toward the mouse aim.
    void update(float dt, World& world, const Vec2& aimWorld, bool jumpHeld, bool grabHeld = false,
                bool gatherHeld = false, bool shiftSplitClick = false);

    bool grounded() const { return grounded_; }
    bool charging() const { return charging_; }
    /// 0..1, fraction of max charge reached.
    float chargeFraction() const { return chargeTimer_ / chargeMaxTime; }
    /// Last-known aim direction (unit) — renderer uses it to draw the arrow.
    Vec2 aimDir() const { return aimDir_; }
    /// Physics-driven eye world positions — lag the blob, overshoot on impact.
    Vec2 leftEye() const { return leftEyePos_; }
    Vec2 rightEye() const { return rightEyePos_; }
    /// Mean perimeter "radius" (max distance centroid → vertex). 0 if no blob.
    float visualRadius() const { return visualRadius_; }
    /// Mass-weighted velocity of the player slime (world units / s).
    Vec2 playerVelocity() const { return playerVel_; }
    bool eyesValid() const { return eyesInit_; }

    const std::vector<SlimePuddle>& puddles() const { return puddles_; }

    static Vec2 playerMassCentroid(const World& world, int tag = playerTag);
    static Vec2 playerControlledCentroid(const World& world, int tag = playerTag);
    static int playerBlobCount(const World& world, int tag = playerTag);

    int myTag() const { return myTag_; }

    /// Pics métalliques coincés dans le corps (sync réseau via `stuckSpikeCount`).
    size_t stuckSpikeCount() const { return embeddedSpikes_.size(); }
    /// Offsets depuis le centroïde pour le rendu (solo / prédiction locale).
    void embeddedSpikeDrawOffsets(const World& world, std::vector<Vec2>& outOffsets) const;

private:
    void applyFragmentGather(float dt, World& world, bool gatherHeld);
    void mergeGatheredFragmentsOnContact(World& world, bool gatherHeld);
    void applySpikeHazard(float dt, World& world);
    void applyEnvironmentProps(float dt, World& world);
    void syncEmbeddedSpikes(float dt, const World& world);
    void updateGrabThrow(float dt, World& world, bool grabHeld);
    void launch(World& world, const Vec2& aimDir);
    void emitLandingTrail(World& world, float impactSpeed);
    void emitContinuousTrail(float dt, World& world);
    void applyTrailGlue(World& world);
    void fadeTrail(float dt);
    float restPressureForBlob(const SoftBody& sb, float taggedMass) const;

    int myTag_ = playerTag;
    uint8_t colorIndex_ = 0;
    /// Time spent in spike overlap this snag (seconds) — used for "lose a chunk" on exit.
    float spikeDwell_ = 0.f;
    /// Cooldown after a spike-induced split so grazing doesn't chain-cut.
    float spikeSplitCd_ = 0.f;
    /// Latest summed spike outward normals while snagged (axis for `splitLargestBlobWithTag`).
    Vec2 spikeSliceHint_{0.f, 0.f};

    struct EmbeddedSpike {
        Vec2 radial{}; ///< Depuis le centroïde vers la surface (longueur ~ rayon visuel).
        float phase = 0.f;
    };
    std::vector<EmbeddedSpike> embeddedSpikes_;
    /// While on spikes: timer to add spikes at a steady rate (no RNG spam).
    float spikeStickTimer_ = 0.f;

    uint32_t grabBodyId_ = 0;
    bool wasGrabHeld_ = false;

    bool grounded_ = false;
    bool wasFootGrounded_ = false;
    bool charging_ = false;
    float chargeTimer_ = 0.f;
    float coyoteTimer_ = 0.f;
    float chargeBufferTimer_ = 0.f;
    float basePressure_ = 0.f;
    float lastFallSpeed_ = 0.f;
    /// Cooldown before next continuous-trail puddle drop (seconds).
    float trailEmitCd_ = 0.f;
    float jumpCooldownRemaining_ = 0.f;
    float manualSplitCooldown_ = 0.f;
    Vec2 aimDir_{0.f, -1.f};

    /// Eye state — virtual mass-spring secondary motion. Eye lags behind the blob
    /// anchor → overshoots on direction changes → settles. Pure visual physics.
    bool eyesInit_ = false;
    Vec2 leftEyePos_{0, 0}, leftEyeVel_{0, 0};
    Vec2 rightEyePos_{0, 0}, rightEyeVel_{0, 0};
    float visualRadius_ = 0.9f;
    Vec2 playerVel_{0, 0};

    std::vector<SlimePuddle> puddles_;
};

} // namespace pe
