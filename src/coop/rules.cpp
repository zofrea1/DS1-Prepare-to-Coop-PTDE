#include "rules.h"

#include "game.h"
#include "hook.h"
#include "log.h"
#include "world.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <map>
#include <vector>

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

__declspec(naked) void h_state6_eax() {
    __asm {
        cmp dword ptr [eax + 0x90], 6
        jne done
        cmp byte ptr [g_strict], 1
    done:
        ret
    }
}

__declspec(naked) void h_state6_edi() {
    __asm {
        cmp dword ptr [edi + 0x90], 6
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

// ---------------------------------------------------------------- the guest's own items
// The item manager (dropped items, treasure: handles with the type in the low 4 bits and an id above, looked up by
// 0xDC4AE0) works one way for the session host or a solo player and another for a summoned client (SessMan state
// 6): a client gives new entries no id (every handle id 0), asks the host to pick an item up (packet 0x2A) instead
// of taking it, skips the event-script cleanup when an item goes away and takes the host's item lists. Remastered
// Seamless Co-op answers its "is client" checks with "is invader" for these, so a co-op guest's items are its own,
// as in single player. The drops above already followed the host's rules; with the rest still on the client's,
// the guest's own drops all shared handle id 0 and the area unload walked a broken list (crash at 0xE94A90). Every
// client check of the item manager follows g_strict now, and the host's item packets are dropped for a guest.
struct State6Site {
    uint32_t at;
    uint8_t modrm;  // 0xB8 eax, 0xB9 ecx, 0xBA edx, 0xBF edi
};
const State6Site kItemClientChecks[] = {
    {0x00DC4BCD, 0xB9},  // 0xDC4BB0 item owner
    {0x00DC5E88, 0xB8},  // 0xDC5E80 pickup: event flag of a treasure, item lot
    {0x00DC64B2, 0xB9},  // 0xDC6490 item enable flag
    {0x00DC68C9, 0xB9},  // 0xDC67E0 new entry: id
    {0x00DC6BA9, 0xB9},  // 0xDC6AC0 new entry: id
    {0x00DC7720, 0xBA},  // 0xDC76F0 removal: event-script cleanup
    {0x00DC778E, 0xB8},  // 0xDC76F0 removal: event flag of the item
    {0x00DC78A9, 0xB9},  // 0xDC78A0 removal (host tells its clients)
    {0x00DC7928, 0xBF},  // 0xDC7920 client asks the host for its list
    {0x00DC7AE7, 0xB9},  // 0xDC79E0 new entry: id
    {0x00DC7C37, 0xB9},  // 0xDC7B30 new entry: id
    {0x00DC7D46, 0xB9},  // 0xDC7D20 pickup
    {0x00DC7E0B, 0xB8},  // 0xDC7E00 pickup
    {0x00DC7E76, 0xB8},  // pickup
    {0x00DC7E9E, 0xB8},  // 0xDC7E90 pickup: client asks the host
    {0x00DC7F7E, 0xB8},  // 0xDC7F70 pickup check
    {0x00DC8E35, 0xB8},  // item lot
    {0x00DC8E86, 0xB8},  // 0xDC8E50 item lot
    {0x00DC8EBB, 0xBF},  // 0xDC8EB0 item lot: client asks the host
    {0x00DC9041, 0xB9},  // 0xDC8F30 item packets: pickups
    {0x00E94AB8, 0xB9},  // 0xE94950 area unload: event-script cleanup
    {0x00E94E00, 0xB8},  // 0xE94DC0 area load: treasure owner
};

// The item packet reader (0xDC8F30) pops each member's packets of type 0x2A with 0xD2A9B0 (eax = buffer size,
// ecx = buffer, stdcall(session, member, type) -> bytes). A co-op guest reads and drops them.
constexpr uint32_t kPopPacket = 0x00D2A9B0;
constexpr uint32_t kItemPopSites[] = {0x00DC8F87, 0x00DC90CB};

int call_pop(uint32_t buf_size, void* buf, uint32_t sess, uint32_t member, uint32_t packet_type) {
    int n = 0;
    __asm {
        push packet_type
        push member
        push sess
        mov eax, buf_size
        mov ecx, buf
        mov edx, kPopPacket
        call edx
        mov n, eax
    }
    return n;
}

int __stdcall item_pop(uint32_t size, void* buf, uint32_t sess, uint32_t member, uint32_t type) {
    int n = call_pop(size, buf, sess, member, type);
    if (g_strict) return n;
    static int dropped = 0;
    for (int guard = 0; n > 0 && guard < 256; ++guard) {
        if (++dropped <= 20) LOG_INFO("[coop] Host item packet dropped (the guest's items are its own)");
        n = call_pop(size, buf, sess, member, type);
    }
    return 0;
}

__declspec(naked) void hk_item_pop() {
    __asm {
        push dword ptr [esp + 12]
        push dword ptr [esp + 12]
        push dword ptr [esp + 12]
        push ecx
        push eax
        call item_pop
        ret 12
    }
}

int install_items() {
    int bad = 0;
    for (const State6Site& site : kItemClientChecks) {
        const uint8_t expect[7] = {0x83, site.modrm, 0x90, 0x00, 0x00, 0x00, 0x06};
        void* helper = site.modrm == 0xB8 ? reinterpret_cast<void*>(&h_state6_eax)
                     : site.modrm == 0xB9 ? reinterpret_cast<void*>(&h_state6_ecx)
                     : site.modrm == 0xBA ? reinterpret_cast<void*>(&h_state6_edx)
                                          : reinterpret_cast<void*>(&h_state6_edi);
        if (!patch_call(site.at, expect, 7, helper)) {
            LOG_ERROR("[coop] Item manager check at %08X does not match this build", site.at);
            ++bad;
        }
    }
    for (uint32_t at : kItemPopSites) {
        if (!redirect_call(at, kPopPacket, reinterpret_cast<void*>(&hk_item_pop))) {
            LOG_ERROR("[coop] Item packet read at %08X does not match this build", at);
            ++bad;
        }
    }
    return bad;
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

// ---------------------------------------------------------------- a death is a death, not a phantom's
// The event script's HavePartyMember (Lua native 0xD5E1D0: PartyMemberInfo+0x14 > 1) decides how a player dies:
// without a party, or as the host, the death is counted and the game respawns them (SoloPlayDeath); a summoned
// player with a party dies as a phantom (PartyGhostDeath: OnGameLeave on every player, ReqularLeavePlayer, home).
// That took a co-op guest out of the session on its first death (and the host saw it vanish). Remastered Seamless
// Co-op answers HavePartyMember with "we invade" in a session, so hosts and co-op guests die as in single player
// (counted, respawned at the bonfire) and invaders keep the game's rules. The guest's join also asks it
// (SummonReloadStart reloads into the host's world only with a party), so a guest is answered this way only once
// it has arrived.
constexpr uint32_t kHavePartyMember = 0x00D5E1D0;
using BoolFn = bool(__cdecl*)();
BoolFn g_have_party_orig = nullptr;

bool __cdecl hk_have_party_member() {
    if (seamless_rules() && (g_state.host || (g_state.guest && game::join_step() == 4))) return false;
    return g_have_party_orig();
}

// ---------------------------------------------------------------- no multiplayer walls
// OnEvent_4034 (OnSessionJustIn: a session starts) raises the white walls that keep phantoms in the host's area
// (Lua_MultiWall: collision, drawing and the wall's SFX on), once per wall GetMultiWallNum (0xD5DC40) counts for
// the map (a fixed table: m10_01 has 11). It runs when IsClient() is false, so the host raises them and every
// player in the host's world meets them. Remastered Seamless Co-op has none: the count is 0 in any seamless role.
constexpr uint32_t kMultiWallNum = 0x00D5DC40;
using IntFn = int(__cdecl*)();
IntFn g_multiwall_orig = nullptr;

int __cdecl hk_multiwall_num() {
    if (g_state.active && !g_state.leaving) return 0;
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

// ---------------------------------------------------------------- teams (Ominous Tome, invaders)
// Who may hit whom comes from the team relation table (0x12DF268, 19 x 19 bytes, row attacker, column target,
// 1 = hostile), read with each character's team (chr+0x74; 0xE68C80 returns it, or 0xF while state 0x84 is on).
// Most callers inline that lookup, so the team itself is changed on the characters. Players are team 1
// (4 hollow). Remastered Seamless Co-op puts allies on 0xF ("allies hostile") or 0x12 ("teams") and invaders on
// 0x10: Remastered has a second, more hostile relation table in which 0xF is hostile to every team. PTDE has
// only the first table, where 0xF is friendly to players; its team hostile to everyone (players, enemies,
// bosses and itself) is 13, and 16 (invaders) is hostile to players only, as in Remastered.
constexpr uint32_t kTeamType = 0x00E68C80;
constexpr uint32_t kTeamOffset = 0x74;
constexpr int kTeamEveryone = 13;
constexpr int kTeamInvader = 16;
void* g_team_orig = nullptr;
std::vector<RemotePlayer> g_remote;
std::map<uint32_t, int> g_team_saved;  // character -> its team before we changed it

bool is_remote_player(uint32_t chr) {
    uint32_t wcm = 0, n = 0, arr = 0;
    if (!read32(game::kWorldChrManVar, &wcm) || !wcm || !read32(wcm + 0x24, &n) || !read32(wcm + 0x28, &arr) || !arr) {
        return false;
    }
    for (uint32_t i = 1; i < n && i < 8; ++i) {
        uint32_t p = 0;
        if (read32(arr + i * 0x20, &p) && p == chr) return true;
    }
    return false;
}

// The team a player's character gets under the session's rules, -1 = the game's own.
int wanted_team(uint32_t chr) {
    if (!g_state.active || g_state.leaving || !chr) return -1;
    if (chr == game::player()) return g_state.invader ? kTeamInvader : -1;
    for (const auto& r : g_remote) {
        if (r.chr != chr || !r.known) continue;
        if (g_state.invader) return -1;  // we invade: everyone else keeps a player team, hostile to ours
        if (r.invader) return kTeamInvader;
        if (g_state.world_rules == 1) return kTeamEveryone;  // allies hostile: everyone fights everyone
        if (g_state.world_rules == 2) {
            // Teams: the host against the guests.
            if (g_state.host) return kTeamEveryone;
            return r.host ? kTeamEveryone : -1;
        }
        return -1;
    }
    return -1;
}

int __stdcall team_override(uint32_t chr) {
    if (!g_state.active || g_state.leaving || !chr) return -1;
    if (chr != game::player() && !is_remote_player(chr)) return -1;
    return wanted_team(chr);
}

void write_team(uint32_t chr, int team) {
    __try {
        *ptr<int>(chr + kTeamOffset) = team;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Every frame: the wanted teams on the characters, the old ones back when the rules no longer apply.
void apply_teams() {
    if (game::loading()) {
        g_team_saved.clear();  // the characters are gone
        return;
    }
    std::vector<uint32_t> chrs;
    if (const uint32_t me = game::player()) chrs.push_back(me);
    for (const auto& r : g_remote) {
        if (r.chr && is_remote_player(r.chr)) chrs.push_back(r.chr);
    }
    for (uint32_t chr : chrs) {
        const int want = wanted_team(chr);
        auto it = g_team_saved.find(chr);
        if (want >= 0) {
            uint32_t cur = 0;
            if (!read32(chr + kTeamOffset, &cur)) continue;
            if (it == g_team_saved.end()) {
                g_team_saved[chr] = static_cast<int>(cur);
                LOG_INFO("[coop] Team of %s character %08X: %u -> %d", chr == game::player() ? "our" : "a remote", chr,
                         cur, want);
            }
            if (static_cast<int>(cur) != want) write_team(chr, want);
        } else if (it != g_team_saved.end()) {
            write_team(chr, it->second);
            LOG_INFO("[coop] Team of character %08X back to %d", chr, it->second);
            g_team_saved.erase(it);
        }
    }
    // Characters that left keep nothing of ours.
    for (auto it = g_team_saved.begin(); it != g_team_saved.end();) {
        bool present = false;
        for (uint32_t c : chrs) present |= c == it->first;
        it = present ? std::next(it) : g_team_saved.erase(it);
    }
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

// ---------------------------------------------------------------- world events run in every player's game
// Map events (EMEVD) test the session with EcTestMultiplayState::Test (0xBB3130, thiscall(cond, dt, event),
// ret 8; cond+0x18 kind: 0 host, 1 client, 2 multiplayer, 3 single player; result in bit 0 of cond+8). The
// SKIP/END IF variants call it directly (0xBB3200). The game's co-op stops most events on clients ("END IF
// client"): only the host's game runs them and the network passes on what it can, so a guest walking alone
// onto the Undead Burg bridge meets no Hellkite and a guest-only boss fight never starts. Remastered Seamless
// Co-op answers the test for every event: "host" passes and "client" fails, so each player's game runs the
// world's events; its two event lists below change the rest. The event id is FrpgEventIns+0x28 (+0x2C slot;
// constructor 0xBC4150).
constexpr uint32_t kMultiplayTest = 0x00BB3130;
constexpr uint32_t kEventId = 0x28;
// Every multiplayer test of these events fails (they act as neither host nor client, single nor multi):
// e.g. the crow that carries the player out of the Undead Asylum (11810150). Ids PTDE does not have are kept.
constexpr int kEventsNoMultiplay[] = {11005844, 11015846, 11105846, 11205846, 11215846, 11305846,
                                      11315846, 11405846, 11415846, 11505846, 11510460, 11515846,
                                      11605846, 11600250, 11705846, 11810150};
// These act as in single player (lifts and warps the game keeps from multiplayer, e.g. 11020301, 11600200).
constexpr int kEventsSinglePlayer[] = {11020301, 11200005, 11200006, 11305000, 11510230, 11600200};
bool g_world_events = true;
using MultiplayTestFn = void(__fastcall*)(uint8_t*, void*, float, uint8_t*);
MultiplayTestFn g_multiplay_orig = nullptr;

template <size_t N>
bool in_list(const int (&list)[N], int id) {
    for (int v : list) {
        if (v == id) return true;
    }
    return false;
}

void __fastcall hk_multiplay_test(uint8_t* cond, void* edx, float dt, uint8_t* event) {
    g_multiplay_orig(cond, edx, dt, event);
    if (!g_world_events || !seamless_rules() || !cond) return;
    uint32_t id = 0xFFFFFFFF;
    if (event) read32(reinterpret_cast<uint32_t>(event) + kEventId, &id);
    const int eid = static_cast<int>(id);
    bool pass;
    if (in_list(kEventsNoMultiplay, eid)) {
        pass = false;
    } else {
        switch (cond[0x18]) {
            case 0: pass = true; break;   // host
            case 1: pass = false; break;  // client
            case 2:                       // multiplayer
                if (!in_list(kEventsSinglePlayer, eid)) return;
                pass = false;
                break;
            case 3:  // single player
                if (!in_list(kEventsSinglePlayer, eid)) return;
                pass = true;
                break;
            default:
                return;
        }
    }
    cond[8] = static_cast<uint8_t>((cond[8] & 0xFE) | (pass ? 1 : 0));
}

// ---------------------------------------------------------------- Black Separation Crystal
// Using it (goods 103) puts SpEffect 7 on the player; the player's update (0xE512A0) sees the effect's separation
// flag and, for the local player, either runs the Lua OnLeaveMagic (a summoned player goes home: 0xE5136A, eax =
// Lua event system) or opens the host's "send phantoms home" menu (0xE51371, 0xC2B4D0: MenuMan+0xBC = 1 when
// white phantoms are present, else 5). Neither fits a seamless session: guests keep their own body (the menu finds
// no phantom) and the Lua leave is skipped by the session guard, which left a box on screen. In a seamless
// session the crystal is the session's own leave instead: a guest goes home, the host closes their world.
constexpr uint32_t kSepGuestSite = 0x00E5136A;
constexpr uint32_t kSepGuestOrig = 0x00D78290;  // eax = Lua event system: calls OnLeaveMagic
constexpr uint32_t kSepHostSite = 0x00E51371;
constexpr uint32_t kSepHostOrig = 0x00C2B4D0;
void (*g_separation_fn)(bool host) = nullptr;

bool __cdecl separation_handled(bool host) {
    if (!seamless_rules() || !g_separation_fn) return false;
    if (host ? !g_state.host : !g_state.guest) return false;
    if (host && g_state.invader_present) return false;  // the game's own rules while an invader is here
    LOG_INFO("[coop] Black Separation Crystal: %s", host ? "the host closes the world" : "a guest goes home");
    g_separation_fn(host);
    return true;
}

__declspec(naked) void hk_separation_guest() {
    __asm {
        pushad
        push 0
        call separation_handled
        add esp, 4
        test al, al
        popad
        jnz handled
        mov edx, kSepGuestOrig
        jmp edx
    handled:
        ret
    }
}

__declspec(naked) void hk_separation_host() {
    __asm {
        pushad
        push 1
        call separation_handled
        add esp, 4
        test al, al
        popad
        jnz handled
        mov edx, kSepHostOrig
        jmp edx
    handled:
        ret
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
void set_separation_listener(void (*fn)(bool host)) { g_separation_fn = fn; }
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
    failed += install_one("guest's own items", install_items() == 0);
    failed += install_one("bonfire party check", redirect_call(0x00D58AE4, kHasParty, reinterpret_cast<void*>(&hk_has_party)));
    {
        static const uint8_t k[6] = {0x8B, 0x0D, 0xA0, 0x84, 0x37, 0x01};  // mov ecx,[013784A0h]
        g_have_party_orig = reinterpret_cast<BoolFn>(
            detour(kHavePartyMember, k, sizeof(k), reinterpret_cast<void*>(&hk_have_party_member)));
        failed += install_one("HavePartyMember (deaths)", g_have_party_orig != nullptr);
    }
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
    {
        // push esi ; mov esi,ecx ; movzx ecx,byte [esi+18h]
        static const uint8_t k[7] = {0x56, 0x8B, 0xF1, 0x0F, 0xB6, 0x4E, 0x18};
        g_multiplay_orig = reinterpret_cast<MultiplayTestFn>(
            detour(kMultiplayTest, k, sizeof(k), reinterpret_cast<void*>(&hk_multiplay_test)));
        failed += install_one("world events (multiplayer state test)", g_multiplay_orig != nullptr);
    }
    failed += install_one("Black Separation Crystal (guest)",
                          redirect_call(kSepGuestSite, kSepGuestOrig, reinterpret_cast<void*>(&hk_separation_guest)));
    failed += install_one("Black Separation Crystal (host)",
                          redirect_call(kSepHostSite, kSepHostOrig, reinterpret_cast<void*>(&hk_separation_host)));
    return failed;
}

void set_guest_world_events(bool on) { g_world_events = on; }

void set_remote_players(const std::vector<RemotePlayer>& players) { g_remote = players; }

void tick() {
    g_main_thread = GetCurrentThreadId();
    // A co-op guest's loot and items follow the host's (single player) rules for as long as it is a guest, also
    // while it leaves (the area unload on the way home must clean up the same way the items were made).
    g_strict = (g_state.active && g_state.guest && !g_state.invader) ? 0 : 1;
    apply_teams();
    if (!seamless_rules()) return;
    if (g_state.host && game::session_state() == 3) game::keep_room_open();
}

}  // namespace coop::rules
