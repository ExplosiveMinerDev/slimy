#include "net/Room.h"
#include "net/Protocol.h"
#include "game/Scene.h"
#include "game/Slime.h"
#include "physics/Body.h"
#include "physics/SoftBody.h"
#include "physics/World.h"

#include <enet/enet.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pe::net {

Room::Room(uint32_t id, std::string name)
    : id_(id), name_(std::move(name)) {
    world_.gravity = {0.f, 22.f};
    buildScene(world_);
    lastEmptyStamp_ = clock::now();
    nextSnap_ = clock::now();
    snapScratch_.reserve(24576);
}

int Room::playerCount() const {
    int n = 0;
    for (auto& s : slots_) if (s.active) ++n;
    return n;
}

float Room::emptySeconds() const {
    if (!isEmpty()) return 0.f;
    return std::chrono::duration<float>(clock::now() - lastEmptyStamp_).count();
}

int Room::slotForPeer(_ENetPeer* peer) const {
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (slots_[(size_t)i].active && slots_[(size_t)i].peer == peer)
            return i;
    }
    return -1;
}

int Room::addPeer(_ENetPeer* peer) {
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (!slots_[(size_t)i].active) {
            Slot& s = slots_[(size_t)i];
            s = {};
            s.active = true;
            s.peer = peer;
            s.aim = {0.f, -1.f};
            // Spawn the slime now so the next snapshot already shows the new player.
            slimes_[(size_t)i].spawn(world_, spawnPosForSlot(i), 0.9f, kSlimeSegments,
                                     Slime::networkedPlayerBlobTag(i));
            s.slimeAlive = true;
            return i;
        }
    }
    return -1;
}

void Room::removePeer(_ENetPeer* peer) {
    int i = slotForPeer(peer);
    if (i < 0) return;
    world_.removeSoftBodiesWithTag(Slime::networkedPlayerBlobTag(i));
    // Reset Slime state so a recycled slot doesn't inherit puddles / eye state.
    slimes_[(size_t)i] = Slime{};
    slots_[(size_t)i] = {};
    if (isEmpty()) lastEmptyStamp_ = clock::now();
}

void Room::setInput(int slot, Vec2 aim, bool jump, bool merge, bool grab, bool respawn) {
    if (slot < 0 || slot >= kMaxPlayers) return;
    Slot& s = slots_[(size_t)slot];
    if (!s.active) return;
    s.aim = aim;
    s.jumpHeld = jump;
    s.mergeHeld = merge;
    s.grabHeld = grab;
    s.respawnHeld = respawn;
}

void Room::tick(float elapsedSec) {
    if (elapsedSec > 0.05f) elapsedSec = 0.05f;

    for (int i = 0; i < kMaxPlayers; ++i) {
        Slot& s = slots_[(size_t)i];
        if (!s.active || !s.slimeAlive) {
            s.mergeHold = 0.f;
            s.mergeLatch = false;
            s.lastRespawnHeld = false;
            continue;
        }
        if (s.mergeHeld) {
            s.mergeHold += elapsedSec;
            if (!s.mergeLatch && s.mergeHold >= 0.52f) {
                world_.mergeSoftBodiesWithTag(Slime::networkedPlayerBlobTag(i),
                                              slimes_[(size_t)i].basePressureTarget());
                s.mergeLatch = true;
            }
        } else {
            s.mergeHold = 0.f;
            s.mergeLatch = false;
        }
    }

    accumulator_ += elapsedSec;
    constexpr float fixedDt = 1.f / 120.f;
    int steps = 0;
    while (accumulator_ >= fixedDt && steps < 6) {
        for (auto& bp : world_.bodies()) {
            if (bp->type == BodyType::Dynamic)
                bp->grabOwnerTag = 0;
        }

        for (int i = 0; i < kMaxPlayers; ++i) {
            Slot& s = slots_[(size_t)i];
            if (!s.active || !s.slimeAlive) continue;
            const bool rh = s.respawnHeld;
            if (rh && !s.lastRespawnHeld) {
                const int tag = Slime::networkedPlayerBlobTag(i);
                world_.removeSoftBodiesWithTag(tag);
                slimes_[(size_t)i].spawn(world_, spawnPosForSlot(i), 0.9f,
                                         kSlimeSegments, tag);
            }
            s.lastRespawnHeld = rh;
        }

        for (int i = 0; i < kMaxPlayers; ++i) {
            Slot& s = slots_[(size_t)i];
            if (!s.active || !s.slimeAlive) continue;
            slimes_[(size_t)i].update(fixedDt, world_, s.aim, s.jumpHeld, s.grabHeld);
        }
        world_.step(fixedDt);
        for (int i = 0; i < kMaxPlayers; ++i) {
            if (!slots_[(size_t)i].active || !slots_[(size_t)i].slimeAlive) continue;
            world_.tryBinarySplitDamagedBlob(Slime::networkedPlayerBlobTag(i));
        }
        accumulator_ -= fixedDt;
        ++steps;
        ++frame_;
    }
}

void Room::broadcastSnapshot(_ENetHost* host) {
    if (!host) return;
    if (playerCount() == 0) return;

    auto now = clock::now();
    if (now < nextSnap_) return;
    // ~30 Hz — 60 Hz × large UDP payloads tends to fragment / backlog queues on small VPS links.
    nextSnap_ = now + std::chrono::milliseconds(33);

    std::vector<const SoftBody*> playerBlobs(kMaxPlayers, nullptr);
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (!slots_[(size_t)i].active) continue;
        const int wantTag = Slime::networkedPlayerBlobTag(i);
        const SoftBody* best = nullptr;
        int bestN = -1;
        for (auto& sb : world_.softBodies()) {
            if (sb->tag != wantTag) continue;
            const int n = (int)sb->points.size();
            if (n > bestN) {
                bestN = n;
                best = sb.get();
            }
        }
        playerBlobs[(size_t)i] = best;
    }

    for (int receiver = 0; receiver < kMaxPlayers; ++receiver) {
        if (!slots_[(size_t)receiver].active) continue;

        snapScratch_.clear();
        std::vector<uint8_t>& buf = snapScratch_;
        ServerSnapshotMsg snap{};
        snap.hdr.type = (uint8_t)MsgType::ServerSnapshot;
        snap.frame = frame_;
        snap.numSlimes = 0;
        snap.numDynamicRigids = 0;

        size_t snapOffset = buf.size();
        buf.resize(buf.size() + sizeof(snap));

        for (int i = 0; i < kMaxPlayers; ++i) {
            const SoftBody* sb = playerBlobs[(size_t)i];
            if (!sb) continue;
            const Slot& s = slots_[(size_t)i];

            SlimeStatePayload payload{};
            payload.ownerId = (uint32_t)i;
            Vec2 c = sb->centroid();
            payload.cx = c.x; payload.cy = c.y;

            Vec2 vSum{0,0}; float mSum = 0.f;
            for (auto& p : sb->points) { vSum += p.vel * p.mass; mSum += p.mass; }
            Vec2 vel = mSum > 1e-8f ? vSum * (1.f / mSum) : Vec2{0,0};
            payload.vx = vel.x; payload.vy = vel.y;

            const int n = (int)sb->points.size();
            const int rIdx = std::max(1, n / 8);
            const int lIdx = ((3 * n) / 8) % n;
            Vec2 lTarget = c + (sb->points[(size_t)lIdx].pos - c) * 0.55f;
            Vec2 rTarget = c + (sb->points[(size_t)rIdx].pos - c) * 0.55f;
            payload.leftEyeX = lTarget.x; payload.leftEyeY = lTarget.y;
            payload.rightEyeX = rTarget.x; payload.rightEyeY = rTarget.y;

            payload.aimX = s.aim.x;
            payload.aimY = s.aim.y;
            if (s.slimeAlive) {
                const Slime& pl = slimes_[(size_t)i];
                payload.chargeFrac = pl.charging() ? pl.chargeFraction() : 0.f;
                payload.isCharging = pl.charging() ? (uint8_t)1 : (uint8_t)0;
            } else {
                payload.chargeFrac = 0.f;
                payload.isCharging = (uint8_t)(s.jumpHeld ? 1 : 0);
            }
            payload.isLocal = (uint8_t)(i == receiver ? 1 : 0);
            payload.numPoints = (uint16_t)n;
            payload.embeddedSpikeCount = 0;
            payload.reserved0 = 0;
            if (s.slimeAlive)
                payload.embeddedSpikeCount =
                    (uint8_t)std::min<size_t>(255, slimes_[(size_t)i].stuckSpikeCount());

            size_t off = buf.size();
            buf.resize(off + sizeof(payload));
            std::memcpy(buf.data() + off, &payload, sizeof(payload));

            for (auto& p : sb->points) {
                float xy[2] = {p.pos.x, p.pos.y};
                size_t po = buf.size();
                buf.resize(po + sizeof(xy));
                std::memcpy(buf.data() + po, xy, sizeof(xy));
            }

            uint16_t nTrail = 0;
            if (s.slimeAlive) {
                const auto& pv = slimes_[(size_t)i].puddles();
                constexpr uint16_t kMaxTrail = 24;
                const size_t start =
                    pv.size() > (size_t)kMaxTrail ? pv.size() - (size_t)kMaxTrail : 0;
                nTrail = (uint16_t)(pv.size() - start);
                size_t tn = buf.size();
                buf.resize(tn + sizeof(nTrail));
                std::memcpy(buf.data() + tn, &nTrail, sizeof(nTrail));
                for (size_t j = start; j < pv.size(); ++j) {
                    TrailDropNet dn{};
                    dn.x = pv[j].pos.x;
                    dn.y = pv[j].pos.y;
                    dn.radius = pv[j].radius;
                    dn.alpha = pv[j].alpha;
                    size_t o = buf.size();
                    buf.resize(o + sizeof(dn));
                    std::memcpy(buf.data() + o, &dn, sizeof(dn));
                }
            } else {
                size_t tn = buf.size();
                buf.resize(tn + sizeof(nTrail));
                std::memcpy(buf.data() + tn, &nTrail, sizeof(nTrail));
            }
            ++snap.numSlimes;
        }

        for (auto& bptr : world_.bodies()) {
            const Body& b = *bptr;
            if (b.type != BodyType::Dynamic) continue;
            DynamicRigidNetState rs{};
            rs.bodyId = b.id;
            rs.x = b.pos.x;
            rs.y = b.pos.y;
            rs.rot = b.rot;
            size_t ro = buf.size();
            buf.resize(ro + sizeof(rs));
            std::memcpy(buf.data() + ro, &rs, sizeof(rs));
            ++snap.numDynamicRigids;
        }

        std::memcpy(buf.data() + snapOffset, &snap, sizeof(snap));

        ENetPacket* pk = enet_packet_create(buf.data(), buf.size(), 0);
        enet_peer_send(slots_[(size_t)receiver].peer, 1, pk);
    }
}

void Room::relayChat(_ENetHost* host, int senderSlot, const uint8_t* utf8, uint8_t byteLen) {
    if (!host) return;
    if (byteLen == 0) return;
    if (byteLen > (uint8_t)kMaxChatPayloadBytes) byteLen = (uint8_t)kMaxChatPayloadBytes;

    const size_t total = sizeof(ServerChatRelayMsg) + (size_t)byteLen;
    std::vector<uint8_t> out(total);
    auto* rh = reinterpret_cast<ServerChatRelayMsg*>(out.data());
    rh->hdr.type = (uint8_t)MsgType::ServerChatRelay;
    rh->senderSlot = (uint32_t)senderSlot;
    rh->byteLen = byteLen;
    std::memcpy(out.data() + sizeof(ServerChatRelayMsg), utf8, byteLen);

    for (int i = 0; i < kMaxPlayers; ++i) {
        const Slot& s = slots_[(size_t)i];
        if (!s.active || !s.peer) continue;
        ENetPacket* pk = enet_packet_create(out.data(), out.size(), ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(s.peer, 0, pk);
    }
}

void Room::disconnectAll() {
    for (int i = 0; i < kMaxPlayers; ++i) {
        Slot& s = slots_[(size_t)i];
        if (!s.active || !s.peer) continue;
        // Send a reliable kick-to-lobby first, then disconnect_later so it flushes.
        ServerKickToLobbyMsg k{};
        k.hdr.type = (uint8_t)MsgType::ServerKickToLobby;
        k.reason = 0;
        ENetPacket* pk = enet_packet_create(&k, sizeof(k), ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(s.peer, 0, pk);
    }
    // Drop simulation state but keep slots cleared so this room can be deleted.
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (slots_[(size_t)i].active)
            world_.removeSoftBodiesWithTag(Slime::networkedPlayerBlobTag(i));
        slots_[(size_t)i] = {};
    }
}

} // namespace pe::net
