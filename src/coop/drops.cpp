// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "drops.h"

#include "game.h"
#include "hook.h"
#include "items.h"
#include "log.h"
#include "party.h"
#include "session.h"
#include "text.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace coop::drops {

namespace {

// ---------------------------------------------------------------- the drops a world gives once
// One row per enemy that a map script removes on every load once its flag is on (Force Character Death or
// disabled), with what the script awards when it dies (Award Items (Including Clients) 2003[36], Award Item Lot
// 2003[04]) and its own NpcParam lot (itemLotId_1, dropped at its death by 0xE80670), unless that script drops its
// treasure again on every load (the NPCs, whose lots carry their own one-time flags). The bosses' rewards from
// common event 970 (a boss flag, then up to three lots) are rows without an enemy. 141 rows, from every map's
// script: the Black Knights, crystal lizards, drakes, Titanite Demons, mimics, Pinwheel's masks, the Stray Demon's
// slab, the Taurus and Capra Demons' weapons, Havel, Ricard's Rapier, the Oolacile Humanity sprites.
struct Row {
    int32_t flag;       // on once it is gone for good: its script's own done flag, or the boss flag
    int32_t entity;     // the enemy (0: common event 970's boss rewards)
    int32_t floor;      // its own drop, on the floor where it dies (0 none)
    int32_t lots[3];    // what its scripts award, into the inventory (0 none)
    int32_t boss_slot;  // event 970's slot that awards a boss's rewards (-1: an enemy's row)
};
const Row kRows[] = {
#include "drops_table.inc"
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
constexpr uint8_t kFloorBit = 1u << 3;  // bits 0-2: lots[0..2]

// Our game's own roll can come a little after the flag: the floor drop waits for the death animation, an award for
// our copy of the enemy to die, and common event 970's second and third lots for 5 s after the first.
constexpr double kWait = 12.0;
// An enemy still standing in our game may yet die here and drop for us: its drop waits for that, for a load (the
// enemy's script then removes it: killed, or disabled with its health as it was, like the crystal lizards) or for
// this long. A drop the mod gave is never given again by the game (allow_award, floor_drop_here), also after the
// session while its flag is on.
constexpr double kAliveWait = 90.0;
constexpr double kForget = 120.0;    // a record without a drop of ours outlives its last change by this much
constexpr double kRetry = 5.0;       // an item that did not fit in the inventory is tried again this often

struct Item {
    uint32_t category;  // 0 weapon, 0x10000000 armor, 0x20000000 ring, 0x40000000 goods
    int32_t id;
    int count;
};
// One drawn entry of a lot: its items, and its one-time flag (-1 none), set once every item is in the inventory.
struct Owed {
    int flag = -1;
    std::vector<Item> items;
};

struct State {
    uint64_t world = 0;   // whose world it was recorded in (session::world_owner)
    double armed_at = 0;  // its flag turned on in our game during a session (0: not yet)
    double touched = 0;   // last change
    int loads = 0;        // g_loads when it was armed
    uint8_t got = 0;      // what our game has: its own roll, or ours
    uint8_t ours = 0;     // ... of which the mod rolls (the game's own late roll of these is skipped)
    uint8_t give = 0;     // still to roll, once the player can take it
    bool missed = false;  // the mod rolls a drop our game missed (the line says so)
    std::vector<Owed> owed;  // rolled for us, not in the inventory yet (it did not fit)
    double retry_at = 0;
    bool told_full = false;
    int landed = 0;          // items put in the inventory at the last delivery
};
std::map<int, State> g_rows;  // row index -> state; one with a drop of ours is kept while the character is loaded
std::vector<int> g_flags;     // every row's flag, sorted
bool g_installed = false;
int g_loads = 0;              // map loads seen
bool g_was_loading = false;
bool g_was_active = false;    // we were in a session at the last tick
std::wstring g_owner;         // the character the records are for

double now() { return GetTickCount64() / 1000.0; }

// Our part of a session: the host or a co-op guest. An invader's game keeps the vanilla drops.
bool active() { return g_installed && session::in_session() && !session::invader(); }

uint8_t needs(const Row& r) {
    uint8_t n = r.floor > 0 ? kFloorBit : 0;
    for (int i = 0; i < 3; ++i) {
        if (r.lots[i] > 0) n |= static_cast<uint8_t>(1u << i);
    }
    return n;
}

int lot_of(const Row& r, int bit) { return bit == 3 ? r.floor : r.lots[bit]; }

// ---------------------------------------------------------------- the game's characters
constexpr uint32_t kChrByEntity = 0x00D6C360;  // eax = entity id -> character, or 0

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

int chr_hp(uint32_t chr) {
    uint32_t hp = 0;
    return chr && read32(chr + 0x2D4, &hp) ? static_cast<int>(hp) : 0;
}

// The enemy cannot drop anything in our game any more: not loaded here, or dead.
bool enemy_gone(int entity) { return entity == 0 || chr_hp(chr_by_entity(entity)) <= 0; }

// ---------------------------------------------------------------- the game's lot roll
// 0x47F640 (stdcall(roll, 0), as the enemy drop 0xDC80A0 and the award 0xDC9170 call it) draws a lot and the lots
// after it (lot + 1, ... while rows exist), skipping an entry whose one-time flag (getItemFlagId) is on, with the
// player's item discovery from 0xDC9FC0 (xmm0). Each drawn entry is 0x50 bytes: +0 the flag to set once it is
// given (-1 none), +5 the number of items, then from +8 per item 0xC bytes: +0 the id (low 28 bits), +4 the
// category in the high nibble (0xF: nothing), +8 the count (byte), as 0x4810E0 decodes them. 0x562B30 frees the
// entries (cdecl). 0xDC9170 then hands each item to the inventory and sets the flag through the block lookup.
struct Roll {
    int32_t lot;
    float luck;
    uint8_t apply;  // 1 in the drop and the award
    uint8_t pad[3];
    int32_t count;
    uint32_t entries;
};
static_assert(sizeof(Roll) == 0x14, "the game's lot roll");
constexpr uint32_t kRollLot = 0x0047F640;
constexpr uint32_t kLuck = 0x00DC9FC0;
constexpr uint32_t kFree = 0x00562B30;
constexpr uint32_t kEntrySize = 0x50;
// The inventory's own add (items.cpp): stdcall(PlayerGameData+0x1B8, category, id, count, 1, 0) -> slot, -1 full.
constexpr uint32_t kInventoryAdd = 0x00C0B700;

bool draw(Roll* rp) {
    __try {
        float luck = 1.0f;
        __asm {
            mov eax, kLuck
            call eax
            movss luck, xmm0
        }
        rp->luck = luck;
        __asm {
            push 0
            push rp
            mov eax, kRollLot
            call eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void free_draw(uint32_t entries) {
    __try {
        __asm {
            push entries
            mov eax, kFree
            call eax
            add esp, 4
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool roll(int lot, std::vector<Owed>* out) {
    Roll r{};
    r.lot = lot;
    r.apply = 1;
    if (!draw(&r)) {
        LOG_ERROR("[coop] Rolling item lot %d failed", lot);
        return false;
    }
    for (int e = 0; e < r.count && e < 16 && r.entries; ++e) {
        const uint32_t base = r.entries + e * kEntrySize;
        uint32_t flag = 0;
        uint8_t n = 0;
        if (!read32(base, &flag) || !read8(base + 5, &n)) break;
        Owed entry;
        entry.flag = static_cast<int32_t>(flag) >= 0 ? static_cast<int32_t>(flag) : -1;
        for (int k = 0; k < n && k < 6; ++k) {
            uint32_t id = 0, cat = 0;
            uint8_t count = 0;
            if (!read32(base + 8 + k * 12, &id) || !read32(base + 12 + k * 12, &cat) ||
                !read8(base + 16 + k * 12, &count)) {
                break;
            }
            const uint32_t c = (cat >> 28) & 0xF;
            if ((c != 0 && c != 1 && c != 2 && c != 4) || count == 0) continue;  // the lot's "nothing"
            entry.items.push_back(Item{c << 28, static_cast<int32_t>(id & 0x0FFFFFFF), count});
        }
        out->push_back(std::move(entry));
    }
    if (r.entries) free_draw(r.entries);
    return true;
}

bool add_to_inventory(const Item& it) {
    const uint32_t pgd = game::player_game_data();
    if (!pgd) return false;
    int slot = -1;
    __try {
        using Fn = int(__stdcall*)(uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t);
        slot = reinterpret_cast<Fn>(kInventoryAdd)(pgd + 0x1B8, it.category, it.id, it.count, 1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot = -1;
    }
    return slot != -1;
}

// ---------------------------------------------------------------- item names
// The name getters (weapon 0xD17D10, armor 0xD17C50, ring 0xD17B90, goods 0xD17DD0) ask the FMG lookup for the
// 1.0.2 patch FMG first and then the base one (MsgRepository offsets below).
constexpr uint32_t kFmgLookup = 0x00D175E0;   // edx = id, [esp+4] = FMG; returns wchar_t* or 0 (cdecl)
constexpr uint32_t kMsgRepoVar = 0x013785DC;

const wchar_t* fmg_text(uint32_t fmg, int32_t id) {
    const wchar_t* s = nullptr;
    __try {
        __asm {
            push fmg
            mov edx, id
            mov eax, kFmgLookup
            call eax
            add esp, 4
            mov s, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s = nullptr;
    }
    return s;
}

std::wstring item_name(const Item& it) {
    static const uint32_t kNames[5][2] = {{0x1D0, 0x30}, {0x1D8, 0x34}, {0x1C8, 0x38}, {0, 0}, {0x1C0, 0x2C}};
    const uint32_t c = it.category >> 28;
    uint32_t repo = 0;
    if (c > 4 || !kNames[c][0] || !read32(kMsgRepoVar, &repo) || !repo) return L"";
    for (uint32_t off : kNames[c]) {
        uint32_t fmg = 0;
        if (!read32(repo + off, &fmg) || !fmg) continue;
        const wchar_t* s = fmg_text(fmg, it.id);
        if (s && *s) {
            std::wstring name(s);
            if (it.count > 1) name += L" x" + std::to_wstring(it.count);
            return name;
        }
    }
    return L"";
}

// ---------------------------------------------------------------- giving
// Rolls one lot for us. The draw is kept until it is all in the inventory, so an item that did not fit is not rolled
// again: a weapon won at 20 % stays won.
void roll_lot(int lot, State* s) {
    std::vector<Owed> drawn;
    if (!roll(lot, &drawn)) return;
    size_t n = 0;
    for (const Owed& e : drawn) n += e.items.size();
    if (!n) LOG_INFO("[coop] One-time drop: lot %d gave nothing (its chance did not come up)", lot);
    for (Owed& e : drawn) s->owed.push_back(std::move(e));
}

// Puts what is owed in the inventory and shows it in the game's item-get window, as the game's award (0xDC8CD0) does.
// An entry's one-time flag is set once all its items are in, as the award (0xDC9170) sets it after giving them; what
// did not fit stays owed. The names of what came without the window go to `names`, of what did not fit to `full`.
void deliver(State* s, std::vector<std::wstring>* names, std::vector<std::wstring>* full) {
    for (auto e = s->owed.begin(); e != s->owed.end();) {
        for (auto it = e->items.begin(); it != e->items.end();) {
            // The add takes a whole stack or nothing (0xC0B700 returns -1); as many as fit go in, the game's award
            // asking 0xDC55C0 how many that is.
            int put = 0;
            for (int k = it->count; k >= 1 && !put; --k) {
                Item part = *it;
                part.count = k;
                if (add_to_inventory(part)) put = k;
            }
            if (put) {
                Item part = *it;
                part.count = put;
                LOG_INFO("[coop] One-time drop: item %08X %d x%d is in the inventory", it->category, it->id, put);
                ++s->landed;
                const std::wstring name = item_name(part);
                if (!items::show_obtained(it->category, it->id, put) && !name.empty()) names->push_back(name);
                it->count -= put;
            }
            if (it->count <= 0) {
                it = e->items.erase(it);
            } else {
                LOG_INFO("[coop] One-time drop: item %08X %d x%d did not fit in the inventory; it stays owed",
                         it->category, it->id, it->count);
                const std::wstring name = item_name(*it);
                if (!name.empty()) full->push_back(name);
                ++it;
            }
        }
        if (e->items.empty()) {
            if (e->flag >= 0) game::set_flag(e->flag, true);
            e = s->owed.erase(e);
        } else {
            ++e;
        }
    }
}

std::wstring join(const std::vector<std::wstring>& names) {
    std::wstring out;
    for (size_t i = 0; i < names.size() && i < 4; ++i) out += (i ? L", " : L"") + names[i];
    if (names.size() > 4) out += L", ...";
    return out;
}

// Standing in the world, alive: what is given now is seen.
bool can_take() {
    const uint32_t me = game::player();
    return game::in_game() && !game::loading() && me && chr_hp(me) > 0;
}

// A record's knowledge of its enemy is void (a new playthrough's enemy, or its flag off); what is owed stays.
void forget(State* s) {
    s->armed_at = 0;
    s->got = 0;
    s->ours = 0;
}

// A record belongs to one world: the same enemy elsewhere (a guest's own world after the trip home, another host's)
// is another enemy, and the flag that says it is gone is that world's. A record from another world is forgotten
// before it is used; what is owed stays.
State& in_this_world(State& s) {
    const uint64_t w = session::world_owner();
    if (s.world != w) {
        if (s.got || s.ours || s.armed_at > 0) {
            LOG_INFO("[coop] One-time drop: a record from another world (%016llX) does not count here", s.world);
        }
        forget(&s);
        s.world = w;
    }
    return s;
}

// Drops our game did not roll: ours to roll.
void claim(State* s, const Row& r, uint8_t missing, double t, const char* when) {
    s->got |= missing;
    s->ours |= missing;
    s->give |= missing;
    s->missed = true;
    s->touched = t;
    LOG_INFO("[coop] One-time drop: %sflag %d (enemy %d) is on and our game did not roll its drop; we roll it", when,
             r.flag, r.entity);
}

// The session is over: the hooks no longer tell a drop of ours from the game's, only record what the game pays (got)
// while a row is armed. A drop is ours now if its enemy can no longer drop here: not loaded, or dead (a corpse whose
// floor drop came while we were down never drops again: 0xE80670 returns without its "dropped" bit then), or, for a
// boss's rewards, a load cut event 970 short. A living enemy stays armed: its death here pays as the game does.
void settle_session_end(double t, bool loading) {
    for (auto& [i, s] : g_rows) {
        const Row& r = kRows[i];
        const uint8_t missing = needs(r) & ~s.got;
        if (s.armed_at <= 0 || !missing) continue;
        const bool gone = r.entity ? enemy_gone(r.entity) : (g_loads != s.loads || loading);
        if (gone) claim(&s, r, missing, t, "the session ended; ");
    }
}

// A load after the session (a trip home, a rest, a death, the title): it removes every one-time enemy whose flag is
// on without a drop (Force Character Death without souls, 0xD66600) and cuts event 970 short, so whatever is still
// armed and missing is ours.
void settle_load(double t) {
    for (auto& [i, s] : g_rows) {
        const Row& r = kRows[i];
        const uint8_t missing = needs(r) & ~s.got;
        if (s.armed_at > 0 && missing) claim(&s, r, missing, t, "a load after the session; ");
    }
}

int row_of_entity(uint32_t chr) {
    if (!chr) return -1;
    for (int i = 0; i < kRowCount; ++i) {
        if (kRows[i].entity && kRows[i].floor > 0 && chr_by_entity(kRows[i].entity) == chr) return i;
    }
    return -1;
}

// ---------------------------------------------------------------- the floor drop
// 0xE80670 (esi = the character) drops a dead enemy's NpcParam lot once (chr+0x1FF bit 0x40, set by its caller
// after this call) when the local player is alive: 0xDC9410 (eax = the NpcParam row, stdcall(ItemMan, handle))
// -> 0xDC80A0, the items on the floor at the corpse. A one-time enemy's drop goes into the inventory instead
// while we are in a session, as its scripted award would.
constexpr uint32_t kFloorDropSite = 0x00E806BD;
constexpr uint32_t kFloorDrop = 0x00DC9410;

int __stdcall floor_drop_here(uint32_t chr) {
    if (!active()) {
        // After the session: a drop the mod rolls is held back (while its flag is on); one the game pays is noted.
        for (auto it = g_rows.begin(); it != g_rows.end(); ++it) {
            const Row& r = kRows[it->first];
            if (!r.entity || r.floor <= 0 || chr_by_entity(r.entity) != chr) continue;
            State& s = in_this_world(it->second);
            if (!game::flag(r.flag)) {
                forget(&s);  // a new playthrough's enemy
                return 1;
            }
            if (s.ours & kFloorBit) {
                LOG_INFO("[coop] One-time drop: enemy %d died here after its drop was given; no second drop",
                         r.entity);
                return 0;
            }
            if (s.armed_at > 0) s.got |= kFloorBit;
            return 1;
        }
        return 1;
    }
    const int row = row_of_entity(chr);
    if (row < 0) return 1;
    State& s = in_this_world(g_rows[row]);
    if ((s.got & kFloorBit) && !game::flag(kRows[row].flag)) forget(&s);  // a new playthrough's enemy
    s.touched = now();
    if (s.got & kFloorBit) {
        LOG_INFO("[coop] One-time drop: enemy %d died here after its drop was given; no second drop",
                 kRows[row].entity);
        return 0;
    }
    s.got |= kFloorBit;
    s.ours |= kFloorBit;
    s.give |= kFloorBit;
    LOG_INFO("[coop] One-time drop: enemy %d died here; its lot %d goes into the inventory", kRows[row].entity,
             kRows[row].floor);
    return 0;
}

__declspec(naked) void hk_floor_drop() {
    __asm {
        push ecx
        push edx
        push eax
        push esi
        call floor_drop_here
        test eax, eax
        pop eax
        pop edx
        pop ecx
        jz handled
        push kFloorDrop
        ret
    handled:
        ret 8
    }
}

}  // namespace

bool install() {
    g_flags.clear();
    for (const Row& r : kRows) g_flags.push_back(r.flag);
    std::sort(g_flags.begin(), g_flags.end());
    g_flags.erase(std::unique(g_flags.begin(), g_flags.end()), g_flags.end());
    if (!redirect_call(kFloorDropSite, kFloorDrop, reinterpret_cast<void*>(&hk_floor_drop))) {
        LOG_ERROR("[coop] Enemy drop call at %08X does not match this build; missed one-time drops are not given",
                  kFloorDropSite);
        return false;
    }
    g_installed = true;
    LOG_INFO("[coop] One-time drops: %d enemies and boss rewards watched", kRowCount);
    return true;
}

bool watched(int flag) { return g_installed && std::binary_search(g_flags.begin(), g_flags.end(), flag); }

void flag_changed(int flag, bool on) {
    if (!on) {
        for (auto& [i, s] : g_rows) {
            if (kRows[i].flag == flag) forget(&s);
        }
        return;
    }
    if (!active() || !game::player_game_data()) return;
    const double t = now();
    for (int i = 0; i < kRowCount; ++i) {
        if (kRows[i].flag != flag) continue;
        State& s = in_this_world(g_rows[i]);
        if (s.armed_at > 0) continue;
        s.armed_at = t;
        s.touched = t;
        s.loads = g_loads;
        LOG_INFO("[coop] One-time drop: flag %d is on (enemy %d): our game's own roll has %.0f s", flag,
                 kRows[i].entity, kWait);
    }
}

bool allow_award(int lot, int event, int slot) {
    if (!g_installed || lot <= 0) return true;
    int row = -1, bit = -1;
    if (event == 970) {
        // A boss's rewards: the row of this slot of the event (each boss has its own; the Humanity lots are shared).
        for (int i = 0; i < kRowCount && row < 0; ++i) {
            if (kRows[i].boss_slot != slot) continue;
            for (int k = 0; k < 3; ++k) {
                if (kRows[i].lots[k] == lot) {
                    row = i;
                    bit = k;
                    break;
                }
            }
        }
    } else if (event > 0) {
        const int flag = event + slot;
        for (int i = 0; i < kRowCount && row < 0; ++i) {
            if (kRows[i].flag != flag) continue;
            for (int k = 0; k < 3; ++k) {
                if (kRows[i].lots[k] == lot) {
                    row = i;
                    bit = k;
                    break;
                }
            }
        }
    }
    if (row < 0) return true;
    const uint8_t b = static_cast<uint8_t>(1u << bit);
    if (!active()) {
        // After the session: a drop the mod rolls is held back (while its flag is on); one the game pays is noted.
        const auto it = g_rows.find(row);
        if (it == g_rows.end()) return true;
        State& s = in_this_world(it->second);
        if (!game::flag(kRows[row].flag)) {
            forget(&s);  // a new playthrough's enemy
            return true;
        }
        if (s.ours & b) {
            LOG_INFO("[coop] One-time drop: our scripts award lot %d after it was given; skipped", lot);
            return false;
        }
        if (s.armed_at > 0) s.got |= b;
        return true;
    }
    State& s = in_this_world(g_rows[row]);
    if ((s.got & b) && !game::flag(kRows[row].flag)) forget(&s);  // a new playthrough's enemy
    s.touched = now();
    if ((s.got & b) && (s.ours & b)) {
        LOG_INFO("[coop] One-time drop: our scripts award lot %d after it was given; skipped", lot);
        return false;
    }
    s.got |= b;
    return true;
}

void tick() {
    if (!g_installed) return;
    if (!game::player_game_data()) {
        // No character. What was taken off the floor or chosen for us is still owed: kept for this character.
        settle_load(now());
        size_t dropped = 0;
        for (auto it = g_rows.begin(); it != g_rows.end();) {
            if (it->second.give || !it->second.owed.empty()) {
                it->second.armed_at = 0;
                ++it;
            } else {
                it = g_rows.erase(it);
                ++dropped;
            }
        }
        if (dropped) LOG_INFO("[coop] One-time drops: no character; %zu record(s) dropped", dropped);
        g_was_active = false;
        return;
    }
    const std::wstring me = party::local_name();
    if (!me.empty()) {
        if (!g_owner.empty() && me != g_owner && !g_rows.empty()) {
            LOG_INFO("[coop] One-time drops: another character; %zu record(s) dropped", g_rows.size());
            g_rows.clear();
        }
        g_owner = me;
    }
    const bool loading = game::loading();
    const bool load_started = loading && !g_was_loading;
    if (load_started) ++g_loads;
    g_was_loading = loading;
    const bool act = active();
    const double t = now();
    if (g_was_active && !act) settle_session_end(t, loading);
    if (!act && load_started) settle_load(t);  // also the warp the session's end asked for a frame earlier
    g_was_active = act;
    if (g_rows.empty()) return;
    const bool take = can_take();
    std::vector<std::wstring> found, missed, full;
    bool any_missed = false;
    for (auto it = g_rows.begin(); it != g_rows.end();) {
        const Row& r = kRows[it->first];
        State& s = it->second;
        if (act && s.armed_at > 0 && t - s.armed_at >= kWait) {
            const uint8_t missing = needs(r) & ~s.got;
            if (missing && (enemy_gone(r.entity) || g_loads != s.loads || t - s.armed_at >= kAliveWait)) {
                claim(&s, r, missing, t, "");
            }
        }
        if ((s.give || !s.owed.empty()) && take && t >= s.retry_at) {
            for (int bit = 0; bit < 4; ++bit) {
                if (s.give & (1u << bit)) roll_lot(lot_of(r, bit), &s);
            }
            s.give = 0;
            std::vector<std::wstring> no_room;
            s.landed = 0;
            deliver(&s, s.missed ? &missed : &found, &no_room);
            if (s.missed && s.landed) any_missed = true;
            if (s.owed.empty()) {
                s.told_full = false;
            } else {
                s.retry_at = t + kRetry;
                if (!s.told_full) full.insert(full.end(), no_room.begin(), no_room.end());
                s.told_full = true;
            }
            s.touched = t;
        }
        const bool waiting = s.give || !s.owed.empty() || (s.armed_at > 0 && (needs(r) & ~s.got));
        if (!s.ours && !waiting && t - s.touched > kForget) {
            it = g_rows.erase(it);
        } else {
            ++it;
        }
    }
    // The item-get window names what came; without it, the action box does.
    if (!found.empty()) text::show(L"Added to your inventory: " + join(found));
    if (!missed.empty()) {
        text::show(L"From a kill you missed: " + join(missed));
    } else if (any_missed) {
        text::show(L"From a kill you missed");
    }
    if (!full.empty()) text::show(L"No room in your inventory for " + join(full) + L"; it comes once there is room");
}

}  // namespace coop::drops
