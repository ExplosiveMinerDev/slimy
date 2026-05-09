#include "net/NetClient.h"
#include "net/Protocol.h"
#include <enet/enet.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace pe::net {

namespace {

constexpr float kTau = 6.2831853f;

bool resolveConnectAddress(ENetAddress* addr, const char* host) {
    if (enet_address_set_host_ip(addr, host) == 0) return true;
    return enet_address_set_host(addr, host) == 0;
}

float lerpAngleRad(float a, float b, float w) {
    float d = std::remainder(b - a, kTau);
    return a + d * w;
}

uint64_t slimeInterpKey(uint32_t ownerId, uint8_t fragmentId) {
    return (uint64_t)ownerId << 32 | (uint64_t)fragmentId;
}

const RigidNetSample* findRigidById(const std::vector<RigidNetSample>& v, uint32_t id) {
    for (const auto& r : v) if (r.bodyId == id) return &r;
    return nullptr;
}

RemoteSlime blendSlime(const RemoteSlime& from, const RemoteSlime& to, float w) {
    if (from.ownerId != to.ownerId || from.fragmentId != to.fragmentId) return to;
    RemoteSlime out = to;
    out.centroid = from.centroid * (1.f - w) + to.centroid * w;
    out.vel = from.vel * (1.f - w) + to.vel * w;
    out.leftEye = from.leftEye * (1.f - w) + to.leftEye * w;
    out.rightEye = from.rightEye * (1.f - w) + to.rightEye * w;
    out.chargeFrac = from.chargeFrac * (1.f - w) + to.chargeFrac * w;
    out.embeddedSpikeCount =
        (uint8_t)std::lround((1.f - w) * (float)from.embeddedSpikeCount +
                             w * (float)to.embeddedSpikeCount);
    if (!from.points.empty() && from.points.size() == to.points.size()) {
        out.points.resize(to.points.size());
        for (size_t i = 0; i < to.points.size(); ++i)
            out.points[i] = from.points[i] * (1.f - w) + to.points[i] * w;
    } else {
        out.points = to.points;
    }
    out.trail = to.trail;
    return out;
}

uint64_t trailQuantKey(float x, float y) {
    const int gx = (int)std::floor(x / 0.065f);
    const int gy = (int)std::floor(y / 0.065f);
    return (uint64_t)(uint32_t)gx << 32 | (uint32_t)gy;
}

JoinFeedback mapJoinResult(JoinResult r) {
    switch (r) {
        case JoinResult::Ok:            return JoinFeedback::Ok;
        case JoinResult::NotFound:      return JoinFeedback::NotFound;
        case JoinResult::Full:          return JoinFeedback::Full;
        case JoinResult::AlreadyInRoom: return JoinFeedback::AlreadyIn;
        case JoinResult::BadName:       return JoinFeedback::BadName;
        case JoinResult::TooManyRooms:  return JoinFeedback::TooManyRooms;
    }
    return JoinFeedback::None;
}

} // namespace

void Client::resetInterpolationState() {
    haveSnapshot_ = false;
    slimesPrev_.clear();
    slimesCurr_.clear();
    rigidsPrev_.clear();
    rigidsCurr_.clear();
    slimesDisplay_.clear();
    rigidsDisplay_.clear();
    netTrailAcc_.clear();
    snapT0_ = snapT1_ = clock::now();
    snapIntervalEma_ = 1.f / 60.f;
    for (auto& b : chatBubbles_) {
        b.text.clear();
        b.remainingSec = 0.f;
    }
}

void Client::pushChatBubble(int slot, std::string&& msg) {
    if (slot < 0 || slot >= kMaxPlayers) return;
    auto& b = chatBubbles_[(size_t)slot];
    b.text = std::move(msg);
    b.remainingSec = 3.f;
}

void Client::ingestSnapshot(uint32_t frame, std::vector<RemoteSlime>&& slimes,
                           std::vector<RigidNetSample>&& rigids) {
    lastFrame_ = frame;
    if (!haveSnapshot_) {
        slimesCurr_ = std::move(slimes);
        rigidsCurr_ = std::move(rigids);
        slimesPrev_ = slimesCurr_;
        rigidsPrev_ = rigidsCurr_;
        snapT0_ = snapT1_ = clock::now();
        haveSnapshot_ = true;
    } else {
        slimesPrev_ = std::move(slimesCurr_);
        slimesCurr_ = std::move(slimes);
        rigidsPrev_ = std::move(rigidsCurr_);
        rigidsCurr_ = std::move(rigids);
        snapT0_ = snapT1_;
        snapT1_ = clock::now();
        const float dt = std::chrono::duration<float>(snapT1_ - snapT0_).count();
        if (dt > 1e-4f && dt < 0.35f)
            snapIntervalEma_ = snapIntervalEma_ * 0.78f + dt * 0.22f;
    }
}

bool Client::connect(const std::string& host, uint16_t port, uint32_t timeoutMs) {
    myId_ = -1;
    lastFrame_ = 0;
    state_ = ClientState::Disconnected;
    roomList_.clear();
    joinFeedback_ = JoinFeedback::None;
    currentRoomId_ = 0;
    serverBuild_ = 0;
    updateNoticeBuild_ = 0;
    updateNoticeUrl_.clear();
    resetInterpolationState();

    if (enet_initialize() != 0) {
        std::fprintf(stderr, "enet_initialize failed\n");
        return false;
    }
    enetReady_ = true;

    host_ = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!host_) {
        std::fprintf(stderr, "enet_host_create (client) failed\n");
        enet_deinitialize();
        enetReady_ = false;
        return false;
    }

    ENetAddress addr{};
    if (!resolveConnectAddress(&addr, host.c_str())) {
        std::fprintf(stderr, "could not resolve host \"%s\"\n", host.c_str());
        enet_host_destroy(host_);
        host_ = nullptr;
        enet_deinitialize();
        enetReady_ = false;
        return false;
    }
    addr.port = port;

    char resolved[64]{};
    if (enet_address_get_host_ip(&addr, resolved, sizeof(resolved)) == 0) {
        std::printf("[client] connecting to %s (UDP %u), timeout %ums\n",
                    resolved, (unsigned)port, (unsigned)timeoutMs);
    }

    peer_ = enet_host_connect(host_, &addr, 2, 0);
    if (!peer_) {
        std::fprintf(stderr, "enet_host_connect failed\n");
        enet_host_destroy(host_);
        host_ = nullptr;
        enet_deinitialize();
        enetReady_ = false;
        return false;
    }
    ENetEvent event;
    if (enet_host_service(host_, &event, timeoutMs) > 0 &&
        event.type == ENET_EVENT_TYPE_CONNECT) {
        connected_ = true;
        state_ = ClientState::Lobby;
        ClientHelloMsg hello{};
        hello.hdr.type = (uint8_t)MsgType::ClientHello;
        hello.protocolVersion = kProtocolVersion;
        ENetPacket* pk = enet_packet_create(&hello, sizeof(hello),
                                            ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(peer_, 0, pk);
        std::printf("[client] connected to %s:%u\n", host.c_str(), (unsigned)port);
        return true;
    }
    std::fprintf(stderr,
        "[client] connection to %s:%u timed out after %ums.\n"
        "  Check: (1) the server hub is running on that PC,\n"
        "  (2) Windows Defender Firewall → allow inbound UDP %u for the server exe,\n"
        "  (3) the server LAN IP is still %s (ipconfig).\n",
        host.c_str(), (unsigned)port, (unsigned)timeoutMs,
        (unsigned)port, host.c_str());
    enet_peer_reset(peer_);
    peer_ = nullptr;
    enet_host_destroy(host_);
    host_ = nullptr;
    enet_deinitialize();
    enetReady_ = false;
    return false;
}

void Client::disconnect() {
    if (!enetReady_) return;
    if (peer_) {
        enet_peer_disconnect(peer_, 0);
        ENetEvent event;
        for (int i = 0; i < 30; ++i) {
            if (enet_host_service(host_, &event, 30) > 0) {
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) break;
                if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
            }
        }
        enet_peer_reset(peer_);
        peer_ = nullptr;
    }
    if (host_) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }
    connected_ = false;
    state_ = ClientState::Disconnected;
    enet_deinitialize();
    enetReady_ = false;
    resetInterpolationState();
}

void Client::requestRoomList() {
    if (!isConnected()) return;
    ClientListRoomsMsg m{};
    m.hdr.type = (uint8_t)MsgType::ClientListRooms;
    ENetPacket* pk = enet_packet_create(&m, sizeof(m), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer_, 0, pk);
}

void Client::createRoom(const std::string& name, int maxPlayers, uint8_t optionsFlags) {
    if (!isConnected()) return;
    if (state_ != ClientState::Lobby) return;
    const size_t n = std::min(name.size(), (size_t)kMaxRoomNameBytes);
    std::vector<uint8_t> buf(sizeof(ClientCreateRoomMsg) + n);
    auto* h = reinterpret_cast<ClientCreateRoomMsg*>(buf.data());
    h->hdr.type = (uint8_t)MsgType::ClientCreateRoom;
    h->nameLen = (uint8_t)n;
    h->maxPlayers = (uint8_t)std::clamp(maxPlayers, 1, kMaxPlayers);
    h->optionsFlags = optionsFlags;
    if (n) std::memcpy(buf.data() + sizeof(ClientCreateRoomMsg), name.data(), n);
    ENetPacket* pk = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer_, 0, pk);
    state_ = ClientState::Joining;
}

void Client::joinRoom(uint32_t roomId) {
    if (!isConnected()) return;
    if (state_ != ClientState::Lobby) return;
    ClientJoinRoomMsg m{};
    m.hdr.type = (uint8_t)MsgType::ClientJoinRoom;
    m.roomId = roomId;
    ENetPacket* pk = enet_packet_create(&m, sizeof(m), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer_, 0, pk);
    state_ = ClientState::Joining;
}

void Client::leaveRoom() {
    if (!isConnected()) return;
    if (state_ != ClientState::InRoom) return;
    ClientLeaveRoomMsg m{};
    m.hdr.type = (uint8_t)MsgType::ClientLeaveRoom;
    ENetPacket* pk = enet_packet_create(&m, sizeof(m), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer_, 0, pk);
    state_ = ClientState::Lobby;
    myId_ = -1;
    currentRoomId_ = 0;
    resetInterpolationState();
}

void Client::sendInput(Vec2 aimWorld, bool jumpHeld, bool mergeHeld, bool grabHeld,
                       bool respawnHeld, bool gatherHeld, bool shiftSplitClick,
                       bool switchFragmentClick, uint8_t colorIndex) {
    if (!isConnected() || state_ != ClientState::InRoom) return;
    ClientInputMsg m{};
    m.hdr.type = (uint8_t)MsgType::ClientInput;
    m.aimX = aimWorld.x;
    m.aimY = aimWorld.y;
    m.jumpHeld = (uint8_t)(jumpHeld ? 1 : 0);
    m.mergeHeld = (uint8_t)(mergeHeld ? 1 : 0);
    m.grabHeld = (uint8_t)(grabHeld ? 1 : 0);
    m.respawnHeld = (uint8_t)(respawnHeld ? 1 : 0);
    m.gatherHeld = (uint8_t)(gatherHeld ? 1 : 0);
    m.shiftSplitClick = (uint8_t)(shiftSplitClick ? 1 : 0);
    m.switchFragmentClick = (uint8_t)(switchFragmentClick ? 1 : 0);
    m.colorIndex = (uint8_t)(colorIndex % 8);
    // Unreliable @ ~60 Hz — reliable input queued behind ACKs and felt like huge "ping".
    ENetPacket* pk = enet_packet_create(&m, sizeof(m), 0);
    enet_peer_send(peer_, 0, pk);
}

void Client::sendChat(const std::string& utf8) {
    if (utf8.empty() || !isConnected() || state_ != ClientState::InRoom) return;
    const size_t n = std::min(utf8.size(), (size_t)kMaxChatPayloadBytes);
    std::string payload = utf8.substr(0, n);
    std::vector<uint8_t> buf(2 + payload.size());
    buf[0] = (uint8_t)MsgType::ClientChat;
    buf[1] = (uint8_t)payload.size();
    if (!payload.empty())
        std::memcpy(buf.data() + 2, payload.data(), payload.size());
    ENetPacket* pk = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer_, 0, pk);
    if (myId_ >= 0 && myId_ < kMaxPlayers)
        pushChatBubble(myId_, std::move(payload));
}

void Client::tickChatBubbles(float dt) {
    if (dt <= 0.f) return;
    for (auto& b : chatBubbles_) {
        if (b.remainingSec <= 0.f) continue;
        b.remainingSec -= dt;
        if (b.remainingSec <= 0.f) {
            b.remainingSec = 0.f;
            b.text.clear();
        }
    }
}

float Client::chatBubbleAlpha(int slot) const {
    if (slot < 0 || slot >= kMaxPlayers) return 0.f;
    const auto& b = chatBubbles_[(size_t)slot];
    if (b.remainingSec <= 0.f || b.text.empty()) return 0.f;
    constexpr float kFade = 0.35f;
    if (b.remainingSec >= kFade) return 1.f;
    return b.remainingSec / kFade;
}

const std::string* Client::chatBubbleText(int slot) const {
    if (slot < 0 || slot >= kMaxPlayers) return nullptr;
    const auto& b = chatBubbles_[(size_t)slot];
    if (b.remainingSec <= 0.f || b.text.empty()) return nullptr;
    return &b.text;
}

void Client::updateNetTrail(float dt) {
    constexpr float kFadeFast = 0.095f;
    constexpr float kFadeSlow = 0.034f;

    std::unordered_set<uint32_t> activeOwners;
    activeOwners.reserve(slimesDisplay_.size());
    for (const auto& rs : slimesDisplay_) activeOwners.insert(rs.ownerId);

    for (const auto& rs : slimesDisplay_) {
        if (!rs.isPrimaryFragment) continue;
        auto& ownerMap = netTrailAcc_[rs.ownerId];
        std::unordered_set<uint64_t> innerSeen;
        innerSeen.reserve(rs.trail.size() * 2 + 4);
        const bool anyDrop = !rs.trail.empty();
        for (const auto& t : rs.trail) {
            const uint64_t k = trailQuantKey(t.x, t.y);
            innerSeen.insert(k);
            SlimePuddle p;
            p.pos = {t.x, t.y};
            p.radius = t.radius;
            p.alpha = t.alpha;
            ownerMap[k] = p;
        }
        for (auto it = ownerMap.begin(); it != ownerMap.end();) {
            if (innerSeen.count(it->first) != 0) { ++it; continue; }
            const float rate = anyDrop ? kFadeFast : kFadeSlow;
            it->second.alpha -= dt * rate;
            if (it->second.alpha <= 0.f) it = ownerMap.erase(it);
            else ++it;
        }
    }
    for (auto it = netTrailAcc_.begin(); it != netTrailAcc_.end();) {
        if (!activeOwners.count(it->first)) it = netTrailAcc_.erase(it);
        else ++it;
    }
}

void Client::copyNetTrailForDraw(std::vector<SlimePuddle>& out) const {
    out.clear();
    size_t total = 0;
    for (const auto& pr : netTrailAcc_) total += pr.second.size();
    out.reserve(std::min(total, (size_t)900));
    for (const auto& pr : netTrailAcc_) {
        for (const auto& cell : pr.second) {
            out.push_back(cell.second);
            if (out.size() >= 900) return;
        }
    }
}

void Client::advanceInterpolation() {
    if (!haveSnapshot_) {
        slimesDisplay_.clear();
        rigidsDisplay_.clear();
        return;
    }
    const auto now = clock::now();
    float span = std::max(snapIntervalEma_, 1.f / 90.f);
    float alpha = std::chrono::duration<float>(now - snapT0_).count() / span;
    alpha = std::clamp(alpha, 0.f, 1.f);

    std::unordered_map<uint64_t, const RemoteSlime*> prevByKey;
    prevByKey.reserve(slimesPrev_.size() * 2 + 8);
    for (const auto& s : slimesPrev_)
        prevByKey.emplace(slimeInterpKey(s.ownerId, s.fragmentId), &s);

    slimesDisplay_.clear();
    slimesDisplay_.reserve(slimesCurr_.size());
    for (const auto& curr : slimesCurr_) {
        const RemoteSlime* prev = nullptr;
        auto pit = prevByKey.find(slimeInterpKey(curr.ownerId, curr.fragmentId));
        if (pit != prevByKey.end()) prev = pit->second;
        if (prev) slimesDisplay_.push_back(blendSlime(*prev, curr, alpha));
        else slimesDisplay_.push_back(curr);
    }

    rigidsDisplay_.clear();
    rigidsDisplay_.reserve(rigidsCurr_.size());
    for (const auto& curr : rigidsCurr_) {
        const RigidNetSample* prev = findRigidById(rigidsPrev_, curr.bodyId);
        RigidNetSample out = curr;
        if (prev) {
            out.pos = prev->pos * (1.f - alpha) + curr.pos * alpha;
            out.rot = lerpAngleRad(prev->rot, curr.rot, alpha);
        }
        rigidsDisplay_.push_back(out);
    }
}

void Client::handleWelcome(const uint8_t* data, size_t len) {
    if (len < sizeof(ServerWelcomeMsg)) return;
    ServerWelcomeMsg w{};
    std::memcpy(&w, data, sizeof(w));
    if (w.protocolVersion != kProtocolVersion) {
        std::fprintf(stderr,
            "[client] protocol mismatch (server %u, us %u) — disconnecting\n",
            w.protocolVersion, kProtocolVersion);
        disconnect();
        return;
    }
    serverBuild_ = w.serverBuild;
    state_ = ClientState::Lobby;
    std::printf("[client] welcome — server build %u\n", (unsigned)serverBuild_);
}

void Client::handleRoomList(const uint8_t* data, size_t len) {
    if (len < sizeof(ServerRoomListMsg)) return;
    ServerRoomListMsg hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    const uint8_t* p = data + sizeof(hdr);
    size_t left = len - sizeof(hdr);
    std::vector<LobbyRoomInfo> out;
    out.reserve(hdr.numRooms);
    for (uint16_t i = 0; i < hdr.numRooms; ++i) {
        if (left < sizeof(RoomInfoNet)) return;
        RoomInfoNet info{};
        std::memcpy(&info, p, sizeof(info));
        p += sizeof(info);
        left -= sizeof(info);
        if (left < info.nameLen) return;
        LobbyRoomInfo lr;
        lr.roomId = info.roomId;
        lr.playerCount = info.playerCount;
        lr.maxPlayers = info.maxPlayers;
        lr.optionsFlags = info.optionsFlags;
        lr.name.assign((const char*)p, (size_t)info.nameLen);
        p += info.nameLen;
        left -= info.nameLen;
        out.push_back(std::move(lr));
    }
    roomList_ = std::move(out);
}

void Client::handleJoinResult(const uint8_t* data, size_t len) {
    if (len < sizeof(ServerJoinResultMsg)) return;
    ServerJoinResultMsg r{};
    std::memcpy(&r, data, sizeof(r));
    JoinResult code = (JoinResult)r.code;
    joinFeedback_ = mapJoinResult(code);
    if (code == JoinResult::Ok) {
        currentRoomId_ = r.roomId;
        myId_ = (int)r.yourSlot;
        state_ = ClientState::InRoom;
        resetInterpolationState();
        std::printf("[client] joined room %u as slot %d\n",
                    (unsigned)r.roomId, myId_);
    } else {
        state_ = ClientState::Lobby;
    }
}

void Client::handleKickToLobby(const uint8_t* data, size_t len) {
    (void)data; (void)len;
    if (state_ == ClientState::InRoom) {
        std::printf("[client] kicked back to lobby\n");
        state_ = ClientState::Lobby;
        myId_ = -1;
        currentRoomId_ = 0;
        resetInterpolationState();
    }
}

void Client::handleVersionInfo(const uint8_t* data, size_t len) {
    if (len < sizeof(ServerVersionInfoMsg)) return;
    ServerVersionInfoMsg v{};
    std::memcpy(&v, data, sizeof(v));
    if (len < sizeof(v) + v.urlLen) return;
    updateNoticeBuild_ = v.newBuild;
    updateNoticeUrl_.assign((const char*)data + sizeof(v), (size_t)v.urlLen);
    std::printf("[client] server announced new build %u (%s)\n",
                (unsigned)v.newBuild, updateNoticeUrl_.c_str());
}

void Client::handleSnapshot(const uint8_t* data, size_t len) {
    if (state_ != ClientState::InRoom) return;
    const uint8_t* p = data;
    size_t left = len;
    if (left < sizeof(ServerSnapshotMsg)) return;
    ServerSnapshotMsg snap;
    std::memcpy(&snap, p, sizeof(snap));
    p += sizeof(snap);
    left -= sizeof(snap);

    std::vector<RemoteSlime> decodedSlimes;
    decodedSlimes.reserve(snap.numSlimes);
    for (uint16_t i = 0; i < snap.numSlimes; ++i) {
        if (left < sizeof(SlimeStatePayload)) return;
        SlimeStatePayload payload;
        std::memcpy(&payload, p, sizeof(payload));
        p += sizeof(payload);
        left -= sizeof(payload);
        size_t needed = (size_t)payload.numPoints * sizeof(float) * 2;
        if (left < needed) return;

        RemoteSlime rs;
        rs.ownerId = payload.ownerId;
        rs.fragmentId = payload.fragmentInfo & kSlimeFragmentIdMask;
        rs.isLocalPlayer = payload.isLocal != 0;
        rs.isPrimaryFragment = (payload.fragmentInfo & kSlimeFragmentPrimaryBit) != 0;
        rs.centroid = {payload.cx, payload.cy};
        rs.vel = {payload.vx, payload.vy};
        rs.leftEye = {payload.leftEyeX, payload.leftEyeY};
        rs.rightEye = {payload.rightEyeX, payload.rightEyeY};
        rs.aim = {payload.aimX, payload.aimY};
        rs.chargeFrac = payload.chargeFrac;
        rs.isCharging = payload.isCharging != 0;
        rs.embeddedSpikeCount = payload.embeddedSpikeCount;
        rs.colorIndex = payload.colorIndex;
        rs.points.resize(payload.numPoints);
        for (uint16_t k = 0; k < payload.numPoints; ++k) {
            float xy[2];
            std::memcpy(xy, p, sizeof(xy));
            p += sizeof(xy);
            left -= sizeof(xy);
            rs.points[k] = {xy[0], xy[1]};
        }
        if (left < sizeof(uint16_t)) return;
        uint16_t nTrail = 0;
        std::memcpy(&nTrail, p, sizeof(nTrail));
        p += sizeof(nTrail);
        left -= sizeof(nTrail);
        if (nTrail > 64) return;
        const size_t trailBytes = (size_t)nTrail * sizeof(TrailDropNet);
        if (left < trailBytes) return;
        rs.trail.resize(nTrail);
        for (uint16_t k = 0; k < nTrail; ++k) {
            TrailDropNet td;
            std::memcpy(&td, p, sizeof(td));
            p += sizeof(td);
            left -= sizeof(td);
            rs.trail[k] = td;
        }
        decodedSlimes.push_back(std::move(rs));
    }

    std::vector<RigidNetSample> decodedRigids;
    decodedRigids.reserve(snap.numDynamicRigids);
    for (uint16_t i = 0; i < snap.numDynamicRigids; ++i) {
        if (left < sizeof(DynamicRigidNetState)) return;
        DynamicRigidNetState raw;
        std::memcpy(&raw, p, sizeof(raw));
        p += sizeof(raw);
        left -= sizeof(raw);
        RigidNetSample r;
        r.bodyId = raw.bodyId;
        r.pos = {raw.x, raw.y};
        r.rot = raw.rot;
        decodedRigids.push_back(r);
    }

    if (decodedSlimes.size() != snap.numSlimes) return;
    if (decodedRigids.size() != snap.numDynamicRigids) return;

    ingestSnapshot(snap.frame, std::move(decodedSlimes), std::move(decodedRigids));
}

void Client::handleChatRelay(const uint8_t* data, size_t len) {
    if (len < sizeof(ServerChatRelayMsg)) return;
    ServerChatRelayMsg rh;
    std::memcpy(&rh, data, sizeof(rh));
    if (rh.senderSlot >= (uint32_t)kMaxPlayers) return;
    if (rh.byteLen > (uint8_t)kMaxChatPayloadBytes) return;
    if (len < sizeof(rh) + rh.byteLen) return;
    std::string msg((const char*)data + sizeof(rh), (size_t)rh.byteLen);
    pushChatBubble((int)rh.senderSlot, std::move(msg));
}

void Client::serviceIncoming() {
    if (!host_) return;
    ENetEvent event;
    while (host_) {
        if (enet_host_service(host_, &event, 0) <= 0) break;
        switch (event.type) {
            case ENET_EVENT_TYPE_RECEIVE: {
                if (event.packet->dataLength >= sizeof(MsgHeader)) {
                    auto type = (MsgType)event.packet->data[0];
                    switch (type) {
                        case MsgType::ServerWelcome:
                            handleWelcome(event.packet->data, event.packet->dataLength);
                            break;
                        case MsgType::ServerRoomList:
                            handleRoomList(event.packet->data, event.packet->dataLength);
                            break;
                        case MsgType::ServerJoinResult:
                            handleJoinResult(event.packet->data, event.packet->dataLength);
                            break;
                        case MsgType::ServerKickToLobby:
                            handleKickToLobby(event.packet->data, event.packet->dataLength);
                            break;
                        case MsgType::ServerVersionInfo:
                            handleVersionInfo(event.packet->data, event.packet->dataLength);
                            break;
                        case MsgType::ServerSnapshot:
                            handleSnapshot(event.packet->data, event.packet->dataLength);
                            break;
                        case MsgType::ServerChatRelay:
                            handleChatRelay(event.packet->data, event.packet->dataLength);
                            break;
                        default: break;
                    }
                }
                enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
                std::printf("[client] disconnected by server\n");
                connected_ = false;
                state_ = ClientState::Disconnected;
                break;
            default: break;
        }
    }
}

HubUpdateCheckResult pollHubForClientUpdate(const std::string& host, uint16_t port,
                                            uint32_t& outNoticeBuild, std::string& outDownloadUrl,
                                            uint32_t wallTimeoutMs) {
    outNoticeBuild = 0;
    outDownloadUrl.clear();
    Client c;
    const uint32_t connectMs = std::min(wallTimeoutMs, 12000u);
    if (!c.connect(host, port, connectMs)) return HubUpdateCheckResult::HubUnreachable;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(wallTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        c.serviceIncoming();
        if (c.needsClientUpgrade()) {
            outNoticeBuild = c.updateNoticeBuild();
            outDownloadUrl = c.updateNoticeUrl();
            c.disconnect();
            return HubUpdateCheckResult::ReadyToDownload;
        }
        if (c.hasUpdateNotice()) {
            outNoticeBuild = c.updateNoticeBuild();
            outDownloadUrl = c.updateNoticeUrl();
            c.disconnect();
            return HubUpdateCheckResult::ClientUpToDate;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    c.disconnect();
    return HubUpdateCheckResult::NoUpdatePublished;
}

} // namespace pe::net
