#include "net/NetServer.h"
#include "net/Protocol.h"
#include "net/Room.h"

#include <enet/enet.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#endif

namespace pe::net {

namespace {

int ipv4Pref(const std::string& s) {
    const char* ip = s.c_str();
    if (std::strncmp(ip, "169.254.", 8) == 0) return 900;
    if (std::strncmp(ip, "172.25.", 7) == 0 || std::strncmp(ip, "172.26.", 7) == 0 ||
        std::strncmp(ip, "172.27.", 7) == 0)
        return 450;
    if (std::strncmp(ip, "192.168.", 8) == 0) return 100;
    if (std::strncmp(ip, "10.", 3) == 0) return 200;
    if (std::strncmp(ip, "172.", 4) == 0) return 350;
    return 400;
}

#ifdef _WIN32
std::vector<std::string> enumerateLocalIPv4ViaHostname() {
    std::vector<std::string> ips;
    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) != 0) return ips;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &res) != 0) return ips;
    for (addrinfo* p = res; p; p = p->ai_next) {
        auto* a = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
        if (std::strncmp(ip, "127.", 4) == 0) continue;
        bool dup = false;
        for (const auto& e : ips)
            if (e == ip) {
                dup = true;
                break;
            }
        if (!dup) ips.push_back(ip);
    }
    freeaddrinfo(res);
    std::sort(ips.begin(), ips.end(),
              [](const std::string& A, const std::string& B) {
                  const int pa = ipv4Pref(A), pb = ipv4Pref(B);
                  if (pa != pb) return pa < pb;
                  return A < B;
              });
    return ips;
}

#endif

std::vector<std::string> enumerateLocalIPv4() {
#ifdef _WIN32
    return enumerateLocalIPv4ViaHostname();
#else
    return {};
#endif
}

#ifdef _WIN32
void trimIpv4Ascii(std::string& out) {
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
}

bool looksLikePublicIPv4(const std::string& out) {
    int dots = 0;
    for (char c : out) {
        if (c == '.')
            ++dots;
        else if (c < '0' || c > '9')
            return false;
    }
    return dots == 3 && out.size() >= 7 && out.size() <= 15;
}
#endif // _WIN32

} // namespace

std::string discoverLanIPv4() {
    auto ips = enumerateLocalIPv4();
    if (ips.empty()) return "(unknown)";
    std::string out = ips[0];
    for (size_t i = 1; i < ips.size(); ++i) {
        out += " / ";
        out += ips[i];
    }
    return out;
}

std::string preferredLanIPv4() {
    auto ips = enumerateLocalIPv4();
    if (ips.empty()) return "";
    for (const auto& ip : ips) {
        if (ipv4Pref(ip) < 500) return ip;
    }
    return ips.front();
}

std::string fetchPublicIPv4() {
#ifdef _WIN32
    HINTERNET sess = WinHttpOpen(L"slimyjourney/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return "";

    DWORD timeoutMs = 3000;
    WinHttpSetTimeouts(sess, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET conn =
        WinHttpConnect(sess, L"api.ipify.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) {
        WinHttpCloseHandle(sess);
        return "";
    }

    HINTERNET req =
        WinHttpOpenRequest(conn, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    std::string out;
    if (req) {
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &read)) break;
                chunk.resize(read);
                out += chunk;
                if (out.size() > 64) break;
            }
        }
        WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);

    trimIpv4Ascii(out);
    if (!looksLikePublicIPv4(out)) return "";
    return out;
#else
    return "";
#endif
}

bool Server::start(uint16_t port, uint32_t serverBuild) {
    if (host_) stop();
    if (enet_initialize() != 0) {
        std::fprintf(stderr, "enet_initialize failed\n");
        return false;
    }
    serverBuild_ = serverBuild;
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    host_ = enet_host_create(&address, kMaxPeers, 2, 0, 0);
    if (!host_) {
        std::fprintf(stderr, "enet_host_create (server) failed on port %u\n", (unsigned)port);
#ifdef _WIN32
        const int wsa = WSAGetLastError();
        std::fprintf(stderr,
                     "  WSAGetLastError=%d — if 10048, the port is already in use (another server? "
                     "slimyjourney client does not bind it). Try: netstat -ano | findstr :%u\n",
                     wsa, (unsigned)port);
#endif
        enet_deinitialize();
        return false;
    }
    lastTickStamp_ = Room::clock::now();
    std::printf("[server] hub listening on UDP *:%u  (max peers %d, max rooms %d, %d/room)\n",
                (unsigned)port, kMaxPeers, kMaxRooms, kMaxPlayers);
    return true;
}

void Server::stop() {
    if (host_) {
        for (auto& kv : peers_) {
            if (kv.first) enet_peer_disconnect_now(kv.first, 0);
        }
        enet_host_destroy(host_);
        host_ = nullptr;
    }
    peers_.clear();
    rooms_.clear();
    enet_deinitialize();
}

int Server::totalInRoom() const {
    int n = 0;
    for (auto& kv : rooms_) n += kv.second->playerCount();
    return n;
}

Room* Server::findRoom(uint32_t roomId) {
    auto it = rooms_.find(roomId);
    return it == rooms_.end() ? nullptr : it->second.get();
}

void Server::serviceIncoming() {
    if (!host_) return;
    ENetEvent event;
    while (enet_host_service(host_, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                onConnect(event.peer);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                onDisconnect(event.peer);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                onReceive(event.peer, event.packet->data, event.packet->dataLength);
                enet_packet_destroy(event.packet);
                break;
            default: break;
        }
    }
}

void Server::onConnect(_ENetPeer* peer) {
    PeerInfo pi{};
    pi.inLobby = true;
    pi.roomId = 0;
    peers_[peer] = pi;
    sendWelcomeTo(peer);
    if (updateAvailable_) sendVersionInfoTo(peer);
    sendRoomListTo(peer);
    std::printf("[server] peer connected (total %d)\n", (int)peers_.size());
}

void Server::onDisconnect(_ENetPeer* peer) {
    auto it = peers_.find(peer);
    if (it == peers_.end()) return;
    bool wasInRoom = !it->second.inLobby;
    uint32_t rid = it->second.roomId;
    peers_.erase(it);
    if (wasInRoom) {
        Room* r = findRoom(rid);
        if (r) {
            r->removePeer(peer);
            broadcastRoomListToLobby();
        }
    }
    std::printf("[server] peer disconnected (total %d)\n", (int)peers_.size());
}

void Server::onReceive(_ENetPeer* peer, const uint8_t* data, size_t len) {
    if (len < sizeof(MsgHeader)) return;
    auto type = (MsgType)data[0];
    switch (type) {
        case MsgType::ClientHello: {
            if (len < sizeof(ClientHelloMsg)) return;
            const auto* h = reinterpret_cast<const ClientHelloMsg*>(data);
            if (h->protocolVersion != kProtocolVersion) {
                std::printf("[server] hello: bad protocol version %u (we run %u) — kicking\n",
                            (unsigned)h->protocolVersion, (unsigned)kProtocolVersion);
                enet_peer_disconnect_later(peer, 0);
            }
            break;
        }
        case MsgType::ClientListRooms:
            handleListRooms(peer);
            break;
        case MsgType::ClientCreateRoom: {
            if (len < sizeof(ClientCreateRoomMsg)) return;
            const auto* m = reinterpret_cast<const ClientCreateRoomMsg*>(data);
            if (len < sizeof(ClientCreateRoomMsg) + (size_t)m->nameLen) return;
            handleCreateRoom(peer, data + sizeof(ClientCreateRoomMsg), m->nameLen);
            break;
        }
        case MsgType::ClientJoinRoom: {
            if (len < sizeof(ClientJoinRoomMsg)) return;
            const auto* m = reinterpret_cast<const ClientJoinRoomMsg*>(data);
            handleJoinRoom(peer, m->roomId);
            break;
        }
        case MsgType::ClientLeaveRoom:
            handleLeaveRoom(peer);
            break;
        case MsgType::ClientInput: {
            if (len < sizeof(ClientInputMsg)) return;
            const auto* m = reinterpret_cast<const ClientInputMsg*>(data);
            handleClientInput(peer, *m);
            break;
        }
        case MsgType::ClientChat:
            handleClientChat(peer, data, len);
            break;
        default:
            break;
    }
}

void Server::handleListRooms(_ENetPeer* peer) {
    sendRoomListTo(peer);
}

void Server::handleCreateRoom(_ENetPeer* peer, const uint8_t* name, uint8_t nameLen) {
    auto pIt = peers_.find(peer);
    if (pIt == peers_.end()) return;
    if (!pIt->second.inLobby) {
        sendJoinResult(peer, JoinResult::AlreadyInRoom, 0, 0);
        return;
    }
    if (rooms_.size() >= (size_t)kMaxRooms) {
        sendJoinResult(peer, JoinResult::TooManyRooms, 0, 0);
        return;
    }
    if (nameLen == 0 || nameLen > (uint8_t)kMaxRoomNameBytes) {
        sendJoinResult(peer, JoinResult::BadName, 0, 0);
        return;
    }
    std::string n((const char*)name, (size_t)nameLen);
    uint32_t rid = nextRoomId_++;
    auto room = std::make_unique<Room>(rid, std::move(n));
    Room* rp = room.get();
    rooms_.emplace(rid, std::move(room));

    int slot = rp->addPeer(peer);
    if (slot < 0) {
        // Should not happen — fresh room is empty.
        rooms_.erase(rid);
        sendJoinResult(peer, JoinResult::Full, 0, 0);
        return;
    }
    pIt->second.inLobby = false;
    pIt->second.roomId = rid;
    sendJoinResult(peer, JoinResult::Ok, rid, (uint32_t)slot);
    broadcastRoomListToLobby();
    std::printf("[server] created room %u \"%s\" (slot %d)\n",
                (unsigned)rid, rp->name().c_str(), slot);
}

void Server::handleJoinRoom(_ENetPeer* peer, uint32_t roomId) {
    auto pIt = peers_.find(peer);
    if (pIt == peers_.end()) return;
    if (!pIt->second.inLobby) {
        sendJoinResult(peer, JoinResult::AlreadyInRoom, 0, 0);
        return;
    }
    Room* rp = findRoom(roomId);
    if (!rp) {
        sendJoinResult(peer, JoinResult::NotFound, roomId, 0);
        return;
    }
    int slot = rp->addPeer(peer);
    if (slot < 0) {
        sendJoinResult(peer, JoinResult::Full, roomId, 0);
        return;
    }
    pIt->second.inLobby = false;
    pIt->second.roomId = roomId;
    sendJoinResult(peer, JoinResult::Ok, roomId, (uint32_t)slot);
    broadcastRoomListToLobby();
    std::printf("[server] peer joined room %u \"%s\" (slot %d)\n",
                (unsigned)roomId, rp->name().c_str(), slot);
}

void Server::handleLeaveRoom(_ENetPeer* peer) {
    auto pIt = peers_.find(peer);
    if (pIt == peers_.end()) return;
    if (pIt->second.inLobby) return;
    uint32_t rid = pIt->second.roomId;
    Room* rp = findRoom(rid);
    if (rp) rp->removePeer(peer);
    pIt->second.inLobby = true;
    pIt->second.roomId = 0;
    sendRoomListTo(peer);
    broadcastRoomListToLobby();
}

void Server::handleClientInput(_ENetPeer* peer, const ClientInputMsg& m) {
    auto pIt = peers_.find(peer);
    if (pIt == peers_.end() || pIt->second.inLobby) return;
    Room* rp = findRoom(pIt->second.roomId);
    if (!rp) return;
    int slot = rp->slotForPeer(peer);
    if (slot < 0) return;
    rp->setInput(slot, {m.aimX, m.aimY},
                 m.jumpHeld != 0, m.mergeHeld != 0, m.grabHeld != 0, m.respawnHeld != 0);
}

void Server::handleClientChat(_ENetPeer* peer, const uint8_t* pkt, size_t len) {
    if (len < sizeof(ClientChatMsg)) return;
    auto pIt = peers_.find(peer);
    if (pIt == peers_.end() || pIt->second.inLobby) return;
    Room* rp = findRoom(pIt->second.roomId);
    if (!rp) return;
    int slot = rp->slotForPeer(peer);
    if (slot < 0) return;

    uint8_t bl = pkt[sizeof(MsgHeader)];
    if (bl > (uint8_t)kMaxChatPayloadBytes) bl = (uint8_t)kMaxChatPayloadBytes;
    if (bl == 0) return;
    if (len < sizeof(ClientChatMsg) + (size_t)bl) return;
    rp->relayChat(host_, slot, pkt + sizeof(ClientChatMsg), bl);
}

void Server::sendWelcomeTo(_ENetPeer* peer) {
    ServerWelcomeMsg w{};
    w.hdr.type = (uint8_t)MsgType::ServerWelcome;
    w.protocolVersion = kProtocolVersion;
    w.serverBuild = serverBuild_;
    ENetPacket* pk = enet_packet_create(&w, sizeof(w), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, pk);
}

void Server::sendRoomListTo(_ENetPeer* peer) {
    std::vector<uint8_t> buf;
    buf.reserve(64 + rooms_.size() * 48);
    ServerRoomListMsg hdr{};
    hdr.hdr.type = (uint8_t)MsgType::ServerRoomList;
    hdr.numRooms = (uint16_t)rooms_.size();
    size_t hdrOff = buf.size();
    buf.resize(buf.size() + sizeof(hdr));

    for (auto& kv : rooms_) {
        const Room& r = *kv.second;
        const std::string& name = r.name();
        uint8_t nameLen = (uint8_t)std::min(name.size(), (size_t)kMaxRoomNameBytes);
        RoomInfoNet info{};
        info.roomId = r.id();
        info.nameLen = nameLen;
        info.playerCount = (uint8_t)r.playerCount();
        info.maxPlayers = (uint8_t)r.maxPlayers();
        size_t off = buf.size();
        buf.resize(off + sizeof(info) + nameLen);
        std::memcpy(buf.data() + off, &info, sizeof(info));
        if (nameLen) std::memcpy(buf.data() + off + sizeof(info), name.data(), nameLen);
    }
    std::memcpy(buf.data() + hdrOff, &hdr, sizeof(hdr));

    ENetPacket* pk = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, pk);
}

void Server::broadcastRoomListToLobby() {
    for (auto& kv : peers_) {
        if (kv.second.inLobby) sendRoomListTo(kv.first);
    }
}

void Server::sendJoinResult(_ENetPeer* peer, JoinResult code, uint32_t roomId, uint32_t slot) {
    ServerJoinResultMsg r{};
    r.hdr.type = (uint8_t)MsgType::ServerJoinResult;
    r.code = (uint8_t)code;
    r.roomId = roomId;
    r.yourSlot = slot;
    ENetPacket* pk = enet_packet_create(&r, sizeof(r), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, pk);
}

void Server::sendVersionInfoTo(_ENetPeer* peer) {
    if (!updateAvailable_) return;
    const uint8_t urlLen = (uint8_t)std::min(updateUrl_.size(), (size_t)255);
    std::vector<uint8_t> buf(sizeof(ServerVersionInfoMsg) + urlLen);
    auto* m = reinterpret_cast<ServerVersionInfoMsg*>(buf.data());
    m->hdr.type = (uint8_t)MsgType::ServerVersionInfo;
    m->newBuild = updateBuild_;
    m->urlLen = urlLen;
    if (urlLen)
        std::memcpy(buf.data() + sizeof(ServerVersionInfoMsg), updateUrl_.data(), urlLen);
    ENetPacket* pk = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, pk);
}

void Server::announceUpdate(uint32_t newBuild, const std::string& releaseUrl) {
    updateAvailable_ = true;
    updateBuild_ = newBuild;
    updateUrl_ = releaseUrl;
    for (auto& kv : peers_) sendVersionInfoTo(kv.first);
}

void Server::tickAndBroadcast() {
    if (!host_) return;
    auto now = Room::clock::now();
    float elapsed = std::chrono::duration<float>(now - lastTickStamp_).count();
    lastTickStamp_ = now;
    if (elapsed > 0.05f) elapsed = 0.05f;

    for (auto& kv : rooms_) {
        kv.second->tick(elapsed);
        kv.second->broadcastSnapshot(host_);
    }
    enet_host_flush(host_);
}

void Server::pruneEmptyRooms() {
    std::vector<uint32_t> doomed;
    for (auto& kv : rooms_) {
        if (kv.second->isEmpty() &&
            kv.second->emptySeconds() >= kRoomPurgeAfterEmptySec)
            doomed.push_back(kv.first);
    }
    if (doomed.empty()) return;
    for (uint32_t id : doomed) {
        std::printf("[server] purging idle room %u\n", (unsigned)id);
        rooms_.erase(id);
    }
    broadcastRoomListToLobby();
}

} // namespace pe::net
