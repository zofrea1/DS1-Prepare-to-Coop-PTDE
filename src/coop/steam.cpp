// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "steam.h"

#include "log.h"
#include "steam_abi.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <set>

namespace coop::steam {

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

std::function<void(uint64_t, uint64_t, bool)> g_member_fn;
std::function<void(uint64_t, int)> g_fail_fn;
std::set<uint64_t> g_accepted;

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

void register_callbacks() {
    g_cb_chat = new Callback<LobbyChatUpdate_t>([](const LobbyChatUpdate_t& u) {
        // k_EChatMemberStateChangeEntered = 1; left/disconnected/kicked/banned = 2/4/8/16
        const bool entered = (u.m_rgfChatMemberStateChange & 1) != 0;
        if (g_member_fn) g_member_fn(u.m_ulSteamIDLobby, u.m_ulSteamIDUserChanged, entered);
    });
    g_cb_p2p_request = new Callback<P2PSessionRequest_t>([](const P2PSessionRequest_t& r) {
        const uint64_t peer = r.m_steamIDRemote.ConvertToUint64();
        if (g_accepted.count(peer) && g_net) {
            g_net->AcceptP2PSessionWithUser(CSteamID(peer));
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

// Swaps the matchmaking interface's vtable for a copy with our two methods (the object is Steam's own).
void hook_matchmaking_instance(void* mm) {
    if (!mm || g_mm_vtable_copy || g_game_tag.empty()) return;
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
    *static_cast<void***>(mm) = g_mm_vtable_copy;
    LOG_INFO("[coop] The game's own lobbies are limited to players with the same password");
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
    g_api.run_callbacks();
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
    if (!game_tag.empty()) {
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
    register_callbacks();
    g_ready = true;
    LOG_INFO("[coop] Steam ready: app %u, user %016llx (%s)", app_id(), static_cast<unsigned long long>(self()),
             persona(self()).c_str());
    return true;
}

uint32_t app_id() { return g_utils ? g_utils->GetAppID() : g_app_id; }
uint64_t self() { return g_user ? g_user->GetSteamID().ConvertToUint64() : 0; }

std::string persona(uint64_t id) {
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
    Bypass bypass;
    return g_mm && lobby && g_mm->SetLobbyData(CSteamID(lobby), key, value);
}

std::string lobby_data(uint64_t lobby, const char* key) {
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
    const int n = g_mm->GetNumLobbyMembers(CSteamID(lobby));
    for (int i = 0; i < n; ++i) out.push_back(g_mm->GetLobbyMemberByIndex(CSteamID(lobby), i).ConvertToUint64());
    return out;
}

void on_lobby_member(std::function<void(uint64_t, uint64_t, bool)> fn) { g_member_fn = std::move(fn); }
void on_p2p_failed(std::function<void(uint64_t, int)> fn) { g_fail_fn = std::move(fn); }

bool send(uint64_t to, const void* data, uint32_t size, bool reliable) {
    if (!g_net || !to) return false;
    g_accepted.insert(to);
    return g_net->SendP2PPacket(CSteamID(to), data, size, reliable ? k_EP2PSendReliable : k_EP2PSendUnreliableNoDelay,
                                kChannel);
}

void poll(const std::function<void(uint64_t, const uint8_t*, uint32_t)>& fn) {
    if (!g_net) return;
    static std::vector<uint8_t> buf(64 * 1024);
    uint32_t size = 0;
    int guard = 0;
    while (g_net->IsP2PPacketAvailable(&size, kChannel) && guard++ < 256) {
        if (size > buf.size()) buf.resize(size);
        CSteamID from;
        uint32_t got = 0;
        if (!g_net->ReadP2PPacket(buf.data(), static_cast<uint32_t>(buf.size()), &got, &from, kChannel)) break;
        fn(from.ConvertToUint64(), buf.data(), got);
    }
}

void accept(uint64_t peer) {
    if (!peer) return;
    g_accepted.insert(peer);
    if (g_net) g_net->AcceptP2PSessionWithUser(CSteamID(peer));
}

void close(uint64_t peer) {
    g_accepted.erase(peer);
    if (g_net && peer) g_net->CloseP2PChannelWithUser(CSteamID(peer), kChannel);
}

}  // namespace coop::steam
