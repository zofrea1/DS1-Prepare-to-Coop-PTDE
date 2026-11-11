// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "steam.h"

#include "log.h"
#include "steamguard.h"  // the FPS Unlock's SteamPauseGuard
#include "steam_abi.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace coop::steam {

void queue_accept(uint64_t peer);  // below (the network thread's queue)

namespace {

using Accessor = void*(__cdecl*)();
using RegisterCallbackFn = void(__cdecl*)(CCallbackBase*, int);
using CallResultFn = void(__cdecl*)(CCallbackBase*, SteamAPICall_t);
using VoidFn = void(__cdecl*)();
using RestartFn = bool(__cdecl*)(uint32_t);

struct Api {
    HMODULE dll = nullptr;
    Accessor matchmaking = nullptr, networking = nullptr, user = nullptr, friends = nullptr, utils = nullptr;
    RegisterCallbackFn register_callback = nullptr;
    CallResultFn register_result = nullptr;
    VoidFn run_callbacks = nullptr;
    RestartFn restart = nullptr;
} g_api;

ISteamMatchmaking009* g_mm = nullptr;
ISteamNetworking005* g_net = nullptr;
ISteamUser017* g_user = nullptr;
ISteamFriends014* g_friends = nullptr;
ISteamUtils006* g_utils = nullptr;
uint32_t g_app_id = 0;
void (*g_tick)() = nullptr;
bool g_ready = false;
bool g_in_tick = false;

// The game's own lobbies (its sessions, signs, invasions, messages) are limited to players with the same
// co-op password: every lobby the game tags gets our tag too, and every search the game makes asks for it.
std::string g_game_tag;
bool g_bypass = false;  // our own lobby calls skip the game filter
void** g_mm_vtable_copy = nullptr;
using RequestLobbyListFn = SteamAPICall_t(__fastcall*)(void*, void*);
using SetLobbyDataFn = bool(__fastcall*)(void*, void*, uint64_t, const char*, const char*);
RequestLobbyListFn g_request_orig = nullptr;
SetLobbyDataFn g_set_data_orig = nullptr;
std::set<uint64_t> g_tagged;

// ------------------------------------------------------------------ slow calls
// Every Steam call waits for the Steam client (steam.exe) to answer it. The game thread makes several a frame (the
// game's SteamAPI_RunCallbacks, our packet reads); when steam.exe is slow to answer, that frame waits: a hitch, which at
// 30 FPS fit in the frame but at 200+ FPS shows. Calls over 4 ms are logged (the first ones each, later a summary a
// minute) so a hitch in the [spike] lines can be tied to Steam.
constexpr double kSlowMs = 4.0;
constexpr int kSlowLoggedEach = 12;
SRWLOCK g_slow_lock = SRWLOCK_INIT;
LONGLONG g_qpf = 0;
struct Slow {
    int logged = 0;
    LONGLONG last = 0;           // the last slow call's end
    int window_n = 0;            // since the last summary
    double window_worst = 0.0;
    const char* window_what = "";
    LONGLONG window_start = 0;
} g_slow;

LONGLONG qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void note_call(const char* what, LONGLONG t0) {
    const LONGLONG t1 = qpc_now();
    if (!g_qpf) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpf = f.QuadPart;
    }
    const double ms = 1000.0 * static_cast<double>(t1 - t0) / static_cast<double>(g_qpf);
    if (ms < kSlowMs) return;
    steamguard_note(ms, t1);
    char line[256] = "";
    AcquireSRWLockExclusive(&g_slow_lock);
    const double since = g_slow.last ? static_cast<double>(t1 - g_slow.last) / static_cast<double>(g_qpf) : 0.0;
    g_slow.last = t1;
    if (!g_slow.window_start) g_slow.window_start = t1;
    ++g_slow.window_n;
    if (ms > g_slow.window_worst) {
        g_slow.window_worst = ms;
        g_slow.window_what = what;
    }
    if (g_slow.logged < kSlowLoggedEach) {
        ++g_slow.logged;
        std::snprintf(line, sizeof(line), "[coop] Steam answered slowly: %s took %.1f ms (%.2f s after the last slow "
                      "one; thread %lu)", what, ms, since, GetCurrentThreadId());
        g_slow.window_n = 0;
        g_slow.window_worst = 0.0;
        g_slow.window_start = t1;
    } else if (static_cast<double>(t1 - g_slow.window_start) / static_cast<double>(g_qpf) >= 60.0) {
        std::snprintf(line, sizeof(line), "[coop] Steam answered slowly %d times in %.0f s (over %.0f ms; longest "
                      "%.1f ms, %s)", g_slow.window_n, static_cast<double>(t1 - g_slow.window_start) /
                      static_cast<double>(g_qpf), kSlowMs, g_slow.window_worst, g_slow.window_what);
        g_slow.window_n = 0;
        g_slow.window_worst = 0.0;
        g_slow.window_start = t1;
    }
    ReleaseSRWLockExclusive(&g_slow_lock);
    if (line[0]) LOG_INFO("%s", line);
}

class Timed {
public:
    explicit Timed(const char* what) : what_(what), t0_(qpc_now()) {}
    ~Timed() { note_call(what_, t0_); }
    Timed(const Timed&) = delete;
    Timed& operator=(const Timed&) = delete;

private:
    const char* what_;
    LONGLONG t0_;
};

// The Steam client's regular pause (SteamPauseGuard, the FPS Unlock's steamguard.h): every Steam call waits while the
// Windows Steam client pauses. Co-op's packets go through Steam on a thread of their own (net_thread below), where that
// wait holds back packets, not the game's frame; the game thread only fills and empties two queues. The Steam id and
// app id are asked once; the lobby member list keeps its last answer while Steam is in a pause.
struct NetCmd {
    enum Kind : uint8_t { kSend, kAccept, kClose } kind;
    bool reliable;
    uint64_t peer;
    std::vector<uint8_t> data;
    LONGLONG queued = 0;  // qpc
};
struct NetIn {
    uint64_t from;
    std::vector<uint8_t> data;
    LONGLONG read = 0;  // qpc
};
// For a line a minute while packets flow: queued to sent, arrived to handled (under g_net_lock).
struct NetStats {
    int sent = 0, received = 0;
    double send_sum = 0, send_max = 0, recv_sum = 0, recv_max = 0;
} g_net_stats;
SRWLOCK g_net_lock = SRWLOCK_INIT;
std::vector<NetCmd> g_net_out;  // game thread -> network thread, in order
std::vector<NetIn> g_net_in;    // network thread -> game thread
HANDLE g_net_wake = nullptr;
HANDLE g_net_thread = nullptr;
constexpr size_t kNetQueueMax = 20000;   // commands queued for sending (a stuck Steam would otherwise grow it)
constexpr size_t kNetInHold = 4000;      // received packets waiting for the game thread: beyond this they stay in
                                         // Steam (a long load), never dropped
uint64_t g_self_id = 0;
uint32_t g_app_id_seen = 0;
std::map<uint64_t, std::vector<uint64_t>> g_members_seen;

std::function<void(uint64_t, uint64_t, bool)> g_member_fn;
std::function<void(uint64_t, int)> g_fail_fn;
std::set<uint64_t> g_accepted;
std::function<bool(uint64_t)> g_accept_check;

// ------------------------------------------------------------------ callbacks
template <typename T>
class Callback final : public CCallbackBase {
public:
    explicit Callback(std::function<void(const T&)> fn) : fn_(std::move(fn)) { m_iCallback = T::k_iCallback; }
    void Run(void* p) override { fn_(*static_cast<const T*>(p)); }
    void Run(void* p, bool, SteamAPICall_t) override { fn_(*static_cast<const T*>(p)); }
    int GetCallbackSizeBytes() override { return sizeof(T); }

private:
    std::function<void(const T&)> fn_;
};

// One-shot call result; deletes itself after delivering.
template <typename T>
class CallResult final : public CCallbackBase {
public:
    CallResult(std::function<void(const T*, bool)> fn) : fn_(std::move(fn)) { m_iCallback = T::k_iCallback; }
    void Run(void* p) override { finish(static_cast<const T*>(p), false); }
    void Run(void* p, bool io_failure, SteamAPICall_t) override { finish(static_cast<const T*>(p), io_failure); }
    int GetCallbackSizeBytes() override { return sizeof(T); }

private:
    void finish(const T* p, bool failed) {
        auto fn = std::move(fn_);
        delete this;
        if (fn) fn(failed ? nullptr : p, failed);
    }
    std::function<void(const T*, bool)> fn_;
};

template <typename T>
void await(SteamAPICall_t call, std::function<void(const T*, bool)> fn) {
    if (call == k_uAPICallInvalid || !g_api.register_result) {
        fn(nullptr, true);
        return;
    }
    auto* cr = new CallResult<T>(std::move(fn));
    g_api.register_result(cr, call);
}

Callback<LobbyChatUpdate_t>* g_cb_chat = nullptr;
Callback<P2PSessionRequest_t>* g_cb_p2p_request = nullptr;
Callback<P2PSessionConnectFail_t>* g_cb_p2p_fail = nullptr;
Callback<SteamServersConnected_t>* g_cb_connected = nullptr;
Callback<SteamServersDisconnected_t>* g_cb_disconnected = nullptr;
bool g_offline = false;  // Steam told us the connection to its servers is lost
int g_reconnects = 0;

void register_callbacks() {
    g_cb_disconnected = new Callback<SteamServersDisconnected_t>([](const SteamServersDisconnected_t& d) {
        if (!g_offline) LOG_INFO("[coop] Steam: the connection to Steam was lost (EResult %d)", d.m_eResult);
        g_offline = true;
    });
    g_cb_connected = new Callback<SteamServersConnected_t>([](const SteamServersConnected_t&) {
        if (!g_offline) return;
        g_offline = false;
        ++g_reconnects;
        LOG_INFO("[coop] Steam: connected again");
    });
    g_api.register_callback(g_cb_disconnected, SteamServersDisconnected_t::k_iCallback);
    g_api.register_callback(g_cb_connected, SteamServersConnected_t::k_iCallback);
    g_cb_chat = new Callback<LobbyChatUpdate_t>([](const LobbyChatUpdate_t& u) {
        // k_EChatMemberStateChangeEntered = 1; left/disconnected/kicked/banned = 2/4/8/16
        const bool entered = (u.m_rgfChatMemberStateChange & 1) != 0;
        if (g_member_fn) g_member_fn(u.m_ulSteamIDLobby, u.m_ulSteamIDUserChanged, entered);
    });
    g_cb_p2p_request = new Callback<P2PSessionRequest_t>([](const P2PSessionRequest_t& r) {
        const uint64_t peer = r.m_steamIDRemote.ConvertToUint64();
        if (!g_net) return;
        static int lines = 0;
        // A request can come before Steam's word that its player entered our lobby: a lobby member is let in now,
        // not when Steam asks again 3.4 s later.
        if (!g_accepted.count(peer) && g_accept_check && g_accept_check(peer)) {
            g_accepted.insert(peer);
            if (++lines <= 40) LOG_INFO("[coop] Steam: %s's session accepted (in our lobby)", persona(peer).c_str());
        }
        if (g_accepted.count(peer)) {
            queue_accept(peer);  // in order with our closes (the network thread)
        } else if (++lines <= 40) {
            LOG_INFO("[coop] Steam: a session request from %s not accepted yet (not in our lobby)", persona(peer).c_str());
        }
    });
    g_cb_p2p_fail = new Callback<P2PSessionConnectFail_t>([](const P2PSessionConnectFail_t& f) {
        if (g_fail_fn) g_fail_fn(f.m_steamIDRemote.ConvertToUint64(), f.m_eP2PSessionError);
    });
    g_api.register_callback(g_cb_chat, LobbyChatUpdate_t::k_iCallback);
    g_api.register_callback(g_cb_p2p_request, P2PSessionRequest_t::k_iCallback);
    g_api.register_callback(g_cb_p2p_fail, P2PSessionConnectFail_t::k_iCallback);
}

// ------------------------------------------------------------------ imports
bool patch_import(const char* name, void* hook, void** orig) {
    HMODULE exe = GetModuleHandleW(nullptr);
    void* target = g_api.dll ? reinterpret_cast<void*>(GetProcAddress(g_api.dll, name)) : nullptr;
    if (!target) return false;
    auto* base = reinterpret_cast<uint8_t*>(exe);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "steam_api.dll") != 0) continue;
        for (auto* slot = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk); slot->u1.Function; ++slot) {
            if (reinterpret_cast<void*>(slot->u1.Function) != target) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), PAGE_READWRITE, &old)) return false;
            *orig = target;
            slot->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), old, &old);
            return true;
        }
    }
    return false;
}

SteamAPICall_t __fastcall hk_request_lobby_list(void* self, void*) {
    steamguard_before_lobby_request(self);  // the distance filter the game set (SteamPauseGuard keeps it until now)
    if (!g_bypass && !g_game_tag.empty()) {
        static_cast<ISteamMatchmaking009*>(self)->AddRequestLobbyListStringFilter("ptdesc_game", g_game_tag.c_str(),
                                                                                  k_ELobbyComparisonEqual);
    }
    return g_request_orig(self, nullptr);
}

bool __fastcall hk_set_lobby_data(void* self, void*, uint64_t lobby, const char* key, const char* value) {
    const bool ok = g_set_data_orig(self, nullptr, lobby, key, value);
    if (!g_bypass && !g_game_tag.empty() && !g_tagged.count(lobby)) {
        g_tagged.insert(lobby);
        g_set_data_orig(self, nullptr, lobby, "ptdesc_game", g_game_tag.c_str());
    }
    return ok;
}

// Swaps the matchmaking interface's vtable for a copy with our methods (the object is Steam's own): the password tag
// on the game's lobbies and searches, and SteamPauseGuard's distance filter (the game sets it every frame).
void hook_matchmaking_instance(void* mm) {
    if (!mm || g_mm_vtable_copy) return;
    void** vt = *static_cast<void***>(mm);
    constexpr int kSlots = 38;  // SteamMatchMaking009: GetFavoriteGameCount .. SetLinkedLobby
    g_mm_vtable_copy = static_cast<void**>(VirtualAlloc(nullptr, kSlots * sizeof(void*), MEM_COMMIT | MEM_RESERVE,
                                                        PAGE_READWRITE));
    if (!g_mm_vtable_copy) return;
    std::memcpy(g_mm_vtable_copy, vt, kSlots * sizeof(void*));
    g_request_orig = reinterpret_cast<RequestLobbyListFn>(vt[4]);
    g_set_data_orig = reinterpret_cast<SetLobbyDataFn>(vt[20]);
    g_mm_vtable_copy[4] = reinterpret_cast<void*>(&hk_request_lobby_list);
    g_mm_vtable_copy[20] = reinterpret_cast<void*>(&hk_set_lobby_data);
    steamguard_take_distance_filter(g_mm_vtable_copy);
    *static_cast<void***>(mm) = g_mm_vtable_copy;
    if (!g_game_tag.empty()) LOG_INFO("[coop] The game's own lobbies are limited to players with the same password");
}

void* __cdecl hk_matchmaking() {
    void* mm = g_api.matchmaking();
    hook_matchmaking_instance(mm);
    return mm;
}

struct Bypass {
    Bypass() { g_bypass = true; }
    ~Bypass() { g_bypass = false; }
};

bool __cdecl hk_restart(uint32_t) {
    // Running as another app id: Steam must not relaunch the game as PTDE.
    return false;
}

void __cdecl hk_run_callbacks() {
    static int64_t last = 0;
    if (steamguard_frame_call(&last)) {  // otherwise Steam keeps the callbacks for a later frame
        Timed timed("the game's SteamAPI_RunCallbacks");
        g_api.run_callbacks();
    }
    if (g_in_tick) return;
    g_in_tick = true;
    if (g_tick) g_tick();
    g_in_tick = false;
}

}  // namespace

bool install_early(uint32_t app_id, void (*tick)(), const std::string& game_tag) {
    g_app_id = app_id;
    g_game_tag = game_tag;
    g_tick = tick;
    g_api.dll = GetModuleHandleA("steam_api.dll");
    if (!g_api.dll) return false;
    if (app_id) {
        wchar_t id[16];
        _snwprintf_s(id, _TRUNCATE, L"%u", app_id);
        SetEnvironmentVariableW(L"SteamAppId", id);
        SetEnvironmentVariableW(L"SteamGameId", id);
    }
    auto get = [](const char* n) { return GetProcAddress(g_api.dll, n); };
    g_api.matchmaking = reinterpret_cast<Accessor>(get("SteamMatchmaking"));
    g_api.networking = reinterpret_cast<Accessor>(get("SteamNetworking"));
    g_api.user = reinterpret_cast<Accessor>(get("SteamUser"));
    g_api.friends = reinterpret_cast<Accessor>(get("SteamFriends"));
    g_api.utils = reinterpret_cast<Accessor>(get("SteamUtils"));
    g_api.register_callback = reinterpret_cast<RegisterCallbackFn>(get("SteamAPI_RegisterCallback"));
    g_api.register_result = reinterpret_cast<CallResultFn>(get("SteamAPI_RegisterCallResult"));
    bool ok = patch_import("SteamAPI_RunCallbacks", reinterpret_cast<void*>(&hk_run_callbacks),
                           reinterpret_cast<void**>(&g_api.run_callbacks));
    {
        void* real = nullptr;
        if (patch_import("SteamMatchmaking", reinterpret_cast<void*>(&hk_matchmaking), &real)) {
            g_api.matchmaking = reinterpret_cast<Accessor>(real);
        } else {
            ok = false;
        }
    }
    if (app_id && app_id != 211420) {
        ok = patch_import("SteamAPI_RestartAppIfNecessary", reinterpret_cast<void*>(&hk_restart),
                          reinterpret_cast<void**>(&g_api.restart)) && ok;
    }
    return ok;
}

DWORD WINAPI net_thread(void*);  // below

bool ready() {
    if (g_ready) return true;
    if (!g_api.matchmaking || !g_api.networking || !g_api.user || !g_api.register_callback) return false;
    g_mm = static_cast<ISteamMatchmaking009*>(g_api.matchmaking());
    hook_matchmaking_instance(g_mm);
    g_net = static_cast<ISteamNetworking005*>(g_api.networking());
    g_user = static_cast<ISteamUser017*>(g_api.user());
    g_friends = g_api.friends ? static_cast<ISteamFriends014*>(g_api.friends()) : nullptr;
    g_utils = g_api.utils ? static_cast<ISteamUtils006*>(g_api.utils()) : nullptr;
    if (!g_mm || !g_net || !g_user) return false;
    // The game's own P2P sends (its session traffic, every frame) go through SteamPauseGuard's queue; ours are on the
    // network thread below and call Steam's methods directly.
    steamguard_wrap_networking(g_net);
    if (!g_net_wake) g_net_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_net_thread && g_net_wake) g_net_thread = CreateThread(nullptr, 0, net_thread, nullptr, 0, nullptr);
    if (!g_net_thread) {
        LOG_ERROR("[coop] Could not start the network thread");
        return false;
    }
    SetThreadPriority(g_net_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    register_callbacks();
    g_ready = true;
    LOG_INFO("[coop] Steam ready: app %u, user %016llx (%s)", app_id(), static_cast<unsigned long long>(self()),
             persona(self()).c_str());
    return true;
}

uint32_t app_id() {
    if (g_app_id_seen) return g_app_id_seen;
    Timed timed("GetAppID");
    if (!g_utils) return g_app_id;
    g_app_id_seen = g_utils->GetAppID();
    return g_app_id_seen;
}
uint32_t server_time() {
    Timed timed("GetServerRealTime");
    return g_utils ? g_utils->GetServerRealTime() : 0;
}
uint64_t self() {
    if (g_self_id) return g_self_id;
    Timed timed("GetSteamID");
    if (!g_user) return 0;
    g_self_id = g_user->GetSteamID().ConvertToUint64();
    return g_self_id;
}

std::string persona(uint64_t id) {
    Timed timed("persona name");
    if (!g_friends) return "?";
    const char* n = id == self() ? g_friends->GetPersonaName() : g_friends->GetFriendPersonaName(CSteamID(id));
    return n ? n : "?";
}

void create_lobby(int max_members, LobbyResult done) {
    Bypass bypass;
    if (!g_mm) return done(false, 0);
    await<LobbyCreated_t>(g_mm->CreateLobby(k_ELobbyTypeInvisible, max_members),
                          [done](const LobbyCreated_t* r, bool) {
                              const bool ok = r && r->m_eResult == 1 && r->m_ulSteamIDLobby;
                              if (!ok && r) LOG_ERROR("[coop] CreateLobby failed: EResult %d", r->m_eResult);
                              done(ok, ok ? r->m_ulSteamIDLobby : 0);
                          });
}

void find_lobbies(const std::vector<std::pair<std::string, std::string>>& equal, int max_results,
                  std::function<void(std::vector<uint64_t>)> done) {
    Bypass bypass;
    if (!g_mm) return done({});
    for (const auto& kv : equal) {
        g_mm->AddRequestLobbyListStringFilter(kv.first.c_str(), kv.second.c_str(), k_ELobbyComparisonEqual);
    }
    g_mm->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
    g_mm->AddRequestLobbyListResultCountFilter(max_results);
    await<LobbyMatchList_t>(g_mm->RequestLobbyList(), [done](const LobbyMatchList_t* r, bool) {
        std::vector<uint64_t> out;
        if (r) {
            for (uint32_t i = 0; i < r->m_nLobbiesMatching; ++i) {
                out.push_back(g_mm->GetLobbyByIndex(static_cast<int>(i)).ConvertToUint64());
            }
        }
        done(out);
    });
}

void join_lobby(uint64_t lobby, LobbyResult done) {
    Bypass bypass;
    if (!g_mm) return done(false, 0);
    await<LobbyEnter_t>(g_mm->JoinLobby(CSteamID(lobby)), [done, lobby](const LobbyEnter_t* r, bool) {
        // k_EChatRoomEnterResponseSuccess = 1
        const bool ok = r && r->m_EChatRoomEnterResponse == 1;
        if (!ok && r) LOG_ERROR("[coop] JoinLobby failed: response %u", r->m_EChatRoomEnterResponse);
        done(ok, lobby);
    });
}

void leave_lobby(uint64_t lobby) {
    if (g_mm && lobby) g_mm->LeaveLobby(CSteamID(lobby));
}

bool set_lobby_data(uint64_t lobby, const char* key, const char* value) {
    Timed timed("SetLobbyData");
    Bypass bypass;
    return g_mm && lobby && g_mm->SetLobbyData(CSteamID(lobby), key, value);
}

std::string lobby_data(uint64_t lobby, const char* key) {
    Timed timed("GetLobbyData");
    if (!g_mm || !lobby) return {};
    const char* v = g_mm->GetLobbyData(CSteamID(lobby), key);
    return v ? v : "";
}

void set_lobby_joinable(uint64_t lobby, bool joinable) {
    if (g_mm && lobby) g_mm->SetLobbyJoinable(CSteamID(lobby), joinable);
}

uint64_t lobby_owner(uint64_t lobby) {
    return g_mm && lobby ? g_mm->GetLobbyOwner(CSteamID(lobby)).ConvertToUint64() : 0;
}

std::vector<uint64_t> lobby_members(uint64_t lobby) {
    std::vector<uint64_t> out;
    if (!g_mm || !lobby) return out;
    if (steamguard_paused()) {
        auto it = g_members_seen.find(lobby);
        if (it != g_members_seen.end()) return it->second;
    }
    Timed timed("lobby members");
    const int n = g_mm->GetNumLobbyMembers(CSteamID(lobby));
    for (int i = 0; i < n; ++i) out.push_back(g_mm->GetLobbyMemberByIndex(CSteamID(lobby), i).ConvertToUint64());
    if (g_members_seen.size() > 64) g_members_seen.clear();
    g_members_seen[lobby] = out;
    return out;
}

bool in_lobby(uint64_t lobby) {
    const uint64_t me = self();
    for (uint64_t m : lobby_members(lobby)) {
        if (m == me) return true;
    }
    return false;
}

bool online() { return !g_offline; }
int reconnects() { return g_reconnects; }

void on_lobby_member(std::function<void(uint64_t, uint64_t, bool)> fn) { g_member_fn = std::move(fn); }
void on_p2p_failed(std::function<void(uint64_t, int)> fn) { g_fail_fn = std::move(fn); }
void set_accept_check(std::function<bool(uint64_t)> fn) { g_accept_check = std::move(fn); }

// Sends what the game thread queued (in order, at once when woken) and reads what arrived (every 4 ms, about as
// often as the game thread did at 225 FPS). A Steam pause holds this thread, not a frame.
using NetSendFn = bool(__fastcall*)(void*, void*, uint64_t, const void*, uint32_t, int, int);
using NetPeerFn = bool(__fastcall*)(void*, void*, uint64_t);
using NetChannelFn = bool(__fastcall*)(void*, void*, uint64_t, int);

// Steam's own networking methods: the ones SteamPauseGuard keeps when it puts its queue in, else the object's.
void* net_method(int slot) {
    void* m = steamguard_networking_original(slot);
    return m ? m : (*reinterpret_cast<void***>(g_net))[slot];
}

DWORD WINAPI net_thread(void*) {
    const auto send_p2p = reinterpret_cast<NetSendFn>(net_method(0));
    const auto accept_p2p = reinterpret_cast<NetPeerFn>(net_method(3));
    const auto close_channel = reinterpret_cast<NetChannelFn>(net_method(5));
    std::vector<NetCmd> cmds;
    std::vector<NetIn> got;
    std::vector<uint8_t> buf(64 * 1024);
    int failures = 0;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double ms_per_tick = 1000.0 / static_cast<double>(f.QuadPart);
    LONGLONG next_line = qpc_now() + f.QuadPart * 60;
    for (;;) {
        WaitForSingleObject(g_net_wake, 4);
        const LONGLONG now = qpc_now();
        if (now >= next_line) {
            next_line = now + f.QuadPart * 60;
            NetStats st;
            AcquireSRWLockExclusive(&g_net_lock);
            st = g_net_stats;
            g_net_stats = NetStats{};
            ReleaseSRWLockExclusive(&g_net_lock);
            if (st.sent + st.received > 0) {
                LOG_INFO("[coop] Network thread, last minute: %d packets sent (queued to sent: average %.1f ms, longest "
                         "%.1f ms), %d received (arrived to handled: average %.1f ms, longest %.1f ms)",
                         st.sent, st.sent ? st.send_sum / st.sent : 0.0, st.send_max, st.received,
                         st.received ? st.recv_sum / st.received : 0.0, st.recv_max);
            }
        }
        AcquireSRWLockExclusive(&g_net_lock);
        cmds.swap(g_net_out);
        ReleaseSRWLockExclusive(&g_net_lock);
        int sent = 0;
        double send_sum = 0, send_max = 0;
        for (const NetCmd& c : cmds) {
            const CSteamID peer(c.peer);
            if (c.kind == NetCmd::kSend) {
                if (!send_p2p(g_net, nullptr, c.peer, c.data.data(), static_cast<uint32_t>(c.data.size()),
                              c.reliable ? k_EP2PSendReliable : k_EP2PSendUnreliableNoDelay, kChannel) &&
                    ++failures <= 20) {
                    LOG_ERROR("[coop] Steam did not take a packet for %016llx (%u bytes)",
                              static_cast<unsigned long long>(c.peer), static_cast<unsigned>(c.data.size()));
                }
                const double d = static_cast<double>(qpc_now() - c.queued) * ms_per_tick;
                ++sent;
                send_sum += d;
                send_max = d > send_max ? d : send_max;
            } else if (c.kind == NetCmd::kAccept) {
                accept_p2p(g_net, nullptr, c.peer);
            } else {
                close_channel(g_net, nullptr, c.peer, kChannel);
            }
        }
        cmds.clear();
        AcquireSRWLockShared(&g_net_lock);
        const bool room = g_net_in.size() < kNetInHold;
        ReleaseSRWLockShared(&g_net_lock);
        uint32_t size = 0;
        for (int guard = 0; room && guard < 256 && g_net->IsP2PPacketAvailable(&size, kChannel); ++guard) {
            if (size > buf.size()) buf.resize(size);
            CSteamID from;
            uint32_t n = 0;
            if (!g_net->ReadP2PPacket(buf.data(), static_cast<uint32_t>(buf.size()), &n, &from, kChannel)) break;
            got.push_back({from.ConvertToUint64(), std::vector<uint8_t>(buf.begin(), buf.begin() + n), qpc_now()});
        }
        if (!got.empty() || sent) {
            AcquireSRWLockExclusive(&g_net_lock);
            for (NetIn& p : got) g_net_in.push_back(std::move(p));
            g_net_stats.sent += sent;
            g_net_stats.send_sum += send_sum;
            g_net_stats.send_max = send_max > g_net_stats.send_max ? send_max : g_net_stats.send_max;
            ReleaseSRWLockExclusive(&g_net_lock);
            got.clear();
        }
    }
}

void net_queue(NetCmd&& c) {
    c.queued = qpc_now();
    AcquireSRWLockExclusive(&g_net_lock);
    if (g_net_out.size() < kNetQueueMax) g_net_out.push_back(std::move(c));
    ReleaseSRWLockExclusive(&g_net_lock);
    if (g_net_wake) SetEvent(g_net_wake);
}

bool send(uint64_t to, const void* data, uint32_t size, bool reliable) {
    if (!g_net || !to) return false;
    g_accepted.insert(to);
    const auto* bytes = static_cast<const uint8_t*>(data);
    net_queue({NetCmd::kSend, reliable, to, std::vector<uint8_t>(bytes, bytes + size)});
    return true;
}

void poll(const std::function<void(uint64_t, const uint8_t*, uint32_t)>& fn) {
    if (!g_net) return;
    static std::vector<NetIn> got;
    AcquireSRWLockExclusive(&g_net_lock);
    got.swap(g_net_in);
    ReleaseSRWLockExclusive(&g_net_lock);
    if (got.empty()) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const LONGLONG now = qpc_now();
    double sum = 0, mx = 0;
    for (const NetIn& p : got) {
        const double d = 1000.0 * static_cast<double>(now - p.read) / static_cast<double>(f.QuadPart);
        sum += d;
        mx = d > mx ? d : mx;
    }
    for (const NetIn& p : got) fn(p.from, p.data.data(), static_cast<uint32_t>(p.data.size()));
    AcquireSRWLockExclusive(&g_net_lock);
    g_net_stats.received += static_cast<int>(got.size());
    g_net_stats.recv_sum += sum;
    g_net_stats.recv_max = mx > g_net_stats.recv_max ? mx : g_net_stats.recv_max;
    ReleaseSRWLockExclusive(&g_net_lock);
    got.clear();
}

void accept(uint64_t peer) {
    if (!peer) return;
    g_accepted.insert(peer);
    if (g_net) net_queue({NetCmd::kAccept, false, peer, {}});
}

void queue_accept(uint64_t peer) {
    if (g_net && peer) net_queue({NetCmd::kAccept, false, peer, {}});
}

void close(uint64_t peer) {
    g_accepted.erase(peer);
    if (g_net && peer) net_queue({NetCmd::kClose, false, peer, {}});
}

}  // namespace coop::steam
