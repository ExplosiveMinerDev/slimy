#pragma once
#include "math/Vec2.h"
#include "net/Protocol.h"
#include "net/Room.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

struct _ENetHost;
struct _ENetPeer;

namespace pe::net {

/// Best-effort: returns the local machine's LAN IPv4 addresses (joined with " / ").
std::string discoverLanIPv4();

/// Best-effort: fetches the public IPv4 from api.ipify.org via WinHTTPS (~3s max).
/// Returns "" on failure or on non-Windows builds.
std::string fetchPublicIPv4();

/// Picks the single most useful LAN IP from `discoverLanIPv4()` output (drops
/// virtual-NIC noise like 169.254.x and Hyper-V/WSL ranges when a real LAN
/// address is present). Returns "" if discovery itself fails.
std::string preferredLanIPv4();

/// Multi-room hub server. One ENet host listens, peers start in a "lobby" state
/// and can list / create / join rooms. Rooms are independent simulations.
class Server {
public:
    bool start(uint16_t port = kDefaultPort, uint32_t serverBuild = kProtocolVersion);
    void stop();

    /// Drain incoming events: connect / disconnect / lobby + in-room messages.
    void serviceIncoming();
    /// Advance every room one wall-clock slice + broadcast snapshots.
    void tickAndBroadcast();
    /// Drop any room that's been empty longer than `kRoomPurgeAfterEmptySec`.
    void pruneEmptyRooms();

    /// Push a "new build available" notice to every lobby peer (and remember it
    /// for peers that connect later).
    void announceUpdate(uint32_t newBuild, const std::string& releaseUrl);

    bool isRunning() const { return host_ != nullptr; }
    int totalConnected() const { return (int)peers_.size(); }
    int totalInRoom() const;
    int roomCount() const { return (int)rooms_.size(); }

private:
    struct PeerInfo {
        uint32_t roomId = 0;     // 0 means lobby (no assigned room)
        bool inLobby = true;
    };

    void onConnect(_ENetPeer* peer);
    void onDisconnect(_ENetPeer* peer);
    void onReceive(_ENetPeer* peer, const uint8_t* data, size_t len);

    void handleListRooms(_ENetPeer* peer);
    void handleCreateRoom(_ENetPeer* peer, const uint8_t* name, uint8_t nameLen);
    void handleJoinRoom(_ENetPeer* peer, uint32_t roomId);
    void handleLeaveRoom(_ENetPeer* peer);
    void handleClientInput(_ENetPeer* peer, const ClientInputMsg& m);
    void handleClientChat(_ENetPeer* peer, const uint8_t* pkt, size_t len);

    void sendWelcomeTo(_ENetPeer* peer);
    void sendRoomListTo(_ENetPeer* peer);
    void broadcastRoomListToLobby();
    void sendJoinResult(_ENetPeer* peer, JoinResult code, uint32_t roomId, uint32_t slot);
    void sendVersionInfoTo(_ENetPeer* peer);

    Room* findRoom(uint32_t roomId);

    _ENetHost* host_ = nullptr;
    uint32_t serverBuild_ = kProtocolVersion;
    uint32_t nextRoomId_ = 1;
    std::unordered_map<uint32_t, std::unique_ptr<Room>> rooms_;
    std::unordered_map<_ENetPeer*, PeerInfo> peers_;

    Room::clock::time_point lastTickStamp_{};

    bool updateAvailable_ = false;
    uint32_t updateBuild_ = 0;
    std::string updateUrl_;
};

} // namespace pe::net
