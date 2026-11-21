// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "farenemies.h"

#include "game.h"
#include "hook.h"
#include "log.h"
#include "rules.h"
#include "visible.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace coop::farenemies {

namespace {

// How the game brings a character in (spectate.cpp has the details): every frame the streaming update asks for each
// character's map entity ([chr+0xC]+0xC = 2) whose display groups (chr+0x264) meet its block's streaming mask, which
// comes from where we stand; the entity's state machine then loads it and makes it active, and only an active one is
// updated, placed and drawn. A character in another player's rooms is not asked for here unless those rooms are ours
// too. After the game's asks, the regular enemies whose groups meet another player's floor's groups (the HitIns
// +0x20, as the game asks for the characters around a floor) are asked for as well, in the block we stand in (its
// collisions are in the physics world here, so their floors hold them).
//
// Who runs them: the session host's game hands each enemy to the game that bids the most (rules.cpp, "who moves an
// enemy"), and only a game that updates an enemy bids for it; the other games' copies follow the network
// (NetAIManipulator), never their own AI, unless the enemy was handed to them (WorldChrMan +0x2B4, 0xE3E250) or has
// SetNoNetSync (chr+0x428). The enemies around another player are updated in their game, which bids for them, so they
// are theirs; here our game never bids for one we only show (rules.cpp net_priority asks shown_only), and one already
// handed to our game (we were near it) is not shown until it has gone to another game. An enemy nobody bids for stays
// nobody's: its copy here stands as its last update left it.
//
// Updated every frame: far from us the game updates a character only every 5th or 30th frame (its rate chr+0xF4, from
// WorldChrMan's pass, ranked by distance from us), so a far enemy moved in jerks; the rate is fixed at every frame
// (chr+0x1FD bit 0, as the event command Set Network Update Rate does) while we show it, and put back after. A
// character at a fixed rate is always drawn by the pass (0xE3F832), and its model is given every display group
// (visible.cpp), as the other players' are, so it is drawn wherever our draw mask of its map ends.
constexpr uint32_t kWcmSets = 0xE4;       // {set, block, WorldBlockChr} x count
constexpr uint32_t kWcmSetCount = 0xE0;
constexpr uint32_t kHandout = 0x2B4;      // WorldChrMan: the enemies handed to this game
constexpr uint32_t kChrRateFlags = 0x1FD; // bit 0: the rate is fixed
constexpr uint32_t kChrRate = 0xF4;
constexpr int kTeamEnemy = 6;
constexpr uint32_t kEnemyVtable = 0x0118979C;  // EnemyIns
constexpr int kMaxShown = 192;
constexpr int kMaxOthers = 8;

// Bosses (their health bars), the Hellkite and the bosses' defeat entities: left to the game's rooms (their fights are
// events of their own: fog walls, health bars, cutscenes).
constexpr int kBosses[] = {1000800, 1010300, 1010301, 1010302, 1010700, 1010750, 1010800, 1010801, 1100160, 1200800,
                           1200801, 1210401, 1210800, 1210820, 1210840, 1300800, 1310800, 1400800, 1410400, 1410600,
                           1410700, 1410802, 1500800, 1510650, 1510800, 1510801, 1510810, 1510811, 1600800, 1600810,
                           1700700, 1700800, 1800800, 1810800, 1810810};

bool g_enabled = true;
struct Shown {
    bool was_fixed = false;  // its rate was fixed before we showed it (an event's): left fixed after
    bool alive = true;
    bool told = false;
};
std::unordered_map<uint32_t, Shown> g_shown;  // character -> while we show it
uint32_t g_list[kMaxShown] = {};
int g_count = 0;

// This frame's counts, for the log.
struct Counts {
    int shown = 0, others_here = 0, ours = 0, bosses = 0, npcs = 0, no_sync = 0, dead = 0, full = 0;
};
Counts g_counts;

bool in_bosses(int entity) {
    for (int b : kBosses) {
        if (b == entity) return true;
    }
    return false;
}

uint32_t block_set(uint32_t wcm, uint32_t block) {
    uint32_t n = 0;
    if (!wcm || !read32(wcm + kWcmSetCount, &n) || n > 64) return 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t set = 0, b = 0;
        if (read32(wcm + kWcmSets + i * 12, &set) && read32(wcm + kWcmSets + i * 12 + 4, &b) && b == block) return set;
    }
    return 0;
}

bool meets(const uint32_t* a, uint32_t b) {
    for (int k = 0; k < 4; ++k) {
        uint32_t y = 0;
        if (read32(b + k * 4, &y) && (a[k] & y)) return true;
    }
    return false;
}

// The enemy was handed to our game (0xE3E250: the handle chr+0x78 -> block (bits 11..15), index (0..10); the entry
// >= 0 is ours).
bool handed_here(uint32_t wcm, uint32_t chr) {
    uint32_t handle = 0, table = 0, blocks = 0, list = 0, n = 0, words = 0;
    if (!read32(chr + 0x78, &handle) || static_cast<int32_t>(handle) < 0 || !read32(wcm + kHandout, &table) || !table) {
        return false;
    }
    const uint32_t block = (handle >> 11) & 0x1F, index = handle & 0x7FF;
    if (!read32(table + 4, &blocks) || block >= blocks || !read32(table + 8 + block * 4, &list) || !list ||
        !read32(list + 8, &n) || index >= n || !read32(list + 0xC, &words) || !words) {
        return false;
    }
    uint32_t w = 0;
    if (!read32(words + (index & ~1u) * 2, &w)) return false;
    const int16_t entry = static_cast<int16_t>((index & 1) ? (w >> 16) : (w & 0xFFFF));
    return entry >= 0;
}

// Our hold on a character ends: its rate as it was (the pass sets it again next frame).
void let_go(uint32_t chr, const Shown& s) {
    if (s.was_fixed) return;
    uint32_t vt = 0;
    if (!read32(chr, &vt) || vt != kEnemyVtable) return;  // gone with its map
    __try {
        uint8_t flags = 0;
        if (read8(chr + kChrRateFlags, &flags) && (flags & 1)) *ptr<uint8_t>(chr + kChrRateFlags) = flags & ~1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Asked for and updated every frame (the game clears the fixed rate at times: kept set). False if it could not be.
bool hold(uint32_t chr, uint32_t ent, uint32_t asked, bool* was_fixed) {
    __try {
        uint8_t flags = 0;
        read8(chr + kChrRateFlags, &flags);
        *was_fixed = (flags & 1) != 0;
        if (asked != 2) *ptr<uint32_t>(ent + 0xC) = 2;
        if (!(flags & 1)) *ptr<uint8_t>(chr + kChrRateFlags) = flags | 1;
        *ptr<int32_t>(chr + kChrRate) = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void let_all_go(bool characters_alive) {
    if (characters_alive) {
        for (const auto& [chr, s] : g_shown) let_go(chr, s);
    }
    g_shown.clear();
    g_count = 0;
    visible::set_extra(nullptr, 0);
}

}  // namespace

void set_enabled(bool on) {
    g_enabled = on;
    LOG_INFO("[coop] Enemies near the other players in our map are moved and drawn here, however far: %s "
             "(EnemiesNearOtherPlayers)",
             on ? "yes" : "no");
}

bool enabled() { return g_enabled; }

bool shown_only(uint32_t chr) {
    for (int i = 0; i < g_count; ++i) {
        if (g_list[i] == chr) return true;
    }
    return false;
}

void ask(uint32_t wcm) {
    Counts c{};
    const auto& st = rules::state();
    if (!g_enabled || !st.active || st.leaving || game::loading()) {
        g_counts = c;
        if (!g_shown.empty()) let_all_go(!game::loading());
        return;
    }
    const uint32_t me = game::player();
    uint32_t my_hit = 0;
    uint8_t my_block = 0xFF;
    if (!me || !read32(me + 0x29C, &my_hit) || !my_hit || !read8(my_hit + 0xF, &my_block) || my_block >= 64) {
        g_counts = c;
        return;
    }
    // The other players standing in our block: their floors' groups.
    uint32_t groups[kMaxOthers][4] = {};
    uint32_t others[kMaxOthers] = {};
    int n_others = 0, n_groups = 0;
    for (uint32_t o : game::remote_player_chrs()) {
        if (n_others < kMaxOthers) others[n_others++] = o;
        uint32_t hit = 0;
        uint8_t b = 0xFF;
        if (!read32(o + 0x29C, &hit) || !hit || !read8(hit + 0xF, &b) || b != my_block || n_groups >= kMaxOthers) continue;
        bool ok = true;
        for (int k = 0; k < 4 && ok; ++k) ok = read32(hit + 0x20 + k * 4, &groups[n_groups][k]);
        if (ok) ++n_groups;
    }
    c.others_here = n_groups;
    const uint32_t set = block_set(wcm, my_block);
    uint32_t count = 0, arr = 0;
    std::unordered_map<uint32_t, Shown> now;
    int listed = 0;
    if (n_groups > 0 && set && read32(set, &count) && read32(set + 4, &arr) && arr && count <= 4096) {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t chr = 0, ent = 0, asked = 0, attached = 0;
            uint8_t never = 1, always = 1;
            if (!read32(arr + i * 0x20, &chr) || !chr || chr == me || !read8(chr + 0x14, &never) ||
                !read8(chr + 0x15, &always) || !read32(chr + 0x54, &attached) || !read32(chr + 0xC, &ent) || !ent ||
                !read32(ent + 0xC, &asked)) {
                continue;
            }
            if (never || always || !attached) continue;
            bool player = false;
            for (int k = 0; k < n_others; ++k) player |= others[k] == chr;
            if (player) continue;
            bool near_other = false;
            for (int k = 0; k < n_groups && !near_other; ++k) near_other = meets(groups[k], chr + 0x264);
            if (!near_other) continue;
            // Asked for by the game itself this frame (its asks are made anew every frame: 2 where the groups meet our
            // mask, else 0): around us, the game's own and ours to bid for, also one we showed until we came near.
            if (asked == 2) continue;
            uint32_t vt = 0;
            if (!read32(chr, &vt) || vt != kEnemyVtable) continue;
            const auto had = g_shown.find(chr);
            uint32_t team = 0, entity = 0, no_sync = 0, hp = 0;
            read32(chr + 0x74, &team);
            read32(chr + 0x208, &entity);
            read32(chr + 0x428, &no_sync);
            read32(chr + 0x2D4, &hp);
            if (static_cast<int>(team) != kTeamEnemy) {
                ++c.npcs;
                continue;
            }
            if (in_bosses(static_cast<int>(entity))) {
                ++c.bosses;
                continue;
            }
            if (no_sync & 0xFF) {
                ++c.no_sync;
                continue;
            }
            if (handed_here(wcm, chr)) {
                ++c.ours;
                continue;
            }
            const bool alive = static_cast<int>(hp) > 0;
            // A dead one only while we already show it (its death and fall play out); a corpse from before is not new.
            if (!alive && had == g_shown.end()) {
                ++c.dead;
                continue;
            }
            if (listed >= kMaxShown) {
                ++c.full;
                continue;
            }
            Shown s = had != g_shown.end() ? had->second : Shown{};
            bool was_fixed = false;
            if (!hold(chr, ent, asked, &was_fixed)) continue;
            if (had == g_shown.end()) s.was_fixed = was_fixed;
            if (!s.told) {
                s.told = true;
                static int lines = 0;
                if (++lines <= 40) {
                    game::Vec3 at{}, mine{};
                    const bool where = game::chr_position(chr, &at) && game::position(&mine, nullptr);
                    const float dx = at.x - mine.x, dy = at.y - mine.y, dz = at.z - mine.z;
                    uint32_t current = 0, net = 0;
                    read32(chr + 0x2C, &current);
                    read32(chr + 0x414, &net);
                    LOG_INFO("[coop] Enemy %d at (%.1f, %.1f, %.1f), %.0f m from us: shown here for another player near "
                             "it (moved by %s)",
                             static_cast<int>(entity), at.x, at.y, at.z,
                             where ? std::sqrt(dx * dx + dy * dy + dz * dz) : -1.0f,
                             current && current == net ? "another game" : "no game yet");
                }
            }
            s.alive = alive;
            now[chr] = s;
            g_list[listed++] = chr;
            ++c.shown;
        }
    }
    // Those no longer shown: their rates back.
    for (const auto& [chr, s] : g_shown) {
        if (now.find(chr) == now.end()) let_go(chr, s);
    }
    g_shown.swap(now);
    g_count = listed;
    visible::set_extra(g_list, listed);
    g_counts = c;
}

void tick() {
    static ULONGLONG last = 0;
    static int lines = 0;
    const ULONGLONG t = GetTickCount64();
    if (t - last < 10000) return;
    last = t;
    const Counts c = g_counts;
    if (c.shown == 0 && c.ours == 0) return;
    uint32_t wcm = 0;
    int active = 0, network = 0;
    read32(game::kWorldChrManVar, &wcm);
    for (int i = 0; i < g_count; ++i) {
        const uint32_t chr = g_list[i];
        uint32_t ent = 0, state = 0, current = 0, net = 0;
        if (read32(chr + 0xC, &ent) && ent && read32(ent + 8, &state) && state == 2) ++active;
        if (read32(chr + 0x2C, &current) && read32(chr + 0x414, &net) && current && current == net) ++network;
    }
    if (++lines <= 120) {
        LOG_INFO("[coop] Enemies near the other players (%d in our map): %d shown here, %d of them active, %d moved by "
                 "another game; left to the game: %d our game runs, %d bosses, %d not enemies, %d with their own AI "
                 "everywhere, %d dead before%s",
                 c.others_here, c.shown, active, network, c.ours, c.bosses, c.npcs, c.no_sync, c.dead,
                 c.full ? " (the list was full)" : "");
    }
}

}  // namespace coop::farenemies
