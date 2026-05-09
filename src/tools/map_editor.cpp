// SlimyJourney map editor: same playground as the game + extra static boxes (.sjmap).
// Tab = switch Edit / Test (physics + slime). In Test: Esc = back to Edit.
// F5 = maps/editor.sjmap (backup)  F9 = reload editor  F6 = maps/default.sjmap (defaut du jeu).

#include "game/MapIO.h"
#include "game/Scene.h"
#include "game/Slime.h"
#include "math/Vec2.h"
#include "physics/Body.h"
#include "physics/World.h"
#include "render/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using pe::SolidMapEntry;
using pe::Vec2;

constexpr const char* kPathEditor = "maps/editor.sjmap";
constexpr const char* kPathGameDefault = "maps/default.sjmap";

constexpr float kFixedDt = 1.f / 120.f;

enum class UiMode { Editing, Testing };

const Color kGrid{60, 120, 90, 90};
const Color kSel{255, 230, 120, 255};
const Color kDrag{255, 255, 200, 200};
const Color kHudPanelBg{14, 18, 26, 238};
const Color kHudPanelBorder{200, 210, 224, 255};
const Color kHudOutline{8, 10, 14, 255};
const Color kHudTitle{255, 255, 252, 255};
const Color kHudLine{218, 228, 240, 255};
const Color kHudMuted{185, 198, 215, 255};

float snapf(float v, float g) {
    if (g <= 0.f) return v;
    return std::round(v / g) * g;
}

Vec2 snapv(Vec2 p, float g) { return {snapf(p.x, g), snapf(p.y, g)}; }

Color colorForTag(int tag) {
    if (tag == pe::Slime::grassTag) return {50, 120, 80, 200};
    if (tag == pe::Slime::stoneTag) return {70, 90, 100, 200};
    if (tag == pe::Slime::mapTestRockTag) return {180, 70, 60, 200};
    if (tag == pe::Slime::airVentTag) return {80, 180, 190, 190};
    return {90, 140, 100, 200};
}

const char* labelForTag(int tag) {
    if (tag == pe::Slime::grassTag) return "grass";
    if (tag == pe::Slime::stoneTag) return "stone";
    if (tag == pe::Slime::mapTestRockTag) return "redrock";
    if (tag == pe::Slime::airVentTag) return "vent";
    return "platform";
}

void cycleTag(int& tag) {
    if (tag == pe::Slime::grassTag) tag = pe::Slime::stoneTag;
    else if (tag == pe::Slime::stoneTag) tag = pe::Slime::platformTag;
    else if (tag == pe::Slime::platformTag) tag = pe::Slime::mapTestRockTag;
    else if (tag == pe::Slime::mapTestRockTag) tag = pe::Slime::airVentTag;
    else tag = pe::Slime::grassTag;
}

void ensureMapsDir() {
    std::error_code ec;
    std::filesystem::create_directories("maps", ec);
}

int hitCustom(const std::vector<SolidMapEntry>& boxes, Vec2 p) {
    for (int i = (int)boxes.size() - 1; i >= 0; --i) {
        const auto& b = boxes[(size_t)i];
        if (std::abs(p.x - b.pos.x) <= b.halfW && std::abs(p.y - b.pos.y) <= b.halfH) return i;
    }
    return -1;
}

void drawHudOutlined(const char* text, int x, int y, int fs, Color fill, Color outline) {
    for (int ox = -1; ox <= 1; ++ox) {
        for (int oy = -1; oy <= 1; ++oy) {
            if (ox || oy)
                DrawText(text, x + ox, y + oy, fs, outline);
        }
    }
    DrawText(text, x, y, fs, fill);
}

void drawHudPanel(int x, int y, int w, int h) {
    DrawRectangle(x, y, w, h, kHudPanelBg);
    DrawRectangleLines(x, y, w, h, kHudPanelBorder);
}

} // namespace

int main() {
    pe::Renderer renderer(1280, 720, 480, 270, "SlimyJourney — Map editor", pe::FramePacing::Vsync);
    renderer.camera.zoom = 14.f;
    renderer.camera.target = pe::kSpawnPos + Vec2{0.f, 4.f};

    pe::World world;
    world.gravity = {0.f, 22.f};
    pe::Slime slime;

    std::vector<SolidMapEntry> customSolids;
    UiMode mode = UiMode::Editing;
    std::string status = "Tab = Test dans le jeu";

    float grid = 0.25f;
    bool snapGrid = true;
    int paintTag = pe::Slime::platformTag;
    int sel = -1;

    bool lDrag = false;
    Vec2 dragStart{};
    bool rDrag = false;
    Vec2 panCamStart{};
    Vec2 panMouseStartCanvas{};

    float accum = 0.f;
    bool pendingShiftSplit = false;
    bool prevShiftSplitDown = false;
    int splitBurstFrames = 0;
    float enterHoldTest = 0.f;
    bool enterMergeLatchTest = false;

    auto rebuildWorld = [&]() {
        world.clear();
        pe::buildSceneCore(world);
        pe::appendSolidMapEntries(world, customSolids);
    };

    ensureMapsDir();
    {
        std::ifstream edProbe(kPathEditor);
        if (edProbe.good()) {
            edProbe.close();
            std::string err;
            if (pe::parseSolidMapFile(kPathEditor, customSolids, &err))
                status = std::string("Charge ") + kPathEditor;
            else {
                customSolids.clear();
                status = err;
            }
        } else {
            std::ifstream defProbe(kPathGameDefault);
            if (defProbe.good()) {
                defProbe.close();
                std::string err;
                if (pe::parseSolidMapFile(kPathGameDefault, customSolids, &err))
                    status = std::string("Charge carte jeu ") + kPathGameDefault;
                else {
                    customSolids.clear();
                    status = err;
                }
            }
        }
    }
    rebuildWorld();

    while (!renderer.shouldClose()) {
        const float frameTime = std::min(GetFrameTime(), 0.05f);
        accum += frameTime;

        const Vec2 mouseCanvas = renderer.mouseInCanvas();
        Vec2 mw = renderer.screenToWorld(mouseCanvas);
        if (snapGrid && mode == UiMode::Editing) mw = snapv(mw, grid);

        if (mode == UiMode::Editing) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                lDrag = true;
                dragStart = mw;
            }
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && lDrag) {
                lDrag = false;
                const float dx = std::abs(mw.x - dragStart.x);
                const float dy = std::abs(mw.y - dragStart.y);
                if (dx < 0.08f && dy < 0.08f) {
                    sel = hitCustom(customSolids, mw);
                } else {
                    Vec2 mn{std::min(dragStart.x, mw.x), std::min(dragStart.y, mw.y)};
                    Vec2 mx{std::max(dragStart.x, mw.x), std::max(dragStart.y, mw.y)};
                    if (snapGrid) {
                        mn = snapv(mn, grid);
                        mx = snapv(mx, grid);
                    }
                    const float hw = (mx.x - mn.x) * 0.5f;
                    const float hh = (mx.y - mn.y) * 0.5f;
                    if (hw >= 0.12f && hh >= 0.12f) {
                        SolidMapEntry e;
                        e.pos = {(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f};
                        e.halfW = hw;
                        e.halfH = hh;
                        e.tag = paintTag;
                        customSolids.push_back(e);
                        sel = (int)customSolids.size() - 1;
                        rebuildWorld();
                        status = "Box added (extra static)";
                    }
                }
            }

            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                rDrag = true;
                panCamStart = renderer.camera.target;
                panMouseStartCanvas = mouseCanvas;
            }
            if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) rDrag = false;
            if (rDrag) {
                Vec2 d = mouseCanvas - panMouseStartCanvas;
                renderer.camera.target = panCamStart - d / renderer.camera.zoom;
            }

            const float wheel = GetMouseWheelMove();
            if (wheel != 0.f) {
                int zi = (int)std::lround(renderer.camera.zoom);
                zi += (wheel > 0.f) ? 1 : -1;
                zi = (int)pe::clamp((float)zi, 4.f, 36.f);
                renderer.camera.zoom = (float)zi;
            }

            if (IsKeyPressed(KEY_T)) {
                cycleTag(paintTag);
                if (sel >= 0 && sel < (int)customSolids.size()) {
                    customSolids[(size_t)sel].tag = paintTag;
                    rebuildWorld();
                }
            }
            if (IsKeyPressed(KEY_G)) {
                snapGrid = !snapGrid;
                status = snapGrid ? "Snap ON" : "Snap OFF";
            }
            if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE)) {
                if (sel >= 0 && sel < (int)customSolids.size()) {
                    customSolids.erase(customSolids.begin() + sel);
                    sel = -1;
                    rebuildWorld();
                    status = "Deleted custom box";
                }
            }
            const float nudge = snapGrid ? grid : 0.25f;
            if (sel >= 0 && sel < (int)customSolids.size()) {
                bool moved = false;
                if (IsKeyPressed(KEY_RIGHT)) {
                    customSolids[(size_t)sel].pos.x += nudge;
                    moved = true;
                }
                if (IsKeyPressed(KEY_LEFT)) {
                    customSolids[(size_t)sel].pos.x -= nudge;
                    moved = true;
                }
                if (IsKeyPressed(KEY_DOWN)) {
                    customSolids[(size_t)sel].pos.y += nudge;
                    moved = true;
                }
                if (IsKeyPressed(KEY_UP)) {
                    customSolids[(size_t)sel].pos.y -= nudge;
                    moved = true;
                }
                if (moved) {
                    rebuildWorld();
                    status = "Moved";
                }
            }

            if (IsKeyPressed(KEY_F5)) {
                ensureMapsDir();
                std::string err;
                if (pe::writeSolidMapFile(kPathEditor, customSolids, &err))
                    status = std::string("Saved ") + kPathEditor;
                else
                    status = err;
            }
            if (IsKeyPressed(KEY_F9)) {
                std::string err;
                if (pe::parseSolidMapFile(kPathEditor, customSolids, &err)) {
                    sel = -1;
                    rebuildWorld();
                    status = std::string("Loaded ") + kPathEditor;
                } else
                    status = err;
            }
            if (IsKeyPressed(KEY_F6)) {
                ensureMapsDir();
                std::string err;
                if (pe::writeSolidMapFile(kPathGameDefault, customSolids, &err))
                    status = std::string("Defaut jeu ecrit -> ") + kPathGameDefault;
                else
                    status = err;
            }

            if (IsKeyPressed(KEY_TAB)) {
                mode = UiMode::Testing;
                rebuildWorld();
                slime.spawn(world, pe::kSpawnPos);
                accum = 0.f;
                pendingShiftSplit = false;
                prevShiftSplitDown = false;
                splitBurstFrames = 0;
                enterHoldTest = 0.f;
                enterMergeLatchTest = false;
                status = "TEST — Esc = Edit   Space = jump   Shift+LMB = split   Ctrl = gather   Enter = merge";
            }
        } else {
            // Test mode: same controls as solo (no chat).
            if (IsKeyPressed(KEY_ESCAPE)) {
                mode = UiMode::Editing;
                rebuildWorld();
                sel = -1;
                pendingShiftSplit = false;
                prevShiftSplitDown = false;
                splitBurstFrames = 0;
                enterHoldTest = 0.f;
                enterMergeLatchTest = false;
                status = "Edit mode";
                renderer.camera.target = pe::kSpawnPos + Vec2{0.f, 4.f};
                renderer.camera.zoom = 14.f;
                continue;
            }

            Vec2 mouseWorld = renderer.screenToWorld(mouseCanvas);
            const bool shiftDown =
                IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            const bool gatherHeld =
                IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            bool jumpHeld =
                (IsKeyDown(KEY_SPACE) ||
                 (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !shiftDown));
            const bool shiftSplitDown =
                shiftDown && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
            if (shiftSplitDown && !prevShiftSplitDown) splitBurstFrames = 4;
            prevShiftSplitDown = shiftSplitDown;
            const bool shiftSplitClick = splitBurstFrames > 0;
            if (splitBurstFrames > 0) --splitBurstFrames;
            if (shiftSplitClick) pendingShiftSplit = true;
            const bool grabHeld = IsKeyDown(KEY_E);

            bool enterDown =
                IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_KP_ENTER);
            if (enterDown) {
                enterHoldTest += frameTime;
                if (!enterMergeLatchTest && enterHoldTest >= 0.52f) {
                    world.mergeSoftBodiesWithTag(slime.myTag(), slime.basePressureTarget());
                    enterMergeLatchTest = true;
                }
            } else {
                enterHoldTest = 0.f;
                enterMergeLatchTest = false;
            }

            int steps = 0;
            while (accum >= kFixedDt && steps < 6) {
                for (auto& bp : world.bodies()) {
                    if (bp->type == pe::BodyType::Dynamic) bp->grabOwnerTag = 0;
                }
                const bool doSplitThisStep = pendingShiftSplit;
                pendingShiftSplit = false;
                slime.update(kFixedDt, world, mouseWorld, jumpHeld, grabHeld, gatherHeld,
                             doSplitThisStep);
                world.step(kFixedDt);
                world.tryBinarySplitDamagedBlob(slime.myTag());
                accum -= kFixedDt;
                ++steps;
            }

            if (pe::Slime::playerBlobCount(world, slime.myTag()) > 0) {
                Vec2 c = pe::Slime::playerControlledCentroid(world, slime.myTag());
                renderer.camera.target += (c - renderer.camera.target) * std::min(1.f, frameTime * 2.65f);
            }

            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                rDrag = true;
                panCamStart = renderer.camera.target;
                panMouseStartCanvas = mouseCanvas;
            }
            if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) rDrag = false;
            if (rDrag) {
                Vec2 d = mouseCanvas - panMouseStartCanvas;
                renderer.camera.target = panCamStart - d / renderer.camera.zoom;
            }
            const float wheel = GetMouseWheelMove();
            if (wheel != 0.f) {
                int zi = (int)std::lround(renderer.camera.zoom);
                zi += (wheel > 0.f) ? 1 : -1;
                zi = (int)pe::clamp((float)zi, 4.f, 36.f);
                renderer.camera.zoom = (float)zi;
            }

            if (IsKeyPressed(KEY_TAB)) {
                mode = UiMode::Editing;
                rebuildWorld();
                sel = -1;
                status = "Edit mode";
            }
        }

        renderer.beginFrame();
        renderer.drawWorld(world, nullptr);
        if (mode == UiMode::Testing && slime.stuckSpikeCount() > 0 &&
            pe::Slime::playerBlobCount(world, slime.myTag()) > 0) {
            std::vector<pe::Vec2> spikeOff;
            slime.embeddedSpikeDrawOffsets(world, spikeOff);
            if (!spikeOff.empty()) {
                pe::Vec2 o = pe::Slime::playerControlledCentroid(world, slime.myTag());
                renderer.drawSlimeEmbeddedSpikes(o, spikeOff);
            }
        }
        if (mode == UiMode::Testing && slime.eyesValid()) {
            renderer.drawSlimeFace(slime.leftEye(), slime.rightEye(), slime.playerVelocity(),
                                   slime.visualRadius());
            if (slime.charging() && pe::Slime::playerBlobCount(world, slime.myTag()) > 0) {
                Vec2 origin = pe::Slime::playerControlledCentroid(world, slime.myTag());
                renderer.drawAimIndicator(origin, slime.aimDir(), slime.chargeFraction());
            }
        }

        // Editor overlay (canvas pixels)
        if (mode == UiMode::Editing) {
            const int iw = renderer.internalWidth();
            const int ih = renderer.internalHeight();

            if (snapGrid && grid > 1e-6f) {
                Vec2 tl = renderer.screenToWorld({0.f, 0.f});
                Vec2 br = renderer.screenToWorld({(float)iw, (float)ih});
                const float gx0 = std::floor(std::min(tl.x, br.x) / grid) * grid;
                const float gx1 = std::ceil(std::max(tl.x, br.x) / grid) * grid;
                const float gy0 = std::floor(std::min(tl.y, br.y) / grid) * grid;
                const float gy1 = std::ceil(std::max(tl.y, br.y) / grid) * grid;
                for (float x = gx0; x <= gx1; x += grid) {
                    Vec2 sa = renderer.worldToScreen({x, tl.y});
                    Vec2 sb = renderer.worldToScreen({x, br.y});
                    DrawLineV({sa.x, sa.y}, {sb.x, sb.y}, kGrid);
                }
                for (float y = gy0; y <= gy1; y += grid) {
                    Vec2 sa = renderer.worldToScreen({tl.x, y});
                    Vec2 sb = renderer.worldToScreen({br.x, y});
                    DrawLineV({sa.x, sa.y}, {sb.x, sb.y}, kGrid);
                }
            }

            for (int i = 0; i < (int)customSolids.size(); ++i) {
                const auto& e = customSolids[(size_t)i];
                Vec2 c = renderer.worldToScreen(e.pos);
                const float rw = e.halfW * renderer.camera.zoom * 2.f;
                const float rh = e.halfH * renderer.camera.zoom * 2.f;
                DrawRectangleLines((int)(c.x - rw * 0.5f), (int)(c.y - rh * 0.5f), (int)rw, (int)rh,
                                   i == sel ? kSel : colorForTag(e.tag));
            }

            if (lDrag) {
                Vec2 a = dragStart;
                Vec2 b = mw;
                Vec2 mn{std::min(a.x, b.x), std::min(a.y, b.y)};
                Vec2 mx{std::max(a.x, b.x), std::max(a.y, b.y)};
                Vec2 c = renderer.worldToScreen((mn + mx) * 0.5f);
                const float rw = (mx.x - mn.x) * renderer.camera.zoom;
                const float rh = (mx.y - mn.y) * renderer.camera.zoom;
                DrawRectangleLines((int)(c.x - rw * 0.5f), (int)(c.y - rh * 0.5f), (int)rw, (int)rh, kDrag);
            }

            constexpr int hx = 10;
            constexpr int hy = 8;
            drawHudPanel(hx - 8, hy - 6, 458, 122);
            drawHudOutlined("MAP EDITOR", hx, hy, 20, kHudTitle, kHudOutline);

            int ly = hy + 22;
            drawHudOutlined(status.c_str(), hx, ly, 13, kHudLine, kHudOutline);
            ly += 17;
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "scene jeu + %d bloc(s) custom   paint=%s   snap=%s   Tab = TEST",
                          (int)customSolids.size(), labelForTag(paintTag), snapGrid ? "on" : "off");
            drawHudOutlined(buf, hx, ly, 13, kHudMuted, kHudOutline);
            ly += 17;
            drawHudOutlined("LMB trace une box / clic = select   RMB = pan   molette = zoom", hx, ly, 13,
                          kHudMuted, kHudOutline);
            ly += 17;
            drawHudOutlined("T tag   G snap grille   Suppr efface", hx, ly, 13, kHudMuted, kHudOutline);
            ly += 17;
            drawHudOutlined("F5 backup editor   F9 recharge editor   F6 = defaut jeu (default.sjmap)   Esc quit",
                          hx, ly, 13, kHudMuted, kHudOutline);
        } else {
            constexpr int hx = 10;
            constexpr int hy = 8;
            drawHudPanel(hx - 8, hy - 6, 430, 72);
            drawHudOutlined("TEST", hx, hy, 20, kHudTitle, kHudOutline);
            drawHudOutlined(status.c_str(), hx, hy + 22, 13, kHudLine, kHudOutline);
            drawHudOutlined("Esc ou Tab = Edit   Space/LMB saut   E grab   RMB pan", hx, hy + 39, 13, kHudMuted,
                          kHudOutline);
        }

        renderer.endFrame();

        if (mode == UiMode::Editing && IsKeyPressed(KEY_ESCAPE)) break;
    }

    return 0;
}
