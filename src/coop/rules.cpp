// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "rules.h"

#include "game.h"
#include "hook.h"
#include "log.h"
#include "session.h"
#include "world.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
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
// Set while this process hosts a seamless session, and cleared only once the game session is solo.
// Survives finish() clearing the role, so a guest disconnect that arrives a moment later still does not
// take the host to the title screen.
bool g_seamless_hosted = false;
// Item own-world writes (see below) must not be broadcast as shared progress.
int g_flag_quiet = 0;
void clear_guest_item_handles();
bool patch_call(uint32_t at, const uint8_t* expect, size_t n, void* helper);

bool seamless_rules() { return g_state.active && !g_state.leaving && !g_state.invader; }

// A co-op guest's leave_session looks to the host like the whole session ended. These callers then run the
// title leave when only one member remains. Vanilla phantom leave does not title-kick the host; block these
// only for the seamless host, in its own world, while the game session is still hosting.
constexpr uint32_t kRetMemberLeft = 0x00D2E83F;
constexpr uint32_t kRetMemberLeftMenu = 0x00DB4A4A;
constexpr uint32_t kRetMemberLeftMenu2 = 0x00DB4AD1;

bool host_stays(uint32_t ret) {
    if (ret != kRetMemberLeft && ret != kRetMemberLeftMenu && ret != kRetMemberLeftMenu2) return false;
    // A host who is disbanding lets the game end the session when the last guest has left, as it does after
    // the Black Separation Crystal's menu.
    if (g_state.closing) return false;
    if (!g_seamless_hosted || g_state.invader || !game::own_world() || game::session_state() != 3) return false;
    if (++g_blocked_leaves <= 50) LOG_INFO("[coop] Kept the host in the world (leave from %08X skipped)", ret);
    return true;
}

bool __stdcall allow_leave(uint32_t ret) {
    if (host_stays(ret)) return false;
    if (!seamless_rules()) return true;
    switch (ret) {
        case kRetStepEnd:
            // This destructor runs for a map load and again a few seconds after the last guest leaves.
            // leave_session would save a spot with no floor. Ending the session here closed the world on
            // its own, and the next join never got past "host data not received".
            if (++g_blocked_leaves <= 50) LOG_INFO("[coop] Kept the session (leave from %08X skipped)", ret);
            return false;
        case kRetLuaLeave:
        case kRetWarpKick:
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

// ---------------------------------------------------------------- a guest leaving is not the master leaving
// When a member's character is taken out of the party (0xEA2A10), 0xD79380 shows the "left" notice and looks at
// that member's summon type (remote player data +0x110). Type 0 means the master of the world: the game shows
// "Disconnected from master of world" (20001025), requests a save, raises flag 4047 and arms the event
// script's OnLeave_Limit for five seconds later. That handler sets the sign warp and kicks the player back to
// where they were summoned from. A co-op guest keeps their own body and is never given a summon type, so on
// the host's side every guest who left looked like the master leaving: five seconds later the host was warped
// to a summon sign that never existed (no map, no floor), and the save made on the way out put them there
// again on the next load. In one's own world nobody else is the master, so the return home is not armed
// there. A co-op guest is taken home by the session code and is not armed either.
constexpr uint32_t kMasterLeftSite = 0x00D79481;  // test bl,bl ; je 0xD794E1 ; call 0xD6B2A0
constexpr uint32_t kMasterLeftSave = 0x00D6B2A0;  // save request, the first call of the armed branch

bool __cdecl arm_master_left() {
    const char* why = nullptr;
    if (game::own_world()) {
        why = "this is our own world";
    } else if (g_state.active && g_state.guest && !g_state.invader) {
        why = "the session takes a co-op guest home";
    }
    if (!why) return true;
    static int lines = 0;
    if (++lines <= 40) LOG_INFO("[coop] A player left: the game's return home is not armed (%s)", why);
    return false;
}

// Returns with ZF set when the armed branch must be skipped (the patched site jumps to the function's end).
__declspec(naked) void hk_master_left() {
    __asm {
        test bl, bl
        jz done
        pushad
        sub esp, 0x80
        movdqu xmmword ptr [esp], xmm0
        movdqu xmmword ptr [esp + 0x10], xmm1
        movdqu xmmword ptr [esp + 0x20], xmm2
        movdqu xmmword ptr [esp + 0x30], xmm3
        movdqu xmmword ptr [esp + 0x40], xmm4
        movdqu xmmword ptr [esp + 0x50], xmm5
        movdqu xmmword ptr [esp + 0x60], xmm6
        movdqu xmmword ptr [esp + 0x70], xmm7
        call arm_master_left
        movdqu xmm0, xmmword ptr [esp]
        movdqu xmm1, xmmword ptr [esp + 0x10]
        movdqu xmm2, xmmword ptr [esp + 0x20]
        movdqu xmm3, xmmword ptr [esp + 0x30]
        movdqu xmm4, xmmword ptr [esp + 0x40]
        movdqu xmm5, xmmword ptr [esp + 0x50]
        movdqu xmm6, xmmword ptr [esp + 0x60]
        movdqu xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        test al, al
        popad
        jz done
        mov eax, kMasterLeftSave
        call eax
        or eax, 1
    done:
        ret
    }
}

bool install_master_left() {
    static const uint8_t k[9] = {0x84, 0xDB, 0x74, 0x5C, 0xE8, 0x16, 0x1E, 0xFF, 0xFF};
    uint8_t code[9] = {0xE8, 0, 0, 0, 0, 0x74, 0x59, 0x90, 0x90};  // call stub ; je 0xD794E1 ; nop ; nop
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_master_left) - (kMasterLeftSite + 5));
    std::memcpy(code + 1, &rel, 4);
    return patch(kMasterLeftSite, k, code, sizeof(code));
}

// ---------------------------------------------------------------- a guest's bloodstain
// The death penalty (0xD78B10, the event script's UpDateBloodMark) writes the bloodstain record
// (GameDataMan+0x1C: position, map, souls, humanity). In one's own world the position is where the player's
// replay record starts (PlayerIns+0x5E0: a few seconds before the death). In someone else's world the game
// takes the place the player was summoned from instead (GameMan+0xAE0), because a phantom's stain belongs in
// their own world. A co-op guest was never summoned from anywhere: the stain went to coordinates that were
// never filled in, in a map they were not in, and the souls could not be found in either world. Remastered
// Seamless Co-op answers this "own world?" test with "not an invader"; a co-op guest's stain is left where
// they fell, in the world they are playing in, and it is at the same place when they are back home.
constexpr uint32_t kBloodWorldSite = 0x00D78BA0;  // cmp byte ptr [eax+0B4Fh], 0 ; the je that follows stays

struct SafeSpot {
    double at;
    game::Vec3 pos;
    int map;
};
SafeSpot g_spots[32];
int g_spot_next = 0, g_spot_count = 0;
double g_last_spot = 0;

double seconds() { return GetTickCount64() / 1000.0; }

// Where the player stood about three seconds ago (while alive), or false when nothing was recorded.
bool recent_safe_spot(SafeSpot* out) {
    const double t = seconds();
    const SafeSpot* best = nullptr;
    for (int i = 0; i < g_spot_count; ++i) {
        const SafeSpot& s = g_spots[i];
        const double age = t - s.at;
        if (age < 2.5 || age > 30.0) continue;
        if (!best || s.at > best->at) best = &s;
    }
    if (!best) return false;
    *out = *best;
    return true;
}

bool __cdecl blood_in_own_world() {
    if (game::own_world()) return true;
    if (!g_state.active || !g_state.guest || g_state.invader || game::session_state() != 6) return false;
    const uint32_t me = game::player();
    uint32_t replay = 0;
    if (!me || !read32(me + 0x5E0, &replay) || replay < 0x10000) return false;
    // The replay record's start is the game's own choice. In another world it may never have been written:
    // then the stain goes to where the player stood a few seconds ago, or to where they are now.
    game::Vec3 here{}, start{};
    uint32_t start_map = 0;
    if (!game::position(&here, nullptr)) return false;
    bool usable = true;
    __try {
        std::memcpy(&start, reinterpret_cast<const void*>(static_cast<uintptr_t>(replay + 0x20)), sizeof(start));
        start_map = *ptr<uint32_t>(replay + 0x38);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const float dx = start.x - here.x, dy = start.y - here.y, dz = start.z - here.z;
    const float far2 = dx * dx + dy * dy + dz * dz;
    const int map = game::map_id();
    if (far2 > 60.0f * 60.0f || (map != -1 && (start_map & 0xFFFF0000) != (static_cast<uint32_t>(map) & 0xFFFF0000))) {
        usable = false;
    }
    if (!usable) {
        SafeSpot spot{};
        if (!recent_safe_spot(&spot)) {
            spot.pos = here;
            spot.map = map;
        }
        __try {
            std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(replay + 0x20)), &spot.pos, sizeof(spot.pos));
            if (spot.map != -1) *ptr<int>(replay + 0x38) = spot.map;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        LOG_INFO("[coop] Bloodstain: the replay record was not usable here; using (%.1f, %.1f, %.1f) map %08X",
                 spot.pos.x, spot.pos.y, spot.pos.z, spot.map);
    } else {
        LOG_INFO("[coop] Bloodstain left in the host's world at (%.1f, %.1f, %.1f) map %08X", start.x, start.y,
                 start.z, start_map);
    }
    return true;
}

// Replaces the compare: ZF set means "someone else's world" to the je that follows. Every register is kept
// (eax is GameMan in the branch for someone else's world).
__declspec(naked) void hk_blood_world() {
    __asm {
        pushad
        sub esp, 0x80
        movdqu xmmword ptr [esp], xmm0
        movdqu xmmword ptr [esp + 0x10], xmm1
        movdqu xmmword ptr [esp + 0x20], xmm2
        movdqu xmmword ptr [esp + 0x30], xmm3
        movdqu xmmword ptr [esp + 0x40], xmm4
        movdqu xmmword ptr [esp + 0x50], xmm5
        movdqu xmmword ptr [esp + 0x60], xmm6
        movdqu xmmword ptr [esp + 0x70], xmm7
        call blood_in_own_world
        movdqu xmm0, xmmword ptr [esp]
        movdqu xmm1, xmmword ptr [esp + 0x10]
        movdqu xmm2, xmmword ptr [esp + 0x20]
        movdqu xmm3, xmmword ptr [esp + 0x30]
        movdqu xmm4, xmmword ptr [esp + 0x40]
        movdqu xmm5, xmmword ptr [esp + 0x50]
        movdqu xmm6, xmmword ptr [esp + 0x60]
        movdqu xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        test al, al
        popad
        ret
    }
}

bool install_blood_world() {
    static const uint8_t k[7] = {0x80, 0xB8, 0x4F, 0x0B, 0x00, 0x00, 0x00};
    return patch_call(kBloodWorldSite, k, sizeof(k), reinterpret_cast<void*>(&hk_blood_world));
}

// ParamInitialize (Lua, 0xD66360) runs on every map load and decides whether our bloodstain stays. A human keeps
// it (0xD78B10 without a new record: the record stays and its marker is put back), and so does a hollow alone. A
// hollow with anyone else in the party (PartyMemberInfo = [GameMan+0xBEC], +0x14 members > 1) loses it
// (InvalidMyBloodMarkInfo, 0xD6D920): in the game's own multiplayer a hollow is never summoned, so that stain
// would belong to another world. A hollow co-op guest, or a hollow host with a guest, is in such a party on every
// load. The guest's stain from the host's world was erased half a second after each load (2026-10-13: "Bloodstain:
// gone" 0.7 s after joining) and only came back at home, from the save. In seamless co-op the party does not
// count here.
constexpr uint32_t kParamInitParty = 0x00D66390;  // cmp dword ptr [eax+14h], 1 ; jg 00D663A4 (erase)
constexpr uint32_t kParamInitErase = 0x00D663A4;

bool __stdcall stain_party_erases(uint32_t party) {
    if (seamless_rules()) {
        static int lines = 0;
        if (++lines <= 10) LOG_INFO("[coop] Our bloodstain stays on this load (hollow, in a co-op party)");
        return false;
    }
    uint32_t members = 0;
    return read32(party + 0x14, &members) && static_cast<int>(members) > 1;
}

// Replaces the compare and the jg: returns to the next instruction (keep) or to the erase call.
__declspec(naked) void hk_param_party() {
    __asm {
        pushad
        push eax  // PartyMemberInfo
        call stain_party_erases
        test al, al
        popad
        jz keep
        mov dword ptr [esp], 0x00D663A4  // kParamInitErase
    keep:
        ret
    }
}

bool install_param_party() {
    static const uint8_t k[6] = {0x83, 0x78, 0x14, 0x01, 0x7F, 0x0E};
    return patch_call(kParamInitParty, k, sizeof(k), reinterpret_cast<void*>(&hk_param_party));
}

// The party census (0xEA1A70) resolves each member handle and then reads the character's type at +0x70.
// A failed lookup leaves esi at 0 and falls through to that read. Quit destroys the characters first, so
// a handle that was not -1 crashes there. An empty result skips the slot, which is what a missing
// character already means.
constexpr uint32_t kPartyCensus = 0x00EA1AE0;  // mov eax, [esi+70h] ; sub eax, edi
constexpr uint32_t kPartyCensusNext = 0x00EA1B02;
void* g_census_orig = nullptr;
void* g_census_next = reinterpret_cast<void*>(kPartyCensusNext);

__declspec(naked) void hk_party_census() {
    __asm {
        test esi, esi
        jz missing
        jmp g_census_orig
    missing:
        jmp g_census_next
    }
}

// ---------------------------------------------------------------- reloads stay in the host's world
// MoveMapStep's constructor (0xEFC470) decides own world vs someone else's by SessMan+0x194 == 3, which the
// game only sets for the first summon reload. A later load (death, following the host) sets the step to 3
// for that decision and puts it back to 4 when the constructor returns. That is all the constructor is for.
//
// The area that streams is chosen earlier. The load starter (0xEF0820) reads GameMan+0x14 before it calls
// the constructor, and a death gets that id from 0xD5F680: if the sign-warp byte (GameMan+0xB00) is set it
// reloads the join map, otherwise it reloads the last bonfire. Writing the bonfire map inside the constructor
// is too late for the first read, so the loading screen and the collision stay on the join area while a later
// step is told a different map. The guest then has no floor.
//
// Where they stand is a separate choice. Once the area is in, 0xEFA944 sees own-world == 0 and copies the
// join position (GameMan+0xAB0) without looking at the bonfire. A death in the host's own area therefore
// stands on the join point, and a death in another area stands there with that area's collision missing.
// The bonfire case of the regen switch (0xD763CA, via 0xD5F680) is what a solo death uses. A guest death in
// this test did not take that case: WarpSelfBloodMark (0xD649D0) stores map 0x01000000 and calls the kick
// directly. That map has no collision. The kick is aimed at the guest's bonfire before its request bit is set.
constexpr uint32_t kMoveMapCtor = 0x00EFC470;
constexpr uint32_t kSpawnChoice = 0x00EEFCE1;  // cmp dword [eax+194h], 3
constexpr uint32_t kSpawnResume = 0x00EEFCFB;  // read the warp map, skip the join-map write
constexpr uint32_t kRegenSwitch = 0x00D76390;  // esi = script; arg picks a warp, 1 = last bonfire
constexpr uint32_t kChooseWarpMap = 0x00D5F680;  // stdcall(point): writes GameMan+0x14 from the bonfire or the sign
constexpr uint32_t kDeathWarpRet = 0x00D763DD;   // the bonfire case's call
constexpr uint32_t kBloodMapWarpRet = 0x00D60078;  // SetSelfBloodMapUid when the death event has no map
constexpr uint32_t kPlaceChoice = 0x00EFA944;    // cmp byte [esi+0xB4F], bl  (own world?)
constexpr uint32_t kPlaceBonfire = 0x00EFAA0C;   // spawn on GameMan+0xB04
void* g_movemap_orig = nullptr;
void* g_spawn_orig = nullptr;
void* g_spawn_resume = reinterpret_cast<void*>(kSpawnResume);
void* g_regen_orig = nullptr;
void* g_choose_orig = nullptr;
void* g_place_orig = nullptr;
void* g_bonfire_place = reinterpret_cast<void*>(kPlaceBonfire);
bool g_marked = false;
// Set for the constructor only, so a death's spawn step does not copy the join map over the bonfire map.
bool g_bonfire_spawn = false;
// Set by the death warp, kept until the placement step of that same load.
bool g_death_respawn = false;
bool g_place_bonfire = false;

// The regen switch checks GameMan+0xB04 before it will call the bonfire case. A guest's rest writes that
// field, and a load clears it again, so it is put back here. The sign-warp byte would make the case reload
// the join map instead of the bonfire; it is cleared so the bonfire is the one that streams.
void __cdecl note_guest_death_warp() {
    if (!seamless_rules() || !g_state.guest || game::session_state() != 6) return;
    const int bonfire = session::guest_respawn_bonfire();
    const int map = game::bonfire_map(bonfire);
    const uint32_t gm = game::game_man();
    if (!gm || bonfire < 0 || map == -1) return;
    *ptr<int>(gm + game::kGmLastBonfire) = bonfire;
    *ptr<int>(gm + 0xB08) = -1;
    *ptr<uint8_t>(gm + 0xB00) = 0;
}

// Return address of 0xD5F680. Two callers reload a dead guest: the bonfire case of the regen switch, and
// SetSelfBloodMapUid when the death event has no map of its own (that one then forces the warp point to -1).
// Every other caller keeps the game's coordinates. The bonfire is written here, before the original body
// reads it; a load clears it, and the sign-warp byte would stream the join map instead.
// stdcall: the naked hook pushes this argument and popad does not drop it. A cdecl callee leaves the
// dword on the stack, popad restores the wrong registers, and the original returns into data.
void __stdcall note_death_respawn_load(uint32_t ret) {
    if (ret != kDeathWarpRet && ret != kBloodMapWarpRet) return;
    if (!seamless_rules() || !g_state.guest || game::session_state() != 6) return;
    const int bonfire = session::guest_respawn_bonfire();
    const int map = game::bonfire_map(bonfire);
    const uint32_t gm = game::game_man();
    if (!gm || bonfire < 0 || map == -1) return;
    *ptr<int>(gm + game::kGmLastBonfire) = bonfire;
    *ptr<int>(gm + 0xB08) = -1;
    *ptr<uint8_t>(gm + 0xB00) = 0;
    g_death_respawn = true;
    LOG_INFO("[coop] Death reloads the bonfire's area (bonfire %d, map %08X)", bonfire, map);
}

// WarpSelfBloodMark's map. Area 1 block 0 is not a level; loading it leaves no floor.
constexpr int kEmptyWarpMap = 0x01000000;

// Writes the guest's respawn bonfire into the warp the kick is about to start, and remembers it for the
// placement step. The kick's caller may already have stored the empty map; this runs first and replaces it.
bool arm_guest_death_reload() {
    if (!seamless_rules() || !g_state.guest || game::session_state() != 6) return false;
    const int bonfire = session::guest_respawn_bonfire();
    const int map = game::bonfire_map(bonfire);
    const uint32_t gm = game::game_man();
    if (!gm || bonfire < 0 || map == -1) return false;
    *ptr<int>(gm + game::kGmLastBonfire) = bonfire;
    *ptr<int>(gm + 0xB08) = -1;
    *ptr<uint8_t>(gm + 0xB00) = 0;
    *ptr<uint8_t>(gm + game::kGmUseBonfire) = 1;
    *ptr<int>(gm + game::kGmWarpMap) = map;
    *ptr<int>(gm + game::kGmWarpPoint) = -1;
    if (!g_death_respawn) {
        LOG_INFO("[coop] Death reloads the bonfire's area (bonfire %d, map %08X)", bonfire, map);
    }
    g_death_respawn = true;
    return true;
}

bool kick_prepare(bool dead) {
    const uint32_t gm = game::game_man();
    if (!gm) return false;
    // A guest who is still in the session is aimed at their respawn bonfire before the map is checked.
    if (seamless_rules() && dead) arm_guest_death_reload();
    int map = 0;
    if (!read32(gm + game::kGmWarpMap, reinterpret_cast<uint32_t*>(&map)) || map != kEmptyWarpMap) return false;
    // WarpSelfBloodMark stores 01000000. That is not a level. A death reloads the bonfire, including
    // after the world was closed and the session rules are no longer on. A living player's warp to a
    // bloodstain in their own world is left to the game. During a session that warp is not started.
    if (dead && game::own_world()) {
        const int bonfire = game::last_bonfire();
        const int real = game::bonfire_map(bonfire);
        if (real != -1) {
            *ptr<int>(gm + game::kGmLastBonfire) = bonfire;
            *ptr<uint8_t>(gm + game::kGmUseBonfire) = 1;
            *ptr<int>(gm + game::kGmWarpMap) = real;
            *ptr<int>(gm + game::kGmWarpPoint) = -1;
            LOG_INFO("[coop] Death reloads the bonfire's area (bonfire %d, map %08X)", bonfire, real);
            return false;
        }
    }
    if (!dead && !seamless_rules()) return false;
    *ptr<int>(gm + game::kGmWarpMap) = -1;
    LOG_INFO("[coop] Warp to empty map 01000000 was not started");
    return true;
}

int clear_visited_temporary_flags();  // the death reload's reset of the maps' xxxx5xxx flags (below)
constexpr int kDeathLatch = 4000;     // OnEvent_4000 starts a death only while it is off; a load at home clears it

void __cdecl before_map_load() {
    g_marked = false;
    g_bonfire_spawn = false;
    // A walk, a follow, or the trip home did not come from the death warp. Drop a stale request so the
    // next placement is not pulled onto the bonfire. The death load itself still has the flag, because
    // the warp set it and this constructor is the first step of that load.
    if (g_state.leaving) g_death_respawn = false;
    g_place_bonfire = g_death_respawn;
    clear_guest_item_handles();
    // Remember the bonfire for the placement step. The warp map is not written here: the load starter
    // has already read it, and replacing it now streams one area and spawns into another.
    if (g_state.guest && !g_state.invader && !g_state.leaving) {
        const int bonfire = session::guest_respawn_bonfire();
        if (bonfire >= 0) {
            if (const uint32_t gm = game::game_man()) *ptr<int>(gm + game::kGmLastBonfire) = bonfire;
        }
    }
    if (seamless_rules() && g_state.guest && game::session_state() == 6 && game::join_step() == 4) {
        game::set_join_step(3);
        g_marked = true;
        if (g_place_bonfire) g_bonfire_spawn = true;
        LOG_INFO("[coop] Map load stays in the host's world");
        if (g_place_bonfire) {
            const int n = clear_visited_temporary_flags();
            const bool latch = game::flag(kDeathLatch);
            if (latch) game::set_flag(kDeathLatch, false);
            LOG_INFO("[coop] Death reload: the host's world's temporary flags (xxxx5xxx) start over, as a load at home "
                     "does (%d block(s)%s)", n, latch ? "; death flag 4000 cleared" : "");
        }
    }
}

void __cdecl after_map_load() {
    if (g_marked && game::join_step() == 3) game::set_join_step(4);
    g_marked = false;
    g_bonfire_spawn = false;
    // The placement step is still ahead of this. It reads g_place_bonfire, which stays set.
    g_death_respawn = false;
}

// True: jump to 0xEEFCFB. The compare itself is not executed, so the join map is left alone.
bool __cdecl use_bonfire_spawn() { return g_bonfire_spawn; }

// True: jump to the bonfire placement. The load clears the use-bonfire byte, so it is set again here,
// after the area has streamed and before the coordinates are chosen.
bool __cdecl place_at_bonfire() {
    if (!g_place_bonfire) return false;
    g_place_bonfire = false;
    const int bonfire = session::guest_respawn_bonfire();
    const uint32_t gm = game::game_man();
    if (!gm || bonfire < 0 || game::bonfire_map(bonfire) == -1) return false;
    *ptr<int>(gm + game::kGmLastBonfire) = bonfire;
    *ptr<int>(gm + 0xB08) = -1;
    *ptr<uint8_t>(gm + game::kGmUseBonfire) = 1;
    *ptr<uint8_t>(gm + 0xB00) = 0;
    LOG_INFO("[coop] Guest placed at bonfire %d", bonfire);
    return true;
}

__declspec(naked) void hk_spawn_choice() {
    __asm {
        pushad
        call use_bonfire_spawn
        test al, al
        popad
        jz orig
        jmp g_spawn_resume
    orig:
        jmp g_spawn_orig
    }
}

__declspec(naked) void hk_regen_switch() {
    __asm {
        pushad
        call note_guest_death_warp
        popad
        jmp g_regen_orig
    }
}

__declspec(naked) void hk_choose_warp_map() {
    __asm {
        pushad
        push dword ptr [esp + 32]  // caller's return address; note_death_respawn_load is stdcall and pops it
        call note_death_respawn_load
        popad
        jmp g_choose_orig
    }
}

__declspec(naked) void hk_place_choice() {
    __asm {
        pushad
        call place_at_bonfire
        test al, al
        popad
        jz orig
        jmp g_bonfire_place
    orig:
        jmp g_place_orig
    }
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
// guest's own world store (+0x14) too, except the flags of the host's world's disasters (see kDisasterFlags).
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

bool host_disaster_flag(int id);  // a flag of the host's world's disaster (see kDisasterFlags)

// Shared progress: flags changed in the host's world travel to the other players (guest -> host -> guests).
void (*g_flag_listener)(int, bool) = nullptr;
bool g_applying_remote = false;
DWORD g_main_thread = 0;
int g_old_value = -1;

// Flags that describe one player rather than the world. Common events set them in each game from that
// game's own player, and the bonfire talk uses some of them as signals:
//   250-258 owned items the bonfire menu asks for (smithboxes, Repair Box, Rite of Kindling, Bottomless Box)
//   702, 706, 710, 715, 717, 719, 740 derived from the local player: where they stand, the Lordvessel
//     (bonfire warp), covenant items, anything that can be attuned, the Pyromancer class
//   750-755, 757-765 the local player's curse and parasite, Oswald's absolution (755: event 770 resets the
//     player's sin), curse removal (754), status messages, and the bonfire ceremonies in progress: kindling
//     (760) and Reverse Hollowing (762, 764). Common event 763 plays the Reverse Hollowing ceremony and then the
//     sitting loop on the local player whenever 762 is set, so a guest's 762 made the host sit down wherever
//     they stood.
// Shared, a guest's Lordvessel gave the host bonfire warp, a host's Bottomless Box was written into a guest's
// own world, and the status flags were sent back and forth as each game put its own player's value back.
//
// The rest of common.emevd's flags that each game works out from its own player (the game runs that script
// on the host only; seamless co-op runs it in every game):
//   259, 711-714, 720 (and 11020102), 722, 780-793: items the player holds; 716, 810: lots the player got
//   280-290 the starting class; 830, 831, 11510150 where the player is; 840-849 gesture triggers
//   850-859 the player's covenant (event 870): a guest in Way of White and a host with none sent 851 back
//     and forth dozens of times a second from the moment the guest arrived (2026-10-12 test)
//   600-603 a counter each game raises itself from the boss flags; 721, 723-728, 730-735 worked out from
//     other flags in each game
//   5000, 5001 the notices of a disaster starting and ending: each game's own disaster events set them and its
//     own notice clears them (see kDisasterEndFlags). Shared, a guest's 5001 arrived after the host's game had
//     shown and cleared its own and showed the host the notice again.
//   6000-8999: the visited world's store has no block of its own for these; the engine points it at the
//     player's own world (0xD384E7), so another player's value would be written into this player's save.
bool personal_flag(int id) {
    if (id >= 250 && id <= 259) return true;
    switch (id) {
        case 702: case 706: case 710: case 715: case 716: case 717: case 719: case 740: case 810:
        case 830: case 831: case 11020102: case 11510150:
            return true;
        default:
            break;
    }
    return (id >= 280 && id <= 290) || (id >= 600 && id <= 603) || (id >= 711 && id <= 714) ||
           (id >= 720 && id <= 735) || (id >= 750 && id <= 755) || (id >= 757 && id <= 765) ||
           (id >= 780 && id <= 793) || (id >= 840 && id <= 859) || id == 5000 || id == 5001 ||
           (id >= 6000 && id <= 8999);
}

// Item flags (5xxxxxxx: item lots and treasure taken) and conversation flags (7xxxxxxx) belong to each player:
// a co-op guest keeps them in their own world (see hk_flag_store).
bool player_flag_group(int id) { return (id >= 50000000 && id < 60000000) || (id >= 70000000 && id < 80000000); }

// A flag the games keep setting against each other is not world progress either: each game works it out from
// its own player. 11815396 ("the player is in the Asylum Demon's arena", set and cleared by the network-sync-off
// events 11815394/11815395 from the local player's position) went back and forth about 130 times a second
// between a guest inside the arena and a host outside it (2026-10-13 test), as 851 did before it. Eight changes
// of one flag within two seconds take it out of the sharing for the rest of the session.
std::map<int, std::pair<ULONGLONG, int>> g_flag_churn;
std::set<int> g_contested;

void note_flag_change(int id) {
    const ULONGLONG t = GetTickCount64();
    auto& c = g_flag_churn[id];
    if (t - c.first > 2000) {
        c.first = t;
        c.second = 0;
    }
    if (++c.second >= 8 && g_contested.insert(id).second) {
        LOG_INFO("[coop] Flag %d keeps flipping between the games: each game keeps its own from now on", id);
    }
}

// Lua multiplayer state (4000-4999) is per player and never shared, nor are the personal flags above.
bool shared_flag(int id) {
    return id >= 0 && !(id >= 4000 && id <= 4999) && !personal_flag(id) && !player_flag_group(id) &&
           !g_contested.count(id);
}

// The map event whose instruction is running: the EMEVD dispatcher (0xBC3AB0, called from the event update
// 0xBBF3B0) takes the event in eax. 0 outside map events (Lua, talk, the engine).
constexpr uint32_t kEventStep = 0x00BC3AB0;
void* g_event_step_orig = nullptr;
uint32_t g_cur_event = 0;
// Map events whose run mirrored a flag into a co-op guest's own world (see after_set_flag, mirror_event_done).
std::set<uint32_t> g_mirroring_events;

__declspec(naked) void hk_event_step() {
    __asm {
        push dword ptr [g_cur_event]
        mov dword ptr [g_cur_event], eax
        call dword ptr [g_event_step_orig]
        pop dword ptr [g_cur_event]
        ret
    }
}

// The 5000 block of a map's flags (xxxx5xxx) is its working state: timers, who stands where, which attack
// comes next. Such a flag stays in the game that set it when
//  * the event that set it switched its network sync off (2000[02] Set Network Sync State: FrpgEmkEventIns+0x50
//    = 0, checked by the sender 0xBC42D0): the game then never tells the other players about that run. The
//    Hellkite's 20 s and 60 s timers are such flags (11015305 from event 11015304, 11015315 from 11010852).
//    Sent across, the timer of the host's game, far away at Firelink, started the guest's dragon on the bridge
//    (2026-10-13 test);
//  * or the event belongs to a map our player is not in: that game judged "who stands where" from afar.
//    The host at Firelink and the guest on the bridge sent 11015306 and 11015309 (from 11015303 and 11015308)
//    back and forth until they were taken out of the sharing (2026-10-13, both tests), and every change made the
//    dragon's events re-plan its AI.
// Flags outside the 5000 block are kept shared from all events: boss kills (9, 10, 11) and arrivals
// (11020001) are set by sync-off events, and a player whose game never saw the kill still needs them.
int event_map(int id);

bool local_working_flag(int id) {
    if (!g_cur_event || id < 10000000 || (id / 1000) % 10 != 5) return false;
    uint32_t sync = 1, event_id = 0;
    if (!read32(g_cur_event + 0x50, &sync) || !read32(g_cur_event + 0x28, &event_id)) return false;
    const char* why = nullptr;
    if ((sync & 0xFF) == 0) {
        why = "does not sync its runs";
    } else {
        const int map = event_map(static_cast<int>(event_id));
        const int here = game::map_id();
        if (map != -1 && here != -1 && (static_cast<uint32_t>(here) & 0xFFFF0000) != static_cast<uint32_t>(map)) {
            why = "belongs to a map we are not in";
        }
    }
    if (!why) return false;
    static std::set<int> logged;
    if (logged.size() < 40 && logged.insert(id).second) {
        LOG_INFO("[coop] Flag %d stays in this game (event %d %s)", id, static_cast<int>(event_id), why);
    }
    return true;
}

void __stdcall before_set_flag(int id) {
    g_old_value = -1;
    if (g_flag_quiet) return;
    if (GetCurrentThreadId() != g_main_thread || !g_flag_listener || g_applying_remote) return;
    if (!seamless_rules() || !shared_flag(id) || local_working_flag(id)) return;
    g_old_value = game::flag(id) ? 1 : 0;
}

void __stdcall after_set_flag(uint32_t efm, int id, uint32_t value) {
    if (g_flag_quiet) return;
    if (!seamless_rules() || !efm) return;
    const bool on = (value & 0xFF) != 0;
    if (g_old_value >= 0 && g_old_value != (on ? 1 : 0) && GetCurrentThreadId() == g_main_thread) {
        g_old_value = -1;
        note_flag_change(id);
        if (!g_contested.count(id)) g_flag_listener(id, on);
    }
    if (!g_state.guest || host_disaster_flag(id)) return;
    uint32_t active = 0;
    if (!read32(efm + 0x10C, &active) || active != efm + 0x90) return;
    *ptr<uint32_t>(efm + 0x10C) = efm + 0x14;
    call_set_flag(efm, id, value & 0xFF);
    *ptr<uint32_t>(efm + 0x10C) = active;
    if (g_cur_event && g_mirroring_events.size() < 4096) g_mirroring_events.insert(g_cur_event);
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

// ---------------------------------------------------------------- the guest's own item and conversation flags
// EventFlagMan+0x14 is the store of the player's own world, +0x90 the store of the world they visit, +0x10C the
// active one. A store has ten groups (a flag's ten-millions digit; entries of 0xC: areas, count, bytes). The
// table at 0x1160150 gives the visited world's store no area at all in group 5 (5xxxxxxx: "already obtained"
// for item lots and treasure) or group 7 (7xxxxxxx: NPC conversations), so the block lookup (0xD38830) finds
// nothing there: every such flag reads off and every write is lost. A summoned phantom never loots or talks;
// a co-op guest does. Oscar's Estus (lot 1082) was given without being marked, so his key (lot 1080) went on
// through lots 1081 (Big Pilgrim's Key) and 1082 (five more Estus). The Asylum Demon's lot 2660 gave a second
// Big Pilgrim's Key (left on the floor) and went on to 2661, the Demon's Great Hammer that only a kill on the
// first encounter unlocks (2026-10-12 test). The lookup takes a co-op guest's group 5 and 7 blocks from their
// own world's store: their items and conversations are their own, read and kept there.
constexpr uint32_t kFlagBlockStore = 0x00D388BA;  // mov ecx,[esp+14h] ; mov ecx,[ecx+10Ch]

// ecx = the store; edi = the group. eax, edx, ebp, edi are live and kept.
__declspec(naked) void hk_flag_store() {
    __asm {
        mov ecx, [esp + 0x18]  // EventFlagMan: the caller's [esp+14h], past our return address
        cmp byte ptr [g_strict], 0
        jne active
        cmp edi, 5
        je own
        cmp edi, 7
        je own
    active:
        mov ecx, [ecx + 0x10C]
        ret
    own:
        add ecx, 0x14
        ret
    }
}

bool install_flag_store() {
    static const uint8_t k[10] = {0x8B, 0x4C, 0x24, 0x14, 0x8B, 0x89, 0x0C, 0x01, 0x00, 0x00};
    return patch_call(kFlagBlockStore, k, sizeof(k), reinterpret_cast<void*>(&hk_flag_store));
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

// ---------------------------------------------------------------- NPCs are people, not ghosts
// A map's character (made by 0xE5B620, or by 0xE66090 for those built like a player: Oscar, Solaire) becomes a
// wandering ghost (chr type 4: a phantom with NpcParam's ghostModelId, never hit) when NpcParam's
// isChangeWanderGhost (+0x145 bit 7, "a wandering ghost while the player is a client") is set and the game is a
// client (SessMan state 6) or was summoned by an NPC (PartyMemberInfo+0x7C != -1). A co-op guest met the host's
// NPCs as phantoms it could talk to but not hurt (Remastered Seamless Co-op keeps them so); it now meets them as
// they are, as in the seamless co-op of the later games. Invaders keep the vanilla rule.
constexpr uint32_t kWanderGhostSites[] = {0x00E5B90D, 0x00E6618F};  // mov al,[eax+145h] ; shr al,7
uint32_t g_npc_people = 0;         // NPCs kept as people (counted at creation)
uint32_t g_npc_people_logged = 0;

int __cdecl npc_wander_ghost() {
    // The role is set before the join's reload, so the first load into the host's world already sees it.
    if (g_state.active && g_state.guest && !g_state.invader) {
        ++g_npc_people;
        return 0;
    }
    return 1;
}

// eax = the NpcParam row; al = the bit, 0 for a co-op guest. Every other register is kept.
__declspec(naked) void hk_wander_ghost() {
    __asm {
        movzx eax, byte ptr [eax + 0x145]
        shr eax, 7
        je done
        push ecx
        push edx
        call npc_wander_ghost
        pop edx
        pop ecx
    done:
        ret
    }
}

int install_wander_ghost() {
    static const uint8_t k[9] = {0x8A, 0x80, 0x45, 0x01, 0x00, 0x00, 0xC0, 0xE8, 0x07};
    int ok = 0;
    for (uint32_t site : kWanderGhostSites) ok += patch_call(site, k, sizeof(k), reinterpret_cast<void*>(&hk_wander_ghost));
    return 2 - ok;
}

// ---------------------------------------------------------------- the souls of a kill
// An enemy's souls (0xE78A40 -> 0xE4DE90: NpcParam getSoul x soul-rate effects, x1.2 for some enemies) are halved
// when IsClient (0xD2A430: SessMan state 6, or summoned by an NPC) says the game is a summoned phantom's. A co-op
// guest got half of the host's souls for every enemy (2026-10-13 test; boss souls come from the event scripts and
// were the same). A co-op guest gets them in full, as the host does; invaders keep the vanilla rule.
constexpr uint32_t kKillSoulsClientSite = 0x00E78AC9;
constexpr uint32_t kIsClient = 0x00D2A430;

bool __cdecl hk_kill_souls_client() {
    if (g_state.active && g_state.guest && !g_state.invader) return false;
    return reinterpret_cast<bool(__cdecl*)()>(kIsClient)();
}

// ---------------------------------------------------------------- a guest's events do not see a missing character as dead
// EcTestChrHpRate (IF HP Ratio, 4[02]; vtable 0x11DD450 slot 1 = 0xBB8380, thiscall(cond, 2 args)): entity at
// +0x18, comparison +0x1C, ratio +0x20, result in bit 0 of +8. The character comes from 0xD6C360 (eax = entity);
// one that is not found counts as ratio 0.0, so "HP at most 0" passes. Vanilla never lets a summoned player rest.
// A co-op guest's rest (and the host's, which resets the guest's world too) rebuilds the area's characters, and
// 70 ms after the guest's first rest in the host's world Oscar's death event (11810520: HP ratio <= 0 -> 1060-1074
// off, 1073 on) ran in the guest's game. The flags are shared, so he was dead in the host's world too, and the
// event's next run dropped his Estus and key where he sat (2026-10-13 test). For a co-op guest an HP test does
// not pass while its character is missing or has no maximum HP, nor during a world reset or the 3 s after one.
// The host's game, where the character lives, still decides, and its flags reach the guest.
// The next test (12:51) killed him the same way with the character there and a maximum HP: his HP itself read 0
// in the guest's game 70 ms after the guest's rest, which also reset the host's world (the host owns him). Vanilla
// never resets a world during a session (bonfires are off with phantoms in it). So the death of the host's NPCs
// (team 8 Ally, 9 Hostile Ally) is the host's to see: a guest's "HP equal to / at most 0" test on one of them never
// passes. The host's own event sees a kill the guest makes (the damage reaches the host's character) and its flags
// come back; the guest's hostility tests (HP at most 90 % and hit by the guest) still pass.
constexpr uint32_t kHpRateVtable = 0x011DD450;
constexpr uint32_t kHpRateTest = 0x00BB8380;
constexpr uint32_t kChrByEntity = 0x00D6C360;  // eax = entity id -> character, or 0
constexpr uint32_t kWorldChrManAddr = 0x0137D644;
constexpr uint32_t kWorldResetField = 0x2CC;     // WorldChrMan: 0 idle, 1..5 a rest's world reset (0xE40D50)
using CondTest = void(__thiscall*)(uint8_t*, int, int);
uint32_t g_hp_rate_orig = 0;
// The host's NPCs that the host's own report killed in our game (its HP sync, or its list of the dead; see
// set_hp_value). Their death test passes: the host's game has seen the death, and our copy of the death event
// may finish as the host's did (Oscar's 11810520, whose end turns him into his corpse on later loads). Cleared
// on every load (the characters are freed).
std::set<uint32_t> g_host_killed;
ULONGLONG g_world_reset_at = 0;

uint32_t chr_by_entity(int entity) {
    uint32_t chr = 0;
    __try {
        __asm {
            mov eax, entity
            mov edx, kChrByEntity
            call edx
            mov chr, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        chr = 0;
    }
    return chr;
}

// True during a world reset and for 3 s after it (called every frame from tick, and by the HP test).
bool world_resetting() {
    uint32_t wcm = 0, state = 0;
    if (read32(kWorldChrManAddr, &wcm) && wcm && read32(wcm + kWorldResetField, &state) && state != 0) {
        g_world_reset_at = GetTickCount64();
        return true;
    }
    return g_world_reset_at && GetTickCount64() - g_world_reset_at < 3000;
}

void __fastcall hk_hp_rate_test(uint8_t* cond, void*, int a, int b) {
    reinterpret_cast<CondTest>(g_hp_rate_orig)(cond, a, b);
    if (!(cond[8] & 1) || !g_state.active || !g_state.guest || g_state.invader) return;
    const int entity = *reinterpret_cast<int*>(cond + 0x18);
    const uint32_t chr = chr_by_entity(entity);
    uint32_t hp = 0, max_hp = 0, team = 0;
    const bool missing = !chr || !read32(chr + 0x2D8, &max_hp) || static_cast<int>(max_hp) <= 0;
    if (chr) {
        read32(chr + 0x2D4, &hp);
        read32(chr + 0x74, &team);  // the team (see kTeamOffset)
    }
    const uint8_t cmp = cond[0x1C];
    const float ratio = *reinterpret_cast<float*>(cond + 0x20);
    const bool death_test = ratio <= 0.0f && (cmp == 0 || cmp == 5);
    const bool npc = !missing && (team == 8 || team == 9);
    const bool resetting = world_resetting();
    if (npc && death_test && g_host_killed.count(chr)) {
        static std::set<int> told;
        if (told.size() < 20 && told.insert(entity).second) {
            LOG_INFO("[coop] HP test on character %d passes: the host's game reported its death", entity);
        }
        return;
    }
    const char* why = missing ? "the character is not here"
                      : npc && death_test ? "the host's game decides its NPCs' deaths"
                      : resetting ? "the world is resetting"
                                  : nullptr;
    if (!why) return;
    cond[8] &= 0xFE;
    static std::set<std::pair<int, const char*>> told;  // once per character and reason (the test runs every frame)
    if (told.size() < 30 && told.insert({entity, why}).second) {
        LOG_INFO("[coop] HP test on character %d held back: %s (HP %d/%d, team %u)", entity, why,
                 static_cast<int>(hp), static_cast<int>(max_hp), team);
    }
}

// ---------------------------------------------------------------- the host's NPCs live through a guest's rest
// and through the host's death
// Every HP change goes through 0xE688E0 (stdcall(chr, hp)): clamped to 0..max into +0x2D4, then at 0 the
// character's own "cannot die" rules (+0x1FF bit 1, its param, SpEffect state 0x8F) keep 1. The SpEffects' per
// second change (0xE72E20) dies there too when it reaches 0. Oscar sits with SpEffect 5492 (99.9 % of his HP
// taken, from event 11815010, which restarts on every rest). In the 15:42 test (2026-10-13) the guest healed him
// before its rest as before (heal_host_npcs), yet his HP read 0 78 ms after it, 40 ms after the host's world reset
// that the guest's rest set off, and he stayed pushable and talkable with 0 HP held by the HP test until the
// rest's own heal: a second 5492 (or the host's) landed after the heal. A death in the guest's game during that
// window is never a death the host sees. So while a guest's world resets and for 5 s after, a host NPC (team 8
// or 9) that is alive keeps 1 HP instead of reaching 0 in the guest's game. The rest's heal then fills it up.
// The host's death killed him the same way (2026-10-16 20:53): the host's world reloaded after its death, and a
// second later his HP read 0 in the guest's game (death animation, his items dropped), while the host's Oscar lived
// and still talked to both. The session host is the one whose HP counts: 0xE40FE0 sends each character's HP when it
// changes (WorldChrMan+0x2B4, a u16 per net handle, 0xF987 = none) and a guest's game sets it (SetHP returning to
// kRetHpSync). So in a guest's game, with the host in the same map, a host NPC the host still reports alive does
// not die of anything else; the host's report of its death (or the host's list of its dead) still kills it.
constexpr uint32_t kSetHp = 0x00E688E0;
constexpr uint32_t kRetHpSync = 0x00E4127B;        // 0xE40FE0: a guest's game takes the host's HP report
constexpr uint32_t kRetHostDeadList = 0x00E441FC;  // 0xE44150: the host's list of its dead characters (kill)
constexpr uint32_t kHpTableField = 0x2B4;          // WorldChrMan: the HP report table
constexpr uint32_t kHpNone = 0xF987;
void* g_set_hp_orig = nullptr;
int g_hp_saves = 0;
int g_hp_kept = 0;

// The host's last HP report for a character, -1 when there is none. Read in place: 0xE551C0 would take the
// table's "new" mark, which the game's own sync waits for.
int host_reported_hp(uint32_t chr) {
    uint32_t handle = 0, wcm = 0, table = 0, n1 = 0, page = 0, n2 = 0, entries = 0, word = 0;
    if (!read32(chr + 0x78, &handle) || static_cast<int>(handle) < 0) return -1;
    if (!read32(kWorldChrManAddr, &wcm) || !wcm || !read32(wcm + kHpTableField, &table) || !table) return -1;
    const uint32_t i1 = (handle >> 11) & 0x1F, i2 = handle & 0x7FF;
    if (!read32(table + 4, &n1) || static_cast<int>(i1) >= static_cast<int>(n1)) return -1;
    if (!read32(table + 8 + i1 * 4, &page) || !page) return -1;
    if (!read32(page + 8, &n2) || static_cast<int>(i2) >= static_cast<int>(n2)) return -1;
    if (!read32(page + 0x18, &entries) || !entries || !read32(entries + i2 * 4, &word)) return -1;
    const uint32_t hp = word & 0xFFFF;
    return hp == kHpNone ? -1 : static_cast<int>(hp);
}

// The host stands in the map we are in (its game has the same characters), as it last said.
bool host_in_our_map() {
    const int here = game::map_id();
    return here != -1 && g_state.host_map != -1 &&
           (static_cast<uint32_t>(here) & 0xFFFF0000u) == (static_cast<uint32_t>(g_state.host_map) & 0xFFFF0000u);
}

bool reset_window(ULONGLONG ms) {
    uint32_t wcm = 0, state = 0;
    if (read32(kWorldChrManAddr, &wcm) && wcm && read32(wcm + kWorldResetField, &state) && state != 0) return true;
    return g_world_reset_at && GetTickCount64() - g_world_reset_at < ms;
}

int __stdcall set_hp_value(uint32_t chr, int hp, uint32_t ret) {
    if (hp > 0 || !chr || !g_state.active || !g_state.guest || g_state.invader) return hp;
    uint32_t cur = 0, team = 0, max_hp = 0;
    if (!read32(chr + 0x2D4, &cur) || static_cast<int>(cur) <= 0 || !read32(chr + 0x74, &team) ||
        (team != 8 && team != 9) || !read32(chr + 0x2D8, &max_hp) || static_cast<int>(max_hp) <= 0) {
        return hp;
    }
    if (reset_window(5000)) {
        if (++g_hp_saves <= 20) {
            LOG_INFO("[coop] The host's NPC %08X (HP %d/%d, team %u) is kept alive during our world reset (the host's "
                     "game decides its NPCs' deaths)", chr, static_cast<int>(cur), static_cast<int>(max_hp), team);
        }
        return 1;
    }
    if (ret == kRetHpSync || ret == kRetHostDeadList) {
        g_host_killed.insert(chr);  // alive until now: the host's own report kills it here
        return hp;
    }
    if (!host_in_our_map()) return hp;
    // No report at all: the host's game is loading and has not sent this character's HP yet, and it still decides
    // (its report of the death comes through the sync above).
    const int reported = host_reported_hp(chr);
    if (reported == 0) return hp;
    if (++g_hp_kept <= 20) {
        if (reported > 0) {
            LOG_INFO("[coop] The host's NPC %08X (HP %d/%d, team %u) stays alive here: the host's game reports it "
                     "alive (HP %d; from %08X)", chr, static_cast<int>(cur), static_cast<int>(max_hp), team, reported,
                     ret);
        } else {
            LOG_INFO("[coop] The host's NPC %08X (HP %d/%d, team %u) stays alive here: the host's game has not "
                     "reported its HP (loading?; from %08X)", chr, static_cast<int>(cur), static_cast<int>(max_hp),
                     team, ret);
        }
    }
    return 1;
}

// The direct kill (0xE6B4E0, stdcall(chr): HP 0, death flags, the death event 0xDA0770) that Force Death, a kill
// for treasure and the "host's dead characters" list (0xE44150) use skips SetHP; the same window holds it back.
constexpr uint32_t kKillChr = 0x00E6B4E0;
void* g_kill_orig = nullptr;

bool __stdcall kill_allowed(uint32_t chr, uint32_t ret) { return set_hp_value(chr, 0, ret) == 0; }

// Our stack: [esp] = return address, [esp+4] = chr. ecx and edx are kept for the original.
__declspec(naked) void hk_kill_chr() {
    __asm {
        push ecx
        push edx
        push dword ptr [esp + 8]    // the return address
        push dword ptr [esp + 0x10] // chr (one push later)
        call kill_allowed
        pop edx
        pop ecx
        test al, al
        jz held
        jmp dword ptr [g_kill_orig]
    held:
        ret 4
    }
}

// ---------------------------------------------------------------- a finished event is finished at home too
// When a map event ends (not restarts), the event's finish (FrpgEmkEventIns vt+0x14, 0x528C50) marks it done in
// 0xBBEEB0 (ecx = the event): the flag "event id + slot" is set straight in the active store, not through the
// flag setter, so neither the flag sharing nor the progress mirror (after_set_flag) ever sees it. Events test that
// flag on later loads ("SKIP IF Event ID And Slot Number"): Oscar's death event 11810520 turns him into his corpse
// with his loot once it is done. A co-op guest who saw Oscar die in the host's world brought his death state home
// (1073, mirrored) but not the end of the event, so at home Oscar sat in his cell alive with nothing left to say
// (2026-10-16 tests). A map event (id 10000000 and up; common events work from each player's own state) finished
// in the host's world in a guest's game is now marked done in the guest's own world as well, when its run there
// mirrored flags home: its work reached the guest's world, so its end does too. An event that only ended (many
// start with "END IF this event is done" or "END IF the boss is dead", which the host's world can answer
// differently from the guest's) changed nothing at home and stays undone there.
constexpr uint32_t kEventDone = 0x00BBEEB0;
void* g_event_done_orig = nullptr;
int g_done_lines = 0;

void mirror_event_done(uint32_t ev) {
    if (!g_state.guest || g_state.invader || !seamless_rules() || !ev) return;
    uint32_t id = 0, slot_word = 0;
    if (!read32(ev + 0x28, &id) || !read32(ev + 0x2C, &slot_word)) return;
    const int slot = static_cast<int16_t>(slot_word & 0xFFFF);
    if (static_cast<int>(id) < 10000000 || slot < 0) return;
    if (!g_mirroring_events.erase(ev)) return;
    const uint32_t efm = game::event_flag_man();
    uint32_t active = 0;
    if (!efm || !read32(efm + 0x10C, &active) || active != efm + 0x90) return;
    const int flag = static_cast<int>(id) + slot;
    ++g_flag_quiet;
    *ptr<uint32_t>(efm + 0x10C) = efm + 0x14;
    call_set_flag(efm, flag, 1);
    *ptr<uint32_t>(efm + 0x10C) = active;
    --g_flag_quiet;
    if (++g_done_lines <= 40) {
        LOG_INFO("[coop] Event %d (slot %d) finished in the host's world: done in our own world too", id, slot);
    }
}

void __fastcall hk_event_done(uint32_t ev) {
    reinterpret_cast<void(__fastcall*)(uint32_t)>(g_event_done_orig)(ev);
    mirror_event_done(ev);
}

// ---------------------------------------------------------------- why a throw did not happen (diagnostic)
// A hit that can start a throw (a backstab, a riposte, a plunging attack) asks 0xE2E8A0 (eax = the character hit,
// esi = the attacker's action controller, chr+0x28 -> +0x2C, ecx = the hit's throw id) whether a ThrowParam row
// allows it; -1 means none did, and the hit lands as plain damage. On 2026-10-16 a co-op guest's plunging attack
// on the Asylum Demon did its damage without the throw while the host's took. The rows' checks (0xE2D640) read the
// characters' throw masks from their effects' states (0xDC0280: state 1 none, 2 and 3 more), the target's "can be
// thrown" animation id (action+0x1E8, 0xFF none), its busy bits (action+0x84 bits 14 and 15), both throw nodes
// (chr+0x330, state +0x10) and the chr+0x202 bit 4 of either side, then the two positions. A failed attempt by our
// own character is logged with all of these.
constexpr uint32_t kThrowTry = 0x00E2E8A0;
void* g_throw_try_orig = nullptr;
int g_throw_lines = 0;

bool has_effect_state(uint32_t chr, int state) {
    uint32_t mgr = 0, e = 0;
    if (!read32(chr + 0x1E0, &mgr) || !mgr || !read32(mgr + 4, &e)) return false;
    for (int guard = 0; e && guard < 128; ++guard) {
        uint32_t flags = 0, row = 0;
        uint8_t st = 0;
        if (!read32(e + 0x14, &flags)) return false;
        if (!(flags & 0x800C0003) && read32(e + 0x2C, &row) && row && read8(row + 0x15A, &st) && st == state) return true;
        if (!read32(e + 0x30, &e)) return false;
    }
    return false;
}

bool chr_xyz(uint32_t chr, float out[3]) {
    uint32_t ctrl = 0, phys = 0;
    if (!read32(chr + 0x28, &ctrl) || !ctrl || !read32(ctrl + 0x1C, &phys) || !phys) return false;
    for (int i = 0; i < 3; ++i) {
        uint32_t v = 0;
        if (!read32(phys + 0x10 + i * 4, &v)) return false;
        std::memcpy(&out[i], &v, 4);
    }
    return true;
}

void __stdcall throw_failed(uint32_t target, uint32_t action, uint32_t throw_id) {
    if (g_throw_lines >= 30 || !g_state.active) return;
    uint32_t a1 = 0, attacker = 0;
    if (!read32(action + 0x10, &a1) || !a1 || !read32(a1 + 0x10, &attacker) || attacker != game::player()) return;
    ++g_throw_lines;
    auto u32 = [](uint32_t at) {
        uint32_t v = 0;
        read32(at, &v);
        return v;
    };
    uint32_t t_action = 0, t_ctrl = 0;
    if (read32(target + 0x28, &t_ctrl) && t_ctrl) read32(t_ctrl + 0x2C, &t_action);
    const uint32_t t_def = t_action ? u32(t_action + 0x1E8) : 0xFFFFFFFF;
    const uint32_t t_busy = t_action ? (u32(t_action + 0x84) >> 14) & 3 : 0;
    const uint32_t a_atk = u32(action + 0x1E4);
    const uint32_t t_node = u32(target + 0x330), a_node = u32(attacker + 0x330);
    uint8_t t_state = 0xFF, a_state = 0xFF, t_202 = 0, a_202 = 0;
    if (t_node) read8(t_node + 0x10, &t_state);
    if (a_node) read8(a_node + 0x10, &a_state);
    read8(target + 0x202, &t_202);
    read8(attacker + 0x202, &a_202);
    const uint32_t current = u32(target + 0x2C), own_ai = u32(target + 0x410), net_ai = u32(target + 0x414);
    const char* control = current && current == own_ai ? "this game" : current && current == net_ai ? "another game" : "?";
    float tp[3] = {}, ap[3] = {};
    const bool pos = chr_xyz(target, tp) && chr_xyz(attacker, ap);
    const float dx = ap[0] - tp[0], dz = ap[2] - tp[2];
    LOG_INFO("[coop] Our throw attempt (throw id %u) on character %d found no throw: target moved by %s, can-be-thrown "
             "id %u (255 none), busy bits %u, throw node %u, 0x202 %02X, states 1/2/3 %d%d%d; ours: throw id %u, node %u, "
             "0x202 %02X, states 1/2/3 %d%d%d; we are %.2f m above it, %.2f m away%s",
             throw_id & 0xFFFF, static_cast<int>(u32(target + 0x6C)), control, t_def, t_busy, t_state, t_202,
             has_effect_state(target, 1), has_effect_state(target, 2), has_effect_state(target, 3), a_atk, a_state, a_202,
             has_effect_state(attacker, 1), has_effect_state(attacker, 2), has_effect_state(attacker, 3),
             pos ? ap[1] - tp[1] : 0.0f, pos ? std::sqrt(dx * dx + dz * dz) : 0.0f, pos ? "" : " (positions unknown)");
}

// eax, ecx and esi are the attempt's inputs; eax returns its answer.
__declspec(naked) void hk_throw_try() {
    __asm {
        push esi
        push ecx
        push eax
        call dword ptr [g_throw_try_orig]
        cmp eax, -1
        jne done
        pushad                       // [esp+32] = eax in, [esp+36] = ecx in, [esp+40] = esi in
        push dword ptr [esp + 36]    // throw id
        push dword ptr [esp + 44]    // action (one push later)
        push dword ptr [esp + 40]    // target (two pushes later)
        call throw_failed
        popad
    done:
        add esp, 12
        ret
    }
}

// Our stack: [esp] = return address, [esp+4] = chr, [esp+8] = hp. ecx and edx are kept for the original.
__declspec(naked) void hk_set_hp() {
    __asm {
        push ecx
        push edx
        push dword ptr [esp + 8]     // the return address
        push dword ptr [esp + 0x14]  // hp (one push later)
        push dword ptr [esp + 0x14]  // chr (two pushes later)
        call set_hp_value
        mov [esp + 0x10], eax        // the hp argument
        pop edx
        pop ecx
        jmp dword ptr [g_set_hp_orig]
    }
}

// ---------------------------------------------------------------- a respawn point set by the world's events
// Set Player Respawn Point (EMEVD 2003[23], case at 0xBC20B2) writes GameMan+0xB04. A co-op guest's respawn is
// the mod's own (session::guest_respawn_bonfire, written back on every load), and it followed only the guest's
// rests: after the crow's flight to Firelink Shrine (event 11810000 sets 1022960) a guest who died there came
// back in the Undead Asylum (2026-10-13 test). The event's point becomes the guest's respawn.
constexpr uint32_t kRespawnPointSite = 0x00BC20BB;  // mov ecx,[013784A0h] ; mov [ecx+0B04h],eax
constexpr uint32_t kGameManAddr = 0x013784A0;

void __stdcall note_event_respawn(int point) {
    if (!seamless_rules() || !g_state.guest || point < 0) return;
    if (point == session::guest_respawn_bonfire()) return;
    session::note_guest_respawn(point);
    LOG_INFO("[coop] The world's events set our respawn to %d", point);
}

// eax = the point. ecx ends as GameMan, as the original leaves it.
__declspec(naked) void hk_respawn_point() {
    __asm {
        mov ecx, kGameManAddr
        mov ecx, [ecx]
        mov [ecx + 0xB04], eax
        pushad
        push eax
        call note_event_respawn
        popad
        ret
    }
}

bool install_respawn_point() {
    static const uint8_t k[12] = {0x8B, 0x0D, 0xA0, 0x84, 0x37, 0x01, 0x89, 0x81, 0x04, 0x0B, 0x00, 0x00};
    return patch_call(kRespawnPointSite, k, sizeof(k), reinterpret_cast<void*>(&hk_respawn_point));
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

// The walls are map objects that stand by default (entity area * 100000 + block * 10000 + 1974..1998, their SFX at
// id + 1). Each map's event 0 takes them down unless the game is a client ("SKIP IF Multiplayer State Client"; m10_01
// skips 22 lines), and the host's OnSessionJustIn raises them again. A co-op guest's events see no client
// (hk_multiplay_test) and take them down too; an invader's game keeps the vanilla answer, so for the invader alone
// the walls stood, and the host and guests could stay behind one the invader could not pass (2026-10-16, Undead
// Burg). In some maps that event-0 test also guards host-only flags (401, 404, 405, the Asylum's item events), so
// the answer stays as it is: the invader's game takes down every wall standing in a loaded map itself, as
// Lua_InvalidMultiWall does (collision off, drawing off, InvalidSfx(id + 1, true)).
struct MapWalls {
    int map;    // area << 24 | block << 16
    int count;  // walls the map's event 0 takes down (the first of kWallOffsets)
};
constexpr MapWalls kMapWalls[] = {{0x0A000000, 2}, {0x0A010000, 11}, {0x0C000000, 5}, {0x0C010000, 5},
                                  {0x0D000000, 1}, {0x0D010000, 1},  {0x0D020000, 1}, {0x0E000000, 3},
                                  {0x0E010000, 7}, {0x0F000000, 1},  {0x0F010000, 1}, {0x10000000, 3},
                                  {0x11000000, 2}, {0x12000000, 1}};
constexpr int kWallOffsets[] = {1994, 1996, 1998, 1988, 1986, 1984, 1982, 1980, 1978, 1976, 1974};  // the Lua order
constexpr uint32_t kObjByEntity = 0x00D6B830;   // eax = entity -> map object or 0 (ecx, edx clobbered)
constexpr uint32_t kSetColiEnable = 0x00D60090;  // stdcall(entity, on): bit 0x04 of object+0x2E
constexpr uint32_t kSetDrawEnable = 0x00D600B0;  // stdcall(entity, on)
constexpr uint32_t kInvalidSfx = 0x00D5F780;     // stdcall(entity, flag)
ULONGLONG g_walls_at = 0;
int g_walls_lines = 0;

uint32_t obj_by_entity(int entity) {
    uint32_t obj = 0;
    __try {
        __asm {
            mov eax, entity
            mov edx, kObjByEntity
            call edx
            mov obj, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        obj = 0;
    }
    return obj;
}

bool lower_wall(int id) {
    __try {
        reinterpret_cast<bool(__stdcall*)(int, int)>(kSetColiEnable)(id, 0);
        reinterpret_cast<bool(__stdcall*)(int, int)>(kSetDrawEnable)(id, 0);
        reinterpret_cast<bool(__stdcall*)(int, int)>(kInvalidSfx)(id + 1, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Once a second while we invade: the standing walls of every loaded map come down.
void invader_walls_tick() {
    if (!g_state.active || !g_state.invader || g_state.leaving || game::loading() || !game::in_game() ||
        game::own_world()) {
        return;
    }
    const ULONGLONG t = GetTickCount64();
    if (t - g_walls_at < 1000) return;
    g_walls_at = t;
    uint32_t lua = 0;
    if (!read32(game::kLuaEventsVar, &lua) || !lua) return;  // the natives need the event system
    int lowered = 0, first = 0;
    for (const MapWalls& m : kMapWalls) {
        const int area = (m.map >> 24) & 0xFF, block = (m.map >> 16) & 0xFF;
        for (int i = 0; i < m.count; ++i) {
            const int id = area * 100000 + block * 10000 + kWallOffsets[i];
            const uint32_t obj = obj_by_entity(id);
            uint8_t flags = 0;
            if (!obj || !read8(obj + 0x2E, &flags) || !(flags & 0x04)) continue;
            if (lower_wall(id)) {
                if (!lowered) first = id;
                ++lowered;
            }
        }
    }
    if (lowered && g_walls_lines < 20) {
        ++g_walls_lines;
        LOG_INFO("[coop] Multiplayer walls taken down for us (invader): %d (first %d)", lowered, first);
    }
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

// The world rules make another player hostile for fighting only (friendly fire): checks that are not about
// hitting someone run with the teams the game gave the characters (see teams_neutral).
int g_neutral = 0;

int __stdcall team_override(uint32_t chr) {
    if (!g_state.active || g_state.leaving || !chr || g_neutral) return -1;
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

// Friendly fire (Ominous Tome, world rules 1) puts allies on team 13 so they can hurt each other. Lock-on and the
// bonfire's "an enemy is here" check are not part of that: they run with the teams the game gave the characters.
// Teams mode (world rules 2) keeps team 13 for those checks, so opponents lock on and a nearby opponent blocks a rest.
bool friendly_fire_only() { return g_state.active && g_state.world_rules == 1; }

void __cdecl teams_neutral() {
    if (g_neutral++ != 0) return;
    if (!friendly_fire_only()) return;
    for (const auto& [chr, saved] : g_team_saved) {
        if (wanted_team(chr) == kTeamEveryone) write_team(chr, saved);
    }
}

void __cdecl teams_restore() {
    if (g_neutral <= 0 || --g_neutral != 0) return;
    if (!friendly_fire_only()) return;
    for (const auto& [chr, saved] : g_team_saved) {
        const int want = wanted_team(chr);
        if (want == kTeamEveryone) write_team(chr, want);
    }
}

// Lock-on (0xDBACD0, stdcall(LockTgtMan, dt, ?), called every frame at 0xE4025B and 0xE41BD9) keeps the
// lockable points whose character is hostile to the player. Friendly fire (the Ominous Tome) lets allies hurt each
// other but not lock on to each other, so the lock-on runs with the teams the game gave them.
constexpr uint32_t kLockOn = 0x00DBACD0;
constexpr uint32_t kLockOnSites[] = {0x00E4025B, 0x00E41BD9};

__declspec(naked) void hk_lock_on() {
    __asm {
        pushad
        call teams_neutral
        popad
        push dword ptr [esp + 12]
        push dword ptr [esp + 12]
        push dword ptr [esp + 12]
        mov eax, kLockOn
        call eax
        pushad
        call teams_restore
        popad
        ret 12
    }
}

// The bonfire refuses a rest while a character hostile to players is about and fighting (0xE3DF90, thiscall
// (WorldChrMan, block), called from the bonfire object at 0xD58B49: a "yes" offers the bonfire's "cannot use"
// prompt instead of the rest, as HasPartyMembers does). A player made hostile by the world rules does not count, so
// a companion does not lock the bonfire. A living co-op teammate in a boss fight does: a rest resets every
// player's world, the boss's and its arena's too in the middle of that fight, while a boss fight is won or lost by
// the team together (spectate.cpp). A teammate who fell and spectates does not count, so a rest still ends a
// fight nobody is left to finish.
constexpr uint32_t kBonfireFoes = 0x00E3DF90;
constexpr uint32_t kBonfireFoesSite = 0x00D58B49;

bool __cdecl teammate_boss_fight() {
    return seamless_rules() && !g_state.invader && g_state.teammate_boss_fight;
}

__declspec(naked) void hk_bonfire_foes() {
    __asm {
        pushad
        call teams_neutral
        popad
        push dword ptr [esp + 4]
        mov eax, kBonfireFoes
        call eax
        pushad
        call teams_restore
        popad
        test al, al
        jnz done
        push ecx
        push edx
        call teammate_boss_fight
        pop edx
        pop ecx
    done:
        ret 4
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

// The disasters themselves are the maps' events. In 14 maps one event waits for black tendency over 50 for six
// frames in a row and then sets the map's disaster flag (e.g. 11010050, the Undead Burg's 13 black phantoms),
// which another event turns into enabled black phantoms; after 600 s it ends the disaster only if the
// tendency is no longer black (flag 1xx15075). A third event ends it at once when 1xx15075 or the global flag
// 735 is on: it clears 735, the disaster flag and 1xx15075, and kills the phantoms. Ash Lake and the Undead
// Asylum have one phantom each, enabled while black and killed when the tendency drops, behind a flag that
// stays on. The disaster flags are saved like any other, so a map that was cursed and is not loaded would bring
// its phantoms back for another 600 s on the next visit. Lifting the curse therefore clears every map's disaster
// flag, and sets 735 when the current map has the event that ends its disaster at once. A guest's game copies the
// flags set in the host's world into the guest's own world (after_set_flag), but not these: a disaster of the host's
// world, from the curse or the host's own black tendency, would otherwise follow the guest home and bring the
// phantoms there for 600 s.
constexpr int kDisasterFlags[] = {11000050, 11010050, 11100050, 11200050, 11210080, 11300050, 11310060, 11325090,
                                  11400080, 11410050, 11500050, 11510050, 11600050, 11700050, 11800050, 11815090};

bool disaster_flag(int id) {
    return std::find(std::begin(kDisasterFlags), std::end(kDisasterFlags), id) != std::end(kDisasterFlags);
}

constexpr int kDisasterEnd = 735;
// Maps (area << 24 | block << 16) whose events end the disaster when flag 735 is on.
constexpr int kDisasterEndMaps[] = {0x0A000000, 0x0A010000, 0x0B000000, 0x0C000000, 0x0C010000,
                                    0x0D000000, 0x0D010000, 0x0E000000, 0x0E010000, 0x0F000000,
                                    0x0F010000, 0x10000000, 0x11000000, 0x12000000};

// The rest of a disaster stays in the host's world as well: each map's "ended" flag (1xx15075 above; the id
// differs per map), 735, and 5000 / 5001, which the events set when a disaster starts and ends. The game shows
// "Disasters are gone after the defeat of the Gravelord Servant" whenever 5001 is on (0xD78E85), then clears
// 4300, 5000 and 5001 straight in the active store (0xD60190, not through the flag setter). Copied into a guest's
// own world, 5001 was never cleared there and showed that notice at home; lifting a host's curse (735 -> the end
// event -> 5001) did it every time. A 735 copied home and not cleared by the end event here (it had already run on
// this load) ended the guest's own next disaster at once.
constexpr int kDisasterEndFlags[] = {11005085, 11015075, 11105075, 11205085, 11215095, 11305069, 11315045,
                                     11405095, 11415095, 11505095, 11515045, 11605095, 11705085, 11805095};

bool host_disaster_flag(int id) {
    return disaster_flag(id) || id == kDisasterEnd || id == 5000 || id == 5001 ||
           std::find(std::begin(kDisasterEndFlags), std::end(kDisasterEndFlags), id) != std::end(kDisasterEndFlags);
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

// A run of a map event that another player's game started reaches this game as an event sync (FrpgEmkEventIns
// slot 4, 0xBC4000: thiscall(event, connection, type, data, size), ret 0x10). Types 1 and 2 move the event's
// program counter (+0x20 points at it) past the condition that passed there, and set its player (+0x44 = 10001 +
// that player's slot, so "the player" 10000 of the run is them) and +0x38. In the game's co-op only the host's
// game runs most map events, so what arrives is the world's own state. In seamless co-op every game runs them,
// also for maps its player is not in: the host standing at Firelink still ran the Undead Burg's Hellkite events,
// judged their "every player outside the bridge" tests from there, and those runs reached the guest on the
// bridge, whose dragon then played the host's moves on top of its own (2026-10-13 test, 19:24-19:27). A run
// counts for the other games only if the player whose game ran it stands in that event's map; any other run is
// put back here and this game's copy of the event goes on from where it was. Common events (ids below
// 10000000) belong to no map and always count, as does a run whose player's map is not known.
constexpr uint32_t kEmkEventVtable = 0x011DCDA8;
constexpr uint32_t kEventSync = 0x00BC4000;
using EventSyncFn = void(__fastcall*)(uint8_t*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t g_event_sync_orig = 0;

// The map of a map event: ids 1AABxxxx are area AA, block B (11015302: m10_01, 0A010000). -1 for common events.
int event_map(int id) {
    if (id < 10000000 || id >= 20000000) return -1;
    return ((id / 100000) % 100) << 24 | ((id / 10000) % 10) << 16;
}

// The map last reported by the player whose character is "the player" entity of a run (10001 + slot), -1 unknown.
int remote_player_map(int entity) {
    const int slot = entity - 10001;
    for (const RemotePlayer& r : g_remote) {
        uint32_t s = 0;
        if (r.known && r.chr && read32(r.chr + 0x410, &s) && static_cast<int>(s) == slot) return r.map;
    }
    return -1;
}

void __fastcall hk_event_sync(uint8_t* ev, void* edx, uint32_t conn, uint32_t type, uint32_t data, uint32_t size) {
    const uint32_t e = reinterpret_cast<uint32_t>(ev);
    uint32_t pc = 0, who = 0, seq = 0;
    const bool saved = ev && read32(e + 0x20, &pc) && pc && read32(e + 0x44, &who) && read32(e + 0x38, &seq);
    const uint16_t at = saved ? *ptr<uint16_t>(pc) : 0;
    reinterpret_cast<EventSyncFn>(g_event_sync_orig)(ev, edx, conn, type, data, size);
    if (!saved || !g_world_events || !seamless_rules()) return;
    const int by = *ptr<int>(e + 0x44);
    if (by < 10001 || by > 10010) return;
    if (static_cast<uint32_t>(by) == who && *ptr<uint16_t>(pc) == at) return;  // the packet changed nothing
    const int id = *ptr<int>(e + 0x28);
    const int map = event_map(id);
    if (map == -1) return;
    const int theirs = remote_player_map(by);
    if (theirs == -1) {
        // remote_player_map compares PlayerIns+0x410 with the run's slot, but +0x410 holds a player id in
        // 10000000..10000039 (PlayerIns vt+0xA8, 0xE46E80), so it never matches and every run is taken (2026-10-17
        // review). Logged a few times to learn how the run's player (10001 + slot) maps to those ids.
        static int told = 0;
        if (told < 5) {
            ++told;
            char ids[160] = "";
            size_t used = 0;
            for (const RemotePlayer& r : g_remote) {
                uint32_t pid = 0;
                if (!r.known || !r.chr || !read32(r.chr + 0x410, &pid) || used + 32 >= sizeof(ids)) continue;
                used += std::snprintf(ids + used, sizeof(ids) - used, "%s%u (map %08X)", used ? ", " : "", pid, r.map);
            }
            LOG_INFO("[coop] Event %d: a run from player %d taken (map not known); remote player ids: %s", id, by,
                     used ? ids : "none");
        }
        return;
    }
    if ((static_cast<uint32_t>(theirs) & 0xFFFF0000) == static_cast<uint32_t>(map)) return;
    *ptr<uint16_t>(pc) = at;
    *ptr<uint32_t>(e + 0x44) = who;
    *ptr<uint32_t>(e + 0x38) = seq;
    static std::map<int, int> counts;
    const int n = ++counts[id];
    if (counts.size() <= 40 && (n == 1 || n == 10 || n == 100)) {
        LOG_INFO("[coop] Event %d: a run from a player in map %08X is not taken here (the event is map %08X's; %d so far)",
                 id, theirs, map, n);
    }
}

// Randomly Set Event Flag In Range (2003[17]: 0xD6C9C0 called from 0xBC1FA6, stdcall(proxy, on), eax = last
// flag, edi = first, esi = the event). In the game's co-op it runs on the host only (a "SKIP IF client" before
// it), so one game picks and the pick reaches the others with the run's other flags. In seamless co-op the run
// that one game started ran again in the other game (event sync) and picked again: the guest's game picked
// 11015322 for the Hellkite, the host's copy of the same run picked 11015333, both flags reached both games and
// two attack events fired at once (2026-10-13, 19:27:00). A run that another player's game started (its player
// is that player, not 10000) leaves the pick to that game, whose flag arrives here.
constexpr uint32_t kRandomFlagSite = 0x00BC1FA6;
constexpr uint32_t kRandomFlag = 0x00D6C9C0;
uint32_t g_random_flag = kRandomFlag;

bool __stdcall random_pick_here(uint32_t ev, int first, int last) {
    if (!g_world_events || !seamless_rules() || !ev) return true;
    uint32_t by = 10000, id = 0;
    if (!read32(ev + 0x44, &by) || static_cast<int>(by) < 10001 || by > 10010) return true;
    read32(ev + 0x28, &id);
    static int lines = 0;
    if (++lines <= 30) {
        LOG_INFO("[coop] Event %d: the random pick among flags %d-%d is left to the game that started this run",
                 static_cast<int>(id), first, last);
    }
    return false;
}

__declspec(naked) void hk_random_flag() {
    __asm {
        pushad
        push eax  // last flag
        push edi  // first flag
        push esi  // the event
        call random_pick_here
        test al, al
        popad
        jz skip
        jmp dword ptr [g_random_flag]
    skip:
        ret 8
    }
}

// ---------------------------------------------------------------- a cutscene that leaves the map takes everyone
// Play Cutscene and Warp Player (EMEVD 2002[04], case 0xBC0356) plays a cutscene for one player and moves them;
// every use in PTDE names "the player" (10000) of the event, the one whose game ran it. When another player's game
// ran the event first, the network hands it on (FrpgEmkEventIns, 0xBC4000) with "the player" set to that
// player's character (type 1: 10001 + its slot) or to whatever entity that game sent (type 2), and from the
// instruction that game had reached. The Undead Asylum's crow (event 11810000) took the guest to Firelink Shrine
// and left the host behind twice: the host's game marked the Asylum as left and moved its respawn, but never
// warped (2026-10-12 and 2026-10-13 tests; the redirect below never ran on the host). So a cutscene that moves
// our own player into another map is announced: once we are there, every other player is warped after us
// (session, kCutFollow). One set off by another player's character is still turned onto ours. A warp inside the
// same map (the Seath capture, Anor Londo) moves only the player who set it off.
constexpr uint32_t kCutWarpSite = 0x00BC0378;  // cmp eax,2710h ; jne +3 ; mov eax,[esi+44h]
constexpr int kLocalPlayerEntity = 10000;
void (*g_cutscene_fn)(int map, int point) = nullptr;

// The request itself (0xD75190, eax = player entity, stdcall(cutscene, method, map, point, 0, 0)) first looks the
// arrival point up (0xD6C490: eax = point, edi = position out, stdcall(rotation out) -> al): a point of a map whose
// data is not loaded is not found, and then neither the cutscene nor the warp happens. The crow's arrival map is
// loaded next to the Undead Asylum only while the player stands on the Asylum's two floors that connect to it
// (m18_01 ConnectCollision h0054B1 / h0099B1 -> m10_02: the roof and the demon's arena). The guest stood there,
// so its game found the point; the host, a few steps back, did not, three tests running (2026-10-13). The
// synced event reached the host's game at the same moment as the guest's own (event sync, 0xBC4000).
// When the lookup fails for such a cutscene of our own player, the map is loaded the way a connecting floor
// loads it (WorldBlockRes +0x61, see below) and the request is made again once its point is there. The crow's
// flight (the event's first cutscene, 40 s) is still playing then, so the arrival cutscene follows it as in the
// game that set it off, and its end moves us into the map in place (0xEE21F0 -> 0xDE36E0) with the event's
// following instructions (the crow: respawn at Firelink's bonfire). If the map cannot be loaded the session
// takes us there with a loading screen once our cutscene is over (take_failed_cutscene).
constexpr uint32_t kPointLookupSite = 0x00D751D2;  // call 0xD6C490 in 0xD75190
constexpr uint32_t kPointLookup = 0x00D6C490;
constexpr uint32_t kCutsceneRequest = 0x00D75190;
struct CutCheck {
    bool active = false;
    bool retry = false;  // our own request, made again once the map was loaded
    int map = -1;
    int point = -1;
    int cutscene = -1;
    int method = 0;
} g_cut_check;
struct CutFailed {
    bool pending = false;
    int map = -1;
    int point = -1;
} g_cut_failed;

// Maps load in blocks (one per map: m10_02 is area 10 block 2). FieldArea ([0x1378520], which the point lookup
// needs too) +4 = WorldRes: +0x10 the block infos (0x3C each, map id at +4), +0x7E0 the number of blocks, +0x7E4
// their WorldBlockRes (0x170 each, same order), +0x7E8 the map we are in, +0x7F0 the in-place switch target.
// Every frame WorldRes's update (0xDD9E30) marks the wanted blocks (+0x62: ours, the previous one while we are
// near it, the switch target, and the blocks whose +0x61 is set, which it clears again); 0xDDB350 adds the blocks
// a connecting floor under the player names (their group masks) and the block loads (+0x68: 0 none .. 7 in use;
// 0xDDE930), its area first. A block wanted by nobody is unloaded two frames later.
constexpr uint32_t kFieldAreaVar = 0x01378520;
constexpr uint32_t kFaWorldRes = 0x4;
constexpr uint32_t kWrBlockInfos = 0x10;
constexpr uint32_t kBlockInfoSize = 0x3C;
constexpr uint32_t kWrBlockCount = 0x7E0;
constexpr uint32_t kWrBlockRes = 0x7E4;
constexpr uint32_t kWrCurrentMap = 0x7E8;
constexpr uint32_t kBlockResSize = 0x170;
constexpr uint32_t kBrWantOnce = 0x61;
constexpr uint32_t kBrState = 0x68;
constexpr uint32_t kBlockInUse = 7;

constexpr uint32_t kFieldAreaVtable = 0x011910DC;  // set by its constructor 0xDE7950, which also fills 0x1378520
uint32_t world_res() {
    uint32_t fa = 0, vt = 0, wr = 0;
    if (!read32(kFieldAreaVar, &fa) || !fa || !read32(fa, &vt) || vt != kFieldAreaVtable ||
        !read32(fa + kFaWorldRes, &wr)) {
        return 0;
    }
    return wr;
}

// The index of `map`'s block, -1 when this world has none.
int map_block(uint32_t wr, int map) {
    uint32_t n = 0, infos = 0;
    if (!wr || !read32(wr + kWrBlockCount, &n) || n == 0 || n > 64 || !read32(wr + kWrBlockInfos, &infos) || !infos) {
        return -1;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t id = 0;
        if (read32(infos + i * kBlockInfoSize + 4, &id) && static_cast<int>(id) == map) return static_cast<int>(i);
    }
    return -1;
}

uint32_t block_res(uint32_t wr, int index) {
    uint32_t res = 0;
    if (!wr || index < 0 || !read32(wr + kWrBlockRes, &res) || !res) return 0;
    return res + static_cast<uint32_t>(index) * kBlockResSize;
}

// Asks for the block this frame, as a connecting floor would.
void want_block(uint32_t wr, int index) {
    const uint32_t res = block_res(wr, index);
    if (!res) return;
    __try {
        *ptr<uint8_t>(res + kBrWantOnce) = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int block_state(uint32_t wr, int index) {
    uint32_t state = 0;
    const uint32_t res = block_res(wr, index);
    if (!res || !read32(res + kBrState, &state)) return -1;
    return static_cast<int>(state);
}

// 0xD6C490 as the request calls it.
bool point_found(int point) {
    float pos[4] = {}, rot[4] = {};
    uint8_t ok = 0;
    __try {
        __asm {
            push edi
            lea edi, pos
            lea edx, rot
            push edx
            mov eax, point
            mov edx, kPointLookup
            call edx
            mov ok, al
            pop edi
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    return ok != 0;
}

// 0xD75190 for our own player (the event's Play Cutscene and Warp Player).
void request_cutscene(int cutscene, int method, int map, int point) {
    __try {
        __asm {
            push 0
            push 0
            push point
            push map
            push method
            push cutscene
            mov eax, 0x2710
            mov edx, kCutsceneRequest
            call edx
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[coop] The cutscene request for map %08X failed", map);
    }
}

// The cutscene manager ([0x1378490]) keeps up to four cutscenes from +0x28, 0x18 bytes each (0xEE1950): the first
// word is the task while one is queued or playing.
bool cutscenes_idle() {
    uint32_t man = 0;
    if (!read32(0x01378490, &man) || !man) return true;
    for (int i = 0; i < 4; ++i) {
        uint32_t task = 0;
        if (!read32(man + 0x28 + i * 0x18, &task) || task) return false;
    }
    return true;
}

// A cutscene of ours whose map is being loaded.
struct CutWait {
    bool active = false;
    bool issued = false;  // the request was made again with the map there
    int cutscene = -1;
    int method = 0;
    int map = -1;
    int point = -1;
    int block = -1;
    double since = 0;
    double issued_at = 0;
    double idle_since = 0;
    int last_state = -2;
} g_cut_wait;

// The block is asked for right before each WorldRes update (0xDD9E30: eax = WorldRes, stdcall(float dt)), from
// its two callers, the field update 0xDE85D0 and the map load's 0xDE8970: a block that misses one update while it
// loads goes back to "not loaded" (0xDDE930 states 1, 2 and 6), and the co-op tick runs with Steam's callbacks,
// not with the game's frames.
constexpr uint32_t kWorldResUpdate = 0x00DD9E30;
constexpr uint32_t kWorldResUpdateSites[2] = {0x00DE86C3, 0x00DE89A6};
uint32_t g_world_res_update = kWorldResUpdate;

void __stdcall before_world_res_update(uint32_t wr) {
    if (g_cut_wait.active && g_cut_wait.block >= 0) want_block(wr, g_cut_wait.block);
}

__declspec(naked) void hk_world_res_update() {
    __asm {
        pushad
        push eax  // WorldRes
        call before_world_res_update
        popad
        jmp dword ptr [g_world_res_update]
    }
}

void give_up_cutscene(const char* why) {
    LOG_INFO("[coop] Our cutscene into map %08X cannot take us (%s): the session takes us there instead", g_cut_wait.map,
             why);
    g_cut_failed.pending = true;
    g_cut_failed.map = g_cut_wait.map;
    g_cut_failed.point = g_cut_wait.point;
    g_cut_wait.active = false;
}

void start_cut_wait(int map, int point, int cutscene, int method) {
    const uint32_t wr = world_res();
    const int block = map_block(wr, map);
    g_cut_wait = CutWait{};
    g_cut_wait.active = true;
    g_cut_wait.cutscene = cutscene;
    g_cut_wait.method = method;
    g_cut_wait.map = map;
    g_cut_wait.point = point;
    g_cut_wait.block = block;
    g_cut_wait.since = seconds();
    if (block < 0) {
        give_up_cutscene("its map is not one this world can load from here");
        return;
    }
    LOG_INFO("[coop] The cutscene's arrival point %d is not loaded in our game: loading map %08X (block %d, state %d) "
             "to play our cutscene %d there", point, map, block, block_state(wr, block), cutscene);
}

// Per frame, game thread.
void cut_wait_tick() {
    if (!g_cut_wait.active) return;
    if (!g_state.active || g_state.invader || !game::in_game() || game::loading()) {
        LOG_INFO("[coop] Waiting for map %08X stopped (the session or the map changed)", g_cut_wait.map);
        g_cut_wait.active = false;
        return;
    }
    const double now = seconds();
    const uint32_t wr = world_res();
    uint32_t current = 0;
    if (wr && read32(wr + kWrCurrentMap, &current) && static_cast<int>(current) == g_cut_wait.map) {
        LOG_INFO("[coop] Our cutscene took us into map %08X (%.1f s after it started)", g_cut_wait.map,
                 now - g_cut_wait.since);
        g_cut_wait.active = false;
        return;
    }
    const uint32_t me = game::player();
    uint32_t hp = 0;
    if (!me || !read32(me + 0x2D4, &hp) || static_cast<int>(hp) <= 0) {
        LOG_INFO("[coop] Waiting for map %08X stopped: we died", g_cut_wait.map);
        g_cut_wait.active = false;
        return;
    }
    const int block = map_block(wr, g_cut_wait.map);
    if (block < 0) {
        give_up_cutscene("its map left this world's list");
        return;
    }
    g_cut_wait.block = block;
    want_block(wr, block);
    const int state = block_state(wr, block);
    if (state != g_cut_wait.last_state) {
        LOG_INFO("[coop] Map %08X: block state %d (%.1f s)", g_cut_wait.map, state, now - g_cut_wait.since);
        g_cut_wait.last_state = state;
    }
    if (!g_cut_wait.issued) {
        if (state == static_cast<int>(kBlockInUse) && point_found(g_cut_wait.point)) {
            g_cut_check = CutCheck{true, true, g_cut_wait.map, g_cut_wait.point, g_cut_wait.cutscene, g_cut_wait.method};
            request_cutscene(g_cut_wait.cutscene, g_cut_wait.method, g_cut_wait.map, g_cut_wait.point);
            g_cut_check.active = false;
            g_cut_wait.issued = true;
            g_cut_wait.issued_at = now;
        } else if (now - g_cut_wait.since > 90.0) {
            give_up_cutscene("its map did not load in 90 s");
        }
        return;
    }
    // Issued: the map stays wanted until the cutscene's end moves us there.
    if (!cutscenes_idle()) {
        g_cut_wait.idle_since = 0;
        return;
    }
    if (g_cut_wait.idle_since == 0) g_cut_wait.idle_since = now;
    if (now - g_cut_wait.issued_at > 2.0 && now - g_cut_wait.idle_since > 3.0) {
        give_up_cutscene("the cutscene ended without moving us");
    }
}

void __stdcall note_point_lookup(int point, int ok) {
    if (!g_cut_check.active) return;
    g_cut_check.active = false;
    if (point != g_cut_check.point) return;
    if (g_cut_check.retry) {
        LOG_INFO("[coop] Map %08X is loaded now (%.1f s): our cutscene %d into it %s", g_cut_check.map,
                 seconds() - g_cut_wait.since, g_cut_check.cutscene, ok ? "is on" : "still found no arrival point");
        if (!ok) give_up_cutscene("its arrival point is missing");
        return;
    }
    if (ok) {
        LOG_INFO("[coop] The cutscene's arrival point %d is loaded here: the game takes us to map %08X", point,
                 g_cut_check.map);
        return;
    }
    start_cut_wait(g_cut_check.map, point, g_cut_check.cutscene, g_cut_check.method);
}

// eax = point, edi = position out, [esp+4] = rotation out; ret 4, al = found. edi, esi, ebx, ebp are kept.
__declspec(naked) void hk_point_lookup() {
    __asm {
        push eax                  // the point
        push dword ptr [esp + 8]  // the rotation out pointer (the caller's argument)
        mov eax, [esp + 4]
        mov edx, kPointLookup
        call edx                  // ret 4 drops the pushed argument
        pop edx                   // the point
        push eax                  // the result
        push ecx
        movzx ecx, al
        push ecx
        push edx
        call note_point_lookup
        pop ecx
        pop eax
        ret 4
    }
}

int __stdcall cut_warp_target(int entity, int area, int block, int point, int method, int cutscene) {
    if (!seamless_rules()) return entity;
    const int dest = (area << 24) | (block << 16);
    const int here = game::map_id();
    if (here == -1 || (static_cast<uint32_t>(here) & 0xFFFF0000u) == static_cast<uint32_t>(dest)) return entity;
    if (entity < kLocalPlayerEntity || entity > kLocalPlayerEntity + 10) {
        LOG_INFO("[coop] Cutscene into map %08X for entity %d is left to the game", dest, entity);
        return entity;
    }
    if (entity == kLocalPlayerEntity) {
        LOG_INFO("[coop] Cutscene %d takes us to map %08X (point %d): the others follow once we are there", cutscene,
                 dest, point);
    } else {
        LOG_INFO("[coop] Cutscene %d into map %08X set off by another player (entity %d): it takes us along", cutscene,
                 dest, entity);
    }
    if (g_cut_wait.active && g_cut_wait.map == dest) {
        // The same cutscene again (an event instance per player): the one we wait with stays.
        return kLocalPlayerEntity;
    }
    g_cut_check = CutCheck{true, false, dest, point, cutscene, method};  // its point lookup comes next
    if (g_cutscene_fn) g_cutscene_fn(dest, point);
    return kLocalPlayerEntity;
}

// eax = the instruction's player entity, edx = its point entity, edi = the cutscene, ebp = the playback method;
// the area and block bytes are at the caller's [esp+0Eh] / [esp+0Fh].
__declspec(naked) void hk_cut_warp() {
    __asm {
        cmp eax, 0x2710
        jne have
        mov eax, [esi + 0x44]
    have:
        pushad
        push edi                               // cutscene
        push ebp                               // method
        push edx                               // point
        movzx ecx, byte ptr [esp + 48 + 0x0F]  // block (past pushad, our return address and three arguments)
        push ecx
        movzx ecx, byte ptr [esp + 52 + 0x0E]  // area
        push ecx
        push eax
        call cut_warp_target
        mov [esp + 28], eax  // popad's eax
        popad
        ret
    }
}

bool install_cut_warp() {
    static const uint8_t k[10] = {0x3D, 0x10, 0x27, 0x00, 0x00, 0x75, 0x03, 0x8B, 0x46, 0x44};
    return patch_call(kCutWarpSite, k, sizeof(k), reinterpret_cast<void*>(&hk_cut_warp));
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

// ---------------------------------------------------------------- per-player treasure
// A guest pickup ORs the item's event-flag bit in the item manager (0xDC76F0) and never calls the flag setter,
// so the host is not told. Treasure flags are group 5, which the visited world's store does not have: the bit
// went nowhere and the item was back on the next load. hk_flag_store now keeps a co-op guest's group 5 in their
// own world, which makes the bookkeeping below a second line of defence. Doors use the same object-flag class
// as chests, so this does not hook those getters: only item-list flags are treated as treasure.
//
// A flag seen on an item is remembered. While the guest's own-world copy of it is on, get_flag reports it on
// (events leave the treasure alone) and the spawned item is disabled locally. The host's copy is untouched.
constexpr uint32_t kItemManVar = 0x0137D6BC;
constexpr uint32_t kItemLookup = 0x00DC4AE0;  // thiscall(manager), stdcall(handle) -> item, ret 4
constexpr uint32_t kItemEnable = 0x00DC6490;  // thiscall(manager), stdcall(handle, enable), ret 8
constexpr uint32_t kItemRemove = 0x00DC76F0;  // stdcall(manager, handle, setFlag), ret 0xC
constexpr uint32_t kGetFlag = 0x00D38C40;     // eax = EventFlagMan, ecx = 0, stdcall(id) -> al, ret 4
constexpr uint32_t kItemLists[] = {0x1C, 0x24, 0x2C, 0x34};
// The bonfire talk (t<map>0<nn>.esd) builds its menu with AddTalkListData(row, text, flag): a row is only added
// when its flag is on. Level Up, Kindle and Reverse Hollowing need 11810000 (the Undead Asylum left behind),
// Reinforce Weapon / Armor and Repair need 250 / 251 / 252, Attune Magic 719 and the Bottomless Box 258; Kindle
// past the second level asks f15(257) (Rite of Kindling). They belong to the character: a guest's active store
// is the host's world, which never has the guest's progress, so those rows were missing or followed the host.
// Read by the talk scripts (and only there, so the world's own events in the host's world keep the host's
// state), a guest gets their own world's value of these. The two talk reads of the flag getter:
constexpr uint32_t kRetTalkRow = 0x00FCF7D6;   // AddTalkListData (talk command 1:19, 0xFCF69A), row flag
constexpr uint32_t kRetTalkFlag = 0x00D4966C;  // GetEventFlag (talk function 15, 0xD495EC)
constexpr int kCharacterFlags[] = {11810000, 250, 251, 252, 257, 258, 719};

bool character_flag(int id) {
    for (int f : kCharacterFlags) {
        if (f == id) return true;
    }
    return false;
}

void* g_getflag_orig = nullptr;
void* g_remove_orig = nullptr;
std::set<int> g_item_flags;
std::set<int> g_owned_flags;  // own-world bit is on; cached so get_flag does not re-enter every check
std::set<uint32_t> g_hidden_items;
int g_in_own_flag = 0;

void clear_guest_item_handles() { g_hidden_items.clear(); }

uint32_t lookup_item(uint32_t manager, uint32_t handle) {
    if (!manager || !handle) return 0;
    uint32_t item = 0;
    __try {
        __asm {
            push handle
            mov ecx, manager
            mov eax, kItemLookup
            call eax
            mov item, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return item;
}

bool disable_item(uint32_t manager, uint32_t handle) {
    __try {
        __asm {
            push 0
            push handle
            mov ecx, manager
            mov eax, kItemEnable
            call eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Own-world bit, through the get_flag trampoline. game::flag() would re-enter the hook. False when it could
// not be read.
bool own_flag(int id, bool* out) {
    const uint32_t efm = game::event_flag_man();
    if (!efm || !g_getflag_orig || id < 0 || g_in_own_flag) return false;
    uint32_t active = 0;
    if (!read32(efm + 0x10C, &active)) return false;
    ++g_in_own_flag;
    *ptr<uint32_t>(efm + 0x10C) = efm + 0x14;
    uint8_t on = 0;
    bool read = true;
    void* fn = g_getflag_orig;
    __try {
        __asm {
            push id
            xor ecx, ecx
            mov eax, efm
            call fn
            mov on, al
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        read = false;
    }
    *ptr<uint32_t>(efm + 0x10C) = active;
    --g_in_own_flag;
    if (read) *out = on != 0;
    return read;
}

bool own_flag_on(int id) {
    bool on = false;
    return own_flag(id, &on) && on;
}

bool guest_owns_flag(int id) {
    if (id < 0 || g_in_own_flag) return false;
    if (g_owned_flags.count(id)) return true;
    if (!g_item_flags.count(id) || !own_flag_on(id)) return false;
    g_owned_flags.insert(id);
    return true;
}

// The getter's answer for a co-op guest: 0 the game's own (active store), 1 on, 2 off.
int __stdcall guest_flag_answer(uint32_t ret, int id) {
    if (!g_state.guest || g_state.invader) return 0;
    if ((ret == kRetTalkRow || ret == kRetTalkFlag) && character_flag(id) && seamless_rules()) {
        bool on = false;
        if (!own_flag(id, &on)) return 0;
        static std::set<int> reported;
        if (reported.insert(id * 2 + (on ? 1 : 0)).second && reported.size() <= 40) {
            LOG_INFO("[coop] Bonfire menu: flag %d is %s in our own world (%s)", id, on ? "on" : "off",
                     ret == kRetTalkRow ? "menu row" : "talk check");
        }
        return on ? 1 : 2;
    }
    return guest_owns_flag(id) ? 1 : 0;
}

__declspec(naked) void hk_get_flag() {
    __asm {
        pushad
        push dword ptr [esp + 36]  // flag id ([esp + 32] is the return address)
        push dword ptr [esp + 36]  // the caller's return address (shifted by the push above)
        call guest_flag_answer
        cmp eax, 1
        popad
        je force_on
        ja force_off
        jmp g_getflag_orig
    force_on:
        mov eax, 1
        ret 4
    force_off:
        xor eax, eax
        ret 4
    }
}

void remember_item_flag(int id) {
    if (id < 0) return;
    g_item_flags.insert(id);
}

void __stdcall note_guest_pickup(uint32_t manager, uint32_t handle) {
    if (!g_state.guest || g_state.invader) return;
    const uint32_t item = lookup_item(manager, handle);
    if (!item) return;
    uint32_t flag = 0;
    if (!read32(item + 0x18, &flag) || (flag & 0x80000000u)) return;
    const int id = static_cast<int>(flag);
    remember_item_flag(id);
    ++g_flag_quiet;
    game::set_own_world_flag(id, true);
    --g_flag_quiet;
    g_owned_flags.insert(id);
    static int lines = 0;
    if (++lines <= 40) {
        LOG_INFO("[coop] Guest took item flag %d; it stays taken for them, the host keeps theirs", id);
    }
}

__declspec(naked) void hk_item_remove() {
    __asm {
        pushad
        push dword ptr [esp + 40]  // handle
        push dword ptr [esp + 40]  // manager (shifted by the push above)
        call note_guest_pickup
        popad
        jmp g_remove_orig
    }
}

void hide_owned_items() {
    if (!g_state.guest || g_state.invader || game::loading() || !g_getflag_orig) return;
    uint32_t manager = 0;
    if (!read32(kItemManVar, &manager) || !manager) return;
    __try {
        for (uint32_t off : kItemLists) {
            uint32_t node = 0;
            if (!read32(manager + off, &node)) continue;
            for (int guard = 0; node && guard < 4096; ++guard) {
                uint32_t next = 0, handle = 0, flag = 0;
                if (!read32(node, &next)) break;
                read32(node + 0x10, &handle);
                read32(node + 0x18, &flag);
                node = next;
                if (!handle || (flag & 0x80000000u)) continue;
                const int id = static_cast<int>(flag);
                remember_item_flag(id);
                if (!guest_owns_flag(id) || !g_hidden_items.insert(handle).second) continue;
                if (!disable_item(manager, handle)) {
                    g_hidden_items.erase(handle);
                    continue;
                }
                static int lines = 0;
                if (++lines <= 40) LOG_INFO("[coop] Item flag %d is already the guest's; hidden here", id);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int lines = 0;
        if (++lines <= 5) LOG_ERROR("[coop] Scanning the item lists failed");
    }
}

void note_hosted_session() {
    if (game::session_state() == 0) {
        g_seamless_hosted = false;
        return;
    }
    if (g_state.host && !g_state.invader && game::own_world() && game::session_state() == 3) g_seamless_hosted = true;
}

// ---------------------------------------------------------------- the host's world, every map of it
// A summoned player is given the host's flags once, on joining (0xD30A20 -> 0xD37F30): the general block and
// the join map's own blocks (groups 1 and 6 of that map). A phantom never leaves that map. A co-op guest does:
// walking on, riding a cutscene (the Asylum's crow to Firelink Shrine) or following a bonfire warp, they met the
// other maps' blocks of the visited world's store as the game had left them: empty, or from an earlier session
// in someone else's world. When a guest arrives the host sends groups 1 and 6 of every map and the guest's
// visited-world store takes them over; later changes come as flags. Group 0 stays the game's own (its 6000-8999
// blocks are the guest's own world's, see personal_flag), and groups 5 and 7 are each player's own.
constexpr int kSnapGroups[] = {1, 6};
constexpr uint32_t kStoreOwn = 0x14;
constexpr uint32_t kStoreVisited = 0x90;
constexpr uint32_t kAreaStride = 0x54;
constexpr int kSubBlocks = 10;
constexpr uint32_t kBlockBytes = 0x80;
constexpr size_t kSnapEntry = 3 + kBlockBytes;  // group, area, sub-block, bits

// Guarded copies (the functions that hold containers cannot use __try themselves).
bool copy_from_game(void* dst, uint32_t src, size_t n) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(static_cast<uintptr_t>(src)), n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool copy_to_game(uint32_t dst, const void* src, size_t n) {
    __try {
        if (src) {
            std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(dst)), src, n);
        } else {
            std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(dst)), 0, n);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Calls fn(group, area, sub-block, block address) for every block of the snapshot groups in a store.
template <typename F>
bool each_flag_block(uint32_t store, F&& fn) {
    for (int g : kSnapGroups) {
        uint32_t areas = 0, count = 0;
        if (!read32(store + g * 0xC, &areas) || !read32(store + g * 0xC + 4, &count) || !areas || count > 64) {
            return false;
        }
        for (uint32_t a = 0; a < count; ++a) {
            for (int sb = 0; sb < kSubBlocks; ++sb) {
                uint32_t block = 0, size = 0;
                const uint32_t at = areas + a * kAreaStride + sb * 8;
                if (!read32(at, &block) || !read32(at + 4, &size)) return false;
                if (block && size == kBlockBytes) fn(g, static_cast<int>(a), sb, block);
            }
        }
    }
    return true;
}

// A load at home starts each map's temporary flags (xxxx5000-xxxx5999: timers, who stands where, which runs of the
// map's events are done) over. A guest's death reload stays in the host's world (the visited store at +0x90) and left
// them as they were, so the map's events started from the old fight's state (docs: "World events"). The visited
// store's temporary blocks (group 1, sub-block 5 of every map) are cleared there, as the host's own reload clears them
// at home. The flag sharing does not see this (each game's own state).
int clear_visited_temporary_flags() {
    const uint32_t efm = game::event_flag_man();
    if (!efm) return 0;
    const uint32_t store = efm + kStoreVisited;
    constexpr int kGroup = 1, kTemporary = 5;
    uint32_t areas = 0, count = 0;
    if (!read32(store + kGroup * 0xC, &areas) || !read32(store + kGroup * 0xC + 4, &count) || !areas || count > 64) {
        return 0;
    }
    int cleared = 0;
    for (uint32_t a = 0; a < count; ++a) {
        uint32_t block = 0, size = 0;
        const uint32_t at = areas + a * kAreaStride + kTemporary * 8;
        if (!read32(at, &block) || !read32(at + 4, &size) || !block || size != kBlockBytes) continue;
        if (copy_to_game(block, nullptr, kBlockBytes)) ++cleared;
    }
    return cleared;
}

int install_one(const char* what, bool ok) {
    if (!ok) LOG_ERROR("[coop] Hook not installed: %s (code does not match this build)", what);
    return ok ? 0 : 1;
}

}  // namespace

bool prepare_warp_kick(bool dead) { return kick_prepare(dead); }

State& state() { return g_state; }

bool disasters_active() {
    for (int id : kDisasterFlags) {
        if (game::flag(id)) return true;
    }
    return false;
}

// The map (area << 24 | block << 16) a disaster flag (1AABxxxx) belongs to.
int disaster_map(int id) { return ((id / 100000) % 100) << 24 | ((id / 10000) % 10) << 16; }

// 735 makes the current map's end event kill its phantoms. That event also sets flag 5001, and the game then shows
// its own notice "Disasters are gone after the defeat of the Gravelord Servant" (text 141210, 0xD78E93), so 735 is
// set only when this map's phantoms are out: its disaster flag was on, or the caller knows the map was cursed
// (a guest, whose copy of the flags may already have been cleared by the host's).
int end_disasters(bool cursed_here) {
    const int map = static_cast<int>(static_cast<uint32_t>(game::map_id()) & 0xFFFF0000u);
    int cleared = 0;
    bool here = cursed_here;
    for (int id : kDisasterFlags) {
        if (game::flag(id)) {
            ++cleared;
            here = here || disaster_map(id) == map;
        }
        game::set_flag(id, false);
    }
    bool end_map = false;
    for (int m : kDisasterEndMaps) end_map = end_map || m == map;
    const bool end_now = end_map && here;
    if (end_now) game::set_flag(kDisasterEnd, true);
    LOG_INFO("[coop] Disasters ended: %d map(s) had one; this map (%08X) %s", cleared, static_cast<unsigned>(map),
             end_now ? "ends its own at once (flag 735)" : end_map ? "had none" : "has no disaster to end at once");
    return cleared;
}

void set_flag_listener(void (*fn)(int id, bool on)) { g_flag_listener = fn; }
void set_cutscene_listener(void (*fn)(int map, int point)) { g_cutscene_fn = fn; }
bool take_failed_cutscene(int* map, int* point) {
    if (!g_cut_failed.pending) return false;
    g_cut_failed.pending = false;
    *map = g_cut_failed.map;
    *point = g_cut_failed.point;
    return true;
}
bool cutscene_loading() { return g_cut_wait.active; }
int world_map() {
    uint32_t map = 0;
    const uint32_t wr = world_res();
    if (!wr || !read32(wr + kWrCurrentMap, &map)) return -1;
    return static_cast<int>(map);
}
bool cutscene_playing() { return !cutscenes_idle(); }
void note_world_reset() { g_world_reset_at = GetTickCount64(); }

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
    if (game::flag(id) != on) note_flag_change(id);
    if (g_contested.count(id)) return;
    g_applying_remote = true;
    game::set_flag(id, on);
    g_applying_remote = false;
}

std::vector<uint8_t> world_flag_snapshot() {
    const uint32_t efm = game::event_flag_man();
    if (!efm) return {};
    std::vector<uint8_t> out(2, 0);
    uint16_t n = 0;
    bool read_all = true;
    const bool ok = each_flag_block(efm + kStoreOwn, [&](int g, int a, int sb, uint32_t block) {
        uint8_t bits[kBlockBytes];
        if (!copy_from_game(bits, block, kBlockBytes)) {
            read_all = false;
            return;
        }
        bool any = false;
        for (uint8_t b : bits) any |= b != 0;
        if (!any) return;
        const size_t at = out.size();
        out.resize(at + kSnapEntry);
        out[at] = static_cast<uint8_t>(g);
        out[at + 1] = static_cast<uint8_t>(a);
        out[at + 2] = static_cast<uint8_t>(sb);
        std::memcpy(&out[at + 3], bits, kBlockBytes);
        ++n;
    });
    if (!ok || !read_all) return {};
    std::memcpy(out.data(), &n, 2);
    return out;
}

int apply_world_flag_snapshot(const uint8_t* data, size_t size) {
    const uint32_t efm = game::event_flag_man();
    uint16_t n = 0;
    if (!efm || !data || size < 2) return -1;
    std::memcpy(&n, data, 2);
    if (size < 2 + static_cast<size_t>(n) * kSnapEntry) return -1;
    // Where each block of the visited world's store is.
    std::map<uint32_t, uint32_t> blocks;  // group << 16 | area << 8 | sub-block -> address
    if (!each_flag_block(efm + kStoreVisited, [&](int g, int a, int sb, uint32_t block) {
            blocks[static_cast<uint32_t>(g << 16 | a << 8 | sb)] = block;
        })) {
        return -1;
    }
    for (uint16_t i = 0; i < n; ++i) {
        const uint8_t* e = data + 2 + static_cast<size_t>(i) * kSnapEntry;
        if (!blocks.count(static_cast<uint32_t>(e[0] << 16 | e[1] << 8 | e[2]))) return -1;
    }
    // The host's empty blocks are empty here too.
    for (const auto& [key, block] : blocks) {
        if (!copy_to_game(block, nullptr, kBlockBytes)) return -1;
    }
    for (uint16_t i = 0; i < n; ++i) {
        const uint8_t* e = data + 2 + static_cast<size_t>(i) * kSnapEntry;
        if (!copy_to_game(blocks[static_cast<uint32_t>(e[0] << 16 | e[1] << 8 | e[2])], e + 3, kBlockBytes)) return -1;
    }
    return n;
}

// ---------------------------------------------------------------- who moves an enemy
// In the game's co-op every game bids for the enemies around (WorldChrMan's update 0xE40A20 -> 0xE6D250,
// stdcall(chr) ret 4: the authority level chr+0x204, plus 256 per step of the enemy's engagement (chr+0xF4), plus
// up to 255 for being near the local player: (1 - distance / 64) x 255). The session host's game hands each enemy
// to the highest bid (0xE57850 -> 0xE558F0) and tells the others (a guest takes its share at 0xE57850). A game
// runs an enemy's own AI (ComManipulator, chr+0x410) only if the enemy was handed to it (WorldChrMan+0x2B4 entry
// >= 0, read by 0xE3E250 from 0xE5A940) or has SetNoNetSync (chr+0x428); otherwise it follows the network
// (NetAIManipulator, chr+0x414). The engagement steps outweigh the distance, so a game far away can win: after the
// guest rested at the Undead Burg bridge bonfire, the host's game at Firelink won the Hellkite (the event
// "Set Network Update Authority" gives both games 4095), and the guest's dragon followed the host's copy, which
// had landed with no floor loaded so far from the host and fell through the bridge (2026-10-13). A game whose
// player is beyond the 64 m of the distance term does not bid while another player stands nearer the enemy.
constexpr uint32_t kNetPrioritySite = 0x00E40C26;
constexpr uint32_t kNetPriority = 0x00E6D250;
constexpr float kBidReach = 64.0f;
std::vector<PeerSpot> g_peer_spots;

float dist3(const game::Vec3& a, const game::Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The world's events switch an enemy's AI on and off in each game by that game's own player (Set Character AI State,
// ChrCtrl+0xC0 bit 0), so the copies can differ. A game whose copy has its AI off does not bid while another player
// is near enough to take the enemy, so it stays with a game where its AI runs; a player who fell counts too (their
// game still runs the enemy while they spectate).
bool ai_off_here(uint32_t chr) {
    uint32_t ctrl = 0;
    uint8_t ai = 1;
    return read32(chr + 0x28, &ctrl) && ctrl && read8(ctrl + 0xC0, &ai) && !(ai & 1);
}

uint32_t __stdcall hk_net_priority(uint32_t chr) {
    using Fn = uint32_t(__stdcall*)(uint32_t);
    const uint32_t bid = reinterpret_cast<Fn>(kNetPriority)(chr);
    if (!bid || !seamless_rules() || g_peer_spots.empty()) return bid;
    game::Vec3 at{}, me{};
    if (!game::chr_position(chr, &at) || !game::position(&me, nullptr)) return bid;
    if (ai_off_here(chr)) {
        for (const PeerSpot& p : g_peer_spots) {
            if (dist3(game::Vec3{p.pos[0], p.pos[1], p.pos[2]}, at) > kBidReach) continue;
            static std::set<uint32_t> logged;
            if (logged.size() < 30 && logged.insert(chr).second) {
                LOG_INFO("[coop] Enemy %08X at (%.1f, %.1f, %.1f): its AI is off in this game; no bid from here while "
                         "another player is near it", chr, at.x, at.y, at.z);
            }
            return 0;
        }
    }
    const float mine = dist3(me, at);
    if (mine <= kBidReach) return bid;
    for (const PeerSpot& p : g_peer_spots) {
        if (p.dead) continue;
        const float theirs = dist3(game::Vec3{p.pos[0], p.pos[1], p.pos[2]}, at);
        if (theirs < mine) {
            static std::set<uint32_t> logged;
            if (logged.size() < 30 && logged.insert(chr).second) {
                LOG_INFO("[coop] Enemy %08X at (%.1f, %.1f, %.1f): no bid from here (we are %.0f m away, a player in map "
                         "%08X is %.0f m away)",
                         chr, at.x, at.y, at.z, mine, p.map, theirs);
            }
            return 0;
        }
    }
    return bid;
}

// Which game moves the Hellkite (1010300), with where it is: logged when that changes and every 10 s while it
// exists here, for the 2026-10-13 report (the dragon fell through the bridge after a rest).
constexpr int kWatchedEnemies[] = {1010300};

void track_watched_enemies() {
    static double last = 0;
    static int lines = 0;
    const double t = seconds();
    if (t - last < 0.5 || lines > 200 || !g_state.active || game::loading()) return;
    last = t;
    static std::map<int, std::pair<int, double>> seen;  // entity -> (control, last line)
    for (int entity : kWatchedEnemies) {
        const uint32_t chr = chr_by_entity(entity);
        uint32_t current = 0, own_ai = 0, net_ai = 0, no_sync = 0;
        game::Vec3 at{}, me{};
        if (!chr || !read32(chr + 0x2C, &current) || !read32(chr + 0x410, &own_ai) || !read32(chr + 0x414, &net_ai) ||
            !game::chr_position(chr, &at)) {
            continue;
        }
        const int control = current == own_ai ? 1 : current == net_ai ? 2 : 0;
        auto& s = seen[entity];
        if (control == s.first && t - s.second < 10.0) continue;
        s = {control, t};
        game::position(&me, nullptr);
        read32(chr + 0x428, &no_sync);  // SetNoNetSync (low byte)
        ++lines;
        LOG_INFO("[coop] Enemy %d: moved by %s, at (%.1f, %.1f, %.1f), %.0f m from us%s", entity,
                 control == 1 ? "this game (own AI)" : control == 2 ? "another game (network)" : "neither (?)", at.x,
                 at.y, at.z, dist3(me, at), (no_sync & 0xFF) ? ", no net sync" : "");
    }
}

// The boss fight's characters (the boss gauges: handles at MenuMan+0xF64 / +0xF78, -1 none): which game moves them,
// whether their AI is on in this game (Set Character AI State: ChrCtrl+0xC0 bit 0, 0xD6C3F0) and what this game bids,
// for the 2026-10-17 report. After a team wipe the guest walked into the Asylum Demon's hall first, and the demon
// then fought only while the host stood close to it and paused when the host walked away. Logged when that changes
// and every 10 s during the fight.
constexpr uint32_t kMenuManVar = 0x013786D0;

uint32_t chr_of_handle(uint32_t handle) {
    uint32_t wcm = 0, table = 0, count = 0, entries = 0, chr = 0;
    if (handle == 0xFFFFFFFF || !read32(game::kWorldChrManVar, &wcm) || !wcm) return 0;
    const uint32_t index = (handle >> 14) & 0x3F, slot = handle & 0x3FFF;
    if (!read32(wcm + 0x240 + index * 4, &table) || !table || !read32(table, &count) || slot >= count ||
        !read32(table + 4, &entries) || !read32(entries + slot * 0x20, &chr)) {
        return 0;
    }
    return chr;
}

void track_boss_control() {
    static double last = 0;
    static int lines = 0;
    const double t = seconds();
    if (t - last < 0.5 || lines > 300 || !g_state.active || game::loading()) return;
    last = t;
    static std::map<uint32_t, std::pair<int, double>> seen;  // gauge handle -> (state, last line)
    uint32_t mm = 0;
    if (!read32(kMenuManVar, &mm) || !mm) return;
    for (uint32_t off : {0xF64u, 0xF78u}) {
        uint32_t h = 0xFFFFFFFF, current = 0, own_ai = 0, net_ai = 0, ctrl = 0, engage = 0, authority = 0;
        uint8_t ai = 0;
        game::Vec3 at{}, me{};
        if (!read32(mm + off, &h) || h == 0xFFFFFFFF) continue;
        const uint32_t chr = chr_of_handle(h);
        if (!chr || !read32(chr + 0x2C, &current) || !read32(chr + 0x410, &own_ai) || !read32(chr + 0x414, &net_ai) ||
            !read32(chr + 0x28, &ctrl) || !read8(ctrl + 0xC0, &ai) || !game::chr_position(chr, &at)) {
            continue;
        }
        read32(chr + 0xF4, &engage);
        read32(chr + 0x204, &authority);
        const int control = current == own_ai ? 1 : current == net_ai ? 2 : 0;
        const int state = control * 2 + (ai & 1);
        auto& s = seen[h];
        if (state == s.first && t - s.second < 10.0) continue;
        s = {state, t};
        game::position(&me, nullptr);
        ++lines;
        LOG_INFO("[coop] Boss %08X: moved by %s, its AI %s in this game (engagement %u, authority %u), at (%.1f, %.1f, "
                 "%.1f), %.0f m from us",
                 h, control == 1 ? "this game (own AI)" : control == 2 ? "another game (network)" : "neither (?)",
                 (ai & 1) ? "on" : "off", engage, authority, at.x, at.y, at.z, dist3(me, at));
    }
}

// Set Character AI State (0xD6C3F0: eax = entity, stdcall(state); from EMEVD 2004[01] at 0xBC299A and from Lua at
// 0xD5FDA4) on a boss (team 7): logged with the event that ran it and whether another player's game started that
// run (event sync), to see which run decides a boss's AI in each game (diagnostic).
constexpr uint32_t kSetAiState = 0x00D6C3F0;
constexpr uint32_t kBossTeam = 7;  // chr+0x74 of a boss
void* g_set_ai_orig = nullptr;

void __stdcall note_ai_state(int entity, int state) {
    if (!g_state.active) return;
    static int lines = 0;
    if (lines >= 80) return;
    const uint32_t chr = chr_by_entity(entity);
    uint32_t team = 0;
    if (!chr || !read32(chr + 0x74, &team) || team != kBossTeam) return;
    uint32_t id = 0, slot = 0, by = 0;
    if (g_cur_event) {
        read32(g_cur_event + 0x28, &id);
        read32(g_cur_event + 0x2C, &slot);
        read32(g_cur_event + 0x44, &by);
    }
    ++lines;
    LOG_INFO("[coop] Boss %d: AI switched %s by %s %d (slot %d)%s; it was %s here", entity, state ? "on" : "off",
             g_cur_event ? "event" : "Lua", static_cast<int>(id), static_cast<int16_t>(slot & 0xFFFF),
             static_cast<int>(by) >= 10001 && static_cast<int>(by) <= 10010 ? ", a run another player's game started"
                                                                            : "",
             ai_off_here(chr) ? "off" : "on");
}

__declspec(naked) void hk_set_ai() {
    __asm {
        pushad
        push dword ptr [esp + 36]  // state: [esp+4] before pushad
        push eax                   // entity
        call note_ai_state
        popad
        jmp dword ptr [g_set_ai_orig]
    }
}

int install() {
    int failed = 0;
    {
        // push ebp ; mov ebp,esp ; and esp,-16
        static const uint8_t k[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
        g_set_ai_orig = detour(kSetAiState, k, sizeof(k), reinterpret_cast<void*>(&hk_set_ai));
        if (!g_set_ai_orig) LOG_ERROR("[coop] Boss AI diagnostic: Set Character AI State does not match this build");
    }
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
    failed += install_one("member left (no return home for the master of the world)", install_master_left());
    failed += install_one("guest bloodstain position", install_blood_world());
    failed += install_one("bloodstain kept in a co-op party", install_param_party());
    {
        // mov eax, [esi+70h] ; sub eax, edi. The following je stays in place.
        static const uint8_t k[5] = {0x8B, 0x46, 0x70, 0x2B, 0xC7};
        g_census_orig = detour(kPartyCensus, k, sizeof(k), reinterpret_cast<void*>(&hk_party_census));
        failed += install_one("party census null character", g_census_orig != nullptr);
    }
    {
        static const uint8_t k[7] = {0x6A, 0xFF, 0x68, 0xE5, 0x34, 0x07, 0x01};  // push -1 ; push 010734E5h
        g_movemap_orig = detour(kMoveMapCtor, k, sizeof(k), reinterpret_cast<void*>(&hk_movemap));
        failed += install_one("map load world choice", g_movemap_orig != nullptr);
    }
    {
        // cmp dword ptr [eax+194h], 3. The following jne is a short jump and stays in place.
        static const uint8_t k[7] = {0x83, 0xB8, 0x94, 0x01, 0x00, 0x00, 0x03};
        g_spawn_orig = detour(kSpawnChoice, k, sizeof(k), reinterpret_cast<void*>(&hk_spawn_choice));
        failed += install_one("guest bonfire spawn", g_spawn_orig != nullptr);
    }
    {
        // xor eax,eax ; cmp [esi+4], eax. The following je is a short jump and stays in place.
        // 39 46 04 is the register form. 83 7E 04 00 (cmp [esi+4], 0) is not what the exe has.
        static const uint8_t k[5] = {0x33, 0xC0, 0x39, 0x46, 0x04};
        g_regen_orig = detour(kRegenSwitch, k, sizeof(k), reinterpret_cast<void*>(&hk_regen_switch));
        failed += install_one("guest death bonfire", g_regen_orig != nullptr);
    }
    {
        // sub esp,8 ; push edi ; lea edi,[esp+4]
        static const uint8_t k[8] = {0x83, 0xEC, 0x08, 0x57, 0x8D, 0x7C, 0x24, 0x04};
        g_choose_orig = detour(kChooseWarpMap, k, sizeof(k), reinterpret_cast<void*>(&hk_choose_warp_map));
        failed += install_one("death area choice", g_choose_orig != nullptr);
    }
    {
        // cmp byte [esi+0xB4F], bl. The following jne stays in place.
        static const uint8_t k[6] = {0x38, 0x9E, 0x4F, 0x0B, 0x00, 0x00};
        g_place_orig = detour(kPlaceChoice, k, sizeof(k), reinterpret_cast<void*>(&hk_place_choice));
        failed += install_one("guest bonfire placement", g_place_orig != nullptr);
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
        // push ebx ; xor bl,bl ; push esi ; mov esi,[esp+0Ch]
        static const uint8_t k[8] = {0x53, 0x32, 0xDB, 0x56, 0x8B, 0x74, 0x24, 0x0C};
        g_getflag_orig = detour(kGetFlag, k, sizeof(k), reinterpret_cast<void*>(&hk_get_flag));
        failed += install_one("guest item flags", g_getflag_orig != nullptr);
    }
    {
        // mov eax,[esp+8] ; mov ecx,[esp+4]
        static const uint8_t k[8] = {0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04};
        g_remove_orig = detour(kItemRemove, k, sizeof(k), reinterpret_cast<void*>(&hk_item_remove));
        failed += install_one("guest item pickup", g_remove_orig != nullptr);
    }
    {
        const int bad = install_loot();
        if (bad) LOG_ERROR("[coop] %d of 6 loot rule patches did not match this build", bad);
        failed += install_one("loot rules", bad == 0);
    }
    failed += install_one("guest's own items", install_items() == 0);
    failed += install_one("guest's own item and conversation flags", install_flag_store());
    failed += install_one("cutscenes into another map take every player", install_cut_warp());
    failed += install_one("cutscene arrival point lookup",
                          redirect_call(kPointLookupSite, kPointLookup, reinterpret_cast<void*>(&hk_point_lookup)));
    for (uint32_t site : kWorldResUpdateSites) {
        failed += install_one("a cutscene's map is loaded for it",
                              redirect_call(site, kWorldResUpdate, reinterpret_cast<void*>(&hk_world_res_update)));
    }
    failed += install_one("bonfire party check", redirect_call(0x00D58AE4, kHasParty, reinterpret_cast<void*>(&hk_has_party)));
    failed += install_one("bonfire foes check",
                          redirect_call(kBonfireFoesSite, kBonfireFoes, reinterpret_cast<void*>(&hk_bonfire_foes)));
    for (uint32_t site : kLockOnSites) {
        failed += install_one("lock-on (friendly fire)", redirect_call(site, kLockOn, reinterpret_cast<void*>(&hk_lock_on)));
    }
    {
        static const uint8_t k[6] = {0x8B, 0x0D, 0xA0, 0x84, 0x37, 0x01};  // mov ecx,[013784A0h]
        g_have_party_orig = reinterpret_cast<BoolFn>(
            detour(kHavePartyMember, k, sizeof(k), reinterpret_cast<void*>(&hk_have_party_member)));
        failed += install_one("HavePartyMember (deaths)", g_have_party_orig != nullptr);
    }
    failed += install_one("talk party check", redirect_call(0x00D4ABFB, kHasParty, reinterpret_cast<void*>(&hk_has_party)));
    failed += install_one("NPCs are people for a co-op guest", install_wander_ghost() == 0);
    failed += install_one("a guest's kills give full souls",
                          redirect_call(kKillSoulsClientSite, kIsClient, reinterpret_cast<void*>(&hk_kill_souls_client)));
    failed += install_one("a guest's HP tests skip missing characters",
                          swap_pointer(kHpRateVtable + 4, kHpRateTest, reinterpret_cast<void*>(&hk_hp_rate_test),
                                       &g_hp_rate_orig));
    {
        static const uint8_t k[5] = {0x8B, 0x4C, 0x24, 0x08, 0x55};  // mov ecx,[esp+8] ; push ebp
        g_set_hp_orig = detour(kSetHp, k, sizeof(k), reinterpret_cast<void*>(&hk_set_hp));
        failed += install_one("the host's NPCs live through a guest's rest", g_set_hp_orig != nullptr);
    }
    {
        static const uint8_t k[6] = {0x51, 0x55, 0x8B, 0x6C, 0x24, 0x0C};  // push ecx ; push ebp ; mov ebp,[esp+0Ch]
        g_kill_orig = detour(kKillChr, k, sizeof(k), reinterpret_cast<void*>(&hk_kill_chr));
        failed += install_one("the host's NPCs live through a guest's rest (kills)", g_kill_orig != nullptr);
    }
    {
        // mov eax,[ecx+28h] ; cmp eax,63h (the flags of the cmp reach the jl after the jump back)
        static const uint8_t k[6] = {0x8B, 0x41, 0x28, 0x83, 0xF8, 0x63};
        g_event_done_orig = detour(kEventDone, k, sizeof(k), reinterpret_cast<void*>(&hk_event_done));
        failed += install_one("finished events are finished at home too", g_event_done_orig != nullptr);
    }
    {
        // sub esp,0Ch ; lea edx,[esp] (a diagnostic: its failure does not stop the co-op)
        static const uint8_t k[6] = {0x83, 0xEC, 0x0C, 0x8D, 0x14, 0x24};
        g_throw_try_orig = detour(kThrowTry, k, sizeof(k), reinterpret_cast<void*>(&hk_throw_try));
        if (!g_throw_try_orig) LOG_ERROR("[coop] Throw diagnostic did not match this build");
    }
    failed += install_one("respawn points set by events", install_respawn_point());
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
    {
        // push ecx ; mov ecx,[eax+78h] ; mov ecx,[ecx]
        static const uint8_t k[6] = {0x51, 0x8B, 0x48, 0x78, 0x8B, 0x09};
        g_event_step_orig = detour(kEventStep, k, sizeof(k), reinterpret_cast<void*>(&hk_event_step));
        failed += install_one("map event working flags stay local", g_event_step_orig != nullptr);
    }
    failed += install_one("map event runs from another map",
                          swap_pointer(kEmkEventVtable + 4 * 4, kEventSync, reinterpret_cast<void*>(&hk_event_sync),
                                       &g_event_sync_orig));
    failed += install_one("random picks of another game's runs",
                          redirect_call(kRandomFlagSite, kRandomFlag, reinterpret_cast<void*>(&hk_random_flag)));
    failed += install_one("enemies moved by a game whose player is near",
                          redirect_call(kNetPrioritySite, kNetPriority, reinterpret_cast<void*>(&hk_net_priority)));
    failed += install_one("Black Separation Crystal (guest)",
                          redirect_call(kSepGuestSite, kSepGuestOrig, reinterpret_cast<void*>(&hk_separation_guest)));
    failed += install_one("Black Separation Crystal (host)",
                          redirect_call(kSepHostSite, kSepHostOrig, reinterpret_cast<void*>(&hk_separation_host)));
    return failed;
}

void set_guest_world_events(bool on) { g_world_events = on; }

void track_safe_position() {
    const double t = seconds();
    if (t - g_last_spot < 0.25) return;
    g_last_spot = t;
    const uint32_t me = game::player();
    uint32_t hp = 0;
    game::Vec3 pos{};
    if (!me || game::loading() || !read32(me + 0x2D4, &hp) || static_cast<int>(hp) <= 0 ||
        !game::position(&pos, nullptr)) {
        return;
    }
    // Not while falling: a stain in the air or at the bottom of a pit cannot be reached.
    static game::Vec3 last{};
    static double last_t = 0;
    const double dt = t - last_t;
    const bool falling = last_t > 0 && dt < 1.0 && (last.y - pos.y) / dt > 4.0;
    last = pos;
    last_t = t;
    if (falling) return;
    g_spots[g_spot_next] = SafeSpot{t, pos, game::map_id()};
    g_spot_next = (g_spot_next + 1) % 32;
    if (g_spot_count < 32) ++g_spot_count;
}

// What happens to our bloodstain, for the report of 2026-10-13 ("the bloodstain, at least for the guest, is not
// working properly"; both of the guest's stains had been written where the game puts them). Logged when it
// changes: where we died; the record (GameDataMan+0x18 exists, [+0x1C]: +0 position, +0x34 humanity, +0x38
// souls, +0x3C map), which the game takes from where our replay record starts, up to about 9 s before the death
// (28 samples of 1/3 s); its marker (BloodStainMan = [LuaEvents+0xC]: +0x60 position object, +0x68 effect),
// which every load puts back (0xEE2E90 -> 0xDB6010); standing within the 1.2 m the game offers it from; taking it.
constexpr uint32_t kLuaEventsVar = 0x0137D790;
constexpr float kStainReach = 1.2f;  // FrpgEventConditionSelfBlood (0xD5B330): radius DAT_01160784

bool read_vec(uint32_t addr, game::Vec3* out) {
    uint32_t v[3] = {};
    if (!read32(addr, &v[0]) || !read32(addr + 4, &v[1]) || !read32(addr + 8, &v[2])) return false;
    std::memcpy(out, v, sizeof(v));
    return true;
}

float distance(const game::Vec3& a, const game::Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void track_bloodstain() {
    static double last = 0;
    static int lines = 0;
    const double t = seconds();
    if (t - last < 0.25 || lines > 150 || !game::in_game()) return;
    last = t;
    auto log_line = [&](const char* fmt, auto... args) {
        ++lines;
        LOG_INFO(fmt, args...);
    };
    const uint32_t me = game::player();
    game::Vec3 here{};
    const bool placed = me && !game::loading() && game::position(&here, nullptr);
    // Our death.
    static bool dead = false;
    static game::Vec3 died_at{};
    static bool died_known = false;
    uint32_t hp = 1;
    if (placed && read32(me + 0x2D4, &hp)) {
        if (static_cast<int>(hp) <= 0 && !dead) {
            dead = true;
            died_at = here;
            died_known = true;
            log_line("[coop] We died at (%.1f, %.1f, %.1f) map %08X%s", here.x, here.y, here.z, game::map_id(),
                     game::flag(kDeathLatch) ? " (death flag 4000 was already on: this death will not start)" : "");
        } else if (static_cast<int>(hp) > 0) {
            dead = false;
        }
    }
    // The record.
    const uint32_t gdm = game::game_data_man();
    uint32_t flags = 0, rec = 0, map = 0, souls = 0, humanity = 0;
    game::Vec3 at{};
    if (!gdm || !read32(gdm + 0x18, &flags) || !read32(gdm + 0x1C, &rec) || !rec || !read_vec(rec, &at) ||
        !read32(rec + 0x3C, &map) || !read32(rec + 0x38, &souls) || !read32(rec + 0x34, &humanity)) {
        return;
    }
    // A record with no souls (-1, after a pick-up or before the first death) shows no marker.
    const bool exists = (flags & 0xFF) != 0 && static_cast<int>(souls) >= 0;
    static bool seen_exists = false;
    static game::Vec3 seen_at{};
    static uint32_t seen_map = 0;
    static int seen_souls = 0;
    static bool seen_once = false;
    static bool standing = false;
    const int player_souls = game::souls();
    static int souls_at_stain = -1;
    if (!seen_once || exists != seen_exists || (exists && (distance(at, seen_at) > 0.05f || map != seen_map ||
                                                           static_cast<int>(souls) != seen_souls))) {
        if (exists) {
            char from_death[64] = "";
            if (died_known) std::snprintf(from_death, sizeof(from_death), ", %.1f m from where we died", distance(at, died_at));
            log_line("[coop] Bloodstain: at (%.1f, %.1f, %.1f) map %08X with %d souls and %d humanity%s", at.x, at.y,
                     at.z, map, static_cast<int>(souls), static_cast<int>(humanity), from_death);
        } else if (seen_once && seen_exists) {
            if (standing && souls_at_stain >= 0) {
                log_line("[coop] Bloodstain taken: souls %d -> %d", souls_at_stain, player_souls);
            } else {
                log_line("[coop] Bloodstain: gone (we were not standing at it)");
            }
        } else if (!seen_once) {
            log_line("[coop] Bloodstain: none");
        }
        seen_exists = exists;
        seen_at = at;
        seen_map = map;
        seen_souls = static_cast<int>(souls);
        seen_once = true;
        died_known = died_known && exists;
    }
    if (!placed) return;
    // The marker, once the map is in.
    uint32_t lua = 0, man = 0, pos_obj = 0, effect = 0;
    read32(kLuaEventsVar, &lua);
    if (lua) read32(lua + 0xC, &man);
    if (man) {
        read32(man + 0x60, &pos_obj);
        read32(man + 0x68, &effect);
    }
    const int marker = (man ? 1 : 0) + (pos_obj ? 2 : 0) + (effect ? 4 : 0);
    static int seen_marker = -1;
    static game::Vec3 seen_marker_at{};
    game::Vec3 marker_at{};
    const bool marker_placed = pos_obj && read_vec(pos_obj, &marker_at);
    if (marker != seen_marker || (marker_placed && distance(marker_at, seen_marker_at) > 0.05f)) {
        if (marker_placed) {
            log_line("[coop] Bloodstain marker at (%.1f, %.1f, %.1f), effect %s; we are %.1f m from it", marker_at.x,
                     marker_at.y, marker_at.z, effect ? "on" : "off", distance(marker_at, here));
        } else {
            log_line("[coop] Bloodstain marker: none (manager %s, record %s)", man ? "present" : "missing",
                     exists ? "exists" : "empty");
        }
        seen_marker = marker;
        seen_marker_at = marker_at;
    }
    // Standing at it.
    if (exists && marker_placed) {
        const float d = distance(marker_at, here);
        if (d <= kStainReach && !standing) {
            standing = true;
            souls_at_stain = player_souls;
            log_line("[coop] At our bloodstain (%.1f m; the game offers it within %.1f m)", d, kStainReach);
        } else if (d > kStainReach + 1.0f && standing) {
            standing = false;
            log_line("[coop] Left our bloodstain without taking it (%.1f m)", d);
        }
    } else if (!exists) {
        standing = false;
    }
}

// A guest keeps the chr type they joined with (own_chr_type): every arrival in the host's world puts it back
// (hk_arrival) and the way home restores it. Reverse Hollowing at a bonfire in the host's world makes a hollow
// guest human (8 -> 0) while they stand there; without this, their next death, the next bonfire warp
// they followed and the trip home made them hollow again, after the humanity was paid.
void track_own_chr_type() {
    if (!g_state.active || !g_state.guest || g_state.invader || g_state.leaving || g_state.own_chr_type != 8) return;
    if (game::loading() || game::own_world()) return;
    const uint32_t me = game::player();
    uint32_t hp = 0;
    if (!me || !read32(me + 0x2D4, &hp) || static_cast<int>(hp) <= 0) return;
    if (game::chr_type() != 0) return;
    g_state.own_chr_type = 0;
    LOG_INFO("[coop] Reverse Hollowing in the host's world: we stay human from now on (was hollow on joining)");
}

void set_remote_players(const std::vector<RemotePlayer>& players) { g_remote = players; }
void set_peer_spots(const std::vector<PeerSpot>& spots) { g_peer_spots = spots; }

void tick() {
    g_main_thread = GetCurrentThreadId();
    // A co-op guest's loot and items follow the host's (single player) rules for as long as it is a guest, also
    // while it leaves (the area unload on the way home must clean up the same way the items were made).
    g_strict = (g_state.active && g_state.guest && !g_state.invader) ? 0 : 1;
    world_resetting();
    cut_wait_tick();
    if (game::loading() && !g_host_killed.empty()) g_host_killed.clear();
    if (game::loading() && !g_mirroring_events.empty()) g_mirroring_events.clear();
    if (!g_state.active && !g_flag_churn.empty()) {
        g_flag_churn.clear();
        g_contested.clear();
    }
    if (g_npc_people != g_npc_people_logged && !game::loading()) {
        LOG_INFO("[coop] %u NPC(s) in the host's world are people, not wandering ghosts",
                 g_npc_people - g_npc_people_logged);
        g_npc_people_logged = g_npc_people;
    }
    apply_teams();
    note_hosted_session();
    if (!g_state.guest) {
        g_item_flags.clear();
        g_owned_flags.clear();
        g_hidden_items.clear();
    } else {
        hide_owned_items();
        if (g_state.active && !g_state.invader) track_safe_position();
        track_own_chr_type();
    }
    track_bloodstain();
    track_watched_enemies();
    track_boss_control();
    invader_walls_tick();
    if (!seamless_rules()) return;
    if (g_state.host && !g_state.closing && game::session_state() == 3) game::keep_room_open();
}

}  // namespace coop::rules
