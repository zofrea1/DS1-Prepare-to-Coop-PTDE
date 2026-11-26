// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
// Dark Souls' Steam achievements while the game runs as Spacewar (app 480).
//
// Prepare to Co-op runs the game as Spacewar (steam.cpp), so Steam answers the game's achievement calls with
// Spacewar's achievements and none of Dark Souls' unlock. The game unlocks by name (ISteamUserStats011, its trophy
// manager FrpgTrophyMan_forWIN32, names ACH_00..ACH_40 in the table 0x12DFFD0): SetAchievement (slot 7) and
// StoreStats (slot 10); after each unlock it asks GetAchievement (slot 6) for ACH_01..ACH_40 and unlocks ACH_00 (all
// achievements) when none is missing; a failed GetAchievement counts as unlocked (0xF7FDC3), so under Spacewar every
// unlock would also claim ACH_00. ClearAchievement (slot 8) only comes from a debug reset (0xF7FBC0).
//
// A helper process (this DLL under rundll32, PtcAchievements below: DllMain leaves a host that is not the game alone)
// runs as Dark Souls (SteamAppId 211420), as Steam Achievement Manager does. The game's stats object gets a copy of
// its vtable where SetAchievement and StoreStats are also sent to the helper (as lines on its standard input) and
// GetAchievement answers ACH_xx from Dark Souls' own state (the helper's lines on its standard output). ACH_00 is never
// taken from the game: the helper unlocks it itself once Steam says ACH_01..ACH_40 are all unlocked. Clears never
// leave the game.
#include "achievements.h"

#include "hook.h"
#include "log.h"
#include "steam.h"
#include "steam_abi.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#pragma comment(linker, "/EXPORT:PtcAchievements=_PtcAchievements@16")

namespace coop::achievements {
namespace {

constexpr uint32_t kDarkSouls = 211420;
constexpr int kCount = 41;  // ACH_00 (every other one) .. ACH_40
// ISteamUserStats011 slots (PTDE's steam_api).
constexpr int kRequestCurrentStats = 0;
constexpr int kGetAchievement = 6;
constexpr int kSetAchievement = 7;
constexpr int kStoreStats = 10;
constexpr int kSlots = 48;  // the interface has 43; the copy only needs to cover every slot the game calls

// The achievement's number for a name ACH_00..ACH_40, else -1.
int index_of(const char* name) {
    if (!name || std::strncmp(name, "ACH_", 4) != 0 || name[4] < '0' || name[4] > '9' || name[5] < '0' ||
        name[5] > '9' || name[6] != '\0') {
        return -1;
    }
    const int i = (name[4] - '0') * 10 + (name[5] - '0');
    return i < kCount ? i : -1;
}

int count_bits(uint64_t m) {
    int n = 0;
    for (; m; m &= m - 1) ++n;
    return n;
}

// ---------------------------------------------------------------- the game's side
bool g_started = false;
HANDLE g_to_helper = nullptr;    // the helper's standard input (ours to write)
HANDLE g_from_helper = nullptr;  // the helper's standard output (ours to read)
HANDLE g_helper = nullptr;
std::mutex g_write_lock;
bool g_write_failed = false;
std::atomic<bool> g_ready{false};
std::atomic<uint64_t> g_mask{0};  // Dark Souls' unlocked achievements, bit = number

void* g_real_vt[kSlots] = {};
void* g_vt[kSlots] = {};

void send(const char* line) {
    std::lock_guard<std::mutex> lock(g_write_lock);
    if (!g_to_helper || g_write_failed) return;
    DWORD n = 0;
    if (!WriteFile(g_to_helper, line, static_cast<DWORD>(std::strlen(line)), &n, nullptr)) {
        g_write_failed = true;
        LOG_INFO("[coop] Achievements: the helper is gone (%lu); the game's unlocks stay with Spacewar", GetLastError());
    }
}

using GetFn = bool(__thiscall*)(void*, const char*, bool*);
using SetFn = bool(__thiscall*)(void*, const char*);
using StoreFn = bool(__thiscall*)(void*);

bool __fastcall hk_get(void* self, void*, const char* name, bool* achieved) {
    const int i = index_of(name);
    if (i >= 0 && g_ready.load()) {
        if (achieved) *achieved = ((g_mask.load() >> i) & 1) != 0;
        return true;
    }
    return reinterpret_cast<GetFn>(g_real_vt[kGetAchievement])(self, name, achieved);
}

bool __fastcall hk_set(void* self, void*, const char* name) {
    const int i = index_of(name);
    if (i >= 1) {
        const bool had = ((g_mask.load() >> i) & 1) != 0;
        LOG_INFO("[coop] Achievements: the game unlocks %s%s", name,
                 had ? " (Dark Souls has it already)" : g_ready.load() ? ": passed to Steam's Dark Souls"
                                                                        : ": passed on (the helper is not ready yet)");
        char line[32];
        std::snprintf(line, sizeof(line), "set %d\n", i);
        send(line);
    } else if (i == 0) {
        LOG_INFO("[coop] Achievements: the game claims %s (all achievements); Steam's own state decides it", name);
    }
    reinterpret_cast<SetFn>(g_real_vt[kSetAchievement])(self, name);
    return true;
}

bool __fastcall hk_store(void* self, void*) {
    send("store\n");
    return reinterpret_cast<StoreFn>(g_real_vt[kStoreStats])(self);
}

// The game's stats object (steam_api's SteamUserStats(), the one the trophy manager keeps at +0x5C) gets our vtable.
bool hook_stats() {
    HMODULE api = GetModuleHandleA("steam_api.dll");
    auto get = api ? reinterpret_cast<void* (*)()>(GetProcAddress(api, "SteamUserStats")) : nullptr;
    void* stats = get ? get() : nullptr;
    if (!stats) return false;
    __try {
        void** vt = *reinterpret_cast<void***>(stats);
        if (vt == g_vt) return true;
        for (int i = 0; i < kSlots; ++i) g_real_vt[i] = g_vt[i] = vt[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    g_vt[kGetAchievement] = reinterpret_cast<void*>(&hk_get);
    g_vt[kSetAchievement] = reinterpret_cast<void*>(&hk_set);
    g_vt[kStoreStats] = reinterpret_cast<void*>(&hk_store);
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(stats), reinterpret_cast<void*>(g_vt));
    return true;
}

DWORD WINAPI read_helper(void*) {
    std::string buf;
    char chunk[512];
    DWORD n = 0;
    while (ReadFile(g_from_helper, chunk, sizeof(chunk), &n, nullptr) && n) {
        buf.append(chunk, n);
        size_t eol;
        while ((eol = buf.find('\n')) != std::string::npos) {
            const std::string line = buf.substr(0, eol);
            buf.erase(0, eol + 1);
            if (line.compare(0, 6, "ready ") == 0 || line.compare(0, 6, "state ") == 0) {
                const uint64_t m = std::strtoull(line.c_str() + 6, nullptr, 16);
                const bool first = !g_ready.exchange(true);
                const uint64_t before = g_mask.exchange(m);
                if (first) {
                    LOG_INFO("[coop] Achievements: Steam's Dark Souls is ready (%d of %d unlocked); the game's unlocks "
                             "go there (SteamAchievements)", count_bits(m), kCount);
                } else if (m != before) {
                    LOG_INFO("[coop] Achievements: Dark Souls now has %d of %d unlocked", count_bits(m), kCount);
                }
            } else if (line.compare(0, 4, "log ") == 0) {
                LOG_INFO("[coop] Achievements: %s", line.c_str() + 4);
            } else if (line.compare(0, 5, "fail ") == 0) {
                LOG_INFO("[coop] Achievements: the helper could not run as Dark Souls (%s); the game's unlocks stay "
                         "with Spacewar", line.c_str() + 5);
            }
        }
    }
    LOG_INFO("[coop] Achievements: the helper ended");
    g_ready = false;
    return 0;
}

// The environment with SteamAppId and SteamGameId set to Dark Souls (ours says Spacewar).
std::wstring helper_environment() {
    std::wstring env;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            if (_wcsnicmp(p, L"SteamAppId=", 11) == 0 || _wcsnicmp(p, L"SteamGameId=", 12) == 0) continue;
            env.append(p);
            env.push_back(L'\0');
        }
        FreeEnvironmentStringsW(block);
    }
    env.append(L"SteamAppId=211420");
    env.push_back(L'\0');
    env.append(L"SteamGameId=211420");
    env.push_back(L'\0');
    env.push_back(L'\0');
    return env;
}

bool start_helper() {
    wchar_t dll[MAX_PATH] = {}, dir[MAX_PATH] = {};
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&start_helper), &self) ||
        !GetModuleFileNameW(self, dll, MAX_PATH)) {
        return false;
    }
    if (!GetSystemWow64DirectoryW(dir, MAX_PATH) && !GetSystemDirectoryW(dir, MAX_PATH)) return false;
    const std::wstring exe = std::wstring(dir) + L"\\rundll32.exe";
    std::wstring cmd = L"\"" + exe + L"\" \"" + dll + L"\",PtcAchievements " + std::to_wstring(GetCurrentProcessId());

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &sa, 1 << 16)) return false;
    if (!CreatePipe(&out_read, &out_write, &sa, 1 << 16)) {
        CloseHandle(in_read);
        CloseHandle(in_write);
        return false;
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    // Only the helper's two pipe ends are passed to it.
    HANDLE inherit[2] = {in_read, out_write};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::string attr_buf(size, '\0');
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(&attr_buf[0]);
    const bool have_list = InitializeProcThreadAttributeList(attrs, 1, 0, &size) &&
                           UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                     sizeof(inherit), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_read;
    si.StartupInfo.hStdOutput = out_write;
    si.StartupInfo.hStdError = out_write;
    if (have_list) si.lpAttributeList = attrs;
    std::wstring env = helper_environment();
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                       (have_list ? EXTENDED_STARTUPINFO_PRESENT : 0),
                                   &env[0], nullptr, &si.StartupInfo, &pi);
    if (have_list) DeleteProcThreadAttributeList(attrs);
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (!ok) {
        LOG_INFO("[coop] Achievements: the helper did not start (%lu)", GetLastError());
        CloseHandle(in_write);
        CloseHandle(out_read);
        return false;
    }
    CloseHandle(pi.hThread);
    g_helper = pi.hProcess;
    g_to_helper = in_write;
    g_from_helper = out_read;
    if (HANDLE t = CreateThread(nullptr, 0, read_helper, nullptr, 0, nullptr)) CloseHandle(t);
    LOG_INFO("[coop] Achievements: helper %lu started as Dark Souls (app %u) for the game's achievements",
             pi.dwProcessId, kDarkSouls);
    return true;
}

// ---------------------------------------------------------------- world events witnessed in a co-op session
// The map scripts award these with EMEVD 2003[28] (handler 0xBC21BF -> 0xF7F790) to the player whose game runs the
// event: the one who rang a bell (the lever is theirs), and for a boss whoever's game saw it die (a co-op guest's
// events run as the host's: rules.cpp hk_multiplay_test). A player elsewhere in the session got neither, though the
// event itself goes home with them (world::goes_home_witnessed). The user's rule (2026-11-26): everyone in a co-op
// session when it happens gets the achievement, wherever they stand. The flag is the award event's own: it comes on as
// the event ends, and rules.cpp share_event_end sends it to the others. Invaders keep the vanilla rule (session.cpp).
// Not here: Reach Anor Londo (33, 11500210), as every player takes the carry themselves; Reach Lordran (28) and the
// endings (1, 2), each player's own.
struct Witnessed {
    int flag;
    int achievement;
    const char* what;
};
constexpr Witnessed kWitnessed[] = {
    {11010700, 29, "the Undead Parish's bell rung"},  // m10_01
    {11400200, 30, "Quelaag's Domain's bell rung"},   // m14_00
    {11310001, 35, "Gravelord Nito defeated"},        // m13_01
    {11410001, 36, "the Bed of Chaos defeated"},      // m14_01
    {11600001, 37, "the Four Kings defeated"},        // m16_00
    {11700001, 38, "Seath the Scaleless defeated"},   // m17_00
    {11510900, 39, "Dark Sun Gwyndolin defeated"},    // m15_01
    {11100000, 40, "Crossbreed Priscilla defeated"},  // m11_00
};
constexpr uint32_t kTrophyHolder = 0x0137CE14;  // +4: the trophy manager, as the 2003[28] handler reads it
constexpr uint32_t kAward = 0x00F7F790;  // fastcall(ecx = number; stack: trophy manager), ret 4: queues the unlock
using AwardFn = void(__fastcall*)(int, int, uint32_t);

const Witnessed* find_witnessed(int flag) {
    for (const Witnessed& w : kWitnessed) {
        if (w.flag == flag) return &w;
    }
    return nullptr;
}

bool call_award(int achievement, uint32_t man) {
    __try {
        reinterpret_cast<AwardFn>(kAward)(achievement, 0, man);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool witnessed_award(int flag) { return find_witnessed(flag) != nullptr; }

void witnessed(int flag, const char* from) {
    const Witnessed* w = find_witnessed(flag);
    if (!w) return;
    // push -1 ; push 0x10528E4 (this build's award function)
    static const uint8_t kStart[7] = {0x6A, 0xFF, 0x68, 0xE4, 0x28, 0x05, 0x01};
    uint32_t holder = 0, man = 0;
    if (std::memcmp(reinterpret_cast<const void*>(kAward), kStart, sizeof(kStart)) != 0 ||
        !read32(kTrophyHolder, &holder) || !holder || !read32(holder + 4, &man) || !man) {
        LOG_INFO("[coop] Achievements: %s in our session (word from %s), but the game's achievement award was not "
                 "found; not given", w->what, from);
        return;
    }
    const bool ok = call_award(w->achievement, man);
    LOG_INFO("[coop] Achievements: %s in our session (word from %s): ACH_%02d for us too%s", w->what, from,
             w->achievement, ok ? "" : " (the game's award call failed)");
}

void test(const char* line, char* reply, size_t size) {
    char what[8] = "";
    int i = -1;
    sscanf_s(line, "%*s %7s %d", what, static_cast<unsigned>(sizeof(what)), &i);
    if (std::strcmp(what, "witness") == 0) {
        if (!witnessed_award(i)) {
            std::snprintf(reply, size, "ach witness %d: not an award event's flag", i);
            return;
        }
        witnessed(i, "a test");
        std::snprintf(reply, size, "ach witness %d: done (see the log)", i);
        return;
    }
    HMODULE api = GetModuleHandleA("steam_api.dll");
    auto get_stats = api ? reinterpret_cast<void* (*)()>(GetProcAddress(api, "SteamUserStats")) : nullptr;
    void* stats = get_stats ? get_stats() : nullptr;
    if (!stats || i < 0 || i >= kCount) {
        std::snprintf(reply, size, "ach: usage ach get|set 0-40 (stats %p)", stats);
        return;
    }
    char name[8];
    std::snprintf(name, sizeof(name), "ACH_%02d", i);
    void** vt = *reinterpret_cast<void***>(stats);
    if (std::strcmp(what, "get") == 0) {
        bool on = false;
        const bool ok = reinterpret_cast<GetFn>(vt[kGetAchievement])(stats, name, &on);
        std::snprintf(reply, size, "ach get %s: %s %d (hooked %d, ready %d)", name, ok ? "ok" : "failed", on ? 1 : 0,
                      vt == g_vt ? 1 : 0, g_ready.load() ? 1 : 0);
    } else if (std::strcmp(what, "set") == 0) {
        if (!g_ready.load() || !((g_mask.load() >> i) & 1)) {
            std::snprintf(reply, size, "ach set %s: refused (only one Dark Souls already has)", name);
            return;
        }
        reinterpret_cast<SetFn>(vt[kSetAchievement])(stats, name);
        reinterpret_cast<StoreFn>(vt[kStoreStats])(stats);
        std::snprintf(reply, size, "ach set %s: sent (hooked %d)", name, vt == g_vt ? 1 : 0);
    }
}

void tick(bool enabled) {
    if (g_started || !enabled || !steam::ready()) return;
    g_started = true;
    if (steam::app_id() == kDarkSouls || steam::app_id() == 0) return;  // the game's own achievements work as they are
    if (!hook_stats()) {
        LOG_INFO("[coop] Achievements: the game's Steam stats object was not found; its unlocks stay with Spacewar");
        return;
    }
    start_helper();
}

}  // namespace coop::achievements

// ---------------------------------------------------------------- the helper (rundll32)
namespace {

HANDLE g_out = nullptr;

void say(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof(line)) - 2) n = static_cast<int>(sizeof(line)) - 2;
    line[n++] = '\n';
    DWORD w = 0;
    WriteFile(g_out, line, static_cast<DWORD>(n), &w, nullptr);
}

std::mutex g_queue_lock;
std::deque<std::string> g_queue;
std::atomic<bool> g_input_closed{false};

DWORD WINAPI read_commands(void* in) {
    std::string buf;
    char chunk[256];
    DWORD n = 0;
    while (ReadFile(static_cast<HANDLE>(in), chunk, sizeof(chunk), &n, nullptr) && n) {
        buf.append(chunk, n);
        size_t eol;
        while ((eol = buf.find('\n')) != std::string::npos) {
            std::lock_guard<std::mutex> lock(g_queue_lock);
            g_queue.push_back(buf.substr(0, eol));
            buf.erase(0, eol + 1);
        }
    }
    g_input_closed = true;
    return 0;
}

}  // namespace

extern "C" void CALLBACK PtcAchievements(HWND, HINSTANCE, LPSTR cmd, int) {
    using namespace coop::achievements;
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (!g_out || !in) return;
    const DWORD parent_pid = cmd ? std::strtoul(cmd, nullptr, 10) : 0;
    HANDLE parent = parent_pid ? OpenProcess(SYNCHRONIZE, FALSE, parent_pid) : nullptr;

    SetEnvironmentVariableW(L"SteamAppId", L"211420");
    SetEnvironmentVariableW(L"SteamGameId", L"211420");
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&PtcAchievements), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"steam_api.dll");
    HMODULE api = LoadLibraryW(path);
    if (!api) {
        say("fail steam_api.dll did not load (%lu)", GetLastError());
        return;
    }
    using InitFn = bool(__cdecl*)();
    using VoidFn = void(__cdecl*)();
    using GetIfaceFn = void*(__cdecl*)();
    auto init = reinterpret_cast<InitFn>(GetProcAddress(api, "SteamAPI_Init"));
    auto run_callbacks = reinterpret_cast<VoidFn>(GetProcAddress(api, "SteamAPI_RunCallbacks"));
    auto shutdown = reinterpret_cast<VoidFn>(GetProcAddress(api, "SteamAPI_Shutdown"));
    auto user_stats = reinterpret_cast<GetIfaceFn>(GetProcAddress(api, "SteamUserStats"));
    auto utils_fn = reinterpret_cast<GetIfaceFn>(GetProcAddress(api, "SteamUtils"));
    if (!init || !run_callbacks || !user_stats || !utils_fn) {
        say("fail steam_api.dll lacks the calls needed");
        return;
    }
    if (!init()) {
        say("fail SteamAPI_Init failed (Steam not running, or Dark Souls not owned by this account)");
        return;
    }
    void* stats = user_stats();
    auto* utils = static_cast<ISteamUtils006*>(utils_fn());
    const uint32_t app = utils ? utils->GetAppID() : 0;
    if (!stats || app != kDarkSouls) {
        say("fail Steam answered as app %u, not Dark Souls (%u)", app, kDarkSouls);
        if (shutdown) shutdown();
        return;
    }
    void** vt = *reinterpret_cast<void***>(stats);
    auto request = reinterpret_cast<bool(__thiscall*)(void*)>(vt[kRequestCurrentStats]);
    auto get = reinterpret_cast<GetFn>(vt[kGetAchievement]);
    auto set = reinterpret_cast<SetFn>(vt[kSetAchievement]);
    auto store = reinterpret_cast<StoreFn>(vt[kStoreStats]);
    request(stats);
    bool b = false;
    bool have = false;
    for (int i = 0; i < 300 && !have; ++i) {
        run_callbacks();
        have = get(stats, "ACH_01", &b);
        if (!have) Sleep(100);
    }
    if (!have) {
        say("fail Steam did not send Dark Souls' achievements in 30 s");
        if (shutdown) shutdown();
        return;
    }
    auto mask = [&]() {
        uint64_t m = 0;
        char name[8];
        for (int i = 0; i < kCount; ++i) {
            std::snprintf(name, sizeof(name), "ACH_%02d", i);
            bool on = false;
            if (get(stats, name, &on) && on) m |= 1ull << i;
        }
        return m;
    };
    say("ready %llx", static_cast<unsigned long long>(mask()));
    if (HANDLE t = CreateThread(nullptr, 0, read_commands, in, 0, nullptr)) CloseHandle(t);

    bool dirty = false;
    auto store_now = [&]() {
        // ACH_00 (all achievements) is Steam's own state's to decide: every other one unlocked.
        const uint64_t m = mask();
        const uint64_t others = ((1ull << kCount) - 1) & ~1ull;
        if ((m & others) == others && !(m & 1)) {
            set(stats, "ACH_00");
            say("log every achievement is unlocked: ACH_00 too");
        }
        store(stats);
        dirty = false;
        say("state %llx", static_cast<unsigned long long>(mask()));
    };
    for (;;) {
        run_callbacks();
        std::deque<std::string> work;
        {
            std::lock_guard<std::mutex> lock(g_queue_lock);
            work.swap(g_queue);
        }
        for (const std::string& line : work) {
            if (line.compare(0, 4, "set ") == 0) {
                const int i = std::atoi(line.c_str() + 4);
                if (i < 1 || i >= kCount) continue;
                char name[8];
                std::snprintf(name, sizeof(name), "ACH_%02d", i);
                bool on = false;
                if (get(stats, name, &on) && on) continue;
                const bool ok = set(stats, name);
                say("log %s unlocked on Steam's Dark Souls%s", name, ok ? "" : " (Steam refused it)");
                dirty = dirty || ok;
            } else if (line == "store" && dirty) {
                store_now();
            }
        }
        if (g_input_closed || (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0)) break;
        Sleep(100);
    }
    if (dirty) store_now();
    for (int i = 0; i < 10; ++i) {
        run_callbacks();
        Sleep(100);
    }
    if (shutdown) shutdown();
}
