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
constexpr int kScaleEnemy = 389100;  // +level 1..4
constexpr int kScaleBoss = 389110;   // +level 1..4
constexpr int kDebuff = 389120;      // +stacks 1..5
constexpr int kBlossomFx = 389130;
constexpr int kFirstFx = 389100, kLastFx = 389130;
constexpr int kRow = 0x170;          // SP_EFFECT_PARAM_ST

Config g_cfg;
uint8_t g_rows[kLastFx - kFirstFx + 1][kRow];
bool g_row_valid[kLastFx - kFirstFx + 1] = {};
bool g_rows_ready = false;
void* g_fx_orig = nullptr;

// ---------------------------------------------------------------- state
bool g_active = false, g_invader = false, g_invader_present = false;
int g_players = 1;
bool g_everyone_dead = false;
bool g_misfortune = false, g_blossom = false;
int g_debuff = 0;
bool g_was_alive = true;
void (*g_rest_fn)() = nullptr;
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

double now() { return GetTickCount64() / 1000.0; }

void put_f(uint8_t* r, int off, float v) { std::memcpy(r + off, &v, 4); }
void put_i(uint8_t* r, int off, int32_t v) { std::memcpy(r + off, &v, 4); }
void put_s(uint8_t* r, int off, int16_t v) { std::memcpy(r + off, &v, 2); }

void neutral_rates(uint8_t* r) {
    put_i(r, 0x00, -1);  // iconId
    for (int off = 0x10; off <= 0x54; off += 4) put_f(r, off, 1.0f);  // max HP .. attack power rates
    for (int off = 0x68; off <= 0x74; off += 4) put_f(r, off, 1.0f);  // defence rates
    for (int off = 0x108; off <= 0x114; off += 4) put_f(r, off, 1.0f);  // resist change rates
    put_s(r, 0x13A, 0);  // changeSuperArmorPoint
    put_f(r, 0x08, -1.0f);  // effectEndurance: until removed
}

void build_rows() {
    uint32_t out[2] = {0, 0};
    void* orig = g_fx_orig;
    uint32_t* pout = out;
    __asm {
        push edi
        mov edi, 7500
        push pout
        call orig
        pop edi
    }
    const uint8_t* tpl = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(out[1]));
    if (!tpl) {
        LOG_ERROR("[coop] SpEffect 7500 not found; scaling and debuffs are off");
        g_rows_ready = true;
        return;
    }
    auto row = [&](int id) {
        uint8_t* r = g_rows[id - kFirstFx];
        std::memcpy(r, tpl, kRow);
        neutral_rates(r);
        g_row_valid[id - kFirstFx] = true;
        return r;
    };
    for (int k = 1; k <= 4; ++k) {
        for (int boss = 0; boss < 2; ++boss) {
            const int hp = boss ? g_cfg.boss_health : g_cfg.enemy_health;
            const int dmg = boss ? g_cfg.boss_damage : g_cfg.enemy_damage;
            const int pos = boss ? g_cfg.boss_posture : g_cfg.enemy_posture;
            uint8_t* r = row((boss ? kScaleBoss : kScaleEnemy) + k);
            put_f(r, 0x10, 1.0f + hp * k / 100.0f);                    // maxHpRate
            const float atk = 1.0f + dmg * k / 100.0f;
            for (int off = 0x48; off <= 0x54; off += 4) put_f(r, off, atk);  // attack power rates
            put_s(r, 0x13A, static_cast<int16_t>(std::min(200, pos * k / 2)));  // poise points
        }
    }
    for (int n = 1; n <= 5; ++n) {
        uint8_t* r = row(kDebuff + n);
        put_i(r, 0x00, 5012);
        put_f(r, 0x10, std::max(0.5f, 1.0f - 0.1f * n));
        put_s(r, 0x13E, 0);  // no category: does not replace other effects
        r[0x140] = 0;
    }
    {
        uint8_t* r = row(kBlossomFx);
        put_i(r, 0x00, 5008);
        put_f(r, 0x10, 0.85f);
        put_f(r, 0xE0, 1.25f);  // soulRate
        put_s(r, 0x13E, 0);
        r[0x140] = 0;
    }
    g_rows_ready = true;
}

void* __stdcall our_fx_row(int32_t id) {
    if (id < kFirstFx || id > kLastFx) return nullptr;
    if (!g_rows_ready) build_rows();
    return g_row_valid[id - kFirstFx] ? g_rows[id - kFirstFx] : nullptr;
}

__declspec(naked) void hk_fx_row() {
    __asm {
        cmp edi, 389100
        jl orig
        cmp edi, 389130
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
void __stdcall after_rest() {
    LOG_INFO("[coop] Rested at a bonfire");
    if (g_debuff) {
        for (int n = 1; n <= 5; ++n) remove(game::player(), kDebuff + n);
        g_debuff = 0;
    }
    if (g_blossom) {
        remove(game::player(), kBlossomFx);
        g_blossom = false;
    }
    g_misfortune = false;
    if (g_rest_fn && g_active && !g_invader) g_rest_fn();
}

__declspec(naked) void hk_regene() {
    __asm {
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

// ---------------------------------------------------------------- scaling
uint32_t boss_handle(int slot) {
    uint32_t mm = 0, h = 0xFFFFFFFF;
    if (!read32(kMenuManVar, &mm) || !mm) return 0xFFFFFFFF;
    read32(mm + (slot ? 0xF78 : 0xF64), &h);
    return h;
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

void scale_tick() {
    if (now() - g_last_scale < 0.5) return;
    g_last_scale = now();
    int level = g_active ? (g_players - 1) + (g_misfortune ? 1 : 0) : 0;
    level = std::max(0, std::min(4, level));
    const uint32_t b0 = boss_handle(0), b1 = boss_handle(1);
    std::set<uint32_t> seen;
    each_enemy([&](uint32_t chr) {
        seen.insert(chr);
        uint32_t handle = 0;
        read32(chr + 4, &handle);
        const bool boss = handle != 0xFFFFFFFF && (handle == b0 || handle == b1);
        const int want = level ? (boss ? kScaleBoss : kScaleEnemy) + level : 0;
        auto it = g_scaled.find(chr);
        const int have = it == g_scaled.end() ? 0 : it->second;
        if (g_fill_hp.count(chr)) {
            g_fill_hp.erase(chr);
            const int m = max_hp(chr);
            if (hp(chr) > 0 && m > 0) *ptr<int>(chr + 0x2D4) = m;
        }
        if (want == have) return;
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
    uint32_t wcm = 0, n = 0, arr = 0;
    if (!read32(kWorldChrManVar, &wcm) || !wcm || !read32(wcm + 0x24, &n) || !read32(wcm + 0x28, &arr) || !arr) return;
    for (uint32_t i = 0; i < n && i < 8; ++i) {
        uint32_t chr = 0;
        if (read32(arr + i * 0x20, &chr) && chr) fn(chr);
    }
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
    if (me && g_was_alive && !alive && g_active && g_cfg.death_debuffs) {
        g_debuff = std::min(5, g_debuff + 1);
        LOG_INFO("[coop] Death debuff now %d", g_debuff);
    }
    if (me) g_was_alive = alive;
    // Re-apply the player's lasting effects after loads (effects do not survive a map load).
    if (me && alive && !game::loading()) {
        if (g_debuff && !find_effect(me, kDebuff + g_debuff, nullptr)) {
            for (int n = 1; n <= 5; ++n) {
                if (n != g_debuff) remove(me, kDebuff + n);
            }
            apply(me, kDebuff + g_debuff);
        }
        if (g_blossom && !find_effect(me, kBlossomFx, nullptr)) apply(me, kBlossomFx);
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

void remote_rest() {
    uint32_t wcm = 0, state = 0;
    if (read32(kWorldChrManVar, &wcm) && wcm && read32(wcm + kWorldReset, &state) && state == 0) {
        *ptr<uint32_t>(wcm + kWorldReset) = 1;
        LOG_INFO("[coop] Another player rested: the world resets");
    }
    g_misfortune = false;
    g_rest_heard = true;
}

void set_misfortune(bool on) { g_misfortune = on; }
bool misfortune() { return g_misfortune; }
void set_blossom(bool on) { g_blossom = on; }

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

}  // namespace coop::world
