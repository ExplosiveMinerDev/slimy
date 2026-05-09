#pragma once
#include "math/Vec2.h"
#include <cstdint>
#include <vector>

namespace pe::net {

constexpr uint32_t kProtocolVersion = 14;
/// Bump when shipping a new Windows client binary (must match server `--build` / manifest for auto-update).
constexpr uint32_t kClientBuild = 17;
/// Max UTF-8 bytes per chat line (wire + UI clipping).
constexpr int kMaxChatPayloadBytes = 96;
/// Max UTF-8 bytes for a user-supplied room name.
constexpr int kMaxRoomNameBytes = 32;
constexpr uint16_t kDefaultPort = 6543;
/// Max simultaneous players per room.
constexpr int kMaxPlayers = 8;
/// Target minimum area (world²) for a sustainable slime fragment after a split.
/// Binary cuts (Shift / spikes / hull damage) require the **source** hull area to be at
/// least ~2× this so both children stay near or above this size — avoids splitting a blob
/// that is already "at the limit", and stops cycling tiny fragments by switching control.
constexpr float kMinSlimeConvexAreaToSplit = 0.50f;
constexpr float kMinParentConvexAreaForBinarySplit = kMinSlimeConvexAreaToSplit * 2.0f;
/// Max simultaneous rooms hosted by one server hub.
constexpr int kMaxRooms = 16;
/// Max simultaneous TCP/ENet peers (lobby + all rooms). 16 * 8 plus headroom.
constexpr int kMaxPeers = 144;
constexpr int kSlimeSegments = 22;
constexpr uint8_t kSlimeFragmentPrimaryBit = 0x80;
constexpr uint8_t kSlimeFragmentIdMask = 0x7F;
/// Empty room is kept around this long before being purged (so reconnect bursts don't lose state).
constexpr float kRoomPurgeAfterEmptySec = 60.f;

enum class MsgType : uint8_t {
    ClientHello        = 1,
    ServerWelcome      = 2,   // lobby-mode welcome (no slot yet)
    ClientInput        = 3,   // in-room only
    ServerSnapshot     = 4,   // in-room only
    ClientChat         = 5,   // in-room only
    ServerChatRelay    = 6,   // in-room only
    ClientListRooms    = 7,   // ask for current room list
    ServerRoomList     = 8,   // pushed on demand AND on every roster change
    ClientCreateRoom   = 9,   // create + auto-join: payload = nameLen + name
    ClientJoinRoom     = 10,  // payload = roomId
    ServerJoinResult   = 11,  // result of create/join (ok | not found | full)
    ClientLeaveRoom    = 12,  // go back to lobby
    ServerKickToLobby  = 13,  // server-side: room destroyed / shut down
    ServerVersionInfo  = 14,  // optional: notify clients of server-side update available
};

enum class JoinResult : uint8_t {
    Ok            = 0,
    NotFound      = 1,
    Full          = 2,
    AlreadyInRoom = 3,
    BadName       = 4,
    TooManyRooms  = 5,
};

#pragma pack(push, 1)
struct MsgHeader {
    uint8_t type;
};

struct ClientHelloMsg {
    MsgHeader hdr;
    uint32_t protocolVersion;
};

struct ServerWelcomeMsg {
    MsgHeader hdr;
    uint32_t protocolVersion;
    /// Server build identifier (for the optional auto-update check).
    uint32_t serverBuild;
};

struct ClientInputMsg {
    MsgHeader hdr;
    float aimX, aimY;        // world-space mouse target
    uint8_t jumpHeld;
    uint8_t mergeHeld;
    uint8_t grabHeld;
    uint8_t respawnHeld;
    uint8_t gatherHeld;
    uint8_t shiftSplitClick;
    uint8_t switchFragmentClick;
    uint8_t colorIndex;
};

/// Reliable client → server. Wire: `hdr` + `byteLen` + `byteLen` payload bytes (UTF-8).
struct ClientChatMsg {
    MsgHeader hdr;
    uint8_t byteLen;
};

/// Reliable server → all clients in same room. Layout: header + `byteLen` UTF-8 bytes.
struct ServerChatRelayMsg {
    MsgHeader hdr;
    uint32_t senderSlot;
    uint8_t byteLen;
};

struct ClientListRoomsMsg {
    MsgHeader hdr;
};

/// One room entry in a server-room-list message. Followed immediately by `nameLen` UTF-8 bytes.
struct RoomInfoNet {
    uint32_t roomId;
    uint8_t nameLen;
    uint8_t playerCount;
    uint8_t maxPlayers;
    uint8_t optionsFlags;
};

struct ServerRoomListMsg {
    MsgHeader hdr;
    uint16_t numRooms;
    // followed by numRooms × (RoomInfoNet + nameLen bytes)
};

struct ClientCreateRoomMsg {
    MsgHeader hdr;
    uint8_t nameLen;
    uint8_t maxPlayers;
    uint8_t optionsFlags;
    // followed by nameLen UTF-8 bytes
};

struct ClientJoinRoomMsg {
    MsgHeader hdr;
    uint32_t roomId;
};

struct ServerJoinResultMsg {
    MsgHeader hdr;
    uint8_t code;            // JoinResult cast
    uint32_t roomId;
    uint32_t yourSlot;       // 0..kMaxPlayers-1 if Ok
};

struct ClientLeaveRoomMsg {
    MsgHeader hdr;
};

struct ServerKickToLobbyMsg {
    MsgHeader hdr;
    uint8_t reason;          // 0 = room shutting down
};

/// Server pushes when it detects (via update poll) a newer build is available.
/// Layout: header + `urlLen` UTF-8 URL bytes (release page).
struct ServerVersionInfoMsg {
    MsgHeader hdr;
    uint32_t newBuild;
    uint8_t urlLen;
};

/// One ground-trail decal in network snapshots (matches `SlimePuddle` layout).
struct TrailDropNet {
    float x = 0.f;
    float y = 0.f;
    float radius = 0.1f;
    float alpha = 1.f;
};

// Per-slime-fragment payload inside a snapshot. After this header come `numPoints` Vec2s,
// then `uint16_t numTrailDrops` and `numTrailDrops` × `TrailDropNet`.
struct SlimeStatePayload {
    uint32_t ownerId;
    float cx, cy;            // centroid (world)
    float vx, vy;            // mass velocity
    float leftEyeX, leftEyeY;
    float rightEyeX, rightEyeY;
    float aimX, aimY;
    float chargeFrac;        // 0..1
    uint8_t isCharging;
    uint8_t isLocal;         // server stamps 1 for receiver, 0 for others
    uint16_t numPoints;
    uint8_t embeddedSpikeCount; ///< Stuck spike props on slime (0..18 capped server-side); visual + physics.
    uint8_t fragmentInfo;    ///< low 7 bits: fragment id; high bit: primary owner fragment.
    uint8_t colorIndex;
};

struct DynamicRigidNetState {
    uint32_t bodyId;
    float x, y, rot;
};

struct ServerSnapshotMsg {
    MsgHeader hdr;
    uint32_t frame;
    uint16_t numSlimes;
    /// Dynamic `Body` instances only (same creation order / ids as solo `buildScene`).
    uint16_t numDynamicRigids;
    // followed by, for each slime fragment:
    //   SlimeStatePayload + numPoints * (float x, float y)
    //   + uint16_t numTrailDrops + numTrailDrops * TrailDropNet
    // then numDynamicRigids × DynamicRigidNetState
};
#pragma pack(pop)

} // namespace pe::net
