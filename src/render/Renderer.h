#pragma once
#include "game/Slime.h"
#include "math/Vec2.h"
#include <raylib.h>
#include <string>

namespace pe {

class World;
class SoftBody;
struct Body;

/// `Vsync` — default for solo and online (smooth display + 60 Hz cap). `FastPresent` — no vsync,
/// higher cap; use if you need several game windows on one machine without capping each at ~60.
enum class FramePacing { Vsync, FastPresent };

struct Camera2D {
    Vec2 target{0, 0};
    Vec2 offset{0, 0};
    float zoom = 24.f; // pixels per world unit (internal canvas pixels)
    float rotation = 0.f;
};

/// Passé au constructeur : créer une nouvelle fenêtre Raylib ou réutiliser celle ouverte par le menu.
enum class RendererWindowMode { CreateWindow, UseExistingWindow };

class Renderer {
public:
    /// windowW/H: initial OS window size; internalW/H: fixed low-res game framebuffer (pixel-perfect).
    Renderer(int windowW, int windowH, int internalW, int internalH, const std::string& title,
             FramePacing pacing = FramePacing::Vsync,
             RendererWindowMode windowMode = RendererWindowMode::CreateWindow);
    ~Renderer();

    /// Si la fenêtre a été créée ailleurs (`UseExistingWindow`), ne pas fermer Raylib au destructeur.
    bool ownsWindow() const { return ownsWindow_; }

    bool shouldClose() const;
    void beginFrame();
    void endFrame();

    void drawWorld(const World& world, const std::vector<SlimePuddle>* slimeTrail = nullptr);
    /// Draws an arrow from a world-space origin in `dir` direction. Length scales
    /// with `chargeFrac` (0..1). Used to show the slime's queued jump trajectory.
    void drawAimIndicator(Vec2 worldOrigin, Vec2 dir, float chargeFrac);
    /// Draws the slime's eyes at the given world positions; pupils point along
    /// `vel`. `radius` is the blob's current visual radius (used to size eyes).
    void drawSlimeFace(Vec2 leftEye, Vec2 rightEye, Vec2 vel, float radius);
    /// Speech bubble anchored just above the slime; `worldAnchorBottom` is the tip toward the player.
    void drawSlimeChatBubble(Vec2 worldAnchorBottom, const char* utf8, float alpha01);
    /// Bottom-centred typing bar while composing chat (pixel canvas coords).
    void drawChatTypingBar(const char* draftUtf8);
    /// Draws a remote slime body (perimeter polygon) from raw world-space points.
    /// Used by the client to render slimes received from the server.
    void drawRemoteSlimeBody(const std::vector<Vec2>& worldPoints, bool isLocalPlayer,
                             uint8_t colorIndex = 0);
    /// Small spike triangles stuck on the slime (solo / exact offsets).
    void drawSlimeEmbeddedSpikes(Vec2 centroidWorld, const std::vector<Vec2>& radialOffsets);
    /// Decorative spikes from count only (online snapshots — angles from golden ratio).
    void drawSlimeEmbeddedSpikesApprox(Vec2 centroidWorld, float radiusWorld, int count,
                                       uint32_t patternSalt);
    /// Persistent top banner inside the pixel canvas (e.g. "HOSTING ...").
    /// Drawn between beginFrame()/endFrame() so it's pixel-perfect and scales.
    void drawHUDBanner(const std::string& text);

    /// Mouse mapped into internal canvas pixels (for zoom / pan / picks).
    Vec2 mouseInCanvas() const;

    Vec2 screenToWorld(const Vec2& canvasPx) const;
    Vec2 worldToScreen(const Vec2& world) const;

    Camera2D camera;

    int internalWidth() const { return internalW_; }
    int internalHeight() const { return internalH_; }

private:
    int internalW_ = 0;
    int internalH_ = 0;
    RenderTexture2D canvas_{};
    bool canvasReady_ = false;
    bool ownsWindow_ = true;

    void drawScaledCanvas();
    void drawBody(const Body& b, unsigned int fillColor, unsigned int outlineColor);
    void drawSoftBody(const SoftBody& sb);
    void drawSlimePuddles(const std::vector<SlimePuddle>& puddles);
};

} // namespace pe
