// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "updates.h"

#include "game.h"
#include "log.h"
#include "overlay.h"
#include "text.h"
#include "version.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::updates {

namespace {

// GitHub's REST API: the newest release that is not a draft or a pre-release. Its tag is "v" and the version.
constexpr wchar_t kHost[] = L"api.github.com";
constexpr wchar_t kPath[] = L"/repos/zofrea1/DS1-Prepare-to-Coop-PTDE/releases/latest";
constexpr wchar_t kReleasesPage[] = L"github.com/zofrea1/DS1-Prepare-to-Coop-PTDE/releases";
constexpr wchar_t kNexusPage[] = L"nexusmods.com/darksouls/mods/1980";
constexpr DWORD kTimeoutMs = 5000;
constexpr size_t kMaxBody = 512 * 1024;

enum State : int { kPending, kCurrent, kNewer, kFailed };
std::atomic<int> g_state{kPending};
std::string g_latest;  // the release's version (no "v"); written by the thread before g_state
std::string g_why;     // why the check failed (for the log)
std::string g_ours = PREPARE_TO_COOP_VERSION;  // what it is compared with (set before the thread starts)

// "1.10.2" -> {1, 10, 2}; anything after the numbers ("-beta") is ignored.
std::vector<int> parse_version(const std::string& v) {
    std::vector<int> out;
    size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
        int n = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') n = n * 10 + (v[i++] - '0');
        out.push_back(n);
        if (i < v.size() && v[i] == '.') {
            ++i;
        } else {
            break;
        }
    }
    return out;
}

bool newer(const std::vector<int>& a, const std::vector<int>& b) {
    for (size_t i = 0; i < a.size() || i < b.size(); ++i) {
        const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

// The string value of "key" in a flat JSON object (the first one: the release's own fields come before its assets').
std::string json_string(const std::string& body, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = body.find(k);
    if (p == std::string::npos) return "";
    p = body.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p = body.find('"', p + 1);
    if (p == std::string::npos) return "";
    const size_t e = body.find('"', p + 1);
    return e == std::string::npos ? "" : body.substr(p + 1, e - p - 1);
}

bool fetch(std::string* body, std::string* why) {
    wchar_t agent[64];
    _snwprintf_s(agent, _TRUNCATE, L"Prepare-to-Coop/%hs", PREPARE_TO_COOP_VERSION);
    HINTERNET session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        *why = "WinHttpOpen " + std::to_string(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", kPath, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    if (!request) {
        *why = "no connection to " + std::string("api.github.com") + " (" + std::to_string(GetLastError()) + ")";
    } else if (!WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1),
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
               !WinHttpReceiveResponse(request, nullptr)) {
        *why = "no answer (" + std::to_string(GetLastError()) + ")";
    } else {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            *why = "HTTP " + std::to_string(status);
        } else {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(request, &avail) && avail > 0 && body->size() < kMaxBody) {
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, &chunk[0], avail, &read) || read == 0) break;
                body->append(chunk.data(), read);
            }
            ok = !body->empty();
            if (!ok) *why = "an empty answer";
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

DWORD WINAPI check_thread(void*) {
    std::string body, why;
    if (!fetch(&body, &why)) {
        g_why = why;
        g_state.store(kFailed);
        return 0;
    }
    const std::string tag = json_string(body, "tag_name");
    const std::vector<int> latest = parse_version(tag);
    if (latest.empty()) {
        g_why = "no version in the answer";
        g_state.store(kFailed);
        return 0;
    }
    g_latest = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
    g_state.store(newer(latest, parse_version(g_ours)) ? kNewer : kCurrent);
    return 0;
}

bool g_started = false;
bool g_reported = false;
bool g_shown = false;  // the notice is on screen
bool g_done = false;   // closed for good (a character was loaded while it showed), or nothing to show

}  // namespace

void start(const char* version) {
    if (g_started) return;
    g_started = true;
    if (version && version[0]) g_ours = version;
    HANDLE t = CreateThread(nullptr, 0, &check_thread, nullptr, 0, nullptr);
    if (t) {
        CloseHandle(t);
    } else {
        g_done = true;
        LOG_ERROR("[coop] Update check: could not start (%lu)", GetLastError());
    }
}

void tick() {
    if (!g_started || g_done) return;
    const int state = g_state.load();
    if (state == kPending) return;
    if (!g_reported) {
        g_reported = true;
        if (state == kFailed) {
            LOG_INFO("[coop] Update check: no answer from GitHub (%s)", g_why.c_str());
        } else {
            LOG_INFO("[coop] Update check: the latest release is %s, this is %s%s", g_latest.c_str(), g_ours.c_str(),
                     state == kNewer ? ": a newer one is out" : "");
        }
    }
    if (state != kNewer) {
        g_done = true;
        return;
    }
    // On the title screen (no character loaded) until the player is in the world; a check that answers in the
    // world waits for the next visit to the title screen.
    const bool title = !game::in_game();
    if (title && !g_shown) {
        const std::wstring latest = text::widen(g_latest), current = text::widen(g_ours);
        overlay::set_notice({L"Prepare to Co-op Edition " + latest + L" is available",
                             L"You have " + current + L". Download the new version from GitHub or NexusMods:",
                             kReleasesPage, kNexusPage});
        g_shown = true;
    } else if (!title && g_shown) {
        overlay::set_notice({});
        g_shown = false;
        g_done = true;
    }
}

}  // namespace coop::updates
