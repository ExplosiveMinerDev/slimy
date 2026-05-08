#include "platform/AutoUpdate.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
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

std::wstring utf8ToWide(const std::string& u) {
    if (u.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), w.data(), n);
    return w;
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

    std::wstring updateExe = std::wstring(tempDir) + L"slimyjourney_update.exe";
    std::wstring batPath = std::wstring(tempDir) + L"slimyjourney_apply.cmd";

    if (!httpDownloadToFile(urlUtf8, updateExe)) return;

    std::wstring newShort = shortPathOrSame(updateExe.c_str());
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
            DeleteFileW(updateExe.c_str());
            return;
        }
        out.write(bat.data(), (std::streamsize)bat.size());
    }

    HINSTANCE sh = ShellExecuteW(nullptr, L"open", batPath.c_str(), nullptr, nullptr, SW_HIDE);
    if ((intptr_t)sh <= 32) {
        std::fprintf(stderr, "[update] ShellExecute script (%p)\n", (void*)sh);
        DeleteFileW(updateExe.c_str());
        DeleteFileW(batPath.c_str());
        return;
    }

    std::fflush(stdout);
    std::fflush(stderr);
    ExitProcess(0);
}

} // namespace pe::platform

#else

namespace pe::platform {

void downloadAndRestartFromUrl(const std::string&) {
    std::fprintf(stderr, "[update] mise a jour auto — uniquement sur Windows\n");
}

} // namespace pe::platform

#endif
