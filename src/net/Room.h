#pragma once
#include "math/Vec2.h"
#include "net/Protocol.h"
#include "physics/World.h"
#include "game/Slime.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

namespace pe::net {

/// One game world hosted by the multi-room server. Holds per-slot input state,
/// world simulation, and per-room snapshot frame counter.
class Room {
public:
    using clock = std::chrono::steady_clock;

    struct Slot {
        bool active = false;
        _ENetPeer* peer = nullptr;
        Vec2 aim{0.f, -1.f};
        bool jumpHeld = false;
        bool mergeHeld = false;
        bool grabHeld = false;
        bool respawnHeld = false;
        bool gatherHeld = false;
        /// Latest frame requested Shift+LMB split (consumed once per tick).
        bool pendingShiftSplit = false;
        bool pendingSwitchFragment = false;
        uint8_t colorIndex = 0;
        // Server-side state mirroring solo merge timing.
        float mergeHold = 0.f;
        bool mergeLatch = false;
        bool lastRespawnHeld = false;
        bool slimeAlive = false;
    };

    Room() = default;
    Room(uint32_t id, std::string name, int maxPlayers = kMaxPlayers,
         uint8_t optionsFlags = 0);
    Room(const Room&) = delete;
    Room& operator=(const Room&) = delete;
    Room(Room&&) = default;
    Room& operator=(Room&&) = default;

    uint32_t id() const { return id_; }
    const std::string& name() const { return name_; }
    int playerCount() const;
    int maxPlayers() const { return maxPlayers_; }
    uint8_t optionsFlags() const { return optionsFlags_; }
    bool isEmpty() const { return playerCount() == 0; }
    /// Wall-clock since the last connected player left (used for purge).
    float emptySeconds() const;

    /// Allocate a free slot for `peer`, spawn its slime. Returns slot or -1 if full.
    int addPeer(_ENetPeer* peer);
    /// Free the slot for `peer` (disconnect or leave-room). No-op if not in this room.
    void removePeer(_ENetPeer* peer);
    /// Find slot index for `peer`, or -1.
    int slotForPeer(_ENetPeer* peer) const;

    /// Apply latest received input from a slot.
    void setInput(int slot, Vec2 aim, bool jump, bool merge, bool grab, bool respawn,
                    bool gather, bool shiftSplitClick, bool switchFragmentClick,
                    uint8_t colorIndex);

    /// Advance the simulation by `elapsedSec` (consumed via fixed-dt accumulator).
    void tick(float elapsedSec);
    /// Send the latest snapshot to all active slots.
    void broadcastSnapshot(_ENetHost* host);

    /// Forward a chat line to every peer in this room (reliable).
    void relayChat(_ENetHost* host, int senderSlot, const uint8_t* utf8, uint8_t byteLen);

    /// Disconnect every peer from this room (used when room is being destroyed).
    void disconnectAll();

    const Slot& slot(int i) const { return slots_[(size_t)i]; }

private:
    uint32_t id_ = 0;
    std::string name_;
    int maxPlayers_ = kMaxPlayers;
    uint8_t optionsFlags_ = 0;
    World world_;
    std::array<Slime, kMaxPlayers> slimes_{};
    std::array<Slot, kMaxPlayers> slots_{};

    uint32_t frame_ = 0;
    float accumulator_ = 0.f;
    clock::time_point lastEmptyStamp_{clock::now()};
    clock::time_point nextSnap_{clock::now()};
    /// Reused each broadcast to avoid per-frame heap churn under load.
    std::vector<uint8_t> snapScratch_;
};

} // namespace pe::net
