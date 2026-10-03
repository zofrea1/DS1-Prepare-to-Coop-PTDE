#include "rules.h"

#include "game.h"
#include "hook.h"
#include "log.h"
#include "world.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

namespace coop::rules {

namespace {

State g_state;

// ---------------------------------------------------------------- leaving the session
// FrpgSessionManagerImp's leave (0xD2B460, esi = manager) is called from many rules. In a seamless session
// the ones that end co-op by design are skipped; network failures, invasions and our own leave still run.
constexpr uint32_t kLeave = 0x00D2B460;
// Return addresses of the calls that are skipped.
constexpr uint32_t kRetLuaLeave = 0x00D62919;        // Lua LeaveSession (boss kill, phantom rules)
constexpr uint32_t kRetWarpKick = 0x00D62984;        // WarpNextStageKick away from someone's world
constexpr uint32_t kRetStepEnd = 0x00EE2A97;         // in-game step ends (any map load by the host)
constexpr uint32_t kRetIdleTimer = 0x00D2D0EC;       // session idle timer
constexpr uint32_t kRetRoomTimeout = 0x00D2EE69;     // host: nobody joined the room in time
constexpr uint32_t kRetMapChange = 0x00EFDDE9;       // host walked into another map block
constexpr uint32_t kRetAfterLoad = 0x00EFCC22;       // after a load with fewer than two members
constexpr uint32_t kRetWarpRestart = 0x00D742CA;     // event restart warp
// PartyMemberInfo reset (0xEA1CB0, stdcall) that follows three of those leaves.
constexpr uint32_t kPartyReset = 0x00EA1CB0;
constexpr uint32_t kRetLuaLeaveParty = 0x00D6291F;
constexpr uint32_t kRetWarpKickParty = 0x00D6298A;
constexpr uint32_t kRetMapChangeParty = 0x00EFDDFA;

void* g_leave_orig = nullptr;
void* g_party_orig = nullptr;
int g_blocked_leaves = 0;

bool seamless_rules() { return g_state.active && !g_state.leaving && !g_state.invader; }

bool __stdcall allow_leave(uint32_t ret) {
    if (!seamless_rules()) return true;
    switch (ret) {
        case kRetLuaLeave:
        case kRetWarpKick:
        case kRetStepEnd:
        case kRetIdleTimer:
        case kRetRoomTimeout:
        case kRetMapChange:
        case kRetAfterLoad:
        case kRetWarpRestart:
            if (++g_blocked_leaves <= 50) LOG_INFO("[coop] Kept the session (leave from %08X skipped)", ret);
            return false;
        default:
            LOG_INFO("[coop] Session leave from %08X allowed", ret);
            return true;
    }
}

bool __stdcall allow_party_reset(uint32_t ret) {
    if (!seamless_rules()) return true;
    return ret != kRetLuaLeaveParty && ret != kRetWarpKickParty && ret != kRetMapChangeParty;
}

__declspec(naked) void hk_leave() {
    __asm {
        pushad
        push dword ptr [esp + 32]
        call allow_leave
        test al, al
        popad
        jz skip
        jmp g_leave_orig
    skip:
        ret
    }
}

__declspec(naked) void hk_party_reset() {
    __asm {
        pushad
        push dword ptr [esp + 32]
        call allow_party_reset
        test al, al
        popad
        jz skip
        jmp g_party_orig
    skip:
        ret 4
    }
}

// ---------------------------------------------------------------- reloads stay in the host's world
// MoveMapStep's constructor (0xEFC470, fastcall + 2 stack args) decides own world vs someone else's by
// SessMan+0x194 == 3, which the game only sets for the first summon reload. For a co-op guest's later loads
// (death, following the host) the step is set to 3 just for the constructor's decision and put back to 4
// afterwards: while it reads 3 the loader would also take the summon spawn point (0xEEFCE1) instead of the
// requested warp (the bonfire), and the arrival code would run again.
constexpr uint32_t kMoveMapCtor = 0x00EFC470;
void* g_movemap_orig = nullptr;
bool g_marked = false;

void __cdecl before_map_load() {
    g_marked = false;
    if (seamless_rules() && g_state.guest && game::session_state() == 6 && game::join_step() == 4) {
        game::set_join_step(3);
        g_marked = true;
        LOG_INFO("[coop] Map load stays in the host's world");
    }
}

void __cdecl after_map_load() {
    if (g_marked && game::join_step() == 3) game::set_join_step(4);
    g_marked = false;
}

__declspec(naked) void hk_movemap() {
    __asm {
        pushad
        call before_map_load
        popad
        push dword ptr [esp + 8]
        push dword ptr [esp + 8]
        call g_movemap_orig
        pushad
        call after_map_load
        popad
        ret 8
    }
}

// ---------------------------------------------------------------- the guest keeps their own body
// On arrival in the host's world (0xD2DED0) the game turns the summoned player into a phantom of the join type
// (0xEA15B0: chr type 1 white, 2 black ...). A co-op guest keeps their own type (human or hollow).
constexpr uint32_t kArrivalSite = 0x00D2DF0F;
constexpr uint32_t kArrivalType = 0x00EA15B0;  // stdcall(uint32_t* out)
constexpr uint32_t kSetChrType = 0x00455FC0;   // esi = PlayerGameData, stdcall(type)

void set_chr_type(uint32_t pgd, int chr_type) {
    __asm {
        push esi
        mov esi, pgd
        push chr_type
        mov eax, kSetChrType
        call eax
        pop esi
    }
}

void __stdcall hk_arrival(uint32_t* out) {
    if (g_state.active && g_state.guest && !g_state.invader && g_state.own_chr_type >= 0) {
        const uint32_t pgd = game::player_game_data();
        if (pgd) {
            set_chr_type(pgd, g_state.own_chr_type);
            if (out) *out = *ptr<uint32_t>(pgd + 0x8C);
        }
        const uint32_t gdm = game::game_data_man();
        if (gdm) *ptr<int>(gdm + 0x60) = -1;  // no pending "next" type that would override it
        LOG_INFO("[coop] Arrived in the host's world as chr type %d", g_state.own_chr_type);
        return;
    }
    using Fn = void(__stdcall*)(uint32_t*);
    reinterpret_cast<Fn>(kArrivalType)(out);
}

// ---------------------------------------------------------------- the host's death does not end the session
// The host's death script broadcasts LuaCallStart(4000, 1), which runs HostDead on every guest (send home).
constexpr uint32_t kLuaCallStart = 0x00D66290;  // thiscall(proxy, id, param) -> bool, ret 8
using LuaCallStartFn = bool(__fastcall*)(void*, void*, int, int);
LuaCallStartFn g_lua_call_start = nullptr;

bool __fastcall hk_lua_call_start(void* self, void*, int id, int param) {
    if (seamless_rules() && g_state.host && id == 4000 && param == 1) {
        LOG_INFO("[coop] Host death: guests stay (event 4000/1 not sent)");
        return true;
    }
    return g_lua_call_start(self, nullptr, id, param);
}

// LockSession (boss fights lock the room) is a no-op in seamless co-op, so players can join any time.
constexpr uint32_t kLuaLockSession = 0x00D5EE60;
using VoidFn = void(__cdecl*)();
VoidFn g_lock_orig = nullptr;

void __cdecl hk_lock_session() {
    if (seamless_rules()) return;
    g_lock_orig();
}

// ---------------------------------------------------------------- progress carries home
// The event flag setter (0xD38CB0: eax = EventFlagMan, cl, push value, push id; ret 8). While a co-op guest is
// in the host's world (active store = the other-world store at +0x90), every flag it sets is written to the
// guest's own world store (+0x14) too.
constexpr uint32_t kSetFlag = 0x00D38CB0;
void* g_set_flag_orig = nullptr;

void call_set_flag(uint32_t efm, int id, uint32_t value) {
    void* fn = g_set_flag_orig;
    __asm {
        push value
        push id
        xor ecx, ecx
        mov eax, efm
        call fn
    }
}

// Shared progress: flags changed in the host's world travel to the other players (guest -> host -> guests).
void (*g_flag_listener)(int, bool) = nullptr;
bool g_applying_remote = false;
DWORD g_main_thread = 0;
int g_old_value = -1;

// Lua multiplayer state (4000-4999) is per player and never shared.
bool shared_flag(int id) { return id >= 0 && !(id >= 4000 && id <= 4999); }

void __stdcall before_set_flag(int id) {
    g_old_value = -1;
    if (GetCurrentThreadId() != g_main_thread || !g_flag_listener || g_applying_remote) return;
    if (!seamless_rules() || !shared_flag(id)) return;
    g_old_value = game::flag(id) ? 1 : 0;
}

void __stdcall after_set_flag(uint32_t efm, int id, uint32_t value) {
    if (!seamless_rules() || !efm) return;
    const bool on = (value & 0xFF) != 0;
    if (g_old_value >= 0 && g_old_value != (on ? 1 : 0) && GetCurrentThreadId() == g_main_thread) {
        g_old_value = -1;
        g_flag_listener(id, on);
    }
    if (!g_state.guest) return;
    uint32_t active = 0;
    if (!read32(efm + 0x10C, &active) || active != efm + 0x90) return;
    *ptr<uint32_t>(efm + 0x10C) = efm + 0x14;
    call_set_flag(efm, id, value & 0xFF);
    *ptr<uint32_t>(efm + 0x10C) = active;
}

__declspec(naked) void hk_set_flag() {
    __asm {
        push ebp
        mov ebp, esp
        pushad
        push dword ptr [ebp + 8]
        call before_set_flag
        push dword ptr [ebp + 12]
        push dword ptr [ebp + 8]
        mov eax, [ebp - 4]
        mov ecx, [ebp - 8]
        call g_set_flag_orig
        push dword ptr [ebp + 12]
        push dword ptr [ebp + 8]
        push dword ptr [ebp - 4]
        call after_set_flag
        popad
        pop ebp
        ret 8
    }
}

// ---------------------------------------------------------------- guests loot like a player
// Enemy drops (0xDC80A0), object drops (0xDC8370) and scripted reward items (GetRateItem, 0xDC9170) skip a
// summoned player: inlined "SessMan state == 6" and "PartyMemberInfo+0x7C != -1" checks. A co-op guest loots
// like the host (each player gets their own drops); invaders keep the vanilla rule.
uint8_t g_strict = 1;  // 0 while a co-op guest's loot rules are relaxed

__declspec(naked) void h_state6_ecx() {
    __asm {
        cmp dword ptr [ecx + 0x90], 6
        jne done
        cmp byte ptr [g_strict], 1
    done:
        ret
    }
}

__declspec(naked) void h_state6_edx() {
    __asm {
        cmp dword ptr [edx + 0x90], 6
        jne done
        cmp byte ptr [g_strict], 1
    done:
        ret
    }
}

// al = (PartyMemberInfo+0x7C != -1) && strict; eax = PartyMemberInfo on entry
__declspec(naked) void h_party_summoned() {
    __asm {
        cmp dword ptr [eax + 0x7C], -1
        setne al
        and al, byte ptr [g_strict]
        ret
    }
}

bool patch_call(uint32_t at, const uint8_t* expect, size_t n, void* helper) {
    uint8_t code[16];
    std::memset(code, 0x90, sizeof(code));
    code[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(helper) - (at + 5));
    std::memcpy(code + 1, &rel, 4);
    return n >= 5 && n <= sizeof(code) && patch(at, expect, code, n);
}

int install_loot() {
    static const uint8_t kCmpEcx[7] = {0x83, 0xB9, 0x90, 0x00, 0x00, 0x00, 0x06};  // cmp dword [ecx+90h],6
    static const uint8_t kCmpEdx[7] = {0x83, 0xBA, 0x90, 0x00, 0x00, 0x00, 0x06};  // cmp dword [edx+90h],6
    static const uint8_t kParty[7] = {0x83, 0x78, 0x7C, 0xFF, 0x0F, 0x95, 0xC0};   // cmp [eax+7Ch],-1 ; setne al
    static const uint8_t kPartyEsi[6] = {0x39, 0x70, 0x7C, 0x0F, 0x95, 0xC0};      // cmp [eax+7Ch],esi ; setne al
    int ok = 0;
    ok += patch_call(0x00DC8101, kCmpEcx, 7, reinterpret_cast<void*>(&h_state6_ecx));
    ok += patch_call(0x00DC810B, kParty, 7, reinterpret_cast<void*>(&h_party_summoned));
    ok += patch_call(0x00DC83D1, kCmpEcx, 7, reinterpret_cast<void*>(&h_state6_ecx));
    ok += patch_call(0x00DC83DB, kParty, 7, reinterpret_cast<void*>(&h_party_summoned));
    ok += patch_call(0x00DC91BA, kCmpEdx, 7, reinterpret_cast<void*>(&h_state6_edx));
    ok += patch_call(0x00DC91D3, kPartyEsi, 6, reinterpret_cast<void*>(&h_party_summoned));
    return 6 - ok;
}

// ---------------------------------------------------------------- bonfires and NPCs in a session
// HasPartyMembers (0xD2A4B0) disables bonfires (bonfire object +0x35) and makes talk scripts (ESD
// function 0x34) refuse service in multiplayer. Seamless co-op lifts it except while an invader is in.
constexpr uint32_t kHasParty = 0x00D2A4B0;

bool __cdecl hk_has_party() {
    if (seamless_rules()) return g_state.invader || g_state.invader_present;
    using Fn = bool(__cdecl*)();
    return reinterpret_cast<Fn>(kHasParty)();
}

// ---------------------------------------------------------------- no multiplayer walls
// OnEvent_4034 (a guest enters a session) raises the white walls that keep phantoms in the host's area,
// counted by GetMultiWallNum (0xD5DC40). A co-op guest gets none.
constexpr uint32_t kMultiWallNum = 0x00D5DC40;
using IntFn = int(__cdecl*)();
IntFn g_multiwall_orig = nullptr;

int __cdecl hk_multiwall_num() {
    if (seamless_rules() && g_state.guest) return 0;
    return g_multiwall_orig();
}

// ---------------------------------------------------------------- bonfire travel and lighting
// WarpNextStage_Bonfire (0xD62CA0, stdcall(bonfire), ret 4) is the bonfire menu's warp. In a seamless session
// only the host travels by bonfire; the guests follow to the same bonfire, and a guest's own bonfire warp does
// nothing (as in Remastered Seamless Co-op).
constexpr uint32_t kBonfireWarp = 0x00D62CA0;
// Util_RequestLevelUpFirst (0xD5D850, stdcall(bonfire), ret 4): a bonfire lit for the first time
// (OnEvent_BonfireFirstLvUp). Lighting one lights it for every player in the session.
constexpr uint32_t kBonfireLit = 0x00D5D850;
using IntStdFn = void(__stdcall*)(int);
IntStdFn g_bonfire_warp_orig = nullptr;
IntStdFn g_bonfire_lit_orig = nullptr;
void (*g_bonfire_warp_fn)(int) = nullptr;
void (*g_bonfire_lit_fn)(int) = nullptr;
bool g_remote_call = false;

void __stdcall hk_bonfire_warp(int bonfire) {
    if (!g_remote_call && seamless_rules()) {
        if (!g_state.host) {
            LOG_INFO("[coop] Bonfire warp to %d: only the host travels by bonfire", bonfire);
            return;
        }
        LOG_INFO("[coop] Host warps to bonfire %d; guests follow", bonfire);
        if (g_bonfire_warp_fn) g_bonfire_warp_fn(bonfire);
    }
    g_bonfire_warp_orig(bonfire);
}

void __stdcall hk_bonfire_lit(int bonfire) {
    if (!g_remote_call && g_state.active && !g_state.invader && g_bonfire_lit_fn) {
        LOG_INFO("[coop] Bonfire %d lit for everyone", bonfire);
        g_bonfire_lit_fn(bonfire);
    }
    g_bonfire_lit_orig(bonfire);
}

// ---------------------------------------------------------------- teams (Ominous Tome)
// ChrIns team type (0xE68C80, eax = character, returns the team: chr+0x74, or 0xF while a "turn hostile"
// state 0x84 is on). Remastered Seamless Co-op answers it for players: an invader is on team 0x10 (hostile to
// everyone), and the host's world rules put co-op allies on team 0xF (all hostile) or 0x12 (teams).
constexpr uint32_t kTeamType = 0x00E68C80;
void* g_team_orig = nullptr;

bool is_remote_player(uint32_t chr) {
    uint32_t wcm = 0, n = 0, arr = 0;
    if (!read32(game::kWorldChrManVar, &wcm) || !wcm || !read32(wcm + 0x24, &n) || !read32(wcm + 0x28, &arr) || !arr) {
        return false;
    }
    for (uint32_t i = 0; i < n && i < 8; ++i) {
        uint32_t p = 0;
        if (read32(arr + i * 0x20, &p) && p == chr) return true;
    }
    return false;
}

int __stdcall team_override(uint32_t chr) {
    if (!g_state.active || g_state.leaving || !chr) return -1;
    if (chr == game::player()) return g_state.invader ? 0x10 : -1;
    if (!is_remote_player(chr)) return -1;
    uint32_t type = 0;
    read32(chr + 0x70, &type);
    const bool their_invader = type == 2 || type == 12;  // black phantom, intruder
    if (g_state.invader) return their_invader ? 0x10 : -1;
    if (their_invader) return 0x10;
    if (g_state.world_rules == 1) return 0x0F;
    if (g_state.world_rules == 2) return 0x12;
    return -1;
}

__declspec(naked) void hk_team() {
    __asm {
        push ecx
        push edx
        push eax
        push eax
        call team_override
        cmp eax, -1
        je vanilla
        add esp, 4
        pop edx
        pop ecx
        ret
    vanilla:
        pop eax
        pop edx
        pop ecx
        jmp g_team_orig
    }
}

// ---------------------------------------------------------------- world tendency (Cursed Pendant)
// EcTestTendency (event condition, vtable 0x11DE5F0 slot 1 = 0xBB1810, thiscall(cond, 2 args)): condition
// +0x18 tendency (0 white, 1 black), +0x19 comparison, result in bit 0 of +8. Remastered Seamless Co-op
// replaces world tendency: white tests are never true; black "above" (2) is true and black "at most" (5) is
// false exactly while the host's world is cursed by the Cursed Pendant; other black tests keep their result.
constexpr uint32_t kTendencyVtable = 0x011DE5F0;
constexpr uint32_t kTendencyTest = 0x00BB1810;

void __fastcall hk_tendency(uint8_t* cond, void*, int, int) {
    const uint8_t type = cond[0x18];
    const uint8_t cmp = cond[0x19];
    if (type == 0) {
        cond[8] &= 0xFE;
        return;
    }
    if (cmp == 2) {
        cond[8] = (cond[8] & 0xFE) | (g_state.infection ? 1 : 0);
    } else if (cmp == 5) {
        cond[8] = (cond[8] & 0xFE) | (g_state.infection ? 0 : 1);
    }
}

int install_one(const char* what, bool ok) {
    if (!ok) LOG_ERROR("[coop] Hook not installed: %s (code does not match this build)", what);
    return ok ? 0 : 1;
}

}  // namespace

State& state() { return g_state; }

void set_flag_listener(void (*fn)(int id, bool on)) { g_flag_listener = fn; }

void set_bonfire_warp_listener(void (*fn)(int)) { g_bonfire_warp_fn = fn; }
void set_bonfire_lit_listener(void (*fn)(int)) { g_bonfire_lit_fn = fn; }

void remote_bonfire_warp(int bonfire) {
    if (!g_bonfire_warp_orig || bonfire < 0) return;
    g_remote_call = true;
    world::bypass_limbo(true);
    g_bonfire_warp_orig(bonfire);
    world::bypass_limbo(false);
    g_remote_call = false;
}

void remote_bonfire_lit(int bonfire) {
    if (!g_bonfire_lit_orig || bonfire < 0) return;
    g_remote_call = true;
    g_bonfire_lit_orig(bonfire);
    g_remote_call = false;
}

void apply_remote_flag(int id, bool on) {
    if (!shared_flag(id)) return;
    g_applying_remote = true;
    game::set_flag(id, on);
    g_applying_remote = false;
}

int install() {
    int failed = 0;
    {
        static const uint8_t k[7] = {0x83, 0xBE, 0x90, 0x00, 0x00, 0x00, 0x00};  // cmp dword [esi+90h],0
        g_leave_orig = detour(kLeave, k, sizeof(k), reinterpret_cast<void*>(&hk_leave));
        failed += install_one("session leave guard", g_leave_orig != nullptr);
    }
    {
        static const uint8_t k[5] = {0x83, 0xEC, 0x10, 0x53, 0x55};  // sub esp,10h ; push ebx ; push ebp
        g_party_orig = detour(kPartyReset, k, sizeof(k), reinterpret_cast<void*>(&hk_party_reset));
        failed += install_one("party reset guard", g_party_orig != nullptr);
    }
    {
        static const uint8_t k[7] = {0x6A, 0xFF, 0x68, 0xE5, 0x34, 0x07, 0x01};  // push -1 ; push 010734E5h
        g_movemap_orig = detour(kMoveMapCtor, k, sizeof(k), reinterpret_cast<void*>(&hk_movemap));
        failed += install_one("map load world choice", g_movemap_orig != nullptr);
    }
    failed += install_one("arrival chr type", redirect_call(kArrivalSite, kArrivalType, reinterpret_cast<void*>(&hk_arrival)));
    {
        static const uint8_t k[8] = {0x8B, 0x44, 0x24, 0x08, 0x8B, 0x54, 0x24, 0x04};
        g_lua_call_start = reinterpret_cast<LuaCallStartFn>(
            detour(kLuaCallStart, k, sizeof(k), reinterpret_cast<void*>(&hk_lua_call_start)));
        failed += install_one("LuaCallStart filter", g_lua_call_start != nullptr);
    }
    {
        static const uint8_t k[7] = {0x57, 0x8B, 0x3D, 0x2C, 0xD8, 0x37, 0x01};  // push edi ; mov edi,[SessMan]
        g_lock_orig = reinterpret_cast<VoidFn>(detour(kLuaLockSession, k, sizeof(k), reinterpret_cast<void*>(&hk_lock_session)));
        failed += install_one("LockSession", g_lock_orig != nullptr);
    }
    {
        static const uint8_t k[8] = {0x80, 0xB8, 0x14, 0x01, 0x00, 0x00, 0x00, 0x56};  // cmp byte [eax+114h],0 ; push esi
        g_set_flag_orig = detour(kSetFlag, k, sizeof(k), reinterpret_cast<void*>(&hk_set_flag));
        failed += install_one("event flag dual write", g_set_flag_orig != nullptr);
    }
    {
        const int bad = install_loot();
        if (bad) LOG_ERROR("[coop] %d of 6 loot rule patches did not match this build", bad);
        failed += install_one("loot rules", bad == 0);
    }
    failed += install_one("bonfire party check", redirect_call(0x00D58AE4, kHasParty, reinterpret_cast<void*>(&hk_has_party)));
    failed += install_one("talk party check", redirect_call(0x00D4ABFB, kHasParty, reinterpret_cast<void*>(&hk_has_party)));
    {
        // mov ecx,[0137D790h] ; jmp 00D69B10h
        static const uint8_t k[11] = {0x8B, 0x0D, 0x90, 0xD7, 0x37, 0x01, 0xE9, 0xC5, 0xBE, 0x00, 0x00};
        g_multiwall_orig = reinterpret_cast<IntFn>(detour(kMultiWallNum, k, sizeof(k), reinterpret_cast<void*>(&hk_multiwall_num), 6));
        failed += install_one("multiplayer walls", g_multiwall_orig != nullptr);
    }
    {
        // push esi ; push edi ; mov edi,[esp+0Ch]
        static const uint8_t k[6] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C};
        g_bonfire_warp_orig = reinterpret_cast<IntStdFn>(detour(kBonfireWarp, k, sizeof(k), reinterpret_cast<void*>(&hk_bonfire_warp)));
        failed += install_one("bonfire warp", g_bonfire_warp_orig != nullptr);
    }
    {
        // mov eax,[esp+4] ; mov [012DE2A0h],eax
        static const uint8_t k[9] = {0x8B, 0x44, 0x24, 0x04, 0xA3, 0xA0, 0xE2, 0x2D, 0x01};
        g_bonfire_lit_orig = reinterpret_cast<IntStdFn>(detour(kBonfireLit, k, sizeof(k), reinterpret_cast<void*>(&hk_bonfire_lit)));
        failed += install_one("bonfire lighting", g_bonfire_lit_orig != nullptr);
    }
    {
        // push ebx ; mov ebx,[eax+74h] ; push edi
        static const uint8_t k[5] = {0x53, 0x8B, 0x58, 0x74, 0x57};
        g_team_orig = detour(kTeamType, k, sizeof(k), reinterpret_cast<void*>(&hk_team));
        failed += install_one("team types", g_team_orig != nullptr);
    }
    failed += install_one("world tendency", swap_pointer(kTendencyVtable + 4, kTendencyTest, reinterpret_cast<void*>(&hk_tendency)));
    return failed;
}

void tick() {
    g_main_thread = GetCurrentThreadId();
    g_strict = (seamless_rules() && g_state.guest) ? 0 : 1;
    if (!seamless_rules()) return;
    if (g_state.host && game::session_state() == 3) game::keep_room_open();
}

}  // namespace coop::rules
