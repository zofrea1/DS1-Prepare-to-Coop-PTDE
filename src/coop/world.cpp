// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "world.h"

#include "deadlist.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "motion.h"
#include "rules.h"
#include "session.h"
#include "spectate.h"
#include "text.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace coop::world {

static void reset_after_rest(const char* who);  // a rest after a lost boss fight (below)

namespace {

// ---------------------------------------------------------------- addresses
constexpr uint32_t kSpEffectGetter = 0x00D1B650;  // edi = id, push out {id, row}; ret 4
constexpr uint32_t kRequestRegene = 0x00D5D820;   // Lua Util_RequestRegene(bonfire): a rest; stdcall
constexpr uint32_t kWarpKick = 0x00D62930;        // Lua WarpNextStageKick: performs a requested warp
constexpr uint32_t kRemoveEffectNode = 0x00DC0960;  // eax = node, push 1, push controller; ret 8
constexpr uint32_t kWorldChrManVar = 0x0137D644;
constexpr uint32_t kWorldReset = 0x2CC;            // WorldChrMan: 0 -> 1 respawns the world's enemies
constexpr uint32_t kMenuManVar = 0x013786D0;       // boss gauges: handles at +0xF64 and +0xF78 (-1 none)
constexpr uint32_t kEnemyVtable = 0x0118979C;      // EnemyIns
constexpr int kApplySlot = 125;                     // ChrIns::ApplySpEffect(id, source, 0, 1,1,1, own)
constexpr uint32_t kPoiseSum = 0x00DBF700;         // eax = a character's SpecialEffect: the poise they add (xmm0)
constexpr uint32_t kOwnPoiseSlot = 0x160;           // ChrIns vtable: own poise (EnemyIns: NpcParam +0x118), x87

// ---------------------------------------------------------------- our SpEffect rows
// Remastered Seamless Co-op's rows and ids: enemy and boss scaling for 1..5 extra players, and the death debuff
// for 1..5 Crimson Blossoms. SpEffectParam has the same 0x170-byte layout in both games.
constexpr int kScaleEnemy = 2147418000;  // + (players - 1), 0..4
constexpr int kScaleBoss = 2147418005;   // + (players - 1), 0..4
constexpr int kDebuff = 2147419000;      // + (blossoms - 1), 0..4
constexpr int kRow = 0x170;              // SP_EFFECT_PARAM_ST
constexpr int kRows = 15;

Config g_cfg;
uint8_t g_rows[kRows][kRow];
bool g_rows_ready = false;
void* g_fx_orig = nullptr;

int row_index(int id) {
    if (id >= kScaleEnemy && id < kScaleEnemy + 10) return id - kScaleEnemy;
    if (id >= kDebuff && id < kDebuff + 5) return 10 + (id - kDebuff);
    return -1;
}

// ---------------------------------------------------------------- state
bool g_active = false, g_invader = false, g_invader_present = false;
int g_players = 1;
bool g_everyone_dead = false;
bool g_was_alive = true;
void (*g_rest_fn)() = nullptr;
void (*g_death_fn)() = nullptr;
void (*g_before_rest_fn)() = nullptr;
int (*g_blossom_count_fn)() = nullptr;
void* g_regene_orig = nullptr;
using CharFn = char(__cdecl*)();
CharFn g_warp_orig = nullptr;
bool g_bypass = false;
bool g_limbo = false;
bool g_team_hold = false;   // a team wipe or a lost fight's reload: our respawn waits for the others' (session)
bool g_reward_hold = false; // our respawn waits for a boss's rewards to come in this game (reward_pending)
const char* reward_pending();
bool g_team_asked = false;  // asked once per death: a second warp of the same death does not wait again
double g_death_at = 0;          // our death, or the end of its wait (spectating, limbo, the team); 0 while we live
double g_fell_at = 0;           // when we fell, for the death we are in
bool g_death_warp = false;      // this death's respawn warp was asked for
bool g_death_finished = false;  // finish_death ran for this death
bool g_return_cleared = false;  // finish_death cleared a stale return to the title for this death
bool g_rest_heard = false;
double g_limbo_since = 0;
bool g_trip_release = false;  // the host's trip home came while we were down (trip_release): no limbo until we live
std::map<uint32_t, int> g_scaled;  // EnemyIns -> applied effect id
// An enemy whose scaling changed, until the game has worked out its new max HP (scale_tick, keep_share).
struct Fill {
    int hp = 0, max = 0;  // its health and max HP before the change
    double at = 0;
};
std::map<uint32_t, Fill> g_fill_hp;
std::map<uint32_t, int> g_base_max;     // EnemyIns -> its max HP before our scaling (when first applied)
std::map<uint32_t, double> g_scaled_at; // EnemyIns -> when our scaling was first applied
std::map<uint32_t, int> g_forced_max;   // EnemyIns -> the max HP we wrote while the game did not update it
double g_last_scale = 0;
int g_debuff_applied = 0;          // debuff effect id on the local player (0 none)
void (*g_wither_fn)() = nullptr;   // the Crimson Blossoms leave the inventory (coop.cpp)
bool (*g_blossom_give_fn)() = nullptr;  // one more Crimson Blossom (coop.cpp)
double g_no_session_since = 0;     // in the world without a session since then (the blossoms go)
const char* g_wither_pending = nullptr;  // they are to wither once we have a character again (why)
bool g_wither_tell = false;
// The health the death debuff took, given back once our max HP is whole again (wither_blossoms).
struct Refill {
    int blossoms = 0;   // the debuff's strength when it came off; 0 nothing to give back
    int hp = 0, max = 0;  // our health and max HP then
    bool grow = false;  // the effect was on: the game raises the max first
    double until = 0;
} g_refill;

double now() { return GetTickCount64() / 1000.0; }

void put_f(uint8_t* r, int off, float v) { std::memcpy(r + off, &v, 4); }
void put_i(uint8_t* r, int off, int32_t v) { std::memcpy(r + off, &v, 4); }
void put_s(uint8_t* r, int off, int16_t v) { std::memcpy(r + off, &v, 2); }

// Builds a row the way Remastered Seamless Co-op does (its field writes, in its order).
void base_row(uint8_t* r) {
    std::memset(r, 0, kRow);
    put_i(r, 0x00, -1);                                  // iconId
    put_f(r, 0x04, -1.0f);                               // conditionHp
    put_f(r, 0x08, -1.0f);                               // effectEndurance: until removed
    for (int off = 0x14; off <= 0x34; off += 4) put_f(r, off, 1.0f);  // max MP/stamina, damage cut rates
    for (int off = 0x68; off <= 0x74; off += 4) put_f(r, off, 1.0f);
    put_f(r, 0x8C, -1.0f);
    put_f(r, 0x90, -1.0f);
    put_i(r, 0x98, -1);
    put_f(r, 0xC8, 1.0f);
    put_f(r, 0xDC, 1.0f);
    put_f(r, 0xE0, 1.0f);
    put_f(r, 0xE4, 1.0f);
    put_f(r, 0xE8, 1.0f);
    put_f(r, 0xF4, 1.0f);
    put_f(r, 0x104, 1.0f);
    put_f(r, 0x118, 1.0f);
    put_f(r, 0x11C, 1.0f);
    put_f(r, 0x120, 1.0f);
    put_i(r, 0x124, -1);
    put_i(r, 0x128, -1);
    put_i(r, 0x12C, -1);
    put_f(r, 0x130, 1.0f);
    put_f(r, 0x134, 1.0f);
    put_s(r, 0x138, -1);
    r[0x141] = 0xFF;                                     // saveCategory
    r[0x147] = 100;                                      // bloodDamageRate
    r[0x15B] = 0xFF;
    r[0x15D] = 0x7F;
    put_s(r, 0x163, -1);
}

void scale_row(uint8_t* r, int n, int health, int damage, int posture) {
    base_row(r);
    const float hp = n * (health / 100.0f) + 1.0f;
    const float dmg = n * (damage / 100.0f) + 1.0f;
    put_f(r, 0x10, hp);                                  // maxHpRate
    for (int off = 0x38; off <= 0x44; off += 4) put_f(r, off, dmg);   // physics/magic/fire/thunder attack rate
    for (int off = 0x48; off <= 0x54; off += 4) put_f(r, off, 1.0f);  // attack power rates
    for (int off = 0x108; off <= 0x114; off += 4) put_f(r, off, hp);  // poison/toxic/bleed/curse resist rate
    // changeSuperArmorPoint as Remastered writes it; unread with stateInfo 0 (the posture is poise_sum's, below).
    put_s(r, 0x13A, static_cast<int16_t>(static_cast<int>(n * (posture / 100.0f) + 1.0f)));
}

void debuff_row(uint8_t* r, int n) {
    base_row(r);
    const float rate = n * -0.1f + 1.0f;
    for (int off = 0x34; off <= 0x54; off += 4) put_f(r, off, 1.0f);
    put_f(r, 0x10, rate);                                // maxHpRate
    for (int off = 0x108; off <= 0x114; off += 4) put_f(r, off, rate);
}

void build_rows() {
    for (int k = 0; k < 5; ++k) {
        scale_row(g_rows[k], k + 1, g_cfg.enemy_health, g_cfg.enemy_damage, g_cfg.enemy_posture);
        scale_row(g_rows[5 + k], k + 1, g_cfg.boss_health, g_cfg.boss_damage, g_cfg.boss_posture);
        debuff_row(g_rows[10 + k], k + 1);
    }
    g_rows_ready = true;
}

// ---------------------------------------------------------------- poise
// A character's poise is its own (vtable +0x160) plus what 0xDBF700 sums over its active effects: the
// changeSuperArmorPoint (+0x13A, int16 points) of each one whose stateInfo (+0x156) is 155, the Wolf Ring's type.
// Our scaling rows are stateInfo 0, as in Remastered Seamless Co-op, so their field was never read, and as
// (short)(1 + n * percent) it would only have added a point or two. 0xDBF700 is replaced: the game's sum is kept,
// and our enemy and boss rows add their posture percent of the character's own poise for each extra player.

// The posture rate of one of our scaling rows (n * percent / 100), 0 for any other row.
float posture_rate(uint32_t row) {
    const auto* r = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(row));
    if (r < g_rows[0] || r >= g_rows[10]) return 0;  // the enemy rows 0..4 and the boss rows 5..9
    const int i = static_cast<int>((r - g_rows[0]) / kRow);
    const int percent = i < 5 ? g_cfg.enemy_posture : g_cfg.boss_posture;
    const float rate = (i % 5 + 1) * (percent / 100.0f);
    return rate < -1.0f ? -1.0f : rate;
}

float __stdcall poise_sum(uint32_t effects) {
    using OwnPoiseFn = float(__thiscall*)(uint32_t chr);
    const uint32_t flags = *ptr<uint32_t>(effects + 0x14);
    float sum = 0, rate = 0;
    for (uint32_t node = *ptr<uint32_t>(effects + 4); node; node = *ptr<uint32_t>(node + 0x30)) {
        const uint32_t row = *ptr<uint32_t>(node + 0x2C);
        if ((*ptr<uint32_t>(node + 0x14) & 0x800C0003) != 0 || !row) continue;  // ending or not in effect
        if ((flags & 0xE00) != 0 && *ptr<uint8_t>(row + 0x156) == 155) sum += *ptr<int16_t>(row + 0x13A);
        rate += posture_rate(row);
    }
    const uint32_t chr = *ptr<uint32_t>(effects + 0x10);  // set by the character when it makes its SpecialEffect
    if (rate != 0 && chr && *ptr<uint32_t>(chr) == kEnemyVtable) {
        const auto own = reinterpret_cast<OwnPoiseFn>(*ptr<uint32_t>(kEnemyVtable + kOwnPoiseSlot));
        sum += own(chr) * rate;
    }
    return sum;
}

// eax = the SpecialEffect; returns in xmm0. Callers keep values in other xmm registers (the status screen's own
// poise in xmm3) and on the x87 stack (the others' own poise) across the call.
__declspec(naked) void hk_poise_sum() {
    __asm {
        push ecx
        push edx
        sub esp, 0x70
        movups [esp], xmm1
        movups [esp + 0x10], xmm2
        movups [esp + 0x20], xmm3
        movups [esp + 0x30], xmm4
        movups [esp + 0x40], xmm5
        movups [esp + 0x50], xmm6
        movups [esp + 0x60], xmm7
        push eax
        call poise_sum
        push eax
        fstp dword ptr [esp]
        movss xmm0, dword ptr [esp]
        add esp, 4
        movups xmm1, [esp]
        movups xmm2, [esp + 0x10]
        movups xmm3, [esp + 0x20]
        movups xmm4, [esp + 0x30]
        movups xmm5, [esp + 0x40]
        movups xmm6, [esp + 0x50]
        movups xmm7, [esp + 0x60]
        add esp, 0x70
        pop edx
        pop ecx
        ret
    }
}

void* __stdcall our_fx_row(int32_t id) {
    const int i = row_index(id);
    if (i < 0) return nullptr;
    if (!g_rows_ready) build_rows();
    return g_rows[i];
}

__declspec(naked) void hk_fx_row() {
    __asm {
        cmp edi, 2147418000
        jl orig
        cmp edi, 2147419004
        jg orig
        push edi
        call our_fx_row
        test eax, eax
        jz orig
        mov ecx, [esp + 4]
        mov [ecx], edi
        mov [ecx + 4], eax
        ret 4
    orig:
        jmp g_fx_orig
    }
}

// ---------------------------------------------------------------- effects on characters
// ChrIns::ApplySpEffect (0xE7E950, vtable slot 125): its last argument says the effect is this game's own. With
// 0 a guest's game (session state 6) refuses an effect on an enemy whose AI another game runs at that moment
// (0xE3E250 on its handle chr+0x78), and an applied effect is sent to the other players (0xE7E850) with its id
// cut to 17 bits, which turns ours (2147418000 + n) into 66448 + n. The guest's copy of an enemy the host was
// controlling kept its own max HP, while the host's HP sync (0xE40FE0: the host's HP every frame, SetHP on the
// guest) carried the scaled HP: clamped to the unscaled max, the bar stayed full until the host's HP fell below
// it, and a hit showed only until the next sync. Each game scales the enemies itself, so the scaling and the
// death debuff are applied as this game's own (1): no refusal and nothing sent. The game's answer is returned,
// so a refusal is tried again.
using ApplyFn = char(__fastcall*)(uint32_t chr, void*, int id, uint32_t src, int a, float f1, float f2, float f3, int own);

bool apply(uint32_t chr, int id) {
    uint32_t vt = 0, fn = 0;
    if (!chr || !read32(chr, &vt) || !read32(vt + kApplySlot * 4, &fn)) return false;
    __try {
        return reinterpret_cast<ApplyFn>(fn)(chr, nullptr, id, chr, 0, 1.0f, 1.0f, 1.0f, 1) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t find_effect(uint32_t chr, int id, uint32_t* ctrl_out) {
    uint32_t ctrl = 0, node = 0;
    if (!chr || !read32(chr + 0x1E0, &ctrl) || !ctrl || !read32(ctrl + 4, &node)) return 0;
    for (int guard = 0; node && guard < 512; ++guard) {
        uint32_t nid = 0;
        if (!read32(node + 0x28, &nid)) return 0;
        if (static_cast<int>(nid) == id) {
            if (ctrl_out) *ctrl_out = ctrl;
            return node;
        }
        if (!read32(node + 0x30, &node)) return 0;
    }
    return 0;
}

void remove(uint32_t chr, int id) {
    uint32_t ctrl = 0;
    const uint32_t node = find_effect(chr, id, &ctrl);
    if (!node) return;
    __try {
        __asm {
            push 1
            push ctrl
            mov eax, node
            mov ecx, kRemoveEffectNode
            call ecx
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int hp(uint32_t chr) {
    uint32_t v = 0;
    return read32(chr + 0x2D4, &v) ? static_cast<int>(v) : 0;
}
int max_hp(uint32_t chr) {
    uint32_t v = 0;
    return read32(chr + 0x2D8, &v) ? static_cast<int>(v) : 0;
}

// ---------------------------------------------------------------- rests
// A guest's rest in the host's world and the host's NPCs. A rest's world reset gives the human NPCs it does not
// rebuild (NpcParam+0x148 bit 1) SpEffect 110, a full heal (0xE64070), and the rest restarts events such as the
// Undead Asylum's 11815010, which puts Oscar back into his dying state with SpEffect 5492 (99.9 % of his HP: 400
// -> 1). In vanilla only a player alone in their world can rest, and the heal comes first. In a guest's game the
// event came first: 1 HP minus 99.9 % left him at 0 (2026-10-13 test: "HP 0/400" 58 ms after the guest's rest), a
// body without collision with his drop lying in it, though he could still be talked to. So right before a guest
// rests, the host's NPCs (teams 8 Ally and 9 Hostile Ally) that are alive in the guest's game get their health
// back first, as the reset would give them.
void heal_host_npcs() {
    uint32_t wcm = 0, sets_info = 0, nsets = 0;
    if (!read32(kWorldChrManVar, &wcm) || !wcm || !read32(wcm + 0x10, &sets_info) || !sets_info ||
        !read32(sets_info + 0xC, &nsets)) {
        return;
    }
    int healed = 0;
    for (uint32_t k = 0; k < nsets + 2 && k < 64; ++k) {
        uint32_t set = 0, n = 0, entries = 0;
        if (!read32(wcm + 0x240 + k * 4, &set) || !set || !read32(set, &n) || !read32(set + 4, &entries)) continue;
        for (uint32_t i = 0; i < n && i < 4096; ++i) {
            uint32_t chr = 0, team = 0, hp = 0, max = 0;
            if (!read32(entries + i * 0x20, &chr) || !chr || !read32(chr + 0x74, &team) || (team != 8 && team != 9) ||
                !read32(chr + 0x2D4, &hp) || !read32(chr + 0x2D8, &max)) {
                continue;
            }
            if (static_cast<int>(hp) <= 0 || static_cast<int>(max) <= 0 || hp >= max) continue;
            *ptr<uint32_t>(chr + 0x2D4) = max;
            ++healed;
        }
    }
    if (healed) LOG_INFO("[coop] %d of the host's NPCs healed before our rest, as the rest would heal them", healed);
}

// Util_RequestRegene (the rest that respawns the world). As in Remastered Seamless Co-op the co-op items the
// player lacks are given and every Crimson Blossom withers before the game's own rest; afterwards the other
// players are told (their worlds reset too).
void __stdcall before_rest(int object_id) {
    LOG_INFO("[coop] Rested at a bonfire (object %d)", object_id);
    rules::note_world_reset();
    const auto& st = rules::state();
    if (st.active && st.guest && !st.invader && !st.leaving && !game::own_world()) heal_host_npcs();
    // The guest's sit timer never writes GameMan+0xB04, so a death kept using the bonfire from the join
    // (Firelink, in the test). The respawn id is the object id plus 1000.
    session::note_guest_rest(object_id);
    if (g_before_rest_fn) g_before_rest_fn();
}

void __stdcall after_rest() {
    if (g_rest_fn && g_active && !g_invader) g_rest_fn();
    deadlist::note_rest();
    reset_after_rest("We");
    session::report_reset_phase();  // for this rest, whether our world reloads or not
}

__declspec(naked) void hk_regene() {
    __asm {
        pushad
        push dword ptr [esp + 36]  // stdcall arg: was [esp+4], pushad added 32
        call before_rest
        popad
        push dword ptr [esp + 4]
        call g_regene_orig
        pushad
        call after_rest
        popad
        ret 4
    }
}

// ---------------------------------------------------------------- the return to the bonfire
// PlayerIns's effect update (0xE51100) asks 0xDC2A40 every frame whether an effect with stateInfo 16 (row +0x156) is
// on the character; for the local player it then calls 0xD76390(1) (esi = [0x137D790]), which clears the summon
// state and kicks WarpNextStageKick (0xD62930) to the last bonfire. The call site, `push edi ; call 0xD76390`:
constexpr uint32_t kHomewardStateCall = 0x00E5143C;
constexpr uint32_t kHomewardWarp = 0x00D76390;
uint32_t g_homeward_warp = kHomewardWarp;
bool g_homeward_hooked = false;
bool (*g_homeward_state)() = nullptr;
uint8_t g_homeward_skip = 0;

void __cdecl homeward_state_decide() { g_homeward_skip = g_homeward_state && g_homeward_state() ? 1 : 0; }

__declspec(naked) void hk_homeward_state() {
    __asm {
        pushad
        call homeward_state_decide
        popad
        cmp byte ptr [g_homeward_skip], 0
        jne skip
        jmp dword ptr [g_homeward_warp]
    skip:
        ret 4
    }
}

// ---------------------------------------------------------------- limbo
// The respawn warp waits instead of the death itself: only when the death event cannot be held (spectate.cpp).
bool limbo_wanted() {
    return g_active && !g_invader && !g_trip_release && local_dead() && g_players >= 2 && !g_everyone_dead &&
           (boss_fight() || rules::state().teammate_boss_fight || g_invader_present);
}

bool debuff_on(uint32_t me) {
    for (int id = kDebuff; id < kDebuff + 5; ++id) {
        if (find_effect(me, id, nullptr)) return true;
    }
    return false;
}

// The Crimson Blossoms wither outside our own rest (before_rest withers them there, and the rest heals): another
// player's rest, or a death whose reload starts the world over (a team wipe, or no teammate to keep the world for).
// The debuff is the price of a death that keeps the world; once the world's enemies are back, it goes. While we
// live, the health it took comes back with the bar (a reload starts at full health anyway).
void wither_blossoms(const char* why, bool tell) {
    if (!g_wither_fn) return;
    const uint32_t me = game::player();
    if (!me || game::loading()) {
        g_wither_pending = why;
        g_wither_tell = tell;
        return;
    }
    g_wither_pending = nullptr;
    const int held = g_blossom_count_fn ? g_blossom_count_fn() : 0;
    if (held > 0) {
        LOG_INFO("[coop] %d Crimson Blossom(s) wither: %s", held, why);
        if (g_cfg.death_debuffs && hp(me) > 0) {
            g_refill.blossoms = std::min(5, held);
            g_refill.hp = hp(me);
            g_refill.max = max_hp(me);
            g_refill.grow = debuff_on(me);
            g_refill.until = now() + 3.0;
        }
        g_wither_fn();
        if (tell) text::show(L"Another player rested: your Crimson Blossoms withered");
    }
    clear_death_debuff();
}

// Our death's respawn warp goes. In a session with a co-op teammate the reload keeps the world's dead characters
// dead, as they are in the other games (deadlist.cpp): a death costs a Crimson Blossom, not the world. A team wipe
// starts the world over in every game, and so does a rest (deadlist::note_rest). Decided now, when the warp goes,
// after any wait: the teammates whose respawn went before ours say how theirs went, and ours follows them
// (session::wipe_reload). True when the reload keeps the world (the others are told with our "went"). When it
// does not, the Crimson Blossoms wither, as at a rest, and the other games' enemy positions from before are dropped.
void arm_pos_drain();
void give_death_blossom();
bool keep_dead_on_reload() {
    if (!g_active || g_invader || !local_dead()) return false;
    bool keep = false;
    if (g_players < 2) {
        wither_blossoms("our reload starts the world over (no teammate)", false);
    } else if (session::wipe_reload(g_fell_at)) {
        LOG_INFO("[coop] Team wipe: our reload starts the world over");
        wither_blossoms("team wipe", false);
    } else {
        keep = deadlist::arm(game::own_world());
        if (!keep) wither_blossoms("our reload starts the world over", false);
    }
    if (!keep) arm_pos_drain();
    if (keep) give_death_blossom();
    return keep;
}

// The game's own ways of sending a summoned player home (the host left or died, the room is gone, a kick) end
// with SetSosSignWarp (GameMan+0xB00 = 1: go back to where the sign was, in the player's own world),
// SetDefaultMapUid(-1) and this warp. A co-op guest stays in the host's world (our session code sends it home
// when the session really ends): the warp becomes a respawn at its bonfire in the host's world, instead of a
// drop at the sign's coordinates into the host's map (the endless fall of the 2026-10-07 test).
void keep_guest_in_world() {
    const auto& st = rules::state();
    const uint32_t gm = game::game_man();
    uint8_t sign_warp = 0;
    if (!st.active || !st.guest || st.invader || st.leaving || !gm || !read8(gm + 0xB00, &sign_warp) || !sign_warp) {
        return;
    }
    *ptr<uint8_t>(gm + 0xB00) = 0;
    const int bonfire = game::last_bonfire();
    const int map = game::bonfire_map(bonfire);
    if (map != -1) {
        *ptr<int>(gm + game::kGmWarpMap) = map;
        *ptr<int>(gm + game::kGmWarpPoint) = -1;
        *ptr<uint8_t>(gm + game::kGmUseBonfire) = 1;
    }
    LOG_INFO("[coop] The game tried to send us home; respawning at bonfire %d in the host's world instead", bonfire);
}

// SetSosSignWarp (0xD5E070, GameMan+0xB00 = 1): the next SetDefaultMapUid then picks the map the player was
// summoned from (GameMan+0xAD0) instead of their bonfire. In one's own world nobody was summoned from anywhere:
// that map was never filled in, and loading it ends at the title screen with a save that has no floor. The
// request is dropped there, so whatever asked for it gets an ordinary reload at the last bonfire.
constexpr uint32_t kSetSignWarp = 0x00D5E070;
using VoidFn = void(__cdecl*)();
VoidFn g_sign_warp_orig = nullptr;

void __cdecl hk_sign_warp() {
    if (game::own_world()) {
        static int lines = 0;
        if (++lines <= 20) LOG_INFO("[coop] A return to the summon sign was asked for in our own world; not set");
        return;
    }
    g_sign_warp_orig();
}

char __cdecl hk_warp_kick() {
    if (local_dead()) g_death_warp = true;
    if (!g_bypass) keep_guest_in_world();
    if (!g_bypass && !g_limbo && !spectate::installed() && limbo_wanted()) {
        g_limbo = true;
        g_rest_heard = false;
        g_limbo_since = now();
        LOG_INFO("[coop] Limbo: the respawn waits until the fight ends, everyone falls or someone rests");
        return 1;
    }
    // Every co-op player has fallen: the respawn waits until every game can reload at once (released in tick).
    if (!g_bypass && g_team_hold) return 1;
    if (!g_bypass && !g_limbo && !g_team_asked && g_active && !g_invader && local_dead()) {
        g_team_asked = true;
        if (session::team_respawn_wait()) {
            g_team_hold = true;
            return 1;
        }
    }
    // A boss's rewards still to come in this game (reward_pending): released in tick.
    if (!g_bypass && g_reward_hold) return 1;
    if (!g_bypass && g_active && !g_invader && local_dead()) {
        if (const char* why = reward_pending()) {
            g_reward_hold = true;
            LOG_INFO("[coop] Our respawn waits for %s", why);
            return 1;
        }
    }
    // Before the original body sets the warp request. A death is aimed at the bonfire; the empty map is not loaded.
    if (!g_bypass && rules::prepare_warp_kick(local_dead())) return 1;
    if (!g_bypass && local_dead()) {
        if (g_active) {
            session::note_respawn_went(keep_dead_on_reload());
        } else {
            wither_blossoms("our death's reload starts the world over", false);
        }
    }
    return g_warp_orig();
}

// ---------------------------------------------------------------- boss fogs sealed while an invader is here
// A boss fog's entry is an EMEVD "Action Button - Boss Room" (3[19]); its test (EcTestActionButton 0xBBD7E0) greys
// the button out for the game's own reasons (state effect 185, a Gravelord or Path of the Dragon phantom in the
// party: invade types 7 and 10). Any other invader leaves it open, the invader cannot follow (the fog takes
// living players and white phantoms), and the host's entry sends the invader home (the boss room notification,
// 2009[06]: Lua 4068 ReportBossArea -> Failed_BossAreaMission, and 0xEA2C80 drops black phantoms from the party).
// With SealBossFogsDuringInvasions (the host's; off by default) the fog stays shut for the host and the guests while
// an invader is in the world, so a fight with an invader cannot be ended by walking into a boss: our answer joins the
// game's at 0xBBD86C. Off, the fogs work as in vanilla.
constexpr uint32_t kBossFogTest = 0x00BBD86C;   // mov ecx,[0137D644h] ; xor eax,eax (the boss-room checks begin)
constexpr uint32_t kBossFogShut = 0x00BBD8AB;   // mov byte [esp+10h],0: the button greyed out
uint32_t g_fog_shut_at = kBossFogShut;
bool g_fog_told = false;

bool __cdecl boss_fog_sealed() {
    const bool sealed = g_active && !g_invader && g_invader_present && session::seal_boss_fogs();
    if (sealed) {
        static double told = -100;
        if (now() - told > 60.0) {
            told = now();
            LOG_INFO("[coop] A boss fog stays shut: an invader is in the world");
        }
    }
    return sealed;
}

__declspec(naked) void hk_boss_fog() {
    __asm {
        pushad
        call boss_fog_sealed
        test al, al
        popad
        jnz shut
        mov ecx, 0x0137D644
        mov ecx, [ecx]
        xor eax, eax
        ret
    shut:
        add esp, 4  // our return address: the game's frame as at 0xBBD86C
        jmp g_fog_shut_at
    }
}

bool install_boss_fog() {
    static const uint8_t k[8] = {0x8B, 0x0D, 0x44, 0xD6, 0x37, 0x01, 0x33, 0xC0};
    uint8_t code[8] = {0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_boss_fog) - (kBossFogTest + 5));
    std::memcpy(code + 1, &rel, 4);
    return patch(kBossFogTest, k, code, sizeof(code));
}

// ---------------------------------------------------------------- enemy positions from before a reload
// The other games' enemy positions (packet type 3) wait in the session's queue while our map loads; on the first frame
// after it the net update (0xE57D01: `do {} while (0xE556F0(table))`, bool stdcall(table), one packet a member a call)
// takes them all into the table (WorldChrMan+0x2B4: pages [table+8+block*4], a flags byte each at [page+0x1C]; bit 0 a
// new position, taken once by the character, 0xE55360). Net handles are the same in every load of a map, so after a
// team wipe a position from another game's last moments of the old world went to our new copy of that enemy. The
// Taurus Demon stood where it had stood when the team fell, its AI off by its event (11015382) and moved by no game,
// and its entrance leap from there took it off the bridge to its death (2026-10-25, the host's game, which loaded 0.3 s
// before the guest's: the guest's game had moved the demon at the end). After a reload that starts the world over in
// every game (a team wipe, a lost boss fight), the new positions that come in our first 1.5 s are dropped: the
// enemies stay where the map makes them until the games send positions from the new world.
constexpr uint32_t kPosReceiveCall = 0x00E57D01;
constexpr uint32_t kPosReceive = 0x00E556F0;
constexpr double kPosDrainTime = 1.5;
bool g_pos_drain_armed = false;    // the next load starts the world over in every game
bool g_pos_drain_loading = false;  // ... and it has begun
double g_pos_drain_armed_at = 0;
double g_pos_drain_until = 0;      // dropping until then (0: not)
int g_pos_dropped = 0;

int clear_new_positions(uint32_t table) {
    uint32_t n1 = 0;
    if (!table || !read32(table + 4, &n1) || n1 > 0x20) return 0;
    int cleared = 0;
    for (uint32_t i = 0; i < n1; ++i) {
        uint32_t page = 0, n2 = 0, flags = 0;
        if (!read32(table + 8 + i * 4, &page) || !page || !read32(page + 8, &n2) || n2 > 0x800 ||
            !read32(page + 0x1C, &flags) || !flags) {
            continue;
        }
        for (uint32_t k = 0; k < n2; ++k) {
            uint8_t* f = ptr<uint8_t>(flags + k);
            if (*f & 1) {
                *f &= 0xFE;
                ++cleared;
            }
        }
    }
    return cleared;
}

// From the load's start (seen by pos_drain_tick) until kPosDrainTime after its end: the net update may run before our
// tick in the first frame after the load.
bool __stdcall hk_pos_receive(uint32_t table) {
    const bool got = reinterpret_cast<bool(__stdcall*)(uint32_t)>(kPosReceive)(table);
    if (got && ((g_pos_drain_armed && g_pos_drain_loading) || (g_pos_drain_until != 0 && now() < g_pos_drain_until))) {
        __try {
            g_pos_dropped += clear_new_positions(table);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return got;
}

void arm_pos_drain() {
    g_pos_drain_armed = true;
    g_pos_drain_loading = false;
    g_pos_drain_armed_at = now();
}

void pos_drain_tick() {
    if (g_pos_drain_until != 0 && now() >= g_pos_drain_until) {
        LOG_INFO("[coop] The world started over: %d enemy position(s) from before our reload dropped", g_pos_dropped);
        g_pos_drain_until = 0;
    }
    if (!g_pos_drain_armed) return;
    if (game::loading()) {
        if (!g_pos_drain_loading) g_pos_dropped = 0;
        g_pos_drain_loading = true;
    } else if (g_pos_drain_loading && game::player()) {
        g_pos_drain_armed = false;
        g_pos_drain_loading = false;
        g_pos_drain_until = now() + kPosDrainTime;
    } else if (now() - g_pos_drain_armed_at > 30.0) {
        g_pos_drain_armed = false;  // no load came
    }
}

// ---------------------------------------------------------------- a boss's rewards for a fallen player
// Every boss's rewards come from common event 970(defeat flag, lot 1, lot 2, lot 3): when the flag turns on it awards
// lot 1 to every player (2003[36]), then 5 s later lots 2 and 3; its first line ends it if the flag is already on
// when it starts, so a load in between loses the rest for good. On 2026-10-26 the host, spectating the guest's Asylum
// Demon fight, respawned 4 s after flag 16 came: it got the Big Pilgrim's Key (lot 2660) but not the Humanity (lot
// 9000). A fallen player's respawn waits for a boss's rewards: until 9 s after a defeat flag turned on in this game
// (the last reward comes 5 s after the flag, then its box stays on screen a while: at 6.5 s the Humanity's box was up
// for 1.5 s before the fade, 2026-10-26), and, after a boss fight we fell in was won (our gauge up, its boss dead)
// with no flag yet (a long death animation), up to 12 s. The flags are every 970's first parameter in the common
// events.
constexpr int kBossDefeatFlags[] = {2,        3,        4,        5,        6,        7,        9,        10,
                                    11,       13,       14,       15,       16,       17,       11010901, 11010902,
                                    11200900, 11210000, 11210001, 11210004, 11410410, 11410900, 11410901, 11510900,
                                    11510902, 11510903, 11810900};
constexpr double kRewardAfterFlag = 9.0;
constexpr double kRewardWait = 12.0;
uint32_t g_defeat_on = 0;        // bit i: kBossDefeatFlags[i] read on lately
bool g_defeat_known = false;     // g_defeat_on holds this load's values
uint32_t g_defeat_store = 0;     // the flag store they were read from (EventFlagMan+0x10C)
double g_defeat_at = 0;          // when a defeat flag last turned on in this game (0 never)
double g_fight_over_at = 0;      // a boss fight ended while we were down (0 none)
bool g_was_fight = false;

uint32_t boss_handle(int slot);

void defeat_tick() {
    const uint32_t me = game::player();
    if (game::loading() || !me) {
        g_defeat_known = false;
        return;
    }
    // A session's end switches the flags read from the visited world's store to our own before the load home, where
    // a boss beaten in one world and not the other would look defeated. A switch of store starts the comparison over.
    // (The Moonlight Butterfly's 11200900 at the end of an invasion, 2026-10-26, was no such switch: it really died in
    // the invaded world, which is what sent the invader home.)
    uint32_t efm = 0, store = 0;
    if (read32(0x0137D7D4, &efm) && efm && read32(efm + 0x10C, &store) && store != g_defeat_store) {
        g_defeat_store = store;
        g_defeat_known = false;
    }
    uint32_t on = 0;
    for (size_t i = 0; i < sizeof(kBossDefeatFlags) / sizeof(kBossDefeatFlags[0]); ++i) {
        if (game::flag(kBossDefeatFlags[i])) on |= 1u << i;
    }
    if (g_defeat_known && (on & ~g_defeat_on)) {
        g_defeat_at = now();
        for (size_t i = 0; i < sizeof(kBossDefeatFlags) / sizeof(kBossDefeatFlags[0]); ++i) {
            if ((on & ~g_defeat_on) & (1u << i)) {
                LOG_INFO("[coop] Boss defeat flag %d is on: its rewards come over the next 5 s", kBossDefeatFlags[i]);
            }
        }
    }
    g_defeat_on = on;
    g_defeat_known = true;
    const bool fight = boss_fight() || rules::state().teammate_boss_fight;
    if (!local_dead()) {
        g_fight_over_at = 0;
    } else if (g_was_fight && !fight) {
        // Won: our gauge is still up with its boss dead (it comes down 10 s later). A lost fight or a rest is not
        // waited for here.
        for (int slot = 0; slot < 2; ++slot) {
            const uint32_t h = boss_handle(slot);
            if (h != 0xFFFFFFFF && rules::gauge_boss_dead(h)) g_fight_over_at = now();
        }
    }
    g_was_fight = fight;
}

const char* reward_pending() {
    const double t = now();
    if (g_defeat_at > 0 && t - g_defeat_at < kRewardAfterFlag) return "a boss's later rewards";
    if (g_fight_over_at > 0 && t - g_fight_over_at < kRewardWait && g_defeat_at < g_fight_over_at - 2.0) {
        return "a boss's defeat (its rewards)";
    }
    return nullptr;
}

// Our death keeps the world: it costs a Crimson Blossom, unless the team defeated a boss while we were down (the
// fight we fell in was won). Shown on screen, as the game's own item boxes are not (items.cpp).
void give_death_blossom() {
    if (!g_cfg.death_debuffs || g_invader || !g_blossom_give_fn) return;
    if (g_fell_at > 0 && g_defeat_at >= g_fell_at) {
        LOG_INFO("[coop] No Crimson Blossom for this death: the team defeated a boss while we were down");
        return;
    }
    if (!g_blossom_give_fn()) return;
    const int n = g_blossom_count_fn ? g_blossom_count_fn() : 1;
    LOG_INFO("[coop] A Crimson Blossom for this death (%d held)", n);
    text::show(L"Crimson Blossom received: maximum HP lowered until anyone rests");
}

// The respawn warp that waited (limbo, a team wipe) goes ahead.
void respawn_now() {
    if (rules::prepare_warp_kick(local_dead())) return;
    if (g_active) session::note_respawn_went(keep_dead_on_reload());
    g_bypass = true;
    g_warp_orig();
    g_bypass = false;
}

// Limbo's respawn goes ahead; after a team wipe it waits for the team's at the barrier like any other.
void release_limbo(const char* why, bool wipe = false) {
    if (!g_limbo) return;
    g_limbo = false;
    LOG_INFO("[coop] Limbo ends (%s)", why);
    if (wipe && !g_team_asked && local_dead()) {
        g_team_asked = true;
        if (session::team_respawn_wait()) {
            g_team_hold = true;
            return;
        }
    }
    respawn_now();
}

// Boss gauges on screen (limbo waits for a boss fight to end).
uint32_t boss_handle(int slot) {
    uint32_t mm = 0, h = 0xFFFFFFFF;
    if (!read32(kMenuManVar, &mm) || !mm) return 0xFFFFFFFF;
    read32(mm + (slot ? 0xF78 : 0xF64), &h);
    return h;
}

// ---------------------------------------------------------------- scaling
// Every enemy gets the scaling row for the number of other co-op players in the session (1..5); bosses get the boss
// row, as in Remastered Seamless Co-op: characters on team 7, and every boss with a health bar (rules::gauge_boss_chrs),
// since some are on the enemies' team 6: the Bell Gargoyles had the enemy row (2026-10-25: the second one died at
// 1799 max HP, 999 x 1.8, against the boss row's 3097, and Solaire seemed to cut through it).
constexpr int kBossTeam = 7;

// The game's own boss buff for co-op (EMEVD 2009[04] Activate Multiplayer-dependant Buffs, at almost every boss fight's
// start: 0xD77620, edx = the event proxy, stdcall(entity), runs the script Lua_MultiDoping) came on top of the boss row:
// the first gargoyle's 2698 max HP was 999 x 1.8 x 1.5. Remastered Seamless Co-op makes its ApplyMultiDoping do
// nothing; so do the two calls here (0xBC0189, 0xBC01A0) while Prepare to Co-op is on.
constexpr uint32_t kMultiDoping = 0x00D77620;
constexpr uint32_t kMultiDopingSites[] = {0x00BC0189, 0x00BC01A0};

void __stdcall no_multi_doping(int entity) {
    static std::set<int> logged;
    if (logged.size() < 40 && logged.insert(entity).second) {
        LOG_INFO("[coop] Boss %d: the game's own co-op buff left out (BossHealthScaling does it)", entity);
    }
}

template <typename F>
void each_enemy(F&& fn) {
    uint32_t wcm = 0, sets_info = 0, nsets = 0;
    if (!read32(kWorldChrManVar, &wcm) || !wcm || !read32(wcm + 0x10, &sets_info) || !sets_info ||
        !read32(sets_info + 0xC, &nsets)) {
        return;
    }
    for (uint32_t k = 0; k < nsets + 2 && k < 64; ++k) {
        uint32_t set = 0, n = 0, entries = 0;
        if (!read32(wcm + 0x240 + k * 4, &set) || !set || !read32(set, &n) || !read32(set + 4, &entries)) continue;
        for (uint32_t i = 0; i < n && i < 4096; ++i) {
            uint32_t chr = 0, vt = 0;
            if (!read32(entries + i * 0x20, &chr) || !chr || !read32(chr, &vt)) continue;
            if (vt == kEnemyVtable) fn(chr);
        }
    }
}

// Diagnostic for the 2026-10-13 report (an enemy the guest hit died with health left on the guest's bar): every
// 0.1 s the hurt enemies' health is sampled, and a death that came while this game last showed more than 10 % is
// logged with both numbers, in both games. Each game's scaling level is logged when it changes.
std::map<uint32_t, std::pair<int, int>> g_enemy_hp;  // EnemyIns -> last HP, max HP while hurt
int g_logged_level = -1;
int g_hp_left_logs = 0;

void enemy_hp_tick() {
    static double last = 0;
    if (!g_active || now() - last < 0.1) return;
    last = now();
    std::set<uint32_t> seen;
    each_enemy([&](uint32_t chr) {
        const int h = hp(chr), m = max_hp(chr);
        auto it = g_enemy_hp.find(chr);
        if (h <= 0) {
            if (it != g_enemy_hp.end() && it->second.second > 0 && it->second.first * 10 > it->second.second &&
                ++g_hp_left_logs <= 40) {
                LOG_INFO("[coop] An enemy died here with %d/%d HP still shown 0.1 s before (scaling level %d)",
                         it->second.first, it->second.second, g_logged_level);
            }
            if (it != g_enemy_hp.end()) g_enemy_hp.erase(it);
            return;
        }
        if (h >= m) {
            if (it != g_enemy_hp.end()) g_enemy_hp.erase(it);
            return;
        }
        seen.insert(chr);
        g_enemy_hp[chr] = {h, m};
    });
    for (auto it = g_enemy_hp.begin(); it != g_enemy_hp.end();) {
        it = seen.count(it->first) ? std::next(it) : g_enemy_hp.erase(it);
    }
}

// The max HP our scaling row gives an enemy whose max HP is `base` (the game multiplies by the row's maxHpRate).
int scaled_max(int base, int id) {
    const int i = row_index(id);
    if (i < 0 || !g_rows_ready) return base;
    float rate = 1.0f;
    std::memcpy(&rate, g_rows[i] + 0x10, 4);
    return static_cast<int>(static_cast<float>(base) * rate);
}

// The game works out an enemy's max HP from its effects only while it updates the enemy: one far from this game's
// player (its update rate) keeps the max HP it had, with our scaling effect on it. HP and damage go between the
// games as numbers, so that copy was hurt by the same amounts as a nearby, scaled one and died first: in the
// 2026-10-23 test the host at Firelink saw a Burg hollow die at 14/85 HP while the guest's copy stood at 43/114,
// each down 71, and a Black Knight died in the host's game part of the way through the guest's fight. Such a copy
// gets the scaled max HP here (the damage it took kept), as the game itself gives it once the enemy updates again.
void settle_max(uint32_t chr, int want) {
    const auto b = g_base_max.find(chr);
    if (b == g_base_max.end() || b->second <= 0) return;
    const int base = b->second, m = max_hp(chr), h = hp(chr);
    const auto f = g_forced_max.find(chr);
    if (!want) {
        // Our scaling is off again: a max HP we wrote goes back, if the game has not changed it since.
        if (f == g_forced_max.end()) return;
        if (m == f->second && m > 0 && h > 0) {
            *ptr<int>(chr + 0x2D8) = base;
            *ptr<int>(chr + 0x2D4) = std::max(1, std::min(base, h >= m ? base : base - (m - h)));
        }
        g_forced_max.erase(f);
        return;
    }
    const auto at = g_scaled_at.find(chr);
    if (at == g_scaled_at.end() || now() - at->second < 1.5) return;
    const int expected = scaled_max(base, want);
    const bool stale = m == base || (f != g_forced_max.end() && m == f->second);
    if (!stale || std::abs(m - expected) <= 1 || h <= 0) return;
    *ptr<int>(chr + 0x2D8) = expected;
    *ptr<int>(chr + 0x2D4) = std::max(1, std::min(expected, h >= m ? expected : expected - (m - h)));
    g_forced_max[chr] = expected;
    static int lines = 0;
    if (++lines <= 20) {
        LOG_INFO("[coop] An enemy this game does not update kept its own max HP: scaled here (%d/%d -> %d/%d)", h, m,
                 hp(chr), expected);
    }
}

// An enemy whose scaling changed keeps its share of health. The game works its max HP out again when it next updates
// it (0xE6C850): the growth is added to its health, a smaller max cuts the health down to it, and the effects that
// change health (0xE72E20) come after, against the new max. So the health it had lost before stayed lost in points:
// a boss at half health when a player joined came back to three quarters, and one that a leaving player took back
// to its own max was whole again. The second Bell Gargoyle comes in at 48 %: its own SpEffect 5490 (NpcParam 535001)
// takes 52 % of its max when it is made. Applied after our row it took 52 % of the scaled max, and our top-up of an
// enemy that had been whole (at the next scaling tick, half a second later) filled it again: it came in whole
// (2026-10-25). Applied before our row, the growth made it 76 %. Once the game has the new max, the health is set to
// the old share of it, less whatever the enemy lost since the change (a hit, such an effect). True when done; an
// enemy the game does not update keeps its max, and settle_max scales that one. A guest leaves the health of an enemy
// the host's game reports to the host: the host's own result comes through the game's HP sync (each change of it),
// and worked out here as well, on top of the host's number, the loss would count twice.
bool keep_share(uint32_t chr, const Fill& f) {
    const int h = hp(chr), m = max_hp(chr);
    if (h <= 0 || m <= 0 || f.max <= 0) return true;
    if (session::guest() && rules::host_hp_report(chr) >= 0) return true;
    if (m == f.max) return now() - f.at > 1.0;
    int lost = 0;
    if (h != f.hp) {
        const int game = m > f.max ? f.hp + (m - f.max) : std::min(f.hp, m);  // the game's own result, nothing lost
        lost = std::max(0, game - h);
    }
    const double share = std::min(1.0, static_cast<double>(f.hp) / f.max);
    const int want = std::max(1, std::min(m, static_cast<int>(share * m + 0.5) - lost));
    if (want != h) *ptr<int>(chr + 0x2D4) = want;
    return true;
}

void scale_tick() {
    enemy_hp_tick();
    if (now() - g_last_scale < 0.5) return;
    g_last_scale = now();
    int level = g_active ? g_players - 1 : 0;
    level = std::max(0, std::min(5, level));
    if (level != g_logged_level) {
        LOG_INFO("[coop] Enemy scaling level %d (%d co-op player(s)): health x%.2f, bosses x%.2f", level, level + 1,
                 level * (g_cfg.enemy_health / 100.0f) + 1.0f, level * (g_cfg.boss_health / 100.0f) + 1.0f);
        g_logged_level = level;
    }
    std::set<uint32_t> seen;
    std::vector<uint32_t> gauge_bosses;
    rules::gauge_boss_chrs(&gauge_bosses);
    each_enemy([&](uint32_t chr) {
        seen.insert(chr);
        uint32_t team = 0;
        read32(chr + 0x74, &team);
        const bool boss = static_cast<int>(team) == kBossTeam ||
                          std::find(gauge_bosses.begin(), gauge_bosses.end(), chr) != gauge_bosses.end();
        const int want = level ? (boss ? kScaleBoss : kScaleEnemy) + level - 1 : 0;
        auto it = g_scaled.find(chr);
        const int have = it == g_scaled.end() ? 0 : it->second;
        const auto fill = g_fill_hp.find(chr);
        if (fill != g_fill_hp.end() && keep_share(chr, fill->second)) g_fill_hp.erase(fill);
        if (hp(chr) <= 0) return;
        // Applied once is not enough: an effect the game has since taken off the enemy is applied again.
        if (!(want == have && (!want || find_effect(chr, want, nullptr)))) {
            const int before = max_hp(chr);
            const Fill was{hp(chr), before, now()};
            if (have) remove(chr, have);
            if (want && apply(chr, want)) {
                if (!have && !g_base_max.count(chr)) {
                    g_base_max[chr] = before;
                    g_scaled_at[chr] = now();
                }
                g_scaled[chr] = want;
                if (!g_fill_hp.count(chr)) g_fill_hp[chr] = was;
            } else {
                g_scaled.erase(chr);
                if (have && !g_fill_hp.count(chr)) g_fill_hp[chr] = was;
                uint32_t handle = 0;
                read32(chr + 0x78, &handle);
                static int refused = 0;
                if (want && ++refused <= 10) {
                    LOG_INFO("[coop] The game refused the scaling on an enemy (handle %d, HP %d/%d); trying again",
                             static_cast<int>(handle), hp(chr), max_hp(chr));
                }
            }
        }
        if (!g_fill_hp.count(chr)) settle_max(chr, g_scaled.count(chr) ? want : 0);
    });
    for (auto it = g_scaled.begin(); it != g_scaled.end();) {
        it = seen.count(it->first) ? std::next(it) : g_scaled.erase(it);
    }
    for (auto it = g_fill_hp.begin(); it != g_fill_hp.end();) {
        it = seen.count(it->first) ? std::next(it) : g_fill_hp.erase(it);
    }
    for (auto it = g_base_max.begin(); it != g_base_max.end();) {
        it = seen.count(it->first) ? std::next(it) : g_base_max.erase(it);
    }
    for (auto it = g_scaled_at.begin(); it != g_scaled_at.end();) {
        it = seen.count(it->first) ? std::next(it) : g_scaled_at.erase(it);
    }
    for (auto it = g_forced_max.begin(); it != g_forced_max.end();) {
        it = seen.count(it->first) ? std::next(it) : g_forced_max.erase(it);
    }
}

// ---------------------------------------------------------------- our own world's bonfires
// The bonfires' state lives in a table beside the event flags ([[0x137E204]+0xB48], a list at +0x24 of nodes {next,
// ?, entry}; entry 0x14 bytes: +4 the bonfire object, +8/+0xA two counts, +0xC lit, +0xD state, +0xE kindle level;
// 0xBD8870 finds one, 0xBD8960 sets +0xD/+0xE, the bonfire menu's Kindle reads +0xE). It is not part of the flag
// stores, so a visit to another world can leave that world's bonfires in it: after the host invaded a world further on
// (2026-10-25), their own Undead Burg bonfire gave 15 Estus (kindled twice), which no one had kindled. While we are in
// our own world the table is copied twice a second; back home from another world, our bonfires whose kindling changed
// meanwhile get theirs back. A change at home is logged with what the session was doing, to find any other way in.
constexpr uint32_t kBonfireOwner = 0x0137E204;
struct BonfireState {
    uint16_t a = 0, b = 0;
    uint8_t lit = 0, state = 0, kindle = 0;
    bool operator==(const BonfireState& o) const {
        return a == o.a && b == o.b && lit == o.lit && state == o.state && kindle == o.kindle;
    }
};
std::map<int, BonfireState> g_home_bonfires;
bool g_away_from_home = false;

// Calls fn(entry address, bonfire object) for each entry; false if the table is not there.
template <typename F>
bool each_bonfire(F&& fn) {
    uint32_t owner = 0, table = 0, head = 0, node = 0;
    if (!read32(kBonfireOwner, &owner) || !owner || !read32(owner + 0xB48, &table) || !table ||
        !read32(table + 0x24, &head) || !head || !read32(head, &node)) {
        return false;
    }
    for (int guard = 0; node && node != head && guard < 512; ++guard) {
        uint32_t entry = 0, object = 0;
        if (read32(node + 8, &entry) && entry && read32(entry + 4, &object)) fn(entry, static_cast<int>(object));
        if (!read32(node, &node)) return false;
    }
    return true;
}

BonfireState read_bonfire(uint32_t entry) {
    BonfireState st;
    uint32_t counts = 0;
    read32(entry + 8, &counts);
    st.a = static_cast<uint16_t>(counts & 0xFFFF);
    st.b = static_cast<uint16_t>(counts >> 16);
    read8(entry + 0xC, &st.lit);
    read8(entry + 0xD, &st.state);
    read8(entry + 0xE, &st.kindle);
    return st;
}

void bonfire_tick() {
    static double last = 0;
    if (now() - last < 0.5) return;
    last = now();
    if (!game::in_game() || game::loading()) return;
    if (!game::own_world()) {
        g_away_from_home = true;  // the copy waits for us at home
        return;
    }
    const bool back = g_away_from_home;
    g_away_from_home = false;
    std::map<int, BonfireState> now_seen;
    const bool read = each_bonfire([&](uint32_t entry, int object) {
        const BonfireState st = read_bonfire(entry);
        const auto it = g_home_bonfires.find(object);
        if (back && it != g_home_bonfires.end() && !(it->second == st)) {
            // The kindling only: a bonfire lit in the host's world is lit in a guest's own world too (its flag).
            BonfireState kept = st;
            if (st.kindle != it->second.kindle || st.a != it->second.a || st.b != it->second.b) {
                *ptr<uint16_t>(entry + 8) = it->second.a;
                *ptr<uint16_t>(entry + 0xA) = it->second.b;
                *ptr<uint8_t>(entry + 0xE) = it->second.kindle;
                kept.a = it->second.a;
                kept.b = it->second.b;
                kept.kindle = it->second.kindle;
            }
            LOG_INFO("[coop] Bonfire %d: changed while we were away (kindle level %d -> %d%s; state %d -> %d, lit %d -> %d "
                     "left as they are)", object, it->second.kindle, st.kindle,
                     kept.kindle != st.kindle ? ", put back" : "", it->second.state, st.state, it->second.lit, st.lit);
            now_seen[object] = kept;
            return;
        }
        if (!back && it != g_home_bonfires.end() && it->second.kindle != st.kindle) {
            LOG_INFO("[coop] Bonfire %d: kindle level %d -> %d at home (%s)", object, it->second.kindle, st.kindle,
                     g_active ? (g_invader_present ? "in a session, an invader here" : "in a session") : "no session");
        } else if (back && it == g_home_bonfires.end() && st.kindle > 0) {
            LOG_INFO("[coop] Bonfire %d: new in our table after a visit, kindle level %d (left as it is)", object,
                     st.kindle);
        }
        now_seen[object] = st;
    });
    if (read) g_home_bonfires.swap(now_seen);
}

// The death debuff: the row for the number of Crimson Blossoms the player carries. With DeathDebuffs off,
// Blossoms still held from a session with it on do nothing (and an applied debuff comes off).
void debuff_tick(uint32_t me) {
    static double last = 0;
    if (now() - last < 0.5) return;
    last = now();
    const int blossoms = g_cfg.death_debuffs && g_blossom_count_fn ? std::min(5, g_blossom_count_fn()) : 0;
    const int want = blossoms > 0 ? kDebuff + blossoms - 1 : 0;
    if (want == g_debuff_applied && (!want || find_effect(me, want, nullptr))) return;
    if (g_debuff_applied) remove(me, g_debuff_applied);
    g_debuff_applied = 0;
    if (want && apply(me, want)) g_debuff_applied = want;
}

// The health the death debuff took comes back once our max HP is whole again (wither_blossoms): the bar grows and
// fills with it, for the share of the max the debuff had (10% a Blossom).
void refill_tick(uint32_t me) {
    if (!g_refill.blossoms) return;
    const int m = me ? max_hp(me) : 0;
    if (!me || game::loading() || hp(me) <= 0 || now() > g_refill.until) {
        LOG_INFO("[coop] The death debuff's health was not given back (%s)",
                 !me || game::loading() ? "a load" : hp(me) <= 0 ? "we fell" : "our max HP did not grow back");
        g_refill = {};
        return;
    }
    if (g_refill.grow && m <= g_refill.max) return;  // the game has not raised the max yet
    const int share = static_cast<int>(m * 0.1 * g_refill.blossoms + 0.5);
    const int target = std::min(m, g_refill.hp + share);
    const int was = hp(me);
    if (was < target) *ptr<int>(me + 0x2D4) = target;
    LOG_INFO("[coop] The death debuff's health came back: %d -> %d of %d (max was %d)", was, std::max(was, target), m,
             g_refill.max);
    g_refill = {};
}

// ---------------------------------------------------------------- shared light
// A player's own light is a ChrLanternSlot (category 0, id 12) in the character's attachment system (ChrAttachSys
// at chr+0x2B4, its slot list at +0x2B8). Every frame the character update calls 0xE79770 (stdcall(chr), from
// 0xE836DA), which for the local player only (vt+0xA4) sets that slot through 0xE05760 (eax = chr+0x2B4,
// stdcall(light, lantern)): +0x10 bit 0 = refreshed, bit 3 = an effect of state 147 (Cast Light 620, the Sunlight
// Maggot 6950), bit 4 = the Skull Lantern (a weapon mode, not an effect; the game's own light check, event
// 11210200, asks for 620, 6950 and "using Skull Lantern"). The attachment update right after (0x46B210 at
// 0xE83718) runs the slot (0xDF9960: a point light at the character from the area's light parameter, for the
// lantern the row 0x1E after it, while the character is drawn) and deletes a slot that was not refreshed, its
// light with it (destructor 0xDF98B0). Other players' characters never get the slot, so nobody saw anyone else's light.
// Applying effect 620 or 6950 to their characters could not change that, and 21 (state 58) was no lantern. Each
// player now reports its slot's bits 3 and 4, and the others refresh the same slot on that player's character in
// its own update, right after 0xE79770, for as long as the light is reported.
constexpr uint32_t kLightCall = 0x00E836DA;        // call 0xE79770 in the character update
constexpr uint32_t kChrLight = 0x00E79770;
constexpr uint32_t kSetPlayerLight = 0x00E05760;
constexpr uint32_t kChrAttachSysVtable = 0x0118FD28;
constexpr int kLightOn = 0x08, kLanternOn = 0x10;
std::map<uint32_t, int> g_light_want;  // other players' characters -> slot bits to keep refreshed
double g_light_heard = 0;              // last share_lights; older than 2 s, the list is dropped

template <typename F>
void each_remote_player(F&& fn) {
    for (uint32_t chr : game::remote_player_chrs()) fn(chr);
}

// The light bits of a character's ChrLanternSlot (0 when it has none). Slot: +4 s16 category, +6 u16 id with
// 0x4000 = in use, +8 next, +0x10 u16 flags (the lookup 0xDCC260).
int slot_light_bits(uint32_t chr) {
    uint32_t node = 0;
    if (!chr || !read32(chr + 0x2B8, &node)) return 0;
    for (int guard = 0; node && guard < 64; ++guard) {
        uint32_t key = 0, flags = 0;
        if (!read32(node + 4, &key) || !read32(node + 0x10, &flags)) return 0;
        const uint32_t category = key & 0xFFFF, id = key >> 16;
        if (category == 0 && (id & 0x3FFF) == 12 && (id & 0x4000)) return flags & (kLightOn | kLanternOn);
        if (!read32(node + 8, &node)) return 0;
    }
    return 0;
}

// In place of the character update's call to 0xE79770: the call itself, then another player's light slot.
void __stdcall hk_chr_light(uint32_t chr) {
    reinterpret_cast<void(__stdcall*)(uint32_t)>(kChrLight)(chr);
    if (g_light_want.empty()) return;
    if (now() - g_light_heard > 2.0) {
        g_light_want.clear();
        return;
    }
    const auto it = g_light_want.find(chr);
    uint32_t vt = 0;
    if (it == g_light_want.end() || !read32(chr + 0x2B4, &vt) || vt != kChrAttachSysVtable) return;
    const uint32_t sys = chr + 0x2B4;
    const uint32_t light = (it->second & kLightOn) ? 1 : 0, lantern = (it->second & kLanternOn) ? 1 : 0;
    __try {
        __asm {
            push lantern
            push light
            mov eax, sys
            mov ecx, kSetPlayerLight
            call ecx
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_light_want.erase(chr);
    }
}

}  // namespace

int local_lights() { return slot_light_bits(game::player()); }

void share_lights(const std::vector<RemoteLight>& peers) {
    std::map<uint32_t, int> want;
    each_remote_player([&](uint32_t chr) {
        for (const auto& r : peers) {
            const int bits = r.mask & (kLightOn | kLanternOn);
            if (r.chr == chr && bits) want[chr] = bits;
        }
    });
    static int lines = 0;
    for (const auto& [chr, bits] : want) {
        const auto it = g_light_want.find(chr);
        if ((it == g_light_want.end() || it->second != bits) && ++lines <= 40) {
            LOG_INFO("[coop] Showing another player's light (%s)",
                     (bits & kLanternOn) && (bits & kLightOn) ? "Skull Lantern, Cast Light or Sunlight Maggot"
                     : (bits & kLanternOn)                    ? "Skull Lantern"
                                                              : "Cast Light or Sunlight Maggot");
        }
    }
    g_light_want.swap(want);
    g_light_heard = now();
}

// PlayerIns::update (0xE73060) looks up a param row through [PlayerIns+0x29C]. After the guest traveled
// from Firelink into Undead Burg, that struct was still set but its table pointer (struct+8) was null or
// 0xA0000. The next instruction reads [table+0x180] and the process dies (both 2026-10-08 crashes, at
// 0xE730D0). The same function already skips this lookup when the struct itself is missing (0xE730FB).
// A missing or unreadable row takes that skip. eax is not live at the use-row instruction (0xE730E1).
// PlayerIns+0x29C is the collision piece the character stands on (set by 0xE6E580, the map hit with its
// group at +0x44). Once that piece's block is unloaded the pointer is left dangling: in the 2026-10-13 tests a
// rest at the Undead Burg bonfire 1011962 left one with table 0, then 0xBF800000 (freed memory), read about
// 20 times a second from then on (1000 faults in a minute) and by every other reader of the field too. Another
// player's character in that state loses the dangling floor (0, which the game treats as standing on no
// collision until it lands on new ground); for our own character the lookup is only skipped.
constexpr uint32_t kParamLookup = 0x00E730C7;

static uint32_t __stdcall param_row(uint32_t obj, int index, uint32_t player) {
    uint32_t table = 0, count = 0, rows = 0, row = 0, inner = 0;
    uint8_t flag = 0;
    static uint32_t bad_obj = 0, bad_table = 0;
    if (index < 0 || !read32(obj + 8, &table)) return 0;
    if (obj == bad_obj && table == bad_table) return 0;  // the same dangling floor: skip without touching it
    const bool ok = table >= 0x10000 && read32(table + 0x180, &count) && index < static_cast<int>(count) &&
                    read32(table + 0x188, &rows) && rows >= 0x10000 &&
                    read32(rows + static_cast<uint32_t>(index) * 4, &row) && row >= 0x10000 &&
                    read32(row + 0x5C, &inner) && inner >= 0x10000 && read8(inner + 0x27, &flag);
    if (ok) return row;
    bad_obj = obj;
    bad_table = table;
    const bool ours = player == game::player();
    uint32_t floor = 0;
    if (!ours && player && read32(player + 0x29C, &floor) && floor == obj) {
        *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(player + 0x29C)) = 0;
    }
    static int logged = 0;
    if (logged < 12) {
        ++logged;
        LOG_INFO("[coop] Player param row skipped (struct %08X index %d table %08X, %s character %08X%s)", obj, index,
                 table, ours ? "our" : "another player's", player, ours ? "" : ": its dangling floor is cleared");
    }
    return 0;
}

static __declspec(naked) void hk_param_row() {
    __asm {
        pushad
        push ebx  // PlayerIns
        push edx
        push eax
        call param_row
        mov dword ptr [esp + 4], eax
        popad
        test esi, esi
        jz no_row
        mov eax, 0E730E1h
        jmp eax
    no_row:
        mov eax, 0E730FBh
        jmp eax
    }
}

bool install_param_guard() {
    // mov eax,[eax+8] ; xor esi,esi ; test edx,edx ; jl +11 ; cmp edx,[eax+180] ; jge +9
    // mov eax,[eax+188] ; mov esi,[eax+edx*4]
    static const uint8_t k[26] = {0x8B, 0x40, 0x08, 0x33, 0xF6, 0x85, 0xD2, 0x7C, 0x11, 0x3B, 0x90, 0x80, 0x01,
                                   0x00, 0x00, 0x7D, 0x09, 0x8B, 0x80, 0x88, 0x01, 0x00, 0x00, 0x8B, 0x34, 0x90};
    if (!bytes_match(kParamLookup, k, sizeof(k))) return false;
    uint8_t jmp[26];
    std::memset(jmp, 0x90, sizeof(jmp));
    jmp[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_param_row) - (kParamLookup + 5));
    std::memcpy(jmp + 1, &rel, 4);
    return patch(kParamLookup, k, jmp, sizeof(jmp));
}

// ---------------------------------------------------------------- a death that does not reach its respawn
// A death is the global script's chain OnEvent_4000 ("YOU DIED") -> SoloPlayDeath -> SoloPlayDeath_TextWait ->
// SoloPlayDeath_1 -> SoloPlayDeath_2, which asks for the respawn warp (WarpNextStageKick) and clears flag 4000. One
// that has not asked for its warp 15 s after it went ahead (a wait for the fight, limbo, the team or a menu does not
// count) is finished with the chain's last step, as is one when another player rests, so nobody stays dead for the
// rest of a session (docs: "A death that does not reach its respawn").
constexpr double kDeathStall = 15.0;
constexpr uint32_t kReturnStateVar = 0x013784A4;  // [[it]]: 2 = back to the title (OnEvent_4000 waits while > 0)

static uint32_t return_state() {
    uint32_t p = 0, v = 0;
    return read32(kReturnStateVar, &p) && p && read32(p, &v) ? v : 0;
}

static void finish_death(const char* why) {
    if (g_death_finished) return;
    // OnEvent_4000 ignores the death while a return to the title is asked for (GetReturnState > 0). One that was
    // asked for (Quit Game, or the network error handlers' RegistReturnTitle) and never happened is dropped first,
    // and the game's own death gets another 15 s to go ahead ("YOU DIED", the bloodstain, the warp).
    uint32_t p = 0;
    if (return_state() != 0 && !g_return_cleared && read32(kReturnStateVar, &p) && p) {
        g_return_cleared = true;
        LOG_INFO("[coop] Our death has not asked for its respawn (%s): a return to the title (state %u) was asked for "
                 "and never happened; dropped, so the death can go ahead", why, return_state());
        *ptr<uint32_t>(p) = 0;
        g_death_at = now();
        return;
    }
    g_death_finished = true;
    LOG_INFO("[coop] Our death has not asked for its respawn (%s; return state %u, flag 4000 %s): finishing it with "
             "SoloPlayDeath_2", why, return_state(), game::flag(4000) ? "on" : "off");
    if (!game::call_lua("SoloPlayDeath_2")) LOG_ERROR("[coop] SoloPlayDeath_2 could not be called");
}

// A menu or dialog is open (the indictment menu after an invader's kill holds the death until it closes; the same
// checks as the co-op items' "in control").
bool menu_open() {
    uint32_t mm = 0, request = 0, dialog = 0, text = 0;
    if (!read32(kMenuManVar, &mm) || mm < 0x10000) return false;
    if (read32(mm + 0x60, &request) && request != 0) return true;
    static const uint32_t kOpen[] = {0x40, 0x4C, 0x78, 0x84, 0x80, 0x50, 0xAC};
    for (uint32_t off : kOpen) {
        uint8_t open = 0;
        if (read8(mm + off, &open) && open) return true;
    }
    return read32(mm + 0x9E8, &dialog) && dialog > 0x10000 && read32(dialog, &text) && text != 0;
}

static void death_watch_tick() {
    if (!local_dead()) {
        g_death_at = 0;
        g_death_warp = false;
        g_death_finished = false;
        g_return_cleared = false;
        return;
    }
    if (g_death_at == 0 || spectate::holding() || g_limbo || g_team_hold || g_reward_hold || game::loading() ||
        menu_open()) {
        g_death_at = now();
        return;
    }
    if (g_active && !g_invader && !g_death_warp && now() - g_death_at > kDeathStall) finish_death("15 s");
}

// ---------------------------------------------------------------- a rest after a lost boss fight
// Only a load puts a boss fight back to its start (the boss, its fog, its flags): a rest's restarted events end at
// once or put the boss to sleep where it stands, and the boss-room flags stay. Every game in the map runs a fight's
// events, so a teammate who waited outside still has the fight going after the fighters fell (docs: "A rest after a
// lost boss fight reloads too"). A rest while a boss that woke in this game still lives (rules::awake_boss) reloads
// this game at its bonfire. It is decided at the rest, before the rest restarts the events. Every pending reload and
// the fallen's respawns start together (session::set_reset_phase), so no game's old world feeds another's new one,
// and ours waits for our menus to close (a load is not started from one).
constexpr double kResetWait = 15.0;  // our reload waits this long for the fallen to reach their respawn warp
constexpr double kResetNag = 8.0;    // how often we are told that others wait on our menu (the line fades)
constexpr double kPhaseWait = 5.0;   // a teammate whose reload phase for the rest never comes is given up after it
static int g_reset_boss = 0;         // the boss whose lost fight our reload starts over (0 none)
static double g_reset_since = 0;
static double g_reset_told = 0;      // when we were last told that the others wait on our menu
static bool g_phase_given_up = false;

bool reset_pending() { return g_reset_boss != 0; }

static void drop_reset(const char* why) {
    LOG_INFO("[coop] Lost boss fight: our world does not reload (%s)", why);
    g_reset_boss = 0;
    session::set_reset_phase(session::kResetNone);
}

static void reset_after_rest(const char* who) {
    if (!g_active || g_invader || g_reset_boss || local_dead()) return;
    const int boss = rules::awake_boss();
    if (!boss) return;
    g_reset_boss = boss;
    g_reset_since = now();
    g_reset_told = 0;
    g_phase_given_up = false;
    session::set_reset_phase(session::kResetWaiting);
    LOG_INFO("[coop] %s rested while boss %d is awake in this game: its fight was lost, our world reloads at our "
             "bonfire with the fallen",
             who, boss);
}

static void reset_tick() {
    if (!g_reset_boss) return;
    const char* drop = nullptr;
    if (!g_active || g_invader) {
        drop = "the session ended";
    } else if (game::loading() || !game::player()) {
        drop = "a load starts it over anyway";
    } else if (local_dead()) {
        drop = "we fell: our death reloads it";
    }
    if (drop) {
        drop_reset(drop);
        return;
    }
    const double waited = now() - g_reset_since;
    // Each teammate's reload phase comes for the rest it answers (kResetPhase): until every one for this rest is in,
    // the phase we hold for them may be from before it.
    const std::string missing = session::reset_phase_missing();
    if (!missing.empty() && waited >= kPhaseWait && !g_phase_given_up) {
        g_phase_given_up = true;
        LOG_INFO("[coop] Lost boss fight: no reload phase from %s for this rest after %.0f s; not waited for",
                 missing.c_str(), kPhaseWait);
    }
    const bool ready = !menu_open() && !rules::cutscene_playing() && (missing.empty() || waited >= kPhaseWait) &&
                       (session::fallen_at_respawn() || waited >= kResetWait);
    session::set_reset_phase(ready ? session::kResetReady : session::kResetWaiting);
    // The others' respawns and reloads wait while we sit in the bonfire menu (or another): we are told so, as they
    // are told that they wait.
    if (!ready && menu_open() && session::others_wait_on_reset() && now() - g_reset_told >= kResetNag) {
        const bool sitting = motion::local_loop_anim() != 0;
        if (g_reset_told == 0) {
            LOG_INFO("[coop] Lost boss fight: the others wait for our %s", sitting ? "bonfire menu" : "menu");
        }
        g_reset_told = now();
        text::show(sitting ? L"The other players are waiting for you to leave the bonfire"
                           : L"The other players are waiting for you to close the menu");
    }
    if (!ready || !session::resets_ready()) return;
    const int bonfire = session::guest() ? session::guest_respawn_bonfire() : game::last_bonfire();
    const int map = game::bonfire_map(bonfire);
    if (map == -1) {
        drop_reset("no bonfire to reload at");
        return;
    }
    LOG_INFO("[coop] Lost boss fight: our world reloads at bonfire %d after %.1f s (boss %d)", bonfire, waited,
             g_reset_boss);
    g_reset_boss = 0;
    session::set_reset_phase(session::kResetNone);
    text::show(L"The boss fight was lost: the world starts over");
    *ptr<int>(game::game_man() + game::kGmLastBonfire) = bonfire;
    rules::arm_reset_reload();
    arm_pos_drain();
    warp_now(map, -1);
}

bool install() {
    int failed = 0;
    if (!install_param_guard()) {
        ++failed;
        LOG_ERROR("[coop] Player param lookup guard did not match this build");
    }
    for (uint32_t site : kMultiDopingSites) {
        if (!redirect_call(site, kMultiDoping, reinterpret_cast<void*>(&no_multi_doping))) {
            LOG_ERROR("[coop] Enemy scaling: the game's own boss co-op buff at %08X did not match this build", site);
        }
    }
    {
        static const uint8_t k[7] = {0x33, 0xC0, 0x53, 0x8B, 0x5C, 0x24, 0x08};
        g_fx_orig = detour(kSpEffectGetter, k, sizeof(k), reinterpret_cast<void*>(&hk_fx_row));
        if (!g_fx_orig) ++failed;
    }
    {
        static const uint8_t k[7] = {0xF7, 0x40, 0x14, 0x00, 0x0E, 0x00, 0x00};  // test dword ptr [eax+14h],0E00h
        if (!detour(kPoiseSum, k, sizeof(k), reinterpret_cast<void*>(&hk_poise_sum))) {
            LOG_ERROR("[coop] Enemy scaling: the poise sum did not match this build (posture scaling is off)");
        }
    }
    {
        // mov eax,[esp+4] ; mov [012DE2B0h],eax
        static const uint8_t k[9] = {0x8B, 0x44, 0x24, 0x04, 0xA3, 0xB0, 0xE2, 0x2D, 0x01};
        g_regene_orig = detour(kRequestRegene, k, sizeof(k), reinterpret_cast<void*>(&hk_regene));
        if (!g_regene_orig) ++failed;
    }
    {
        static const uint8_t k[5] = {0xA1, 0x00, 0x87, 0x37, 0x01};  // mov eax,[01378700h]
        g_warp_orig = reinterpret_cast<CharFn>(detour(kWarpKick, k, sizeof(k), reinterpret_cast<void*>(&hk_warp_kick)));
        if (!g_warp_orig) ++failed;
    }
    {
        static const uint8_t k[5] = {0xA1, 0xA0, 0x84, 0x37, 0x01};  // mov eax,[013784A0h]
        g_sign_warp_orig = reinterpret_cast<VoidFn>(detour(kSetSignWarp, k, sizeof(k), reinterpret_cast<void*>(&hk_sign_warp)));
        if (!g_sign_warp_orig) ++failed;
    }
    if (!install_boss_fog()) {
        LOG_ERROR("[coop] Boss fogs: the boss-room button test did not match this build (not sealed for invasions)");
    }
    if (!redirect_call(kPosReceiveCall, kPosReceive, reinterpret_cast<void*>(&hk_pos_receive))) {
        LOG_ERROR("[coop] Enemy positions: the net update's receive call did not match this build (kept after reloads)");
    }
    if (!redirect_call(kLightCall, kChrLight, reinterpret_cast<void*>(&hk_chr_light))) {
        LOG_ERROR("[coop] Shared light: the character update's light call did not match this build (not shared)");
    }
    g_homeward_hooked = redirect_call(kHomewardStateCall, kHomewardWarp, reinterpret_cast<void*>(&hk_homeward_state));
    if (!g_homeward_hooked) {
        LOG_ERROR("[coop] Homeward miracle: the return-to-bonfire call did not match this build (not usable in a "
                  "session)");
    }
    if (failed) LOG_ERROR("[coop] %d world rule hook(s) did not match this build", failed);
    return failed == 0;
}

Config g_own_cfg;  // the INI's

void configure(const Config& cfg) {
    g_cfg = cfg;
    g_own_cfg = cfg;
    g_rows_ready = false;
}

// Each game scales the enemies itself, but the host's game decides their health for every game, as a number (the
// HP sync, 0xE40FE0). With the host at BossHealthScaling 105 and the guest at 115 (2026-10-26) the Asylum Demon's
// whole health filled 95 % of the guest's bar (x2.05 of x2.15), all fight long. So a guest takes the host's six.
void set_host_scaling(const int* s) {
    Config want = g_cfg;
    if (s) {
        want.enemy_health = s[0], want.enemy_damage = s[1], want.enemy_posture = s[2];
        want.boss_health = s[3], want.boss_damage = s[4], want.boss_posture = s[5];
    } else {
        want.enemy_health = g_own_cfg.enemy_health, want.enemy_damage = g_own_cfg.enemy_damage;
        want.enemy_posture = g_own_cfg.enemy_posture, want.boss_health = g_own_cfg.boss_health;
        want.boss_damage = g_own_cfg.boss_damage, want.boss_posture = g_own_cfg.boss_posture;
    }
    if (want.enemy_health == g_cfg.enemy_health && want.enemy_damage == g_cfg.enemy_damage &&
        want.enemy_posture == g_cfg.enemy_posture && want.boss_health == g_cfg.boss_health &&
        want.boss_damage == g_cfg.boss_damage && want.boss_posture == g_cfg.boss_posture) {
        return;
    }
    g_cfg = want;
    build_rows();         // in place: the effects already on enemies point at these rows and take the new values
    g_logged_level = -1;   // the level line again, with the new rates
    LOG_INFO("[coop] Enemy scaling: %s (health %d/%d %%, damage %d/%d %%, posture %d/%d %% per extra player, "
             "enemies/bosses)",
             s ? "the host's settings" : "our own settings again", g_cfg.enemy_health, g_cfg.boss_health,
             g_cfg.enemy_damage, g_cfg.boss_damage, g_cfg.enemy_posture, g_cfg.boss_posture);
}

void tick() {
    bonfire_tick();
    pos_drain_tick();
    const uint32_t me = game::player();
    const bool alive = me && hp(me) > 0;
    if (me && g_was_alive && !alive) {
        g_fell_at = now();
        if (g_active && g_death_fn) g_death_fn();
    }
    if (me) g_was_alive = alive;
    if (g_wither_pending && me && !game::loading()) wither_blossoms(g_wither_pending, g_wither_tell);
    // The debuff follows the Crimson Blossoms in the inventory (effects do not survive a map load).
    if (me && alive && !game::loading()) {
        debuff_tick(me);
    } else if (!me) {
        g_debuff_applied = 0;
    }
    refill_tick(me);
    if (g_limbo) {
        if (!local_dead()) {
            g_limbo = false;
        } else if (g_rest_heard) {
            release_limbo("someone rested");
        } else if (g_everyone_dead) {
            release_limbo("everyone fell", true);
        } else if (!boss_fight() && !rules::state().teammate_boss_fight && !g_invader_present) {
            release_limbo("the fight is over");
        } else if (!g_active) {
            release_limbo("the session ended");
        } else if (now() - g_limbo_since > 600.0) {
            release_limbo("time limit");
        }
    }
    if (!local_dead()) {  // alive again, or loading (no character)
        g_team_asked = false;
        g_trip_release = false;
    }
    deadlist::tick();
    death_watch_tick();
    reset_tick();
    if (g_team_hold) {
        if (!local_dead()) {
            g_team_hold = false;  // a load took over (the trip home) or we live again
        } else if (session::team_respawn_go()) {
            g_team_hold = false;
            respawn_now();
        }
    }
    defeat_tick();
    if (g_reward_hold) {
        if (!local_dead()) {
            g_reward_hold = false;
        } else if (!reward_pending()) {
            g_reward_hold = false;
            LOG_INFO("[coop] Our respawn goes: the boss's rewards are in");
            respawn_now();
        }
    }
    // The Crimson Blossoms belong to a session: in the world with none for 3 s (home again, the session closed, a
    // character loaded with some from an earlier session), they wither. At home they lie in the own world's key
    // array, which a visit to another world does not show, so one from a death at home came back after a visit.
    if (me && !g_active && !game::loading()) {
        if (g_no_session_since == 0) g_no_session_since = now();
        if (now() - g_no_session_since > 3.0 && g_blossom_count_fn && g_blossom_count_fn() > 0) {
            wither_blossoms("no session (they last a session)", false);
        }
    } else {
        g_no_session_since = 0;
    }
    if (!game::loading()) {
        scale_tick();
    } else {
        // A load frees every enemy, and the next ones may get the same addresses: what we knew goes with them.
        g_scaled.clear();
        g_fill_hp.clear();
        g_base_max.clear();
        g_scaled_at.clear();
        g_forced_max.clear();
    }
}

void set_session(bool active, bool invader, bool invader_present) {
    // An invader arrives: the boss fogs shut for the co-op players until it is gone (SealBossFogsDuringInvasions).
    if (active && !invader && invader_present && !g_fog_told && session::seal_boss_fogs()) {
        g_fog_told = true;
        text::show(L"The boss fogs are sealed while an invader is in this world");
    }
    if (!invader_present || !active) g_fog_told = false;
    g_active = active;
    g_invader = invader;
    g_invader_present = invader_present;
    if (!active) {
        g_players = 1;
        g_everyone_dead = false;
    }
}

void set_players(int coop_players) { g_players = std::max(1, coop_players); }
void set_everyone_dead(bool all_dead) { g_everyone_dead = all_dead; }
void set_rest_handler(void (*fn)()) { g_rest_fn = fn; }
bool set_homeward_state_handler(bool (*fn)()) {
    g_homeward_state = fn;
    return g_homeward_hooked;
}
void set_before_rest_handler(void (*fn)()) { g_before_rest_fn = fn; }
void set_death_handler(void (*fn)()) { g_death_fn = fn; }
void set_blossom_counter(int (*fn)()) { g_blossom_count_fn = fn; }
void set_blossom_wither(void (*fn)()) { g_wither_fn = fn; }
void set_blossom_give(bool (*fn)()) { g_blossom_give_fn = fn; }

void clear_death_debuff() {
    const uint32_t me = game::player();
    int removed = 0;
    if (me) {
        for (int id = kDebuff; id < kDebuff + 5; ++id) {
            if (!find_effect(me, id, nullptr)) continue;
            remove(me, id);
            ++removed;
        }
    }
    if (removed || g_debuff_applied) {
        LOG_INFO("[coop] Death debuff cleared (%d effect(s))", removed);
    }
    g_debuff_applied = 0;
}

void remote_rest() {
    uint32_t wcm = 0, state = 0;
    if (read32(kWorldChrManVar, &wcm) && wcm && read32(wcm + kWorldReset, &state) && state == 0) {
        rules::note_world_reset();
        *ptr<uint32_t>(wcm + kWorldReset) = 1;
        LOG_INFO("[coop] Another player rested: the world resets");
    }
    g_rest_heard = true;
    // Anyone's rest brings the world's enemies back: everyone's Crimson Blossoms wither, as the one who rested.
    if (g_active && !g_invader) wither_blossoms("another player rested", true);
    deadlist::note_rest();
    spectate::someone_rested();
    reset_after_rest("Another player");
    session::report_reset_phase();  // for this rest, whether our world reloads or not
    // A death that is not waiting for the fight but is stuck before its respawn goes ahead too.
    // (An ordinary death reaches its warp in about 8 s; one that has not after 10 s is stuck.)
    if (local_dead() && g_active && !g_invader && !g_death_warp && !spectate::holding() && g_death_at != 0 &&
        now() - g_death_at > 10.0) {
        finish_death("another player rested");
    }
}

bool scale_ready(uint32_t chr) {
    int level = g_active ? g_players - 1 : 0;
    level = std::max(0, std::min(5, level));
    if (!level) return true;
    const auto it = g_scaled.find(chr);
    if (it == g_scaled.end() || g_fill_hp.count(chr)) return false;
    const auto b = g_base_max.find(chr);
    if (b == g_base_max.end()) return true;
    return std::abs(max_hp(chr) - scaled_max(b->second, it->second)) <= 1;
}

// A gauge counts unless its boss is here and dead (a gauge left up after its fight: rules::own_gauge_tick).
bool boss_fight() {
    for (int slot = 0; slot < 2; ++slot) {
        const uint32_t h = boss_handle(slot);
        if (h != 0xFFFFFFFF && !rules::gauge_boss_dead(h)) return true;
    }
    return false;
}

// The death we are in is part of a team wipe, as keep_dead_on_reload decides it when the warp goes (asked by the
// death text, deathtext.cpp).
bool team_wipe_death() {
    // The death script can ask before tick() has seen the fall (a fall death starts its text in the same frame).
    const double fell = g_was_alive ? now() : g_fell_at;
    return g_active && !g_invader && local_dead() && g_players >= 2 && session::wipe_reload(fell);
}

bool local_dead() {
    const uint32_t me = game::player();
    return me && hp(me) <= 0;
}

bool has_effect(uint32_t chr, int id) { return find_effect(chr, id, nullptr) != 0; }

bool in_limbo() { return g_limbo; }

void trip_release() {
    g_trip_release = true;
    release_limbo("the host's trip home");
}

void warp_now(int map, int point) {
    // This warp replaces a respawn that waits for the team (the trip home): it must not go as well.
    if (g_team_hold) LOG_INFO("[coop] Team wipe: our respawn no longer waits (another warp goes instead)");
    g_team_hold = false;
    g_reward_hold = false;
    g_team_asked = true;
    g_limbo = false;
    g_bypass = true;
    game::warp(map, point);
    g_bypass = false;
}

void bypass_limbo(bool on) { g_bypass = on; }

}  // namespace coop::world
