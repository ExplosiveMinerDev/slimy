// Headless multi-room server entry point. No raylib / rendering — just physics
// + ENet networking. Builds into slimyjourney_server.exe for self-hosted Windows.
//
// Args:
//   --port <p>            UDP port (default 6543)
//   --build <n>           Numeric build id advertised to clients (default = protocol version)
//   --manifest <path>     Path to a key=value manifest. The server polls it every 30s. If
//                         `build=N` is greater than the current build, every connected
//                         client receives a ServerVersionInfo notice. If `--auto-restart`
//                         is also set, the process then exits with code 100 after a short
//                         grace period so an external launcher can swap the binary.
//   --auto-restart        Enable the exit-100 behaviour described above.
//
// Manifest format (one entry per line, optional):
//   build=11
//   url=https://example.com/update.manifest   (client : manifest texte multi-fichiers ou exe direct)
//   url=https://example.com/slimyjourney.exe    (lien exe PE unique, compat ancienne version client)

#include "net/NetServer.h"
#include "net/Protocol.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void sigHandler(int) { g_stop.store(true, std::memory_order_relaxed); }

bool parseManifestFile(const std::string& path, uint32_t& outBuild, std::string& outUrl) {
    std::ifstream f(path);
    if (!f.good()) return false;
    bool any = false;
    std::string line;
    while (std::getline(f, line)) {
        // strip CR
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "build") {
            try { outBuild = (uint32_t)std::stoul(val); any = true; }
            catch (...) {}
        } else if (key == "url") {
            outUrl = val;
            any = true;
        }
    }
    return any;
}

} // namespace

int main(int argc, char** argv) {
    using namespace pe::net;

    uint16_t port = kDefaultPort;
    uint32_t serverBuild = kProtocolVersion;
    std::string manifestPath;
    bool autoRestart = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s expects a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (std::strcmp(a, "--port") == 0)        port = (uint16_t)std::atoi(next("--port"));
        else if (std::strcmp(a, "--build") == 0)  serverBuild = (uint32_t)std::strtoul(next("--build"), nullptr, 10);
        else if (std::strcmp(a, "--manifest") == 0) manifestPath = next("--manifest");
        else if (std::strcmp(a, "--auto-restart") == 0) autoRestart = true;
        else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            std::printf(
                "slimyjourney_server\n"
                "  --port <p>          UDP port (default %u)\n"
                "  --build <n>         Build id advertised to clients (default %u)\n"
                "  --manifest <path>   Poll this file for build/url updates\n"
                "  --auto-restart      Exit with code 100 after announcing an update\n",
                (unsigned)kDefaultPort, (unsigned)kProtocolVersion);
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a);
            return 2;
        }
    }

    std::signal(SIGINT, sigHandler);
    std::signal(SIGTERM, sigHandler);

    Server server;
    if (!server.start(port, serverBuild)) return 1;

    {
        const std::string pub = fetchPublicIPv4();
        const std::string lan = preferredLanIPv4();
        if (!pub.empty()) {
            std::printf("[server] online: %s:%u\n", pub.c_str(), (unsigned)port);
            std::printf("[server] (router must forward UDP %u to %s)\n",
                        (unsigned)port,
                        lan.empty() ? "this machine" : lan.c_str());
        } else if (!lan.empty()) {
            std::printf("[server] LAN: %s:%u  (no public IP detected)\n",
                        lan.c_str(), (unsigned)port);
        } else {
            std::printf("[server] listening on UDP *:%u (no IP detected)\n",
                        (unsigned)port);
        }
    }

    using clock = std::chrono::steady_clock;
    auto nextPrune = clock::now() + std::chrono::seconds(10);
    auto nextManifestPoll = clock::now();
    auto pendingExitAt = clock::time_point::max();
    bool exitForUpdate = false;

    std::printf("[server] running. ctrl-c to quit.\n");

    while (!g_stop.load(std::memory_order_relaxed)) {
        server.serviceIncoming();
        server.tickAndBroadcast();

        const auto now = clock::now();

        if (now >= nextPrune) {
            server.pruneEmptyRooms();
            nextPrune = now + std::chrono::seconds(10);
        }

        if (!manifestPath.empty() && now >= nextManifestPoll) {
            uint32_t newBuild = 0;
            std::string newUrl;
            if (parseManifestFile(manifestPath, newBuild, newUrl) && newBuild > serverBuild) {
                std::printf("[server] manifest reports new build %u (we run %u)\n",
                            (unsigned)newBuild, (unsigned)serverBuild);
                server.announceUpdate(newBuild, newUrl);
                if (autoRestart && !exitForUpdate) {
                    exitForUpdate = true;
                    pendingExitAt = now + std::chrono::seconds(60);
                    std::printf("[server] auto-restart armed: exiting in 60s with code 100\n");
                }
            }
            nextManifestPoll = now + std::chrono::seconds(30);
        }

        if (exitForUpdate && now >= pendingExitAt) {
            std::printf("[server] exit-for-update grace elapsed — bye\n");
            server.stop();
            return 100;
        }

        if (server.totalConnected() == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::printf("[server] shutting down\n");
    server.stop();
    return 0;
}
