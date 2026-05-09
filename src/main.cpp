#include "physics/World.h"
#include "physics/Body.h"
#include "render/Renderer.h"
#include "game/Scene.h"
#include "game/Slime.h"
#include "net/NetClient.h"
#include "net/Protocol.h"
#include "platform/AutoUpdate.h"
#include <raylib.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <mutex>

using namespace pe;

namespace {

constexpr float kChatBubbleFadeSec = 0.35f;
/** Default hub address shown in Online → SERVER IP (GCP or replace for your host).
 *  Utilise aussi pour « Mise a jour » : interroge ce hub (UDP) pour l'annonce serveur. */
constexpr const char* kDefaultJoinHost = "35.232.34.254";

/// Windows: téléchargement MAJ client en cours (affiche un libellé lobby / HUD).
std::atomic<bool> g_clientAutoUpdateBusy{false};

inline float speechBubbleAlpha(float remainingSec) {
    if (remainingSec <= 0.f) return 0.f;
    if (remainingSec >= kChatBubbleFadeSec) return 1.f;
    return remainingSec / kChatBubbleFadeSec;
}

inline void popLastUtf8Codepoint(std::string& s) {
    while (!s.empty()) {
        const unsigned char c = (unsigned char)s.back();
        s.pop_back();
        if ((c & 0xC0u) != 0x80u) break;
    }
}

void bootstrapClientWindow() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(1680, 945, "Slimy Journey");
    SetWindowMinSize(560, 380);
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    SetTextureFilter(GetFontDefault().texture, TEXTURE_FILTER_POINT);
}

void applyNetRigidsToWorld(World& world, const std::vector<net::RigidNetSample>& rigids) {
    for (const auto& s : rigids) {
        for (auto& bp : world.bodies()) {
            if (bp->id != s.bodyId || bp->type != BodyType::Dynamic) continue;
            bp->pos = s.pos;
            bp->rot = s.rot;
            bp->vel = {0.f, 0.f};
            bp->angVel = 0.f;
            break;
        }
    }
}

// =====================================================================
//  SINGLE-PLAYER MODE
// =====================================================================

void resetScene(World& world, Slime& slime) {
    world.clear();
    buildScene(world);
    slime.spawn(world, kSpawnPos);
}

int runSinglePlayer(bool reuseWindow = false) {
    Renderer renderer(1680, 945, 480, 270, "SlimyJourney — Solo", FramePacing::Vsync,
                      reuseWindow ? RendererWindowMode::UseExistingWindow
                                  : RendererWindowMode::CreateWindow);
    renderer.camera.zoom = 14.f;
    renderer.camera.target = kSpawnPos + Vec2{0.f, 4.f};

    World world;
    world.gravity = {0.f, 22.f};

    Slime slime;
    buildScene(world);
    slime.spawn(world, kSpawnPos);

    const float fixedDt = 1.f / 120.f;
    float accumulator = 0.f;
    Vec2 panStart{0, 0};
    bool panning = false;
    float enterHold = 0.f;
    bool enterMergeLatch = false;

    bool chatOpen = false;
    std::string chatDraft;
    float soloBubbleRemain = 0.f;
    std::string soloBubbleText;

    bool returnToMenu = false;
    while (!renderer.shouldClose() && !returnToMenu) {
        float frameTime = std::min(GetFrameTime(), 0.05f);
        accumulator += frameTime;

        if (IsKeyPressed(KEY_ESCAPE)) {
            if (chatOpen) chatOpen = false;
            else { returnToMenu = true; break; }
        }

        const bool openingChatWithT = !chatOpen && IsKeyPressed(KEY_T);
        if (openingChatWithT) chatOpen = true;

        if (chatOpen) {
            bool skipDuplicateHotkeyT = openingChatWithT;
            int cp = GetCharPressed();
            while (cp > 0) {
                if (skipDuplicateHotkeyT && cp == 116) { // 't' same frame as KEY_T
                    skipDuplicateHotkeyT = false;
                    cp = GetCharPressed();
                    continue;
                }
                skipDuplicateHotkeyT = false;
                int nbytes = 0;
                const char* enc = CodepointToUTF8(cp, &nbytes);
                if (enc && nbytes > 0 &&
                    chatDraft.size() + (size_t)nbytes <= (size_t)net::kMaxChatPayloadBytes)
                    chatDraft.append(enc, (size_t)nbytes);
                cp = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE)) popLastUtf8Codepoint(chatDraft);
            if (IsKeyPressed(KEY_ENTER) && !chatDraft.empty()) {
                soloBubbleText = chatDraft;
                soloBubbleRemain = 3.f;
                chatDraft.clear();
                chatOpen = false;
            }
        }

        if (soloBubbleRemain > 0.f) {
            soloBubbleRemain -= frameTime;
            if (soloBubbleRemain <= 0.f) {
                soloBubbleRemain = 0.f;
                soloBubbleText.clear();
            }
        }

        Vec2 mouseCanvas = renderer.mouseInCanvas();
        Vec2 mouseWorld = renderer.screenToWorld(mouseCanvas);
        const bool chatTyping = chatOpen;
        bool jumpHeld =
            (IsKeyDown(KEY_SPACE) || IsMouseButtonDown(MOUSE_BUTTON_LEFT)) && !chatTyping;
        const bool grabHeld = IsKeyDown(KEY_E) && !chatTyping;

        bool enterDown =
            (IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_KP_ENTER)) && !chatTyping;
        if (enterDown) {
            enterHold += frameTime;
            if (!enterMergeLatch && enterHold >= 0.52f) {
                world.mergeSoftBodiesWithTag(slime.myTag(), slime.basePressureTarget());
                enterMergeLatch = true;
            }
        } else {
            enterHold = 0.f;
            enterMergeLatch = false;
        }

        if (!chatOpen && IsKeyPressed(KEY_R)) {
            resetScene(world, slime);
            enterHold = 0.f;
            enterMergeLatch = false;
        }

        float wheel = GetMouseWheelMove();
        if (wheel != 0.f) {
            Vec2 mouse = renderer.mouseInCanvas();
            Vec2 worldBefore = renderer.screenToWorld(mouse);
            int zi = (int)std::lround(renderer.camera.zoom);
            zi += (wheel > 0.f) ? 1 : -1;
            zi = (int)clamp((float)zi, 4.f, 36.f);
            renderer.camera.zoom = (float)zi;
            Vec2 worldAfter = renderer.screenToWorld(mouse);
            renderer.camera.target += (worldBefore - worldAfter);
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            panning = true;
            panStart = renderer.mouseInCanvas();
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) panning = false;
        if (panning) {
            Vec2 m = renderer.mouseInCanvas();
            Vec2 d = m - panStart;
            renderer.camera.target -= d / renderer.camera.zoom;
            panStart = m;
        }

        int steps = 0;
        while (accumulator >= fixedDt && steps < 6) {
            for (auto& bp : world.bodies()) {
                if (bp->type == BodyType::Dynamic) bp->grabOwnerTag = 0;
            }
            slime.update(fixedDt, world, mouseWorld, jumpHeld, grabHeld);
            world.step(fixedDt);
            world.tryBinarySplitDamagedBlob(slime.myTag());
            accumulator -= fixedDt;
            ++steps;
        }

        if (Slime::playerBlobCount(world, slime.myTag()) > 0) {
            Vec2 c = Slime::playerMassCentroid(world, slime.myTag());
            renderer.camera.target += (c - renderer.camera.target) * std::min(1.f, frameTime * 2.65f);
        }

        renderer.beginFrame();
        renderer.drawWorld(world, &slime.puddles());
        if (slime.stuckSpikeCount() > 0 && Slime::playerBlobCount(world, slime.myTag()) > 0) {
            std::vector<Vec2> spikeOff;
            slime.embeddedSpikeDrawOffsets(world, spikeOff);
            if (!spikeOff.empty()) {
                Vec2 o = Slime::playerMassCentroid(world, slime.myTag());
                renderer.drawSlimeEmbeddedSpikes(o, spikeOff);
            }
        }
        if (slime.eyesValid()) {
            renderer.drawSlimeFace(slime.leftEye(), slime.rightEye(),
                                   slime.playerVelocity(), slime.visualRadius());
        }
        if (slime.charging() && Slime::playerBlobCount(world, slime.myTag()) > 0) {
            Vec2 origin = Slime::playerMassCentroid(world, slime.myTag());
            renderer.drawAimIndicator(origin, slime.aimDir(), slime.chargeFraction());
        }
        if (soloBubbleRemain > 0.f && !soloBubbleText.empty() &&
            Slime::playerBlobCount(world, slime.myTag()) > 0) {
            Vec2 c = Slime::playerMassCentroid(world, slime.myTag());
            const float radius = slime.visualRadius();
            const Vec2 anchor = c + Vec2{0.f, -radius - 0.1f};
            renderer.drawSlimeChatBubble(anchor, soloBubbleText.c_str(),
                                           speechBubbleAlpha(soloBubbleRemain));
        }
        if (chatOpen) renderer.drawChatTypingBar(chatDraft.c_str());
        renderer.endFrame();
    }
    return 0;
}

// =====================================================================
//  ONLINE — lobby browser + in-room session
// =====================================================================

const char* joinFeedbackText(net::JoinFeedback f) {
    switch (f) {
        case net::JoinFeedback::None:         return "";
        case net::JoinFeedback::Ok:           return "joined";
        case net::JoinFeedback::NotFound:     return "room introuvable";
        case net::JoinFeedback::Full:         return "room pleine";
        case net::JoinFeedback::AlreadyIn:    return "deja dans une room";
        case net::JoinFeedback::BadName:      return "nom de room invalide";
        case net::JoinFeedback::TooManyRooms: return "serveur sature (max rooms)";
    }
    return "";
}

// Pixel-canvas room browser. Returns true if we've left the lobby (entered a
// room or asked to disconnect). User intent is communicated via mutations on
// `client` (createRoom / joinRoom / disconnect requested via `wantQuit`).
struct LobbyUIState {
    std::string createDraft;
    bool createFocused = false;
    bool justRequested = false;
};

void drawLobbyBrowser(net::Client& client, LobbyUIState& ui, bool& wantQuit,
                      const std::string& serverAddr, bool updateDownloading) {
    const int W = GetScreenWidth();
    const int H = GetScreenHeight();

    // Same palette family as main menu (green pixel theme).
    DrawRectangle(0, 0, W, H, ::Color{ 10, 22, 14, 255 });

    const ::Color ink      { 232, 248, 236, 255 };
    const ::Color inkDim   { 140, 188, 156, 255 };
    const ::Color panelBg  { 18, 58, 38, 255 };
    const ::Color rowBg    { 36, 94, 64, 255 };
    const ::Color rowHi    { 62, 158, 104, 255 };
    const ::Color rowOff   { 30, 58, 44, 255 };
    const ::Color border   { 100, 200, 130, 255 };
    const ::Color accent   { 100, 200, 130, 255 };
    const ::Color warn     { 255, 185, 165, 255 };
    const ::Color fieldBg  { 12, 42, 28, 255 };

    const ::Vector2 m = GetMousePosition();
    const bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

    // Centered panel.
    const int colW = std::min(720, W - 80);
    const int colX = (W - colW) / 2;
    const int panelY = 42;
    const int panelH = H - 84;
    DrawRectangle(colX, panelY, colW, panelH, panelBg);
    DrawRectangleLines(colX, panelY, colW, panelH, border);

    // Header
    DrawText("SERVERS", colX + 20, panelY + 18, 28, ink);
    DrawText(serverAddr.c_str(), colX + 20, panelY + 52, 14, inkDim);

    const auto& rooms = client.roomList();

    // Room list
    int listY = panelY + 84;
    const int rowH = 34;
    const int rowGap = 4;
    const int listBottom = H - 166;
    const int listMaxRows = std::max(1, (listBottom - listY) / (rowH + rowGap));
    const int rowsToDraw = std::min((int)rooms.size(), listMaxRows);

    for (int i = 0; i < rowsToDraw; ++i) {
        const auto& r = rooms[(size_t)i];
        const bool full = r.playerCount >= r.maxPlayers;
        Rectangle rect{ (float)colX, (float)listY, (float)colW, (float)rowH };
        const bool hot = !full && CheckCollisionPointRec(m, rect);
        ::Color bg = full ? rowOff : (hot ? rowHi : rowBg);
        DrawRectangle((int)rect.x, (int)rect.y, (int)rect.width, (int)rect.height, bg);
        if (hot) DrawRectangle((int)rect.x, (int)rect.y + (int)rect.height - 2, (int)rect.width, 2, accent);

        DrawText(r.name.c_str(), (int)rect.x + 14, (int)rect.y + 9, 18,
                 full ? inkDim : ink);

        char pc[32];
        std::snprintf(pc, sizeof(pc), "%d / %d", r.playerCount, r.maxPlayers);
        const int pcW = MeasureText(pc, 16);
        DrawText(pc, (int)rect.x + (int)rect.width - 14 - pcW,
                 (int)rect.y + 9, 16, full ? warn : inkDim);

        if (hot && click && !full) {
            client.joinRoom(r.roomId);
            ui.justRequested = true;
        }
        listY += rowH + rowGap;
    }
    if (rooms.empty()) {
        DrawText("Aucun serveur dispo", colX + 14, panelY + 96, 16, inkDim);
        listY = panelY + 124;
    }

    // Create-room field (single line, Enter to confirm).
    const int inputY = H - 116;
    Rectangle ipBox{ (float)colX, (float)inputY, (float)colW, 36.f };
    const bool hotInput = CheckCollisionPointRec(m, ipBox);
    DrawRectangle((int)ipBox.x, (int)ipBox.y, (int)ipBox.width, (int)ipBox.height, fieldBg);
    DrawRectangleLines((int)ipBox.x, (int)ipBox.y, (int)ipBox.width, (int)ipBox.height,
                       ui.createFocused ? accent : border);
    if (ui.createFocused)
        DrawRectangle((int)ipBox.x, (int)ipBox.y, 3, (int)ipBox.height, accent);

    if (ui.createDraft.empty() && !ui.createFocused) {
        DrawText("Creer un serveur...",
                 (int)ipBox.x + 16, (int)ipBox.y + 11, 16, inkDim);
    } else {
        DrawText(ui.createDraft.c_str(),
                 (int)ipBox.x + 16, (int)ipBox.y + 11, 16, ink);
        if (ui.createFocused && ((int)(GetTime() * 2.f) % 2) == 0) {
            const int cx = (int)ipBox.x + 16 + MeasureText(ui.createDraft.c_str(), 16);
            DrawRectangle(cx, (int)ipBox.y + 9, 2, 20, ink);
        }
    }

    if (click) ui.createFocused = hotInput;

    if (ui.createFocused) {
        int cp = GetCharPressed();
        while (cp > 0) {
            int nbytes = 0;
            const char* enc = CodepointToUTF8(cp, &nbytes);
            if (enc && nbytes > 0 &&
                ui.createDraft.size() + (size_t)nbytes <= (size_t)net::kMaxRoomNameBytes)
                ui.createDraft.append(enc, (size_t)nbytes);
            cp = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE)) popLastUtf8Codepoint(ui.createDraft);
        if (IsKeyPressed(KEY_ENTER) && !ui.createDraft.empty()) {
            client.createRoom(ui.createDraft);
            ui.justRequested = true;
        }
    }

    // Status / version notice (single line above hint).
    const auto fb = client.joinFeedback();
    if (fb != net::JoinFeedback::None && fb != net::JoinFeedback::Ok) {
        const char* msg = joinFeedbackText(fb);
        DrawText(msg, colX, inputY - 28, 14, warn);
    } else if (updateDownloading) {
        DrawText("Telechargement de la mise a jour...", colX, inputY - 28, 14, accent);
    } else if (client.needsClientUpgrade()) {
        char tail[128];
        std::snprintf(tail, sizeof(tail), "maj dispo  build %u  %s",
                      (unsigned)client.updateNoticeBuild(),
                      client.updateNoticeUrl().c_str());
        DrawText(tail, colX, inputY - 28, 14, accent);
    }

    // Footer hint.
    const char* hint = ui.createFocused
        ? "Enter: creer | Esc: retour"
        : "Click: rejoindre | Esc: retour";
    DrawText(hint, colX, H - 56, 14, inkDim);

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (ui.createFocused) {
            ui.createFocused = false;
            ui.createDraft.clear();
        } else {
            wantQuit = true;
        }
    }
}

bool runOnlineSession(const std::string& host, uint16_t port, std::string& errOut,
                      bool reuseWindow = false) {
    net::Client client;
    if (!client.connect(host, port)) {
        errOut =
            "Pas de reponse (UDP). Souvent MAUVAISE IP (pare-feu deja OK).\n"
            "Verifiez l'IP du HOST (ipconfig).\n"
            "Le serveur tourne ? slimyjourney_server.exe lance ?";
        return false;
    }

    Renderer renderer(1680, 945, 480, 270, "SlimyJourney — Online", FramePacing::FastPresent,
                        reuseWindow ? RendererWindowMode::UseExistingWindow
                                    : RendererWindowMode::CreateWindow);
    renderer.camera.zoom = 14.f;
    renderer.camera.target = kSpawnPos + Vec2{0.f, 4.f};

    World viewWorld;
    buildScene(viewWorld);

    Vec2 panStart{0, 0};
    bool panning = false;
    bool chatOpen = false;
    std::string chatDraft;
    LobbyUIState lobbyUi;

    char serverAddr[80];
    std::snprintf(serverAddr, sizeof(serverAddr), "%s:%u", host.c_str(), (unsigned)port);

    bool quitToMenu = false;
    while (!renderer.shouldClose() && !quitToMenu) {
        client.serviceIncoming();
#if defined(_WIN32)
        if (client.isConnected() && client.needsClientUpgrade() &&
            !g_clientAutoUpdateBusy.exchange(true)) {
            std::string url = client.updateNoticeUrl();
            std::thread([url]() {
                pe::platform::downloadAndRestartFromUrl(url);
                g_clientAutoUpdateBusy = false;
            }).detach();
        }
#endif
        if (!client.isConnected()) break;

        const float frameTime = std::min(GetFrameTime(), 0.05f);
        const auto state = client.state();

        if (state == net::ClientState::Lobby || state == net::ClientState::Joining) {
            // Use a plain raylib frame (no game canvas) for the lobby UI.
            BeginDrawing();
            ClearBackground(::Color{ 8, 12, 18, 255 });
            bool wantQuit = false;
            drawLobbyBrowser(client, lobbyUi, wantQuit, serverAddr,
                               g_clientAutoUpdateBusy.load());
            if (state == net::ClientState::Joining) {
                const int W = GetScreenWidth();
                DrawRectangle(W / 2 - 90, 14, 180, 22, ::Color{ 30, 50, 70, 230 });
                DrawText("connexion...", W / 2 - 50, 18, 18, ::Color{ 210, 230, 250, 255 });
            }
            EndDrawing();
            if (wantQuit) { quitToMenu = true; break; }
            continue;
        }

        // === In-room game loop ===
        if (state != net::ClientState::InRoom) {
            // Spurious state — render nothing this frame.
            BeginDrawing();
            ClearBackground(::Color{ 8, 12, 18, 255 });
            EndDrawing();
            continue;
        }

        if (IsKeyPressed(KEY_ESCAPE)) {
            if (chatOpen) chatOpen = false;
            else {
                client.leaveRoom();
                chatDraft.clear();
                chatOpen = false;
                continue;
            }
        }

        // T opens chat only when closed — typing "t" must not toggle the panel off.
        const bool openingChatWithT = !chatOpen && IsKeyPressed(KEY_T);
        if (openingChatWithT) chatOpen = true;

        if (chatOpen) {
            bool skipDuplicateHotkeyT = openingChatWithT;
            int cp = GetCharPressed();
            while (cp > 0) {
                if (skipDuplicateHotkeyT && cp == 116) {
                    skipDuplicateHotkeyT = false;
                    cp = GetCharPressed();
                    continue;
                }
                skipDuplicateHotkeyT = false;
                int nbytes = 0;
                const char* enc = CodepointToUTF8(cp, &nbytes);
                if (enc && nbytes > 0 &&
                    chatDraft.size() + (size_t)nbytes <= (size_t)net::kMaxChatPayloadBytes)
                    chatDraft.append(enc, (size_t)nbytes);
                cp = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE)) popLastUtf8Codepoint(chatDraft);
            if (IsKeyPressed(KEY_ENTER) && !chatDraft.empty()) {
                client.sendChat(chatDraft);
                chatDraft.clear();
                chatOpen = false;
            }
        }

        Vec2 mouseCanvas = renderer.mouseInCanvas();
        Vec2 mouseWorld = renderer.screenToWorld(mouseCanvas);
        const bool chatTyping = chatOpen;
        bool jumpHeld =
            (IsKeyDown(KEY_SPACE) || IsMouseButtonDown(MOUSE_BUTTON_LEFT)) && !chatTyping;
        const bool mergeHeld =
            (IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_KP_ENTER)) && !chatTyping;
        const bool grabHeld = IsKeyDown(KEY_E) && !chatTyping;
        const bool respawnHeld = IsKeyDown(KEY_R) && !chatTyping;

        float wheel = GetMouseWheelMove();
        if (wheel != 0.f) {
            Vec2 mouse = renderer.mouseInCanvas();
            Vec2 worldBefore = renderer.screenToWorld(mouse);
            int zi = (int)std::lround(renderer.camera.zoom);
            zi += (wheel > 0.f) ? 1 : -1;
            zi = (int)clamp((float)zi, 4.f, 36.f);
            renderer.camera.zoom = (float)zi;
            Vec2 worldAfter = renderer.screenToWorld(mouse);
            renderer.camera.target += (worldBefore - worldAfter);
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            panning = true;
            panStart = renderer.mouseInCanvas();
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) panning = false;
        if (panning) {
            Vec2 m = renderer.mouseInCanvas();
            Vec2 d = m - panStart;
            renderer.camera.target -= d / renderer.camera.zoom;
            panStart = m;
        }

        client.sendInput(mouseWorld, jumpHeld, mergeHeld, grabHeld, respawnHeld);
        client.tickChatBubbles(frameTime);
        client.advanceInterpolation();
        client.updateNetTrail(frameTime);
        applyNetRigidsToWorld(viewWorld, client.displayRigids());

        for (auto& rs : client.displaySlimes()) {
            if (rs.isLocalPlayer) {
                renderer.camera.target +=
                    (rs.centroid - renderer.camera.target) * std::min(1.f, frameTime * 2.65f);
                break;
            }
        }

        renderer.beginFrame();
        std::vector<SlimePuddle> netTrails;
        client.copyNetTrailForDraw(netTrails);
        renderer.drawWorld(viewWorld, netTrails.empty() ? nullptr : &netTrails);
        for (auto& rs : client.displaySlimes()) {
            renderer.drawRemoteSlimeBody(rs.points, rs.isLocalPlayer);
            if (rs.embeddedSpikeCount > 0 && !rs.points.empty()) {
                float rad = 0.9f;
                for (auto& p : rs.points)
                    rad = std::max(rad, distance(p, rs.centroid));
                renderer.drawSlimeEmbeddedSpikesApprox(rs.centroid, rad, (int)rs.embeddedSpikeCount,
                                                        rs.ownerId);
            }
        }
        for (auto& rs : client.displaySlimes()) {
            float radius = 0.9f;
            if (!rs.points.empty()) {
                float maxR = 0.f;
                for (auto& p : rs.points)
                    maxR = std::max(maxR, distance(p, rs.centroid));
                radius = maxR;
            }
            renderer.drawSlimeFace(rs.leftEye, rs.rightEye, rs.vel, radius);
        }
        for (auto& rs : client.displaySlimes()) {
            const int sid = (int)rs.ownerId;
            const std::string* bubble = client.chatBubbleText(sid);
            if (!bubble || bubble->empty()) continue;
            float radius = 0.9f;
            if (!rs.points.empty()) {
                float maxR = 0.f;
                for (auto& p : rs.points)
                    maxR = std::max(maxR, distance(p, rs.centroid));
                radius = maxR;
            }
            Vec2 anchor = rs.centroid + Vec2{0.f, -radius - 0.1f};
            renderer.drawSlimeChatBubble(anchor, bubble->c_str(),
                                         client.chatBubbleAlpha(sid));
        }
        if (chatOpen) renderer.drawChatTypingBar(chatDraft.c_str());
        for (auto& rs : client.displaySlimes()) {
            if (!rs.isLocalPlayer || rs.points.empty()) continue;
            if (!rs.isCharging || rs.chargeFrac <= 0.01f) continue;
            Vec2 dir = (rs.aim - rs.centroid).normalized();
            if (dir.len() < 0.08f) continue;
            renderer.drawAimIndicator(rs.centroid, dir, rs.chargeFrac);
        }
        if (g_clientAutoUpdateBusy.load()) {
            renderer.drawHUDBanner("Telechargement de la mise a jour...");
        } else if (client.needsClientUpgrade()) {
            char banner[160];
            std::snprintf(banner, sizeof(banner),
                          "UPDATE DISPONIBLE  build %u  %s",
                          (unsigned)client.updateNoticeBuild(),
                          client.updateNoticeUrl().c_str());
            renderer.drawHUDBanner(banner);
        }
        renderer.endFrame();
    }

    client.disconnect();
    return true;
}

// =====================================================================
//  MAIN MENU
// =====================================================================

struct MenuResult {
    enum Mode { Quit, Solo, Online };
    Mode mode = Quit;
    std::string ip = kDefaultJoinHost;
    uint16_t port = net::kDefaultPort;
};

MenuResult runMenu(std::string& joinFailHint) {
    SetTargetFPS(60);

    MenuResult res;
    const std::string errBanner = joinFailHint;
    joinFailHint.clear();
    bool joinPanel = !errBanner.empty();
    static std::string s_joinIpDraft = kDefaultJoinHost;
    std::atomic<bool> menuUpdateBusy{false};
    std::mutex menuAuMsgMtx;
    std::string menuAuMsg;

    std::string ipBuf = s_joinIpDraft;
    bool decided = false;

    constexpr int kPixW = 360;
    constexpr int kPixH = 260;
    RenderTexture2D pix = LoadRenderTexture(kPixW, kPixH);
    SetTextureFilter(pix.texture, TEXTURE_FILTER_POINT);

    const ::Color bg      { 24, 56, 40, 255 };
    const ::Color bgInset { 18, 46, 32, 255 };
    const ::Color ink     { 232, 248, 236, 255 };
    const ::Color inkDim  { 130, 178, 152, 255 };
    const ::Color inkMuted{ 95, 130, 112, 255 };
    const ::Color btn     { 38, 118, 76, 255 };
    const ::Color btnHi   { 54, 148, 98, 255 };
    const ::Color line    { 108, 208, 138, 255 };
    const ::Color accent  { 140, 235, 175, 255 };
    const ::Color fieldBg { 12, 38, 26, 255 };

    auto flatBtn = [&](int x, int y, int w, int h, const char* label, bool hot, bool enabled,
                       int fs) {
        const ::Color face = enabled ? (hot ? btnHi : btn) : ::Color{ 34, 74, 54, 255 };
        const ::Color txt = enabled ? ink : inkMuted;
        DrawRectangle(x, y, w, h, face);
        DrawRectangleLines(x, y, w, h, line);
        if (enabled)
            DrawRectangle(x + 1, y + 1, w - 2, 1, ::Color{ 255, 255, 255, 35 });
        const int lw = MeasureText(label, fs);
        DrawText(label, x + (w - lw) / 2, y + (h - fs) / 2, fs, txt);
    };

    while (!WindowShouldClose() && !decided) {
        const int Wscr = GetScreenWidth();
        const int Hscr = GetScreenHeight();
        const float scale = std::min(Wscr / (float)kPixW, Hscr / (float)kPixH);
        const float dstW = kPixW * scale;
        const float dstH = kPixH * scale;
        const float ox = (Wscr - dstW) * 0.5f;
        const float oy = (Hscr - dstH) * 0.5f;
        const ::Vector2 smp = GetMousePosition();
        const ::Vector2 mp{ (smp.x - ox) / scale, (smp.y - oy) / scale };
        const bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

        BeginTextureMode(pix);
        ClearBackground(bg);

        DrawRectangle(1, 1, kPixW - 2, kPixH - 2, bgInset);
        DrawRectangleLines(0, 0, kPixW, kPixH, line);

        const int bw = 220;
        const int bx = (kPixW - bw) / 2;
        const int bh = 28;
        const int gap = 10;

        const ::Color warnCol{ 255, 185, 165, 255 };

        if (joinPanel) {
            DrawText("SERVEUR", bx, 18, 16, accent);
            DrawText("Adresse du hub (UDP)", bx, 38, 9, inkDim);

            Rectangle ipBox{ (float)bx, 52.f, (float)bw, 26.f };
            DrawRectangle((int)ipBox.x, (int)ipBox.y, (int)ipBox.width, (int)ipBox.height,
                          fieldBg);
            DrawRectangleLines((int)ipBox.x, (int)ipBox.y, (int)ipBox.width, (int)ipBox.height,
                               line);
            DrawText(ipBuf.c_str(), (int)ipBox.x + 6, (int)ipBox.y + 7, 11, ink);
            const int cx = (int)ipBox.x + 6 + MeasureText(ipBuf.c_str(), 11);
            if (((int)(GetTime() * 2.f) % 2) == 0)
                DrawRectangle(cx, (int)ipBox.y + 5, 2, 13, ink);

            int ch;
            while ((ch = GetCharPressed()) != 0) {
                if (ipBuf.size() < 60 && ch >= 32 && ch < 127)
                    ipBuf.push_back((char)ch);
            }
            if (IsKeyPressed(KEY_BACKSPACE) && !ipBuf.empty()) ipBuf.pop_back();

            const bool pasteHeld =
                IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
            if (pasteHeld && IsKeyPressed(KEY_V)) {
                const char* clip = GetClipboardText();
                if (clip && clip[0]) {
                    std::string pasted(clip);
                    const size_t nl = pasted.find_first_of("\r\n");
                    if (nl != std::string::npos) pasted.resize(nl);
                    ipBuf.clear();
                    for (char c : pasted) {
                        if (ipBuf.size() >= 60) break;
                        const unsigned char u = (unsigned char)c;
                        if (u >= 32 && u < 127) ipBuf.push_back(c);
                    }
                }
            }

            int eyDraw = 88;
            if (!errBanner.empty()) {
                const int errFs = 8;
                size_t a = 0;
                int nlines = 0;
                while (a <= errBanner.size() && nlines < 6) {
                    size_t b = errBanner.find('\n', a);
                    if (b == std::string::npos) b = errBanner.size();
                    std::string errLine = errBanner.substr(a, b - a);
                    if (!errLine.empty())
                        DrawText(errLine.c_str(), bx, eyDraw, errFs, warnCol);
                    eyDraw += errFs + 2;
                    ++nlines;
                    if (b >= errBanner.size()) break;
                    a = b + 1;
                }
            }

            int y = std::max(118, eyDraw + 6);
            Rectangle rOk{ (float)bx, (float)y, (float)bw, (float)bh }; y += bh + gap;
            Rectangle rBk{ (float)bx, (float)y, (float)bw, (float)bh };

            const bool hOk = CheckCollisionPointRec(mp, rOk);
            const bool hBk = CheckCollisionPointRec(mp, rBk);
            flatBtn(bx, (int)rOk.y, bw, bh, "CONNECTER", hOk, true, 13);
            flatBtn(bx, (int)rBk.y, bw, bh, "RETOUR", hBk, true, 13);

            if (click) {
                if (hOk && !ipBuf.empty()) {
                    s_joinIpDraft = ipBuf;
                    res.mode = MenuResult::Online;
                    res.ip = ipBuf;
                    decided = true;
                } else if (hBk) joinPanel = false;
            }
            if (IsKeyPressed(KEY_ENTER) && !ipBuf.empty()) {
                s_joinIpDraft = ipBuf;
                res.mode = MenuResult::Online;
                res.ip = ipBuf;
                decided = true;
            }
            if (IsKeyPressed(KEY_ESCAPE)) joinPanel = false;
        } else {
            const char* title = "SLIMY JOURNEY";
            const int ts = 24;
            DrawText(title, (kPixW - MeasureText(title, ts)) / 2, 16, ts, ink);
            char ver[48];
            std::snprintf(ver, sizeof(ver), "build %u", (unsigned)net::kClientBuild);
            DrawText(ver, (kPixW - MeasureText(ver, 10)) / 2, 44, 10, inkMuted);

            int y = 68;
            Rectangle rSolo  { (float)bx, (float)y, (float)bw, (float)bh }; y += bh + gap;
            Rectangle rOnline{ (float)bx, (float)y, (float)bw, (float)bh }; y += bh + gap;
            Rectangle rUpdate{ (float)bx, (float)y, (float)bw, (float)bh }; y += bh + gap;
            Rectangle rQuit  { (float)bx, (float)y, (float)bw, (float)bh };

            const bool hS = CheckCollisionPointRec(mp, rSolo);
            const bool hO = CheckCollisionPointRec(mp, rOnline);
            const bool hU = CheckCollisionPointRec(mp, rUpdate);
            const bool hQ = CheckCollisionPointRec(mp, rQuit);

            const bool updBtnOk = !menuUpdateBusy.load();

            flatBtn(bx, (int)rSolo.y,   bw, bh, "SOLO",    hS, true, 14);
            flatBtn(bx, (int)rOnline.y, bw, bh, "EN LIGNE", hO, true, 14);
            flatBtn(bx, (int)rUpdate.y, bw, bh, "MISE A JOUR", hU, updBtnOk, 14);
            flatBtn(bx, (int)rQuit.y,   bw, bh, "QUITTER", hQ, true, 14);

            if (menuUpdateBusy.load()) {
                const char* pend = "Verification / mise a jour...";
                DrawText(pend, (kPixW - MeasureText(pend, 11)) / 2, 218, 11, accent);
            } else {
                std::lock_guard<std::mutex> lk(menuAuMsgMtx);
                if (!menuAuMsg.empty())
                    DrawText(menuAuMsg.c_str(), (kPixW - MeasureText(menuAuMsg.c_str(), 10)) / 2,
                             218, 10,
                             menuAuMsg.find("ECHEC") != std::string::npos ? warnCol : inkDim);
            }

            if (click) {
                if (hS) {
                    res.mode = MenuResult::Solo;
                    decided = true;
                } else if (hO)
                    joinPanel = true;
                else if (hU && updBtnOk) {
                    if (!menuUpdateBusy.exchange(true)) {
                        menuAuMsg.clear();
                        const std::string hubForUpdate =
                            s_joinIpDraft.empty() ? std::string(kDefaultJoinHost) : s_joinIpDraft;
                        std::thread([hubForUpdate, &menuUpdateBusy, &menuAuMsg, &menuAuMsgMtx]() {
                            uint32_t nb = 0;
                            std::string du;
                            const net::HubUpdateCheckResult r = net::pollHubForClientUpdate(
                                hubForUpdate, net::kDefaultPort, nb, du, 8000u);
                            menuUpdateBusy.store(false);
                            std::unique_lock<std::mutex> lk(menuAuMsgMtx);
                            switch (r) {
                                case net::HubUpdateCheckResult::HubUnreachable:
                                    menuAuMsg = "ECHEC : hub injoignable (" + hubForUpdate + ")";
                                    break;
                                case net::HubUpdateCheckResult::NoUpdatePublished:
                                    menuAuMsg =
                                        "Pas d'annonce MAJ sur ce hub (--manifest serveur).";
                                    break;
                                case net::HubUpdateCheckResult::ClientUpToDate:
                                    menuAuMsg = "Client a jour.";
                                    break;
                                case net::HubUpdateCheckResult::ReadyToDownload:
#if defined(_WIN32)
                                    menuAuMsg = "Telechargement / installation...";
                                    lk.unlock();
                                    pe::platform::downloadAndRestartFromUrl(du);
                                    lk.lock();
                                    menuAuMsg =
                                        "ECHEC : telechargement (voir console pour details)";
#else
                                    menuAuMsg = "MAJ disponible — installer sous Windows.";
#endif
                                    break;
                            }
                        }).detach();
                    }
                } else if (hQ) {
                    res.mode = MenuResult::Quit;
                    decided = true;
                }
            }
            if (IsKeyPressed(KEY_ESCAPE)) {
                res.mode = MenuResult::Quit;
                decided = true;
            }
        }

        char footer[96];
        if (joinPanel)
            std::snprintf(footer, sizeof(footer), "port %u · ESC retour · Ctrl+V coller",
                          (unsigned)net::kDefaultPort);
        else
            std::snprintf(footer, sizeof(footer), "UDP %u · ESC quitter",
                          (unsigned)net::kDefaultPort);
        DrawText(footer, (kPixW - MeasureText(footer, 9)) / 2, kPixH - 16, 9, inkMuted);

        EndTextureMode();

        BeginDrawing();
        ClearBackground(::Color{ 10, 22, 14, 255 });
        const Rectangle src{ 0.f, 0.f, (float)pix.texture.width, -(float)pix.texture.height };
        const Rectangle dst{ ox, oy, dstW, dstH };
        DrawTexturePro(pix.texture, src, dst, ::Vector2{0.f, 0.f}, 0.f, WHITE);
        EndDrawing();
    }
    UnloadRenderTexture(pix);
    if (!decided && WindowShouldClose()) res.mode = MenuResult::Quit;
    return res;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 ||
                      std::strcmp(argv[1], "-h") == 0)) {
        std::printf(
            "SlimyJourney (client)\n"
            "  (no args)              launch menu (Solo / En ligne / Mise à jour)\n"
            "  --client [host] [port] connect to a server hub directly\n"
            "  --solo                 skip menu, go straight to solo game\n"
            "Map editor + fichier maps/default.sjmap (F6 = defaut du jeu)\n"
            "Server: see slimyjourney_server.exe (separate executable for 24/7 hosting).\n");
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--client") == 0) {
        std::string host = "127.0.0.1";
        uint16_t port = net::kDefaultPort;
        if (argc >= 3) host = argv[2];
        if (argc >= 4) port = (uint16_t)std::atoi(argv[3]);
        std::string err;
        return runOnlineSession(host, port, err, false) ? 0 : 2;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--solo") == 0) {
        return runSinglePlayer(false);
    }

    bootstrapClientWindow();
    std::string joinFailHint;
    while (true) {
        MenuResult m = runMenu(joinFailHint);
        switch (m.mode) {
            case MenuResult::Solo:
                runSinglePlayer(true);
                SetTargetFPS(60);
                break;
            case MenuResult::Online: {
                std::string err;
                if (!runOnlineSession(m.ip, m.port, err, true)) joinFailHint = err;
                SetTargetFPS(60);
                break;
            }
            case MenuResult::Quit:
            default:
                if (IsWindowReady()) CloseWindow();
                return 0;
        }
    }
}
