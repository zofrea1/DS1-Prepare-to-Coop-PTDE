// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "world.h"

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
bool g_team_asked = false;  // asked once per death: a second warp of the same death does not wait again
double g_death_at = 0;          // our death, or the end of its wait (spectating, limbo, the team); 0 while we live
bool g_death_warp = false;      // this death's respawn warp was asked for
bool g_death_finished = false;  // finish_death ran for this death
bool g_return_cleared = false;  // finish_death cleared a stale return to the title for this death
bool g_rest_heard = false;
double g_limbo_since = 0;
std::map<uint32_t, int> g_scaled;  // EnemyIns -> applied effect id
std::set<uint32_t> g_fill_hp;      // enemies to top up after their max HP grew
double g_last_scale = 0;
int g_debuff_applied = 0;          // debuff effect id on the local player (0 none)

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
    reset_after_rest("We");
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

// ---------------------------------------------------------------- limbo
// The respawn warp waits instead of the death itself: only when the death event cannot be held (spectate.cpp).
bool limbo_wanted() {
    return g_active && !g_invader && local_dead() && g_players >= 2 && !g_everyone_dead &&
           (boss_fight() || g_invader_present);
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
    // Before the original body sets the warp request. A death is aimed at the bonfire; the empty map is not loaded.
    if (!g_bypass && rules::prepare_warp_kick(local_dead())) return 1;
    if (!g_bypass && g_active && local_dead()) session::note_respawn_went();
    return g_warp_orig();
}

// ---------------------------------------------------------------- boss fogs sealed while an invader is here
// A boss fog's entry is an EMEVD "Action Button - Boss Room" (3[19]); its test (EcTestActionButton 0xBBD7E0) greys
// the button out for the game's own reasons (state effect 185, a Gravelord or Path of the Dragon phantom in the
// party: invade types 7 and 10). Any other invader leaves it open, the invader cannot follow (the fog takes
// living players and white phantoms), and the host's entry sends the invader home (the boss room notification,
// 2009[06]: Lua 4068 ReportBossArea -> Failed_BossAreaMission, and 0xEA2C80 drops black phantoms from the party).
// In a session the fog stays shut for the host and the guests while an invader is in the world, so a fight with
// an invader cannot be ended by walking into a boss: our answer joins the game's at 0xBBD86C.
constexpr uint32_t kBossFogTest = 0x00BBD86C;   // mov ecx,[0137D644h] ; xor eax,eax (the boss-room checks begin)
constexpr uint32_t kBossFogShut = 0x00BBD8AB;   // mov byte [esp+10h],0: the button greyed out
uint32_t g_fog_shut_at = kBossFogShut;
bool g_fog_told = false;

bool __cdecl boss_fog_sealed() {
    const bool sealed = g_active && !g_invader && g_invader_present;
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

// The respawn warp that waited (limbo, a team wipe) goes ahead.
void respawn_now() {
    if (rules::prepare_warp_kick(local_dead())) return;
    if (g_active) session::note_respawn_went();
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
// Every enemy gets the scaling row for the number of other co-op players in the session (1..5); characters
// on team 7 (bosses) get the boss row. As in Remastered Seamless Co-op.
constexpr int kBossTeam = 7;

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
    each_enemy([&](uint32_t chr) {
        seen.insert(chr);
        uint32_t team = 0;
        read32(chr + 0x74, &team);
        const bool boss = static_cast<int>(team) == kBossTeam;
        const int want = level ? (boss ? kScaleBoss : kScaleEnemy) + level - 1 : 0;
        auto it = g_scaled.find(chr);
        const int have = it == g_scaled.end() ? 0 : it->second;
        if (g_fill_hp.count(chr)) {
            g_fill_hp.erase(chr);
            const int m = max_hp(chr);
            if (hp(chr) > 0 && m > 0) *ptr<int>(chr + 0x2D4) = m;
        }
        if (hp(chr) <= 0) return;
        // Applied once is not enough: an effect the game has since taken off the enemy is applied again.
        if (want == have && (!want || find_effect(chr, want, nullptr))) return;
        if (have) remove(chr, have);
        const bool full = hp(chr) >= max_hp(chr) && hp(chr) > 0;
        if (want && apply(chr, want)) {
            g_scaled[chr] = want;
            if (full) g_fill_hp.insert(chr);
        } else {
            g_scaled.erase(chr);
            uint32_t handle = 0;
            read32(chr + 0x78, &handle);
            static int refused = 0;
            if (want && ++refused <= 10) {
                LOG_INFO("[coop] The game refused the scaling on an enemy (handle %d, HP %d/%d); trying again",
                         static_cast<int>(handle), hp(chr), max_hp(chr));
            }
        }
    });
    for (auto it = g_scaled.begin(); it != g_scaled.end();) {
        it = seen.count(it->first) ? std::next(it) : g_scaled.erase(it);
    }
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
static bool menu_open() {
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
    if (g_death_at == 0 || spectate::holding() || g_limbo || g_team_hold || game::loading() || menu_open()) {
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
static int g_reset_boss = 0;         // the boss whose lost fight our reload starts over (0 none)
static double g_reset_since = 0;
static double g_reset_told = 0;      // when we were last told that the others wait on our menu

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
    // The rest's message is reliable and can arrive ahead of the rester's report that its reload waits: until every
    // teammate has reported since the rest, a reload phase of none is from before it.
    const bool ready = !menu_open() && !rules::cutscene_playing() && session::heard_since(g_reset_since) &&
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
    warp_now(map, -1);
}

bool install() {
    int failed = 0;
    if (!install_param_guard()) {
        ++failed;
        LOG_ERROR("[coop] Player param lookup guard did not match this build");
    }
    {
        static const uint8_t k[7] = {0x33, 0xC0, 0x53, 0x8B, 0x5C, 0x24, 0x08};
        g_fx_orig = detour(kSpEffectGetter, k, sizeof(k), reinterpret_cast<void*>(&hk_fx_row));
        if (!g_fx_orig) ++failed;
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
    if (!redirect_call(kLightCall, kChrLight, reinterpret_cast<void*>(&hk_chr_light))) {
        LOG_ERROR("[coop] Shared light: the character update's light call did not match this build (not shared)");
    }
    if (failed) LOG_ERROR("[coop] %d world rule hook(s) did not match this build", failed);
    return failed == 0;
}

void configure(const Config& cfg) {
    g_cfg = cfg;
    g_rows_ready = false;
}

void tick() {
    const uint32_t me = game::player();
    const bool alive = me && hp(me) > 0;
    if (me && g_was_alive && !alive && g_active && g_death_fn) g_death_fn();
    if (me) g_was_alive = alive;
    // The debuff follows the Crimson Blossoms in the inventory (effects do not survive a map load).
    if (me && alive && !game::loading()) {
        debuff_tick(me);
    } else if (!me) {
        g_debuff_applied = 0;
    }
    if (g_limbo) {
        if (!local_dead()) {
            g_limbo = false;
        } else if (g_rest_heard) {
            release_limbo("someone rested");
        } else if (g_everyone_dead) {
            release_limbo("everyone fell", true);
        } else if (!boss_fight() && !g_invader_present) {
            release_limbo("the fight is over");
        } else if (!g_active) {
            release_limbo("the session ended");
        } else if (now() - g_limbo_since > 600.0) {
            release_limbo("time limit");
        }
    }
    if (!local_dead()) g_team_asked = false;  // alive again, or loading (no character)
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
    if (!game::loading()) scale_tick();
}

void set_session(bool active, bool invader, bool invader_present) {
    // An invader arrives: the boss fogs shut for the co-op players until it is gone.
    if (active && !invader && invader_present && !g_fog_told) {
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
void set_before_rest_handler(void (*fn)()) { g_before_rest_fn = fn; }
void set_death_handler(void (*fn)()) { g_death_fn = fn; }
void set_blossom_counter(int (*fn)()) { g_blossom_count_fn = fn; }

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
    spectate::someone_rested();
    reset_after_rest("Another player");
    // A death that is not waiting for the fight but is stuck before its respawn goes ahead too.
    // (An ordinary death reaches its warp in about 8 s; one that has not after 10 s is stuck.)
    if (local_dead() && g_active && !g_invader && !g_death_warp && !spectate::holding() && g_death_at != 0 &&
        now() - g_death_at > 10.0) {
        finish_death("another player rested");
    }
}

bool boss_fight() { return boss_handle(0) != 0xFFFFFFFF || boss_handle(1) != 0xFFFFFFFF; }

bool local_dead() {
    const uint32_t me = game::player();
    return me && hp(me) <= 0;
}

bool in_limbo() { return g_limbo; }

void warp_now(int map, int point) {
    // This warp replaces a respawn that waits for the team (the trip home): it must not go as well.
    if (g_team_hold) LOG_INFO("[coop] Team wipe: our respawn no longer waits (another warp goes instead)");
    g_team_hold = false;
    g_team_asked = true;
    g_limbo = false;
    g_bypass = true;
    game::warp(map, point);
    g_bypass = false;
}

void bypass_limbo(bool on) { g_bypass = on; }

}  // namespace coop::world
