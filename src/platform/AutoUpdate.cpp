#include "platform/AutoUpdate.h"
#include "net/Protocol.h"

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace pe::platform {
namespace {

constexpr size_t kMaxManifestBytes = 512u * 1024u;

std::wstring utf8ToWide(const std::string& u) {
    if (u.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), w.data(), n);
    return w;
}

std::string trimAscii(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

bool splitHttpUrl(const std::string& url, bool& https, std::string& host,
                  INTERNET_PORT& port, std::string& pathAndQuery) {
    const char* p = url.c_str();
    if (std::strncmp(p, "https://", 8) == 0) {
        https = true;
        p += 8;
    } else if (std::strncmp(p, "http://", 7) == 0) {
        https = false;
        p += 7;
    } else {
        std::fprintf(stderr, "[update] URL doit commencer par http:// ou https://\n");
        return false;
    }

    const char* slash = std::strchr(p, '/');
    std::string authority;
    if (slash) {
        authority.assign(p, slash);
        pathAndQuery.assign(slash);
    } else {
        authority = p;
        pathAndQuery = "/";
    }
    if (authority.empty()) return false;

    const size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        const std::string ps = authority.substr(colon + 1);
        const int pi = std::atoi(ps.c_str());
        if (pi <= 0 || pi > 65535) return false;
        port = (INTERNET_PORT)pi;
    } else {
        host = authority;
        port = https ? (INTERNET_PORT)443 : (INTERNET_PORT)80;
    }
    return !host.empty();
}

bool httpGetUtf8(const std::string& urlUtf8, std::string& out, size_t maxBytes) {
    out.clear();
    bool https = false;
    std::string host;
    INTERNET_PORT port = 0;
    std::string pathAndQuery;
    if (!splitHttpUrl(urlUtf8, https, host, port, pathAndQuery)) return false;

    std::wstring hostW = utf8ToWide(host);
    std::wstring objectW = utf8ToWide(pathAndQuery);
    if (hostW.empty() || objectW.empty()) return false;

    HINTERNET session =
        WinHttpOpen(L"SlimyJourney/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    HINTERNET conn = WinHttpConnect(session, hostW.c_str(), port, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD flags = WINHTTP_FLAG_REFRESH;
    if (https) flags |= WINHTTP_FLAG_SECURE;

    HINTERNET req =
        WinHttpOpenRequest(conn, L"GET", objectW.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD sz = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                             WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    std::vector<uint8_t> chunk(65536);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), (DWORD)chunk.size(), &read)) {
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return false;
        }
        if (read == 0) break;
        if (out.size() + read > maxBytes) {
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return false;
        }
        out.append(reinterpret_cast<const char*>(chunk.data()), read);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return true;
}

bool parseHubReleaseText(const std::string& text, uint32_t& outBuild, std::string& outUrl) {
    outBuild = 0;
    outUrl.clear();
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        line = trimAscii(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trimAscii(line.substr(0, eq));
        std::string val = trimAscii(line.substr(eq + 1));
        if (key == "build") {
            try {
                outBuild = (uint32_t)std::stoul(val);
            } catch (...) {}
        } else if (key == "url") {
            outUrl = val;
        }
    }
    return outBuild != 0 && !outUrl.empty();
}

bool httpDownloadToFile(const std::string& urlUtf8, const std::wstring& destPath) {
    bool https = false;
    std::string host;
    INTERNET_PORT port = 0;
    std::string pathAndQuery;
    if (!splitHttpUrl(urlUtf8, https, host, port, pathAndQuery)) return false;

    std::wstring hostW = utf8ToWide(host);
    std::wstring objectW = utf8ToWide(pathAndQuery);
    if (hostW.empty() || objectW.empty()) {
        std::fprintf(stderr, "[update] URL invalide (hote ou chemin vide)\n");
        return false;
    }

    HINTERNET session =
        WinHttpOpen(L"SlimyJourney/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        std::fprintf(stderr, "[update] WinHttpOpen failed (%lu)\n",
                     (unsigned long)GetLastError());
        return false;
    }

    HINTERNET conn = WinHttpConnect(session, hostW.c_str(), port, 0);
    if (!conn) {
        std::fprintf(stderr, "[update] WinHttpConnect failed (%lu)\n",
                     (unsigned long)GetLastError());
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD flags = WINHTTP_FLAG_REFRESH;
    if (https) flags |= WINHTTP_FLAG_SECURE;

    HINTERNET req =
        WinHttpOpenRequest(conn, L"GET", objectW.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        std::fprintf(stderr, "[update] WinHttpOpenRequest failed (%lu)\n",
                     (unsigned long)GetLastError());
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        std::fprintf(stderr, "[update] HTTP request failed (%lu)\n",
                     (unsigned long)GetLastError());
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD sz = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                             WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        std::fprintf(stderr, "[update] HTTP status %lu (attendu 200)\n",
                     (unsigned long)status);
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    HANDLE file = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "[update] impossible creer fichier temp (%lu)\n",
                     (unsigned long)GetLastError());
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    std::vector<uint8_t> chunk(65536);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), (DWORD)chunk.size(), &read)) {
            std::fprintf(stderr, "[update] WinHttpReadData (%lu)\n",
                         (unsigned long)GetLastError());
            CloseHandle(file);
            DeleteFileW(destPath.c_str());
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return false;
        }
        if (read == 0) break;
        DWORD written = 0;
        if (!WriteFile(file, chunk.data(), read, &written, nullptr) || written != read) {
            std::fprintf(stderr, "[update] ecriture fichier incomplete\n");
            CloseHandle(file);
            DeleteFileW(destPath.c_str());
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return false;
        }
    }

    CloseHandle(file);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return true;
}

bool ensureDirsForFilePath(const std::wstring& fullPath) {
    size_t pos = fullPath.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos < 3) return true;
    std::wstring dir = fullPath.substr(0, pos);
    size_t i = 0;
    if (dir.size() >= 2 && dir[1] == L':')
        i = 2;
    while (i < dir.size()) {
        while (i < dir.size() && dir[i] == L'\\') ++i;
        size_t j = i;
        while (j < dir.size() && dir[j] != L'\\') ++j;
        if (j > i) {
            std::wstring partial = dir.substr(0, j);
            CreateDirectoryW(partial.c_str(), nullptr);
        }
        i = j;
    }
    return true;
}

bool safeRelativeUtf8(const std::string& rel) {
    if (rel.empty()) return false;
    if (rel[0] == '/' || rel[0] == '\\') return false;
    if (rel.find(':') != std::string::npos) return false;
    if (rel.find("..") != std::string::npos) return false;
    return true;
}

std::wstring normalizeRelPathWide(std::wstring p) {
    for (wchar_t& c : p) {
        if (c == L'/') c = L'\\';
    }
    while (!p.empty() && (p.front() == L'\\')) p.erase(p.begin());
    return p;
}

std::wstring basenameWide(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return p;
    return p.substr(pos + 1);
}

std::wstring installDirFromExe(const wchar_t* exePath) {
    std::wstring s(exePath);
    size_t pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L".";
    return s.substr(0, pos);
}

struct ManifestEntry {
    std::string relUtf8;
    std::string url;
};

bool parseSlimyManifest(const std::string& text, std::vector<ManifestEntry>& out,
                        std::string& errOut) {
    out.clear();
    bool sawMagic = false;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        line = trimAscii(line);
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line.find("slimy-manifest") != std::string::npos) sawMagic = true;
            continue;
        }
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string cmd = line.substr(0, sp);
        if (cmd != "file") continue;
        const std::string rest = trimAscii(line.substr(sp + 1));
        const size_t sp2 = rest.find(' ');
        if (sp2 == std::string::npos) {
            errOut = "manifest: ligne file incomplet (chemin url)";
            return false;
        }
        std::string rel = trimAscii(rest.substr(0, sp2));
        std::string url = trimAscii(rest.substr(sp2 + 1));
        if (!safeRelativeUtf8(rel)) {
            errOut = "manifest: chemin refuse (" + rel + ")";
            return false;
        }
        if (url.empty()) {
            errOut = "manifest: URL vide";
            return false;
        }
        out.push_back({std::move(rel), std::move(url)});
    }
    if (!sawMagic) {
        errOut = "manifest: ligne # slimy-manifest manquante";
        return false;
    }
    if (out.empty()) {
        errOut = "manifest: aucune ligne file";
        return false;
    }
    return true;
}

bool readFileAllUtf8(const std::wstring& path, std::string& out, size_t maxBytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const auto sz = (size_t)f.tellg();
    if (sz > maxBytes) return false;
    f.seekg(0);
    out.assign(sz, '\0');
    if (sz > 0) f.read(out.data(), (std::streamsize)sz);
    return true;
}

bool fileStartsWithMz(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    char buf[2] = {};
    f.read(buf, 2);
    return f.gcount() == 2 && buf[0] == 'M' && buf[1] == 'Z';
}

std::wstring shortPathOrSame(const wchar_t* longPath) {
    wchar_t shortBuf[MAX_PATH];
    DWORD r = GetShortPathNameW(longPath, shortBuf, MAX_PATH);
    if (r != 0 && r < MAX_PATH) return std::wstring(shortBuf);
    return std::wstring(longPath);
}

std::string escapeCmdBatchArg(const std::wstring& w) {
    std::string u;
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "\"\"";
    u.resize((size_t)n - 1);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, u.data(), n, nullptr, nullptr);
    std::string out = "\"";
    for (char c : u) {
        if (c == '\"') out += "\"\"";
        else out += c;
    }
    out += '\"';
    return out;
}

bool applyLegacySingleExe(const std::wstring& tempExe, const wchar_t* exePath,
                          const wchar_t* tempDir) {
    std::wstring batPath = std::wstring(tempDir) + L"slimyjourney_apply.cmd";

    std::wstring newShort = shortPathOrSame(tempExe.c_str());
    std::wstring oldShort = shortPathOrSame(exePath);

    std::string newEsc = escapeCmdBatchArg(newShort);
    std::string oldEsc = escapeCmdBatchArg(oldShort);

    std::string bat = "@echo off\r\n";
    bat += "ping 127.0.0.1 -n 3 >nul\r\n";
    bat += "copy /y ";
    bat += newEsc;
    bat += " ";
    bat += oldEsc;
    bat += " >nul\r\n";
    bat += "start \"\" ";
    bat += oldEsc;
    bat += "\r\n";
    bat += "del \"%~f0\"\r\n";

    {
        std::ofstream out(batPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "[update] ecriture script cmd impossible\n");
            return false;
        }
        out.write(bat.data(), (std::streamsize)bat.size());
    }

    HINSTANCE sh = ShellExecuteW(nullptr, L"open", batPath.c_str(), nullptr, nullptr, SW_HIDE);
    if ((intptr_t)sh <= 32) {
        std::fprintf(stderr, "[update] ShellExecute script (%p)\n", (void*)sh);
        DeleteFileW(batPath.c_str());
        return false;
    }

    std::fflush(stdout);
    std::fflush(stderr);
    ExitProcess(0);
    return true;
}

} // namespace

void downloadAndRestartFromUrl(const std::string& urlUtf8) {
    wchar_t exePath[MAX_PATH];
    const DWORD nExe = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (nExe == 0 || nExe >= MAX_PATH) {
        std::fprintf(stderr, "[update] GetModuleFileNameW failed\n");
        return;
    }

    wchar_t tempDir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempDir) == 0) {
        std::fprintf(stderr, "[update] GetTempPathW failed\n");
        return;
    }

    const wchar_t* exeBase = wcsrchr(exePath, L'\\');
    if (exeBase) ++exeBase;
    else exeBase = exePath;

    /// Première étape : télécharger vers un fichier temporaire pour inspecter (manifest vs exe).
    std::wstring probePath = std::wstring(tempDir) + L"slimyjourney_update_probe.bin";
    DeleteFileW(probePath.c_str());

    if (!httpDownloadToFile(urlUtf8, probePath)) return;

    LARGE_INTEGER fsz{};
    HANDLE hp = CreateFileW(probePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hp == INVALID_HANDLE_VALUE) {
        DeleteFileW(probePath.c_str());
        return;
    }
    GetFileSizeEx(hp, &fsz);
    CloseHandle(hp);

    /// Gros fichier unique → exe direct (compat ancienne URL).
    if ((uint64_t)fsz.QuadPart > (uint64_t)kMaxManifestBytes) {
        if (!fileStartsWithMz(probePath)) {
            std::fprintf(stderr, "[update] fichier trop gros et pas un exe PE\n");
            DeleteFileW(probePath.c_str());
            return;
        }
        std::wstring destExe = std::wstring(tempDir) + L"slimyjourney_update.exe";
        DeleteFileW(destExe.c_str());
        if (!MoveFileExW(probePath.c_str(), destExe.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            std::fprintf(stderr, "[update] renommage probe→exe (%lu)\n",
                         (unsigned long)GetLastError());
            DeleteFileW(probePath.c_str());
            return;
        }
        applyLegacySingleExe(destExe, exePath, tempDir);
        return;
    }

    std::string manifestText;
    if (!readFileAllUtf8(probePath, manifestText, kMaxManifestBytes)) {
        std::fprintf(stderr, "[update] lecture probe impossible\n");
        DeleteFileW(probePath.c_str());
        return;
    }

    std::vector<ManifestEntry> entries;
    std::string parseErr;
    const bool isManifest = parseSlimyManifest(manifestText, entries, parseErr);

    if (!isManifest) {
        if (fileStartsWithMz(probePath)) {
            std::wstring destExe = std::wstring(tempDir) + L"slimyjourney_update.exe";
            DeleteFileW(destExe.c_str());
            if (!MoveFileExW(probePath.c_str(), destExe.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
                std::fprintf(stderr, "[update] renommage probe exe (%lu)\n",
                             (unsigned long)GetLastError());
                DeleteFileW(probePath.c_str());
                return;
            }
            applyLegacySingleExe(destExe, exePath, tempDir);
            return;
        }
        std::fprintf(stderr, "[update] %s\n", parseErr.empty() ? "fichier inconnu" : parseErr.c_str());
        DeleteFileW(probePath.c_str());
        return;
    }

    DeleteFileW(probePath.c_str());

    const std::wstring installDir = installDirFromExe(exePath);

    std::wstring exeUpdateTemp;
    bool needExeRestart = false;

    for (size_t i = 0; i < entries.size(); ++i) {
        const ManifestEntry& e = entries[i];
        std::wstring relW = normalizeRelPathWide(utf8ToWide(e.relUtf8));
        const std::wstring destFull = installDir + L"\\" + relW;

        std::wstring tmpFile =
            std::wstring(tempDir) + L"slimyj_part_" + std::to_wstring((unsigned long)i) + L".bin";

        if (!httpDownloadToFile(e.url, tmpFile)) {
            std::fprintf(stderr, "[update] echec telechargement %s\n", e.relUtf8.c_str());
            return;
        }

        const bool targetsExe = (_wcsicmp(basenameWide(relW).c_str(), exeBase) == 0);

        if (targetsExe) {
            exeUpdateTemp = tmpFile;
            needExeRestart = true;
            continue;
        }

        if (!ensureDirsForFilePath(destFull)) {
            std::fprintf(stderr, "[update] chemins invalides pour %s\n", e.relUtf8.c_str());
            DeleteFileW(tmpFile.c_str());
            return;
        }

        if (!CopyFileW(tmpFile.c_str(), destFull.c_str(), FALSE)) {
            std::fprintf(stderr, "[update] copie vers %s (%lu)\n", e.relUtf8.c_str(),
                         (unsigned long)GetLastError());
            DeleteFileW(tmpFile.c_str());
            return;
        }
        DeleteFileW(tmpFile.c_str());
        std::printf("[update] installe %s\n", e.relUtf8.c_str());
    }

    if (!needExeRestart || exeUpdateTemp.empty()) {
        std::printf("[update] OK — fichiers installes (relancer le jeu si besoin)\n");
        return;
    }

    std::wstring batPath = std::wstring(tempDir) + L"slimyjourney_apply.cmd";

    std::wstring newShort = shortPathOrSame(exeUpdateTemp.c_str());
    std::wstring oldShort = shortPathOrSame(exePath);

    std::string newEsc = escapeCmdBatchArg(newShort);
    std::string oldEsc = escapeCmdBatchArg(oldShort);

    std::string bat = "@echo off\r\n";
    bat += "ping 127.0.0.1 -n 3 >nul\r\n";
    bat += "copy /y ";
    bat += newEsc;
    bat += " ";
    bat += oldEsc;
    bat += " >nul\r\n";
    bat += "start \"\" ";
    bat += oldEsc;
    bat += "\r\n";
    bat += "del \"%~f0\"\r\n";

    {
        std::ofstream out(batPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "[update] ecriture script cmd impossible\n");
            return;
        }
        out.write(bat.data(), (std::streamsize)bat.size());
    }

    HINSTANCE sh = ShellExecuteW(nullptr, L"open", batPath.c_str(), nullptr, nullptr, SW_HIDE);
    if ((intptr_t)sh <= 32) {
        std::fprintf(stderr, "[update] ShellExecute script (%p)\n", (void*)sh);
        DeleteFileW(batPath.c_str());
        return;
    }

    std::fflush(stdout);
    std::fflush(stderr);
    ExitProcess(0);
}

namespace {
AutomaticUpdateResult tryAutomaticUpdateImpl(const std::string& hubManifestCheckUrl) {
    if (hubManifestCheckUrl.empty()) return AutomaticUpdateResult::NotConfigured;
    std::string body;
    if (!httpGetUtf8(hubManifestCheckUrl, body, 65536)) return AutomaticUpdateResult::FetchFailed;
    uint32_t b = 0;
    std::string dl;
    if (!parseHubReleaseText(body, b, dl)) return AutomaticUpdateResult::BadManifest;
    if (b <= pe::net::kClientBuild) return AutomaticUpdateResult::UpToDate;
    downloadAndRestartFromUrl(dl);
    return AutomaticUpdateResult::DownloadFailed;
}
} // namespace

AutomaticUpdateResult tryAutomaticUpdate(const std::string& hubManifestCheckUrl) {
    return tryAutomaticUpdateImpl(hubManifestCheckUrl);
}

} // namespace pe::platform

#else

namespace pe::platform {

AutomaticUpdateResult tryAutomaticUpdate(const std::string& hubManifestCheckUrl) {
    if (hubManifestCheckUrl.empty()) return AutomaticUpdateResult::NotConfigured;
    return AutomaticUpdateResult::UnsupportedPlatform;
}

void downloadAndRestartFromUrl(const std::string&) {
    std::fprintf(stderr, "[update] mise a jour auto — uniquement sur Windows\n");
}

} // namespace pe::platform

#endif
