// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "deadlist.h"

#include "game.h"
#include "hook.h"
#include "log.h"
#include "world.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace coop::deadlist {

namespace {

// The "CHR " list: {'CHR ', version}, then for each character set (a map block's characters) {key, record size,
// count, a bit per character: dead}, ending with key -1. WorldChrMan 0xE3E540 builds it (0xE42080 per set: HP <= 0
// and not left out by its param, +0x149 bit 3); a host sends it to a summoned phantom when it joins (0xD30910,
// packet 15). A map load applies it to the sets it finds (0xE40EC0 from 0xEFCE7A -> 0xE44150: each marked
// character is killed in its dead pose through 0xE6B4E0) and frees it (0xEB9580): GameMan's own-world copy in the
// own world (GameMan+0xB4F), the host's copy in someone else's. In vanilla the host's copy is only ever the one
// from the join, used up by the phantom's arrival, and a death clears the own world's (ClearMyWorldState, before
// WarpNextStageKick), so every death reload brought the world's dead back in that player's game alone.
constexpr uint32_t kWorldChrManVar = 0x0137D644;
constexpr uint32_t kBuildList = 0x00E3E540;  // stdcall(WorldChrMan, out) -> size; out 0 = the size only
constexpr uint32_t kSetList = 0x00EB9910;    // al = own world, edi = size, stdcall(data): GameMan's copy replaced
constexpr uint32_t kFreeList = 0x00EB9580;   // esi = GameMan, stdcall(own world): GameMan's copy freed
constexpr uint32_t kGmHostList = 0xBC4;      // GameMan: the host's copy (size, then data), for a load in its world
constexpr uint32_t kGmOwnList = 0xBCC;       // GameMan: the own world's copy (size, then data)
constexpr uint32_t kMagic = 0x43485220, kVersion = 0x10080300, kEnd = 0xFFFFFFFF;

struct Set {
    uint32_t key = 0;
    uint32_t n = 0;
    std::vector<uint8_t> dead;  // a bit per character
};
using List = std::vector<Set>;

List g_host;                       // a guest: the host's latest list during our death
std::map<uint64_t, List> g_mates;  // the host: each guest's latest list during our death
bool g_dead = false;               // we are dead (tick)
bool g_rested = false;             // a rest came during our death: the world started over everywhere
List g_ours;                       // our own list when our respawn warp went
bool g_armed = false;              // a reload is still to take the list we gave the game
bool g_armed_own = false;          // ... the own world's copy (we host), else the host's copy (a guest)
bool g_loading_seen = false;       // a load began since

double now() { return GetTickCount64() / 1000.0; }

// ---- hurt enemies: {kHurtMagic, count}, then {net handle, HP, max HP} each. The net handle (chr+0x78: block << 11 |
// place in the block) is what the game's own HP and damage sync use to name an enemy in every game.
constexpr uint32_t kHurtMagic = 0x54525548;  // 'HURT'
constexpr uint32_t kEnemyVtable = 0x0118979C;
constexpr uint32_t kSetHp = 0x00E688E0;      // stdcall(chr, hp)
constexpr double kHurtWindow = 20.0;         // after our reload, the health goes back within this time or not at all
struct Hurt {
    int32_t hp = 0;
    int32_t max_hp = 0;
};
using HurtList = std::map<uint32_t, Hurt>;   // net handle -> health
HurtList g_host_hurt;                         // a guest: the host's hurt enemies during our death
std::map<uint64_t, HurtList> g_mates_hurt;   // the host: each guest's
HurtList g_hurt_apply;                        // what our reload is to put back
double g_hurt_until = 0;                      // 0: nothing to put back
bool g_hurt_armed = false;                    // our respawn warp went with the world kept; lists still come in
bool g_hurt_loaded = false;                   // ... and our load has begun since (the old world is gone)
bool g_hurt_own = false;                      // the reload is in our own world (we host)
int g_hurt_applied = 0;

bool parse_hurt(const uint8_t* d, size_t size, HurtList* out) {
    out->clear();
    uint32_t head[2];
    if (!d || size < 8) return false;
    std::memcpy(head, d, 8);
    if (head[0] != kHurtMagic || head[1] > 4096 || size < 8 + head[1] * 12ull) return false;
    for (uint32_t i = 0; i < head[1]; ++i) {
        int32_t w[3];
        std::memcpy(w, d + 8 + i * 12, 12);
        if (w[1] > 0 && w[2] > 0 && w[1] < w[2]) (*out)[static_cast<uint32_t>(w[0])] = Hurt{w[1], w[2]};
    }
    return true;
}

// Our character for a net handle, as the damage packets find it (0xE413B0): WorldChrMan+0x240 + block * 4.
uint32_t chr_by_net(uint32_t handle) {
    uint32_t wcm = 0, set = 0, n = 0, entries = 0, chr = 0, own = 0;
    const uint32_t block = (handle >> 11) & 0x1F, at = handle & 0x7FF;
    if (block > 0x1C || !read32(kWorldChrManVar, &wcm) || !wcm || !read32(wcm + 0x240 + block * 4, &set) || !set ||
        !read32(set, &n) || at >= n || !read32(set + 4, &entries) || !read32(entries + at * 0x20, &chr) || !chr ||
        !read32(chr + 0x78, &own) || own != handle) {
        return 0;
    }
    return chr;
}

void set_hp(uint32_t chr, int hp) {
    using SetHpFn = void(__stdcall*)(uint32_t, int);
    __try {
        reinterpret_cast<SetHpFn>(kSetHp)(chr, hp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The lowest health any list gives an enemy (its share of its max HP).
void merge_hurt(HurtList* base, const HurtList& add) {
    for (const auto& [h, v] : add) {
        auto it = base->find(h);
        if (it == base->end() || static_cast<double>(v.hp) * it->second.max_hp <
                                     static_cast<double>(it->second.hp) * v.max_hp) {
            (*base)[h] = v;
        }
    }
}

void collect_hurt(bool own) {
    g_hurt_apply.clear();
    if (own) {
        for (const auto& [id, mate] : g_mates_hurt) merge_hurt(&g_hurt_apply, mate);
    } else {
        g_hurt_apply = g_host_hurt;
    }
}

// After our reload: each enemy on the list goes down to its share of health in the other games, once the co-op
// scaling of its health is in place here. Health only ever goes down here.
void hurt_tick() {
    if (g_hurt_until == 0) return;
    if (game::loading() || !game::player()) {
        g_hurt_loaded = true;
        g_hurt_until = now() + kHurtWindow;
        return;
    }
    if (!g_hurt_loaded) {
        g_hurt_until = now() + kHurtWindow;  // still in the old world, at the respawn warp
        return;
    }
    g_hurt_armed = false;
    if (now() > g_hurt_until || g_hurt_apply.empty()) {
        if (!g_hurt_apply.empty() || g_hurt_applied) {
            LOG_INFO("[coop] Our reload put back the health of %d hurt enemies (%u not found here)", g_hurt_applied,
                     static_cast<unsigned>(g_hurt_apply.size()));
        }
        g_hurt_apply.clear();
        g_hurt_until = 0;
        g_hurt_applied = 0;
        return;
    }
    static double last = 0;
    if (now() - last < 0.25) return;
    last = now();
    for (auto it = g_hurt_apply.begin(); it != g_hurt_apply.end();) {
        const uint32_t chr = chr_by_net(it->first);
        uint32_t vt = 0, hp = 0, max_hp = 0;
        if (!chr || !read32(chr, &vt) || vt != kEnemyVtable || !read32(chr + 0x2D4, &hp) ||
            !read32(chr + 0x2D8, &max_hp) || static_cast<int>(max_hp) <= 0 || !world::scale_ready(chr)) {
            ++it;
            continue;
        }
        if (static_cast<int>(hp) > 0) {
            const double share = static_cast<double>(it->second.hp) / it->second.max_hp;
            const int want = std::max(1, static_cast<int>(share * static_cast<int>(max_hp) + 0.5));
            if (want < static_cast<int>(hp)) {
                set_hp(chr, want);
                ++g_hurt_applied;
            }
        }
        it = g_hurt_apply.erase(it);
    }
}

bool parse(const uint8_t* d, size_t size, List* out) {
    out->clear();
    uint32_t w[3];
    if (!d || size < 8) return false;
    std::memcpy(w, d, 8);
    if (w[0] != kMagic || w[1] != kVersion) return false;
    size_t at = 8;
    while (at + 12 <= size) {
        std::memcpy(w, d + at, 12);
        if (w[0] == kEnd) return true;
        const uint32_t bytes = (w[2] + 7) / 8;
        if (w[2] == 0 || w[2] > 0x10000 || w[1] != 12 + bytes || at + w[1] > size) return false;
        Set s;
        s.key = w[0];
        s.n = w[2];
        s.dead.assign(d + at + 12, d + at + w[1]);
        out->push_back(std::move(s));
        at += w[1];
    }
    return false;
}

std::vector<uint8_t> serialize(const List& list) {
    std::vector<uint8_t> out(8);
    const uint32_t head[2] = {kMagic, kVersion};
    std::memcpy(out.data(), head, 8);
    for (const Set& s : list) {
        const uint32_t w[3] = {s.key, static_cast<uint32_t>(12 + s.dead.size()), s.n};
        const size_t at = out.size();
        out.resize(at + 12 + s.dead.size());
        std::memcpy(out.data() + at, w, 12);
        if (!s.dead.empty()) std::memcpy(out.data() + at + 12, s.dead.data(), s.dead.size());
    }
    const uint32_t end[3] = {kEnd, 0, 0};
    const size_t at = out.size();
    out.resize(at + 12);
    std::memcpy(out.data() + at, end, 12);
    return out;
}

int dead_count(const List& list) {
    int n = 0;
    for (const Set& s : list) {
        for (uint8_t b : s.dead) {
            for (; b; b &= b - 1) ++n;
        }
    }
    return n;
}

Set* find(List& list, uint32_t key) {
    for (Set& s : list) {
        if (s.key == key) return &s;
    }
    return nullptr;
}

// Dead in either list is dead.
void merge_dead(List* base, const List& add) {
    for (const Set& a : add) {
        Set* b = find(*base, a.key);
        if (!b) {
            base->push_back(a);
        } else if (b->n == a.n && b->dead.size() == a.dead.size()) {
            for (size_t i = 0; i < a.dead.size(); ++i) b->dead[i] |= a.dead[i];
        }
    }
}

// The sets `base` does not have, from `add`.
void add_missing(List* base, const List& add) {
    for (const Set& a : add) {
        if (!find(*base, a.key)) base->push_back(a);
    }
}

uint32_t call_build(uint32_t wcm, uint8_t* out) {
    using BuildFn = uint32_t(__stdcall*)(uint32_t, uint8_t*);
    __try {
        return reinterpret_cast<BuildFn>(kBuildList)(wcm, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// GameMan's copy replaced through the game's own setter: its heap, which the load frees it with.
bool call_set(bool own, const uint8_t* data, uint32_t bytes) {
    const uint32_t fn = kSetList, flag = own ? 1 : 0;
    __try {
        __asm {
            pushad
            mov edi, bytes
            push data
            mov eax, flag
            call fn
            popad
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void call_free(bool own) {
    const uint32_t gm = game::game_man(), fn = kFreeList, flag = own ? 1 : 0;
    if (!gm) return;
    __try {
        __asm {
            pushad
            mov esi, gm
            push flag
            call fn
            popad
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The size of GameMan's copy (0: none, or a load took it).
uint32_t game_list_size(bool own) {
    uint32_t gm = game::game_man(), size = 0;
    if (!gm || !read32(gm + (own ? kGmOwnList : kGmHostList), &size)) return 0;
    return size;
}

// Gives the game the list our reload is to take: ours merged with the guests' (we host; dead in any game is dead
// there too, a guest's kill during our load included), or the host's (a guest: the host's game decides the HP of
// the characters both games have; ours fills in the sets the host does not have loaded).
bool give(const char* why) {
    List list = g_ours;
    char from[64];
    if (g_armed_own) {
        for (const auto& [id, mate] : g_mates) merge_dead(&list, mate);
        std::snprintf(from, sizeof(from), "ours and %u guest list(s)", static_cast<unsigned>(g_mates.size()));
    } else if (!g_host.empty()) {
        list = g_host;
        add_missing(&list, g_ours);
        std::snprintf(from, sizeof(from), "the host's list");
    } else {
        std::snprintf(from, sizeof(from), "our own list: none came from the host");
    }
    const std::vector<uint8_t> data = serialize(list);
    const uint32_t size = static_cast<uint32_t>(data.size());
    if (!call_set(g_armed_own, data.data(), size) || game_list_size(g_armed_own) != size) {
        LOG_ERROR("[coop] %s: the list of the world's dead could not be handed to the game", why);
        return false;
    }
    LOG_INFO("[coop] %s: our reload keeps %d dead character(s) of this world dead (%s)", why, dead_count(list), from);
    return true;
}

}  // namespace

std::vector<uint8_t> build() {
    uint32_t wcm = 0;
    if (game::loading() || !game::player() || !read32(kWorldChrManVar, &wcm) || !wcm) return {};
    const uint32_t size = call_build(wcm, nullptr);
    if (size < 20 || size > 0x100000) return {};
    std::vector<uint8_t> out(size);
    if (call_build(wcm, out.data()) != size) return {};
    return out;
}

void note_list(uint64_t from, bool from_host, const uint8_t* data, size_t size) {
    List list;
    if (!parse(data, size, &list)) {
        static int lines = 0;
        if (++lines <= 10) LOG_INFO("[coop] A list of the world's dead that does not read (%u bytes)", unsigned(size));
        return;
    }
    // Kept until we stand in the world again (tick): one that comes while we live is gone the same frame.
    const bool first = from_host ? g_host.empty() : !g_mates.count(from);
    if (first && (g_dead || g_armed)) {
        LOG_INFO("[coop] The %s list of the world's dead came (%d dead)", from_host ? "host's" : "guest's",
                 dead_count(list));
    }
    if (from_host) {
        g_host = std::move(list);
    } else {
        g_mates[from] = std::move(list);
    }
    // Kills during our load: a list our reload has not taken yet gets them.
    if (g_armed && from_host != g_armed_own && game_list_size(g_armed_own)) give("A newer list came during our load");
}

std::vector<uint8_t> build_hurt() {
    std::vector<uint8_t> out;
    uint32_t wcm = 0, info = 0, blocks = 0;
    if (game::loading() || !game::player() || !read32(kWorldChrManVar, &wcm) || !wcm || !read32(wcm + 0x10, &info) ||
        !info || !read32(info + 0xC, &blocks) || blocks > 0x1C) {
        return out;
    }
    std::vector<int32_t> words;
    for (uint32_t k = 0; k < blocks; ++k) {
        uint32_t set = 0, n = 0, entries = 0;
        if (!read32(wcm + 0x240 + k * 4, &set) || !set || !read32(set, &n) || !read32(set + 4, &entries) || n > 0x800) {
            continue;
        }
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t chr = 0, vt = 0, net = 0, hp = 0, max_hp = 0;
            if (!read32(entries + i * 0x20, &chr) || !chr || !read32(chr, &vt) || vt != kEnemyVtable ||
                !read32(chr + 0x78, &net) || static_cast<int>(net) < 0 || !read32(chr + 0x2D4, &hp) ||
                !read32(chr + 0x2D8, &max_hp) || static_cast<int>(hp) <= 0 || hp >= max_hp) {
                continue;
            }
            words.push_back(static_cast<int32_t>(net));
            words.push_back(static_cast<int32_t>(hp));
            words.push_back(static_cast<int32_t>(max_hp));
        }
    }
    const uint32_t head[2] = {kHurtMagic, static_cast<uint32_t>(words.size() / 3)};
    out.resize(8 + words.size() * 4);
    std::memcpy(out.data(), head, 8);
    if (!words.empty()) std::memcpy(out.data() + 8, words.data(), words.size() * 4);
    return out;
}

void note_hurt(uint64_t from, bool from_host, const uint8_t* data, size_t size) {
    HurtList list;
    if (!parse_hurt(data, size, &list)) return;
    // Merged, not replaced: a list holds only the sender's loaded map blocks, and one sent after the sender walked
    // away from an enemy simply leaves it out. That is no heal (only a rest heals, and note_rest clears the lists):
    // the lower share of health wins, and an enemy a later list leaves out keeps what an earlier one said.
    merge_hurt(from_host ? &g_host_hurt : &g_mates_hurt[from], list);
    // Until our load begins the newest lists count (an enemy hurt while we were on our way to the warp).
    if (g_hurt_armed && !g_hurt_loaded && from_host != g_hurt_own) collect_hurt(g_hurt_own);
}

void note_rest() {
    g_host.clear();
    g_mates.clear();
    g_host_hurt.clear();
    g_mates_hurt.clear();
    g_hurt_apply.clear();
    g_hurt_until = 0;
    g_hurt_armed = false;
    if (g_dead) g_rested = true;
    if (!g_armed) return;
    g_armed = false;
    if (game_list_size(g_armed_own)) call_free(g_armed_own);
    LOG_INFO("[coop] A rest during our load: our reload starts the world over too");
}

bool arm(bool own_world) {
    if (g_rested) {
        LOG_INFO("[coop] A rest came during our death: our reload starts the world over, as everyone's did");
        return false;
    }
    const std::vector<uint8_t> raw = build();
    if (!parse(raw.data(), raw.size(), &g_ours)) {
        LOG_INFO("[coop] Our world's dead characters could not be read: our reload starts the world over");
        return false;
    }
    g_armed_own = own_world;
    g_loading_seen = false;
    g_armed = give("Our death's respawn warp goes");
    if (g_armed) {
        g_hurt_own = own_world;
        g_hurt_armed = true;
        g_hurt_loaded = false;
        g_hurt_applied = 0;
        collect_hurt(own_world);
        g_hurt_until = now() + kHurtWindow;
        LOG_INFO("[coop] Our reload is to put back the health of %u hurt enemies (%s)",
                 static_cast<unsigned>(g_hurt_apply.size()), own_world ? "the guests' lists" : "the host's list");
    }
    return g_armed;
}

void tick() {
    const uint32_t me = game::player();
    uint32_t hp = 0;
    const bool dead = me && read32(me + 0x2D4, &hp) && static_cast<int>(hp) <= 0;
    if (me && !dead && !game::loading()) {
        // We stand in the world again.
        g_dead = false;
        g_rested = false;
        g_host.clear();
        g_mates.clear();
        if (!g_hurt_armed) {
            g_host_hurt.clear();
            g_mates_hurt.clear();
        }
    } else if (dead) {
        g_dead = true;
    }
    hurt_tick();
    if (!g_armed) return;
    if (game::loading()) g_loading_seen = true;
    if (!game_list_size(g_armed_own)) {
        g_armed = false;
        LOG_INFO("[coop] Our reload took the list of the world's dead");
    } else if (g_loading_seen && me && !game::loading()) {
        g_armed = false;
        call_free(g_armed_own);
        LOG_INFO("[coop] Our load did not take the list of the world's dead: dropped");
    }
}

}  // namespace coop::deadlist
