#include "render/Renderer.h"
#include "game/Slime.h"
#include "physics/World.h"
#include "physics/Body.h"
#include "physics/SoftBody.h"
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pe {

namespace {

inline ::Color asColor(unsigned int c) {
    return ::Color{
        (unsigned char)((c >> 24) & 0xFF),
        (unsigned char)((c >> 16) & 0xFF),
        (unsigned char)((c >> 8) & 0xFF),
        (unsigned char)(c & 0xFF)};
}

struct PixelViewport {
    int scale = 1;
    float ox = 0.f;
    float oy = 0.f;
    int destW = 0;
    int destH = 0;
};

PixelViewport computePixelViewport(int internalW, int internalH) {
    PixelViewport vp;
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();
    vp.scale = std::max(1, std::min(sw / internalW, sh / internalH));
    vp.destW = internalW * vp.scale;
    vp.destH = internalH * vp.scale;
    vp.ox = ((float)sw - (float)vp.destW) * 0.5f;
    vp.oy = ((float)sh - (float)vp.destH) * 0.5f;
    return vp;
}

inline int iround(float x) { return (int)std::floor(x + 0.5f); }

constexpr float kTau = 6.2831853f;

} // namespace

Renderer::Renderer(int windowW, int windowH, int internalW, int internalH, const std::string& title,
                   FramePacing pacing, RendererWindowMode windowMode)
    : internalW_(internalW), internalH_(internalH),
      ownsWindow_(windowMode == RendererWindowMode::CreateWindow) {
    if (ownsWindow_) {
        unsigned flags = FLAG_WINDOW_RESIZABLE;
        if (pacing == FramePacing::Vsync)
            flags |= FLAG_VSYNC_HINT;
        SetConfigFlags(flags);
        InitWindow(windowW, windowH, title.c_str());
        SetWindowMinSize(640, 360);
        SetExitKey(KEY_NULL);
        if (pacing == FramePacing::Vsync)
            SetTargetFPS(60);
        else {
            SetTargetFPS(240);
        }
    } else {
        SetWindowTitle(title.c_str());
        if (pacing == FramePacing::Vsync)
            SetTargetFPS(60);
        else
            SetTargetFPS(240);
    }

    canvas_ = LoadRenderTexture(internalW_, internalH_);
    SetTextureFilter(canvas_.texture, TEXTURE_FILTER_POINT);
    SetTextureFilter(GetFontDefault().texture, TEXTURE_FILTER_POINT);
    canvasReady_ = true;

    camera.offset = {(float)internalW_ * 0.5f, (float)internalH_ * 0.5f};
}

Renderer::~Renderer() {
    if (canvasReady_) {
        UnloadRenderTexture(canvas_);
        canvasReady_ = false;
    }
    if (ownsWindow_ && IsWindowReady())
        CloseWindow();
}

bool Renderer::shouldClose() const { return WindowShouldClose(); }

void Renderer::drawScaledCanvas() {
    PixelViewport vp = computePixelViewport(internalW_, internalH_);
    Rectangle source = {0.f, 0.f, (float)internalW_, -(float)internalH_};
    Rectangle dest = {vp.ox, vp.oy, (float)vp.destW, (float)vp.destH};
    DrawTexturePro(canvas_.texture, source, dest, {0.f, 0.f}, 0.f, WHITE);
}

void Renderer::beginFrame() {
    BeginTextureMode(canvas_);

    // Bright daylight sky — 3 simple bands, light blue → soft cyan → cream
    // near horizon. Underground stays a deep slate.
    const ::Color skyTop{122, 178, 232, 255};
    const ::Color skyMid{170, 214, 244, 255};
    const ::Color skyHorizon{212, 232, 248, 255};
    const ::Color voidCol{38, 30, 52, 255};
    const float groundY = 10.f;
    const float groundLineF = (groundY - camera.target.y) * camera.zoom
                              + (float)internalH_ * 0.5f;
    const int line = std::clamp((int)groundLineF, 0, internalH_);

    if (line > 0) {
        const int b1 = line / 3;
        const int b2 = (line * 2) / 3;
        DrawRectangle(0, 0, internalW_, b1, skyTop);
        DrawRectangle(0, b1, internalW_, b2 - b1, skyMid);
        DrawRectangle(0, b2, internalW_, line - b2, skyHorizon);
    }
    if (line < internalH_) {
        DrawRectangle(0, line, internalW_, internalH_ - line, voidCol);
    }

    // Soft sun — one round disc, fixed-canvas anchor with mild parallax.
    {
        int sunX = internalW_ * 4 / 5 - (int)(camera.target.x * 0.05f * camera.zoom);
        sunX = ((sunX % internalW_) + internalW_) % internalW_;
        int sunY = internalH_ / 6 - (int)(camera.target.y * 0.03f * camera.zoom);
        sunY = std::clamp(sunY, 12, internalH_ / 3);
        DrawCircle(sunX, sunY, 10.f, ::Color{255, 240, 200, 90});
        DrawCircle(sunX, sunY, 7.f,  ::Color{255, 252, 230, 255});
    }

    camera.offset = {(float)internalW_ * 0.5f, (float)internalH_ * 0.5f};
}

void Renderer::endFrame() {
    EndTextureMode();
    BeginDrawing();
    ClearBackground(::Color{14, 12, 22, 255});
    drawScaledCanvas();
    EndDrawing();
}

Vec2 Renderer::mouseInCanvas() const {
    PixelViewport vp = computePixelViewport(internalW_, internalH_);
    Vector2 m = GetMousePosition();
    float vx = (m.x - vp.ox) / (float)vp.scale;
    float vy = (m.y - vp.oy) / (float)vp.scale;
    return {vx, vy};
}

Vec2 Renderer::worldToScreen(const Vec2& w) const {
    // Snap scroll to whole pixels while keeping smooth camera.target — avoids scene shimmer + cam jitter.
    float sx = std::floor(camera.target.x * camera.zoom + 0.5f);
    float sy = std::floor(camera.target.y * camera.zoom + 0.5f);
    float x = w.x * camera.zoom - sx + camera.offset.x;
    float y = w.y * camera.zoom - sy + camera.offset.y;
    return {(float)iround(x), (float)iround(y)};
}

Vec2 Renderer::screenToWorld(const Vec2& canvasPx) const {
    float sx = std::floor(camera.target.x * camera.zoom + 0.5f);
    float sy = std::floor(camera.target.y * camera.zoom + 0.5f);
    return {
        (canvasPx.x - camera.offset.x + sx) / camera.zoom,
        (canvasPx.y - camera.offset.y + sy) / camera.zoom};
}

namespace {

/// Single opaque fan + crisp outline + tiny spec pixels (no translucent overlay —
/// second fans / alpha circles caused dirty greys on concave blobs).
void drawSlimyBlob(const std::vector<Vector2>& poly, int n, int cx, int cy, float zoom,
                  ::Color fillBody, ::Color outline) {
    std::vector<Vector2> fan;
    fan.reserve((size_t)n + 2);
    fan.push_back({(float)cx, (float)cy});
    for (int i = 0; i < n; ++i) fan.push_back(poly[(size_t)i]);
    fan.push_back(poly[0]);
    DrawTriangleFan(fan.data(), (int)fan.size(), fillBody);

    const ::Color rimHi{92, 215, 130, 255};
    for (int i = 0; i < n; ++i) {
        const Vector2& a = poly[(size_t)i];
        const Vector2& b = poly[(size_t)((i + 1) % n)];
        int my = (iround(a.y) + iround(b.y)) / 2;
        if (my < cy + 1)
            DrawLine((int)a.x, (int)a.y, (int)b.x, (int)b.y, rimHi);
        else
            DrawLine((int)a.x, (int)a.y, (int)b.x, (int)b.y, outline);
    }

    const int ox = std::max(2, iround(2.f + zoom * 0.08f));
    const int oy = std::max(2, iround(2.f + zoom * 0.07f));
    DrawPixel(cx - ox, cy - oy, ::Color{200, 255, 210, 255});
    DrawPixel(cx - ox + 1, cy - oy, ::Color{240, 255, 248, 255});
    DrawPixel(cx - ox, cy - oy + 1, ::Color{150, 235, 175, 255});
    DrawPixel(cx - ox - 1, cy - oy, ::Color{120, 210, 150, 255});
}

struct BodyStyle {
    ::Color fill, outline, accent, deep;
    bool hasGrassTop = false;
    bool isCircleMetal = false;
    bool isCrateWood = false;
};

BodyStyle styleForBody(const Body& b) {
    BodyStyle s;
    if (b.tag == Slime::spikeHazardTag) {
        s.fill = {142, 48, 72, 255};
        s.outline = {62, 22, 36, 255};
        s.accent = {255, 210, 118, 255};
        s.deep = {88, 32, 52, 255};
    } else if (b.tag == Slime::grassTag) {
        s.fill = {110, 78, 46, 255};       // warm dirt/earth tone for daylight
        s.outline = {52, 32, 18, 255};
        s.accent = {148, 220, 96, 255};    // bright spring-grass green
        s.deep = {72, 50, 28, 255};
        s.hasGrassTop = true;
    } else if (b.tag == Slime::stoneTag) {
        s.fill = {72, 78, 98, 255};
        s.outline = {26, 30, 42, 255};
        s.accent = {128, 138, 168, 255};
        s.deep = {42, 48, 64, 255};
    } else if (b.tag == Slime::platformTag) {
        s.fill = {118, 78, 48, 255};
        s.outline = {44, 28, 16, 255};
        s.accent = {200, 158, 108, 255};
        s.deep = {72, 48, 30, 255};
        s.hasGrassTop = true;
    } else if (b.tag == Slime::crateTag) {
        s.fill = {184, 124, 70, 255};
        s.outline = {72, 40, 16, 255};
        s.accent = {238, 196, 132, 255};
        s.deep = {110, 70, 36, 255};
        s.isCrateWood = true;
    } else if (b.tag == Slime::ballTag) {
        s.fill = {178, 184, 198, 255};
        s.outline = {52, 56, 70, 255};
        s.accent = {244, 248, 255, 255};
        s.deep = {88, 92, 108, 255};
        s.isCircleMetal = true;
    } else if (b.tag == Slime::mapTestRockTag) {
        s.fill = {188, 72, 62, 255};
        s.outline = {92, 28, 24, 255};
        s.accent = {236, 152, 132, 255};
        s.deep = {118, 42, 38, 255};
    } else if (b.type == BodyType::Static) {
        s.fill = {84, 88, 108, 255};
        s.outline = {30, 32, 44, 255};
        s.accent = {148, 154, 178, 255};
        s.deep = {44, 48, 64, 255};
    } else {
        s.fill = {200, 144, 92, 255};
        s.outline = {66, 38, 18, 255};
        s.accent = {236, 184, 132, 255};
        s.deep = {132, 88, 50, 255};
    }
    return s;
}

} // namespace

void Renderer::drawBody(const Body& b, unsigned int /*fillColor*/, unsigned int /*outlineColor*/) {
    BodyStyle st = styleForBody(b);

    auto fanFill = [&](const std::vector<Vector2>& poly, ::Color fill) {
        size_t n = poly.size();
        if (n < 3) return;
        for (size_t i = 1; i + 1 < n; ++i)
            DrawTriangle(poly[0], poly[i + 1], poly[i], fill);
    };
    auto outlinePoly = [&](const std::vector<Vector2>& poly, ::Color line) {
        size_t n = poly.size();
        for (size_t i = 0; i < n; ++i) {
            size_t j = (i + 1) % n;
            DrawLine((int)poly[i].x, (int)poly[i].y, (int)poly[j].x, (int)poly[j].y, line);
        }
    };

    if (b.shape.type == ShapeType::Circle) {
        Mat2 R(b.rot);
        int seg = (int)std::clamp(b.shape.radius * camera.zoom * 0.9f + 8.f, 10.f, 22.f);
        std::vector<Vector2> poly;
        poly.reserve((size_t)seg);
        for (int i = 0; i < seg; ++i) {
            float a = kTau * (float)i / (float)seg;
            Vec2 local{std::cos(a) * b.shape.radius, std::sin(a) * b.shape.radius};
            Vec2 p = worldToScreen(b.pos + R.mul(local));
            poly.push_back({(float)iround(p.x), (float)iround(p.y)});
        }
        fanFill(poly, st.fill);
        outlinePoly(poly, st.outline);
        // Single 1-pixel highlight at top-left for "ball" feel — no soft glows.
        Vec2 cs = worldToScreen(b.pos);
        int cx = iround(cs.x);
        int cy = iround(cs.y);
        int rPx = iround(b.shape.radius * camera.zoom);
        if (rPx >= 4) {
            int hx = cx - rPx / 3;
            int hy = cy - rPx / 3;
            DrawRectangle(hx, hy, 2, 2, st.accent);
            DrawPixel(hx, hy, ::Color{255, 255, 255, 255});
        }
        // Orientation tick (rolls visibly so user sees rotation).
        Vec2 tipWorld = b.pos + Vec2{std::cos(b.rot), std::sin(b.rot)} * b.shape.radius * 0.7f;
        Vec2 ts = worldToScreen(tipWorld);
        DrawLine(cx, cy, iround(ts.x), iround(ts.y), st.outline);
        return;
    }

    // Convex polygon body
    Mat2 R(b.rot);
    size_t n = b.shape.vertices.size();
    std::vector<Vector2> poly;
    std::vector<Vec2> wpts;
    poly.reserve(n);
    wpts.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Vec2 w = b.pos + R.mul(b.shape.vertices[i]);
        wpts.push_back(w);
        Vec2 p = worldToScreen(w);
        poly.push_back({(float)iround(p.x), (float)iround(p.y)});
    }

    fanFill(poly, st.fill);

    // Grass top: 2-px stripe + tufts that sway with time. Only on grass-tag
    // (skipping wood platforms which use hasGrassTop only for the highlight line).
    if (st.hasGrassTop) {
        const bool isGrass = (b.tag == Slime::grassTag);
        const float t = (float)GetTime();
        for (size_t i = 0; i < n; ++i) {
            size_t j = (i + 1) % n;
            Vec2 e = wpts[j] - wpts[i];
            Vec2 outward = Vec2{e.y, -e.x}.normalized();
            if (outward.y < -0.55f) {
                Vec2 a = worldToScreen(wpts[i]);
                Vec2 c2 = worldToScreen(wpts[j]);
                int ax = iround(a.x), ay = iround(a.y);
                int bx = iround(c2.x), by = iround(c2.y);
                // 2-px bright accent stripe along the top edge.
                DrawLine(ax, ay, bx, by, st.accent);
                DrawLine(ax, ay - 1, bx, by - 1, st.accent);

                if (!isGrass) continue;

                // Animated grass tufts. Sample world-x along the top edge.
                float wxStart = wpts[i].x, wxEnd = wpts[j].x;
                if (wxStart > wxEnd) std::swap(wxStart, wxEnd);
                const float worldStep = 0.45f;  // ~ tuft every 0.45 world units
                for (float wx = std::ceil(wxStart / worldStep) * worldStep;
                     wx < wxEnd; wx += worldStep) {
                    // Sway horizontal offset (in pixels).
                    float sway = std::sin(t * 1.6f + wx * 0.9f) * 1.2f
                               + std::sin(t * 2.7f + wx * 1.7f) * 0.6f;
                    Vec2 base = worldToScreen({wx, wpts[i].y});
                    int tx = iround(base.x);
                    int ty = iround(base.y) - 1;     // sit on the accent stripe
                    int dx = iround(sway);
                    DrawPixel(tx + dx,        ty - 1, st.accent);
                    DrawPixel(tx + dx,        ty - 2, st.accent);
                    // Brighter tip
                    DrawPixel(tx + dx + (sway > 0.f ? 1 : -1), ty - 2,
                              ::Color{200, 240, 150, 255});
                }
            }
        }
    }
    outlinePoly(poly, st.outline);
    if (b.tag == Slime::spikeHazardTag && n >= 3) {
        size_t tip = 0;
        for (size_t i = 1; i < n; ++i)
            if (wpts[i].y < wpts[tip].y) tip = i;
        Vec2 ts = worldToScreen(wpts[tip]);
        int tx = iround(ts.x), ty = iround(ts.y);
        DrawPixel(tx, ty, st.accent);
        DrawPixel(tx - 1, ty, st.accent);
        DrawPixel(tx, ty - 1, ::Color{255, 248, 210, 255});
    }
}

void Renderer::drawSoftBody(const SoftBody& sb) {
    int n = (int)sb.points.size();
    if (n < 3) return;

    Vec2 c = sb.centroid();
    float r = 0.f;
    float maxY = sb.points[0].pos.y;
    float minX = sb.points[0].pos.x, maxX = sb.points[0].pos.x;
    Vec2 vel = sb.averageVelocity();
    for (auto& p : sb.points) {
        r = std::max(r, distance(p.pos, c));
        maxY = std::max(maxY, p.pos.y);
        minX = std::min(minX, p.pos.x);
        maxX = std::max(maxX, p.pos.x);
    }
    float widthW = maxX - minX;

    // (Drop shadow removed.)
    (void)maxY; (void)widthW;

    // 2) Build screen polygon (each point pinned to integer pixel).
    std::vector<Vector2> poly;
    poly.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        Vec2 p = worldToScreen(sb.points[(size_t)i].pos);
        poly.push_back({(float)iround(p.x), (float)iround(p.y)});
    }

    Vec2 cs = worldToScreen(c);
    int cx = iround(cs.x);
    int cy = iround(cs.y);

    const ::Color fillBody{46, 168, 86, 255};
    const ::Color outlineCol{16, 78, 38, 255};
    drawSlimyBlob(poly, n, cx, cy, camera.zoom, fillBody, outlineCol);

    // (Eyes are drawn separately via drawSlimeFace — they have their own
    //  spring-damped motion driven by Slime, not by raw mesh anchors.)
    (void)vel;
}

void Renderer::drawSlimePuddles(const std::vector<SlimePuddle>& puddles) {
    for (const auto& drop : puddles) {
        Vec2 s = worldToScreen(drop.pos);
        float rz = drop.radius * camera.zoom;
        int r = std::max(2, iround(rz));
        int cx = iround(s.x);
        int cy = iround(s.y);
        unsigned char a =
            (unsigned char)std::clamp(drop.alpha * 218.f, 100.f, 235.f);
        DrawCircle(cx, cy, (float)r, ::Color{34, 118, 54, a});
        DrawCircle(cx + 1, cy - 1, std::max(1.f, (float)r - 2.f), ::Color{52, 158, 78, 255});
    }
}

void Renderer::drawWorld(const World& world, const std::vector<SlimePuddle>* slimeTrail) {
    auto drawOne = [&](Body& b) { drawBody(b, 0, 0); };

    for (auto& b : world.bodies()) {
        if (b->type == BodyType::Dynamic && b->grabOwnerTag != 0) continue;
        drawOne(*b);
    }
    if (slimeTrail && !slimeTrail->empty()) drawSlimePuddles(*slimeTrail);
    for (auto& sb : world.softBodies()) drawSoftBody(*sb);
    for (auto& b : world.bodies()) {
        if (b->type != BodyType::Dynamic || b->grabOwnerTag == 0) continue;
        drawOne(*b);
    }
}

void Renderer::drawSlimeEmbeddedSpikes(Vec2 centroidWorld, const std::vector<Vec2>& radialOffsets) {
    const ::Color fillSpike{178, 42, 72, 255};
    const ::Color edgeSpike{62, 18, 34, 255};
    const ::Color tipHi{255, 228, 210, 255};
    for (const Vec2& off : radialOffsets) {
        float L = off.len();
        if (L < 1e-5f) continue;
        Vec2 dir = off * (1.f / L);
        Vec2 base = centroidWorld + dir * L;
        Vec2 tip = centroidWorld + dir * (L + 0.15f);
        Vec2 side = Vec2{-dir.y, dir.x} * 0.065f;
        Vector2 p0 = {(float)iround(worldToScreen(tip).x), (float)iround(worldToScreen(tip).y)};
        Vector2 p1 = {(float)iround(worldToScreen(base + side).x),
                      (float)iround(worldToScreen(base + side).y)};
        Vector2 p2 = {(float)iround(worldToScreen(base - side).x),
                      (float)iround(worldToScreen(base - side).y)};
        DrawTriangle(p0, p1, p2, fillSpike);
        DrawTriangleLines(p0, p1, p2, edgeSpike);
        DrawPixel(iround(p0.x), iround(p0.y), tipHi);
    }
}

void Renderer::drawSlimeEmbeddedSpikesApprox(Vec2 centroidWorld, float radiusWorld, int count,
                                             uint32_t patternSalt) {
    if (count <= 0 || radiusWorld <= 1e-4f) return;
    std::vector<Vec2> tmp;
    tmp.reserve(1);
    const float golden = 2.39996322972865332f;
    for (int i = 0; i < count; ++i) {
        float ang = i * golden + patternSalt * 0.004189f;
        Vec2 dir{std::cos(ang), std::sin(ang)};
        tmp.clear();
        tmp.push_back(dir * (radiusWorld * 0.88f));
        drawSlimeEmbeddedSpikes(centroidWorld, tmp);
    }
}

void Renderer::drawRemoteSlimeBody(const std::vector<Vec2>& worldPoints, bool isLocalPlayer) {
    int n = (int)worldPoints.size();
    if (n < 3) return;

    // Centroid (unweighted — no mass info from snapshot).
    Vec2 c{0, 0};
    for (auto& p : worldPoints) c += p;
    c = c * (1.f / (float)n);

    Vec2 cs = worldToScreen(c);
    int cx = iround(cs.x), cy = iround(cs.y);

    std::vector<Vector2> poly;
    poly.reserve((size_t)n);
    for (auto& p : worldPoints) {
        Vec2 sp = worldToScreen(p);
        poly.push_back({(float)iround(sp.x), (float)iround(sp.y)});
    }

    const ::Color fillBody{46, 168, 86, 255};
    const ::Color outlineCol{16, 78, 38, 255};
    (void)isLocalPlayer;
    drawSlimyBlob(poly, n, cx, cy, camera.zoom, fillBody, outlineCol);
}

void Renderer::drawSlimeFace(Vec2 leftEye, Vec2 rightEye, Vec2 vel, float radius) {
    if (radius <= 0.f) return;
    const ::Color outlineCol{16, 78, 38, 255};
    Vec2 leftS  = worldToScreen(leftEye);
    Vec2 rightS = worldToScreen(rightEye);
    int lx = iround(leftS.x), ly = iround(leftS.y);
    int rx = iround(rightS.x), ry = iround(rightS.y);

    int eyeBlock = std::clamp(iround(radius * 0.20f * camera.zoom), 3, 6);
    int half = eyeBlock / 2;
    DrawRectangle(lx - half - 1, ly - half - 1, eyeBlock + 2, eyeBlock + 2, outlineCol);
    DrawRectangle(rx - half - 1, ry - half - 1, eyeBlock + 2, eyeBlock + 2, outlineCol);
    DrawRectangle(lx - half, ly - half, eyeBlock, eyeBlock, ::Color{250, 250, 252, 255});
    DrawRectangle(rx - half, ry - half, eyeBlock, eyeBlock, ::Color{250, 250, 252, 255});

    int pupil = std::max(1, eyeBlock / 2);
    Vec2 vDir{0.f, 0.f};
    float vlen = vel.len();
    if (vlen > 0.4f) vDir = vel * (1.f / vlen);
    int pdx = std::clamp(iround(vDir.x * 2.f), -half, half);
    int pdy = std::clamp(iround(vDir.y * 2.f), -half, half);
    DrawRectangle(lx - pupil / 2 + pdx, ly - pupil / 2 + pdy, pupil, pupil,
                  ::Color{18, 18, 30, 255});
    DrawRectangle(rx - pupil / 2 + pdx, ry - pupil / 2 + pdy, pupil, pupil,
                  ::Color{18, 18, 30, 255});
}

void Renderer::drawAimIndicator(Vec2 worldOrigin, Vec2 dir, float chargeFrac) {
    if (chargeFrac <= 0.01f) return;
    chargeFrac = std::clamp(chargeFrac, 0.f, 1.f);
    float L = dir.len();
    if (L < 1e-4f) return;
    Vec2 d = dir / L;

    // Length scales with charge: short = barely held, long = full charge.
    const float minLenWorld = 0.6f;
    const float maxLenWorld = 4.5f;
    const float lenWorld = minLenWorld + (maxLenWorld - minLenWorld) * chargeFrac;

    Vec2 originSp = worldToScreen(worldOrigin);
    Vec2 tipSp = worldToScreen(worldOrigin + d * lenWorld);
    int ox = iround(originSp.x), oy = iround(originSp.y);
    int tx = iround(tipSp.x), ty = iround(tipSp.y);

    // Color shifts from yellow → orange → red as charge maxes out.
    ::Color col;
    if (chargeFrac < 0.5f) {
        unsigned char r = (unsigned char)(220 + chargeFrac * 70);
        unsigned char g = (unsigned char)(220 - chargeFrac * 80);
        col = {r, g, 60, 255};
    } else {
        unsigned char r = 255;
        unsigned char g = (unsigned char)(180 - (chargeFrac - 0.5f) * 280);
        col = {r, std::max<unsigned char>(g, 40), 60, 255};
    }
    ::Color outlineCol{36, 18, 12, 255};

    // Shaft: 2-px line (drawn 3 times offset for thickness, all integer pixels).
    auto drawShaft = [&](::Color c) {
        DrawLine(ox, oy, tx, ty, c);
        DrawLine(ox + 1, oy, tx + 1, ty, c);
        DrawLine(ox, oy + 1, tx, ty + 1, c);
    };
    // Outline shaft (slight halo)
    DrawLine(ox - 1, oy, tx - 1, ty, outlineCol);
    DrawLine(ox, oy - 1, tx, ty - 1, outlineCol);
    DrawLine(ox + 2, oy, tx + 2, ty, outlineCol);
    DrawLine(ox, oy + 2, tx, ty + 2, outlineCol);
    drawShaft(col);

    // Arrow head: 2 small lines at the tip rotated by ±25° from (-d).
    float headLen = 4.f + chargeFrac * 5.f;
    float a = std::atan2(d.y, d.x);
    auto headTip = [&](float ang) {
        Vec2 hd{std::cos(ang), std::sin(ang)};
        Vec2 p = tipSp + (-hd) * headLen;  // points back from tip, hd is the direction
        return Vec2{(float)iround(p.x), (float)iround(p.y)};
    };
    Vec2 hL = headTip(a - 0.45f);
    Vec2 hR = headTip(a + 0.45f);
    DrawLine((int)hL.x, (int)hL.y, tx, ty, outlineCol);
    DrawLine((int)hR.x, (int)hR.y, tx, ty, outlineCol);
    DrawLine((int)hL.x, (int)hL.y - 1, tx, ty - 1, col);
    DrawLine((int)hR.x, (int)hR.y - 1, tx, ty - 1, col);
    DrawLine((int)hL.x, (int)hL.y, tx, ty, col);
    DrawLine((int)hR.x, (int)hR.y, tx, ty, col);
}

void Renderer::drawSlimeChatBubble(Vec2 worldAnchorBottom, const char* utf8, float alpha01) {
    if (!utf8 || utf8[0] == '\0' || alpha01 <= 0.02f) return;
    const unsigned char alpha = (unsigned char)std::clamp(alpha01 * 255.f, 0.f, 255.f);
    const int fs = 8;
    std::string display = utf8;
    const int maxCanvasW = 148;
    auto measure = [&](const char* s) { return MeasureText(s, fs); };
    if (measure(display.c_str()) > maxCanvasW) {
        while (!display.empty() &&
               measure((display + "...").c_str()) > maxCanvasW)
            display.pop_back();
        display += "...";
    }
    const int tw = measure(display.c_str());
    const int padX = 5;
    const int padY = 3;
    const int bw = tw + padX * 2;
    const int bh = fs + padY * 2;
    const int tail = 5;

    Vec2 bot = worldToScreen(worldAnchorBottom);
    const float bx = bot.x - (float)bw * 0.5f;
    const float by = bot.y - (float)bh - (float)tail;

    ::Color fill{(unsigned char)252, (unsigned char)248, (unsigned char)255, alpha};
    ::Color border{(unsigned char)52, (unsigned char)44, (unsigned char)72, alpha};
    ::Color ink{(unsigned char)36, (unsigned char)30, (unsigned char)56, alpha};

    DrawRectangleRounded(::Rectangle{bx, by, (float)bw, (float)bh}, 4.f, 10, fill);
    DrawRectangleRoundedLines(::Rectangle{bx, by, (float)bw, (float)bh}, 4.f, 10, border);

    Vector2 tip{(float)iround(bot.x), (float)iround(bot.y)};
    Vector2 tL{tip.x - 4.f, bot.y - (float)tail};
    Vector2 tR{tip.x + 4.f, bot.y - (float)tail};
    DrawTriangle(tip, tL, tR, fill);
    DrawTriangleLines(tip, tL, tR, border);

    DrawText(display.c_str(), iround(bx + padX), iround(by + padY), fs, ink);
}

void Renderer::drawChatTypingBar(const char* draftUtf8) {
    const char* draft = draftUtf8 ? draftUtf8 : "";
    const int fs = 10;
    char line[256];
    std::snprintf(line, sizeof(line), "> %s_", draft);
    const int tw = MeasureText(line, fs);
    const int pad = 8;
    const int boxW = std::min(internalW_ - pad * 2, tw + pad * 2);
    const int x = (internalW_ - boxW) / 2;
    const int y = internalH_ - fs - pad * 3;
    DrawRectangle(x, y - 2, boxW, fs + 8, ::Color{12, 10, 22, 210});
    DrawRectangleLines(x, y - 2, boxW, fs + 8, ::Color{90, 82, 130, 255});
    DrawText(line, x + pad, y, fs, ::Color{210, 206, 236, 255});
}

void Renderer::drawHUDBanner(const std::string& text) {
    if (text.empty()) return;
    const int fs = 10;
    const int tw = MeasureText(text.c_str(), fs);
    const int x = (internalW_ - tw) / 2;
    const int y = 4;
    DrawRectangle(x - 4, y - 2, tw + 8, fs + 4, ::Color{0, 0, 0, 170});
    DrawText(text.c_str(), x, y, fs, ::Color{160, 235, 175, 255});
}

} // namespace pe
