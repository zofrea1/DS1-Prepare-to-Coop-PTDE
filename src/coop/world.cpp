#include "world.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace coop::world {

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
constexpr int kApplySlot = 125;                     // ChrIns::ApplySpEffect(id, source, 0, 1,1,1, 0)

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
using ApplyFn = char(__fastcall*)(uint32_t chr, void*, int id, uint32_t src, int a, float f1, float f2, float f3, int b);

bool apply(uint32_t chr, int id) {
    uint32_t vt = 0, fn = 0;
    if (!chr || !read32(chr, &vt) || !read32(vt + kApplySlot * 4, &fn)) return false;
    __try {
        reinterpret_cast<ApplyFn>(fn)(chr, nullptr, id, chr, 0, 1.0f, 1.0f, 1.0f, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
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
// Util_RequestRegene (the rest that respawns the world). As in Remastered Seamless Co-op the co-op items the
// player lacks are given and every Crimson Blossom withers before the game's own rest; afterwards the other
// players are told (their worlds reset too).
void __stdcall before_rest() {
    LOG_INFO("[coop] Rested at a bonfire");
    if (g_before_rest_fn) g_before_rest_fn();
}

void __stdcall after_rest() {
    if (g_rest_fn && g_active && !g_invader) g_rest_fn();
}

__declspec(naked) void hk_regene() {
    __asm {
        pushad
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
bool limbo_wanted() {
    return g_active && !g_invader && local_dead() && g_players >= 2 && !g_everyone_dead &&
           (boss_fight() || g_invader_present);
}

char __cdecl hk_warp_kick() {
    if (!g_bypass && !g_limbo && limbo_wanted()) {
        g_limbo = true;
        g_rest_heard = false;
        g_limbo_since = now();
        LOG_INFO("[coop] Limbo: the respawn waits until the fight ends, everyone falls or someone rests");
        return 1;
    }
    return g_warp_orig();
}

void release_limbo(const char* why) {
    if (!g_limbo) return;
    g_limbo = false;
    LOG_INFO("[coop] Limbo ends (%s)", why);
    g_bypass = true;
    g_warp_orig();
    g_bypass = false;
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

void scale_tick() {
    if (now() - g_last_scale < 0.5) return;
    g_last_scale = now();
    int level = g_active ? g_players - 1 : 0;
    level = std::max(0, std::min(5, level));
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
        if (want == have || hp(chr) <= 0) return;
        if (have) remove(chr, have);
        const bool full = hp(chr) >= max_hp(chr) && hp(chr) > 0;
        if (want && apply(chr, want)) {
            g_scaled[chr] = want;
            if (full) g_fill_hp.insert(chr);
        } else {
            g_scaled.erase(chr);
        }
    });
    for (auto it = g_scaled.begin(); it != g_scaled.end();) {
        it = seen.count(it->first) ? std::next(it) : g_scaled.erase(it);
    }
}

// The death debuff: the row for the number of Crimson Blossoms the player carries.
void debuff_tick(uint32_t me) {
    static double last = 0;
    if (now() - last < 0.5) return;
    last = now();
    const int blossoms = g_blossom_count_fn ? std::min(5, g_blossom_count_fn()) : 0;
    const int want = blossoms > 0 ? kDebuff + blossoms - 1 : 0;
    if (want == g_debuff_applied && (!want || find_effect(me, want, nullptr))) return;
    if (g_debuff_applied) remove(me, g_debuff_applied);
    g_debuff_applied = 0;
    if (want && apply(me, want)) g_debuff_applied = want;
}

// ---------------------------------------------------------------- shared light
// Light sources show only on their owner's screen: Skull Lantern (point light, SpEffect 21), Cast Light (620)
// and the Sunlight Maggot (6950). Each player reports which they carry; the others put the same effect on that
// player's character in their own game.
constexpr int kLights[3] = {21, 620, 6950};
std::map<uint32_t, int> g_light_applied;  // remote PlayerIns -> mask we applied

bool chr_pos(uint32_t chr, float out[3]) {
    uint32_t ctrl = 0, phys = 0;
    if (!read32(chr + 0x28, &ctrl) || !ctrl || !read32(ctrl + 0x1C, &phys) || !phys) return false;
    for (int i = 0; i < 3; ++i) {
        uint32_t v = 0;
        if (!read32(phys + 0x10 + i * 4, &v)) return false;
        std::memcpy(&out[i], &v, 4);
    }
    return true;
}

template <typename F>
void each_remote_player(F&& fn) {
    for (uint32_t chr : game::remote_player_chrs()) fn(chr);
}

}  // namespace

int local_lights() {
    const uint32_t me = game::player();
    int mask = 0;
    for (int i = 0; i < 3; ++i) {
        if (me && find_effect(me, kLights[i], nullptr)) mask |= 1 << i;
    }
    return mask;
}

void share_lights(const std::vector<RemoteLight>& peers) {
    std::map<uint32_t, int> want;
    each_remote_player([&](uint32_t chr) {
        float p[3];
        if (!chr_pos(chr, p)) return;
        int best_mask = 0;
        float best = 4.0f * 4.0f;  // within 4 m of where the player says they are
        for (const auto& r : peers) {
            const float dx = p[0] - r.pos[0], dy = p[1] - r.pos[1], dz = p[2] - r.pos[2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < best) {
                best = d;
                best_mask = r.mask;
            }
        }
        want[chr] = best_mask;
    });
    for (auto& [chr, mask] : want) {
        const int had = g_light_applied.count(chr) ? g_light_applied[chr] : 0;
        for (int i = 0; i < 3; ++i) {
            const bool on = (mask >> i) & 1, was = (had >> i) & 1;
            if (on && !find_effect(chr, kLights[i], nullptr)) apply(chr, kLights[i]);
            if (!on && was) remove(chr, kLights[i]);
        }
        g_light_applied[chr] = mask;
    }
    for (auto it = g_light_applied.begin(); it != g_light_applied.end();) {
        it = want.count(it->first) ? std::next(it) : g_light_applied.erase(it);
    }
}

bool install() {
    int failed = 0;
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
            release_limbo("everyone fell");
        } else if (!boss_fight() && !g_invader_present) {
            release_limbo("the fight is over");
        } else if (!g_active) {
            release_limbo("the session ended");
        } else if (now() - g_limbo_since > 600.0) {
            release_limbo("time limit");
        }
    }
    if (!game::loading()) scale_tick();
}

void set_session(bool active, bool invader, bool invader_present) {
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

void remote_rest() {
    uint32_t wcm = 0, state = 0;
    if (read32(kWorldChrManVar, &wcm) && wcm && read32(wcm + kWorldReset, &state) && state == 0) {
        *ptr<uint32_t>(wcm + kWorldReset) = 1;
        LOG_INFO("[coop] Another player rested: the world resets");
    }
    g_rest_heard = true;
}

bool boss_fight() { return boss_handle(0) != 0xFFFFFFFF || boss_handle(1) != 0xFFFFFFFF; }

bool local_dead() {
    const uint32_t me = game::player();
    return me && hp(me) <= 0;
}

bool in_limbo() { return g_limbo; }

void warp_now(int map, int point) {
    g_bypass = true;
    game::warp(map, point);
    g_bypass = false;
}

void bypass_limbo(bool on) { g_bypass = on; }

}  // namespace coop::world
