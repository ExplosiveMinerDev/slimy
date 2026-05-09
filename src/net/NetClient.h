#pragma once
#include "game/Slime.h"
#include "math/Vec2.h"
#include "net/Protocol.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

namespace pe::net {

/// Client-side mirror of one remote slime, decoded from a server snapshot.
struct RemoteSlime {
    uint32_t ownerId = 0;
    uint8_t fragmentId = 0;
    bool isLocalPlayer = false;
    bool isPrimaryFragment = true;
    Vec2 centroid{0, 0};
    Vec2 vel{0, 0};
    Vec2 leftEye{0, 0}, rightEye{0, 0};
    Vec2 aim{0.f, -1.f};
    float chargeFrac = 0.f;
    bool isCharging = false;
    uint8_t embeddedSpikeCount = 0;
    uint8_t colorIndex = 0;
    std::vector<Vec2> points;
    std::vector<TrailDropNet> trail;
};

/// Interpolated rigid body pose for rendering (matches `Body::id` from `buildScene`).
struct RigidNetSample {
    uint32_t bodyId = 0;
    Vec2 pos{};
    float rot = 0.f;
};

/// Single entry from the server-side room list.
struct LobbyRoomInfo {
    uint32_t roomId = 0;
    std::string name;
    int playerCount = 0;
    int maxPlayers = kMaxPlayers;
    uint8_t optionsFlags = 0;
};

enum class ClientState : uint8_t {
    Disconnected = 0,
    Lobby        = 1,
    Joining      = 2,
    InRoom       = 3,
};

enum class JoinFeedback : uint8_t {
    None         = 0,
    Ok           = 1,
    NotFound     = 2,
    Full         = 3,
    AlreadyIn    = 4,
    BadName      = 5,
    TooManyRooms = 6,
};

/// Résultat d'une connexion courte au hub pour savoir si une MAJ client est annoncée (`ServerVersionInfo`).
enum class HubUpdateCheckResult {
    HubUnreachable,
    NoUpdatePublished, ///< Timeout sans `ServerVersionInfo` (serveur sans `--manifest` / pas d'annonce).
    ClientUpToDate,  ///< Annonce reçue mais build ≤ `kClientBuild` ou pas d'URL utile.
    ReadyToDownload, ///< `needsClientUpgrade()` — remplit `outNoticeBuild` / `outDownloadUrl`.
};

/// Connexion UDP courte au hub : lit l'annonce de mise à jour du serveur (même flux qu'en ligne).
HubUpdateCheckResult pollHubForClientUpdate(const std::string& host, uint16_t port,
                                            uint32_t& outNoticeBuild, std::string& outDownloadUrl,
                                            uint32_t wallTimeoutMs = 8000);

class Client {
public:
    bool connect(const std::string& host, uint16_t port = kDefaultPort,
                 uint32_t timeoutMs = 10000);
    void disconnect();

    /// Lobby-side commands.
    void requestRoomList();
    void createRoom(const std::string& name, int maxPlayers = kMaxPlayers,
                    uint8_t optionsFlags = 0);
    void joinRoom(uint32_t roomId);
    void leaveRoom();

    /// In-room input + chat.
    void sendInput(Vec2 aimWorld, bool jumpHeld, bool mergeHeld, bool grabHeld,
                   bool respawnHeld, bool gatherHeld, bool shiftSplitClick,
                   bool switchFragmentClick = false, uint8_t colorIndex = 0);
    void sendChat(const std::string& utf8);

    void tickChatBubbles(float dt);
    float chatBubbleAlpha(int slot) const;
    const std::string* chatBubbleText(int slot) const;

    void serviceIncoming();
    void advanceInterpolation();

    const std::vector<RemoteSlime>& displaySlimes() const { return slimesDisplay_; }
    const std::vector<RigidNetSample>& displayRigids() const { return rigidsDisplay_; }

    void updateNetTrail(float dt);
    void copyNetTrailForDraw(std::vector<SlimePuddle>& out) const;

    bool isConnected() const { return peer_ && connected_; }
    int myPlayerId() const { return myId_; }
    uint32_t lastSnapshotFrame() const { return lastFrame_; }

    /// Lobby/state accessors.
    ClientState state() const { return state_; }
    const std::vector<LobbyRoomInfo>& roomList() const { return roomList_; }
    /// Latest join result; consumed by the UI then cleared by `clearJoinFeedback()`.
    JoinFeedback joinFeedback() const { return joinFeedback_; }
    void clearJoinFeedback() { joinFeedback_ = JoinFeedback::None; }
    /// Set when the server pushes a ServerVersionInfo. Empty url is OK.
    bool hasUpdateNotice() const { return updateNoticeBuild_ != 0; }
    uint32_t updateNoticeBuild() const { return updateNoticeBuild_; }
    const std::string& updateNoticeUrl() const { return updateNoticeUrl_; }
    /// True when the hub announced a strictly newer build and a non-empty download URL (Windows auto-update).
    bool needsClientUpgrade() const {
        return updateNoticeBuild_ > kClientBuild && !updateNoticeUrl_.empty();
    }

private:
    using clock = std::chrono::steady_clock;

    void resetInterpolationState();
    void ingestSnapshot(uint32_t frame, std::vector<RemoteSlime>&& slimes,
                        std::vector<RigidNetSample>&& rigids);
    void pushChatBubble(int slot, std::string&& msg);

    void handleWelcome(const uint8_t* data, size_t len);
    void handleRoomList(const uint8_t* data, size_t len);
    void handleJoinResult(const uint8_t* data, size_t len);
    void handleKickToLobby(const uint8_t* data, size_t len);
    void handleVersionInfo(const uint8_t* data, size_t len);
    void handleSnapshot(const uint8_t* data, size_t len);
    void handleChatRelay(const uint8_t* data, size_t len);

    _ENetHost* host_ = nullptr;
    _ENetPeer* peer_ = nullptr;
    bool enetReady_ = false;
    bool connected_ = false;
    int myId_ = -1;
    uint32_t lastFrame_ = 0;

    ClientState state_ = ClientState::Disconnected;
    std::vector<LobbyRoomInfo> roomList_;
    JoinFeedback joinFeedback_ = JoinFeedback::None;
    uint32_t currentRoomId_ = 0;

    uint32_t serverBuild_ = 0;
    uint32_t updateNoticeBuild_ = 0;
    std::string updateNoticeUrl_;

    bool haveSnapshot_ = false;
    clock::time_point snapT0_{};
    clock::time_point snapT1_{};
    std::vector<RemoteSlime> slimesPrev_;
    std::vector<RemoteSlime> slimesCurr_;
    std::vector<RigidNetSample> rigidsPrev_;
    std::vector<RigidNetSample> rigidsCurr_;

    std::vector<RemoteSlime> slimesDisplay_;
    std::vector<RigidNetSample> rigidsDisplay_;

    std::unordered_map<uint32_t, std::unordered_map<uint64_t, SlimePuddle>> netTrailAcc_;

    float snapIntervalEma_ = 1.f / 60.f;

    struct SlotChatBubble {
        std::string text;
        float remainingSec = 0.f;
    };
    std::array<SlotChatBubble, kMaxPlayers> chatBubbles_{};
};

} // namespace pe::net
