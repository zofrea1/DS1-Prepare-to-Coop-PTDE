// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "itemshare.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace coop::itemshare {

namespace {

// ---------------------------------------------------------------- the game's floor items
// The item manager ([0x137D6BC]) keeps the items on the floor as objects of 0xC0 bytes; +0x10 is the entry they are
// made from (0x90 bytes): +0x00 the handle (the kind in the low 4 bits, an id above, given by the manager that makes
// it), +0x08 the event flag a pickup sets (-1 none), +0x0C the character it came from (-1), +0x10 the position (x, y, z,
// w), +0x20 the floor it lies on, +0x24 the pickup radius, +0x28 flags (0x40 can be picked up, 0x80 the other players'
// too), +0x2C the number of items, +0x30 the items, 16 bytes each: {id with its category in the top 4 bits, -1, count,
// durability}, six at most.
// A player's drop from the inventory (0xDC8C60 from the menus -> 0xDC88E0, also the part of a pickup that did not fit)
// is kind 1, made at the player's feet. Unless the item's param says otherwise (weapon +0x104 bit 0, armor +0xDE bit 6,
// ring +0x3C bit 2, goods +0x46 bit 3) it gets 0x80, and then goes through 0xDC7E90: a summoned client asks the host
// for it (packet 0x2A, kind 3), the host (or a solo player) makes it with 0xDC7E00 -> 0xDC7D20 and the host tells its
// clients (kind 4): in vanilla co-op the host's phantoms see the host's drop and can pick it up. Prepare to Co-op runs
// every game's item manager as its own world's (rules.cpp, kItemClientChecks: a guest's items are its own, and the
// host's item packets are dropped), so until 1.4.0 a drop lay in its player's game alone.
// Pickup (0xDC9460, stdcall(manager, handle), from the action 0xD535A0): an item with 0x80 goes through 0xDC8EB0 (a
// client asks the host, kind 5; else 0xDC8E50: removed with 0xDC78A0, which tells clients, and given), one without is
// removed with 0xDC76F0 and given with 0xDC8CD0 (the items that do not fit are dropped again).
// A drop we share loses its 0x80 at once (hk_net_create, before 0xDC7E00 reads it at 0xDC7E1B), and our copies of the
// others' never have it: the game's own channel stays out of it. With 0x80 a host's drop also went out as packet 0x2A
// kind 4; co-op guests drop those, but an invader's game made it, could pick it up (kind 5, which the host's game
// answers at 0xDC906A with 0xDC78A0 and kind 6, past our pickup) and keep it while every co-op copy stayed on the
// floor (Grok review be211f6c). Without 0x80 the game removes it with 0xDC76F0 and gives it with 0xDC8CD0.
// Kind 1 holds 30 at most: 0xDC7B30 removes the oldest (0xDC76F0, from 0xDC7BAA) while [manager+0x14] >= 30 before it
// links a new one, past our pickup too. A copy is made only while there is room, and one the cap took from our floor
// (gone outside a load) is made again once there is (tick).
constexpr uint32_t kItemManVar = 0x0137D6BC;
constexpr uint32_t kNetCreate = 0x00DC7D20;  // ecx = entry, edx = manager -> the object (a shared item made here)
constexpr uint32_t kCreate = 0x00DC7C80;     // ecx = entry, edx = manager -> the object (by kind)
constexpr uint32_t kPickup = 0x00DC9460;     // stdcall(manager, handle)
constexpr uint32_t kFind = 0x00DC4AE0;       // thiscall(manager, handle) -> the object, 0 none
constexpr uint32_t kRemove = 0x00DC76F0;     // stdcall(manager, handle, 1)
constexpr uint32_t kPlayerDrop = 1;
constexpr uint32_t kCanPickUp = 0x40;
constexpr uint32_t kShared = 0x80;
constexpr int kMaxItems = 6;
constexpr uint32_t kDropCount = 0x14;  // the manager's count of kind-1 items
constexpr uint32_t kDropCap = 30;      // 0xDC7B30 removes the oldest at this many
constexpr float kMakeNear = 60.0f;     // a copy is made once our player is this near it
constexpr int kMaxRemade = 3;          // a copy taken off our floor that often is not made again
constexpr size_t kMaxDrops = 96;
constexpr float kAtOurFeet = 5.0f;   // a drop of ours lies here (a queued one waits a frame or so)
constexpr double kClaimWait = 3.0;  // a request the host did not answer may be made again after this

// The registry: every shared drop of this session, ours and the others', in every game.
struct Shared {
    Drop d;
    uint32_t handle = 0;  // our copy of it (the original if it is ours); 0 none here (not made yet, or gone)
    bool made = false;    // our copy was made: never made again (a load took it away, as it takes our own drops)
    bool taken = false;
    uint64_t taker = 0;
    double claimed_at = 0;  // our request to pick it up went to the host then (a guest)
    bool loaded_since = false;  // a load came since our copy was made: a copy missing now went with it
    int remade = 0;             // times our copy was made again after the cap (or a block) took it
    // The host gave it to us while our copy was gone from our floor (not by a load) and could not be made again then:
    // the cap's 30 left no room, or the making failed. Made and taken by tick once there is room; the others took
    // theirs off their floors on the host's word, so this is the only way it still comes to us.
    bool granted = false;
    int grant_tries = 0;  // makings that failed for it
};
std::vector<Shared> g_shared;

bool g_enabled = true;
bool g_coop = false;
bool g_host = false;
uint64_t g_self = 0;
uint32_t g_seq = 0;
bool g_granting = false;  // our own pickup of a drop the host gave us: not a request
void (*g_out_drop)(const Drop&) = nullptr;
void (*g_out_claim)(uint64_t, uint32_t) = nullptr;
void (*g_out_taken)(uint64_t, uint32_t, uint64_t) = nullptr;

using CreateFn = uint32_t(__fastcall*)(void* entry, uint32_t man);
using PickupFn = void(__stdcall*)(uint32_t man, uint32_t handle);
CreateFn g_net_create_orig = nullptr;
PickupFn g_pickup_orig = nullptr;
int g_lines = 0;

double now() { return GetTickCount64() / 1000.0; }

uint32_t manager() {
    uint32_t m = 0;
    return read32(kItemManVar, &m) ? m : 0;
}

// thiscall(manager, handle), ret 4: a fastcall with edx unused takes the same registers and stack.
using FindFn = uint32_t(__fastcall*)(uint32_t man, uint32_t unused, uint32_t handle);

uint32_t find_object(uint32_t man, uint32_t handle) {
    __try {
        return reinterpret_cast<FindFn>(kFind)(man, 0, handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool read_entry(uint32_t obj, uint8_t* out) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(obj + 0x10)), kEntryBytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t create(uint8_t* entry, uint32_t man) {
    __try {
        return reinterpret_cast<CreateFn>(kCreate)(entry, man);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void remove_object(uint32_t man, uint32_t handle) {
    __try {
        reinterpret_cast<void(__stdcall*)(uint32_t, uint32_t, int)>(kRemove)(man, handle, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

uint32_t entry_u32(const uint8_t* e, int at) {
    uint32_t v = 0;
    std::memcpy(&v, e + at, 4);
    return v;
}

float entry_f(const uint8_t* e, int at) {
    float v = 0;
    std::memcpy(&v, e + at, 4);
    return v;
}

// A floor item of ours that a drop's entry names: the same handle, kind, items and place.
bool is_copy(uint32_t obj, uint32_t handle, const uint8_t* e) {
    uint8_t now_e[kEntryBytes];
    if (!read_entry(obj, now_e)) return false;
    if (entry_u32(now_e, 0) != handle || (handle & 0xF) != kPlayerDrop) return false;
    const uint32_t n = entry_u32(e, 0x2C);
    if (entry_u32(now_e, 0x2C) != n || n < 1 || n > kMaxItems) return false;
    for (uint32_t i = 0; i < n; ++i) {
        if (entry_u32(now_e, 0x30 + i * 16) != entry_u32(e, 0x30 + i * 16) ||
            entry_u32(now_e, 0x38 + i * 16) != entry_u32(e, 0x38 + i * 16)) {
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(entry_f(now_e, 0x10 + i * 4) - entry_f(e, 0x10 + i * 4)) > 0.05f) return false;
    }
    return true;
}

// Our copy of a drop, if it is still on our floor.
bool copy_here(Shared& s, uint32_t man) {
    if (!s.handle) return false;
    const uint32_t obj = find_object(man, s.handle);
    if (obj && is_copy(obj, s.handle, s.d.entry)) return true;
    s.handle = 0;
    return false;
}

Shared* find(uint64_t origin, uint32_t seq) {
    for (Shared& s : g_shared) {
        if (s.d.origin == origin && s.d.seq == seq) return &s;
    }
    return nullptr;
}

Shared* by_handle(uint32_t man, uint32_t handle) {
    for (Shared& s : g_shared) {
        if (s.handle == handle && copy_here(s, man)) return &s;
    }
    return nullptr;
}

Shared& add(const Drop& d) {
    if (g_shared.size() >= kMaxDrops) {
        // The oldest taken one goes first, else the oldest.
        auto it = std::find_if(g_shared.begin(), g_shared.end(), [](const Shared& s) { return s.taken && !s.granted; });
        g_shared.erase(it != g_shared.end() ? it : g_shared.begin());
    }
    Shared s;
    s.d = d;
    g_shared.push_back(s);
    return g_shared.back();
}

// What a drop holds, for the log: "id x count" for each item.
std::string items_text(const uint8_t* e) {
    std::string t;
    const uint32_t n = std::min<uint32_t>(entry_u32(e, 0x2C), kMaxItems);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t id = entry_u32(e, 0x30 + i * 16);
        char b[48];
        std::snprintf(b, sizeof(b), "%s%u:%u x%u", t.empty() ? "" : ", ", id >> 28, id & 0x0FFFFFFF,
                      entry_u32(e, 0x38 + i * 16));
        t += b;
    }
    return t;
}

const char* who(uint64_t id) { return id == g_self ? "us" : "another player"; }

// A drop from another game's entry, checked before our item manager makes it.
bool sane(const uint8_t* e) {
    const uint32_t n = entry_u32(e, 0x2C);
    if ((entry_u32(e, 0) & 0xF) != kPlayerDrop || n < 1 || n > kMaxItems) return false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t cat = entry_u32(e, 0x30 + i * 16) >> 28;
        if (cat != 0 && cat != 1 && cat != 2 && cat != 4) return false;
    }
    for (int i = 0; i < 4; ++i) {
        const float v = entry_f(e, 0x10 + i * 4);
        if (!std::isfinite(v) || std::fabs(v) > 100000.0f) return false;
    }
    return true;
}

// ---------------------------------------------------------------- our drops
// (The part of a pickup that did not fit is dropped this way too, at our feet: shared as a drop of ours.) True when it
// is shared: the caller takes the game's own bit off.
bool note_created(uint32_t obj) {
    if (!g_enabled || !g_coop) return false;
    uint8_t e[kEntryBytes];
    if (!read_entry(obj, e)) return false;
    const uint32_t handle = entry_u32(e, 0), flags = entry_u32(e, 0x28);
    if ((handle & 0xF) != kPlayerDrop || !(flags & kShared) || !(flags & kCanPickUp)) return false;
    // Only our player's own drop: one at our feet (a host's game also makes the drops its clients ask for, kind 3).
    game::Vec3 me{};
    if (!game::position(&me, nullptr)) return false;
    const float dx = entry_f(e, 0x10) - me.x, dz = entry_f(e, 0x18) - me.z;
    if (std::sqrt(dx * dx + dz * dz) > kAtOurFeet || !sane(e)) return false;
    // A count of our own, from the time we started: a game started again does not repeat a drop the others still know.
    if (!g_seq) g_seq = static_cast<uint32_t>(std::time(nullptr)) << 4;
    Drop d;
    d.origin = g_self;
    d.seq = ++g_seq;
    d.map = game::map_id();
    std::memcpy(d.entry, e, sizeof(d.entry));
    d.entry[0] = static_cast<uint8_t>(kPlayerDrop);  // no handle: each game's manager gives its own
    d.entry[1] = d.entry[2] = d.entry[3] = 0;
    const uint32_t own_flags = flags & ~kShared;
    std::memcpy(d.entry + 0x28, &own_flags, 4);
    Shared& s = add(d);
    s.handle = handle;
    s.made = true;
    if (++g_lines <= 200) {
        LOG_INFO("[coop] Our drop (%s) lies on the other co-op players' floors too (map %08X)", items_text(e).c_str(),
                 d.map);
    }
    if (g_out_drop) g_out_drop(d);
    return true;
}

uint32_t __fastcall hk_net_create(void* entry, uint32_t man) {
    const uint32_t obj = g_net_create_orig(entry, man);
    if (obj && note_created(obj)) {
        uint32_t flags = 0;
        if (read32(obj + 0x38, &flags)) *ptr<uint32_t>(obj + 0x38) = flags & ~kShared;
    }
    return obj;
}

// Room for one more kind-1 item without the cap taking the oldest off a floor.
bool room(uint32_t man) {
    uint32_t n = 0;
    return read32(man + kDropCount, &n) && n < kDropCap;
}

// Our copy of a drop, made by our item manager as one of its own (a handle of its own, no 0x80). The handle, 0 if not.
uint32_t make_copy(Shared& s, uint32_t man) {
    uint8_t e[kEntryBytes];
    std::memcpy(e, s.d.entry, sizeof(e));
    e[0] = static_cast<uint8_t>(kPlayerDrop);
    e[1] = e[2] = e[3] = 0;
    const uint32_t flags = (entry_u32(e, 0x28) | kCanPickUp) & ~kShared;
    std::memcpy(e + 0x28, &flags, 4);
    const uint32_t obj = create(e, man);
    uint32_t handle = 0;
    if (!obj || !read32(obj + 0x10, &handle)) return 0;
    s.handle = handle;
    s.loaded_since = false;
    return handle;
}

// ---------------------------------------------------------------- pickups
void take_ours(Shared& s, uint32_t man) {
    s.taken = true;
    s.taker = g_self;
    const uint32_t handle = s.handle;
    s.handle = 0;
    g_granting = true;
    g_pickup_orig(man, handle);
    g_granting = false;
}

void __stdcall hk_pickup(uint32_t man, uint32_t handle) {
    if (g_granting || !g_enabled || !g_coop) {
        g_pickup_orig(man, handle);
        return;
    }
    Shared* s = by_handle(man, handle);
    if (!s) {
        g_pickup_orig(man, handle);
        return;
    }
    if (g_host) {
        // The host decides: the first to ask has it.
        if (++g_lines <= 200) LOG_INFO("[coop] Picked up a shared drop (%s)", items_text(s->d.entry).c_str());
        const uint64_t origin = s->d.origin;
        const uint32_t seq = s->d.seq;
        take_ours(*s, man);
        if (g_out_taken) g_out_taken(origin, seq, g_self);
        return;
    }
    // A guest asks the host and picks it up once the host says it is ours (remote_taken).
    if (now() - s->claimed_at >= kClaimWait) {
        s->claimed_at = now();
        if (++g_lines <= 200) {
            LOG_INFO("[coop] Asked the host for a shared drop (%s, dropped by %s)", items_text(s->d.entry).c_str(),
                     who(s->d.origin));
        }
        if (g_out_claim) g_out_claim(s->d.origin, s->d.seq);
    }
}

// Our copy of a drop someone else took: off our floor.
void drop_copy(Shared& s) {
    const uint32_t man = manager();
    if (man && !game::loading() && copy_here(s, man)) remove_object(man, s.handle);
    s.handle = 0;
}

}  // namespace

bool install() {
    // push ebp ; mov ebp,esp ; and esp,-16
    static const uint8_t kCreateCode[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    // sub esp,6Ch ; push ebx ; mov ebx,[esp+74h]
    static const uint8_t kPickupCode[8] = {0x83, 0xEC, 0x6C, 0x53, 0x8B, 0x5C, 0x24, 0x74};
    // 0xDC76F0 is detoured by rules.cpp (per-player treasure, hk_item_remove) before we get here: its own bytes or that
    // jump. Until this was seen the check refused, and no drop was ever shared (2026-11-01 test).
    uint8_t first = 0;
    const bool remove_ok = bytes_match(kRemove, "\x8B\x44\x24\x08\x8B\x4C\x24\x04", 8) ||
                           (read8(kRemove, &first) && first == 0xE9);
    if (!bytes_match(kCreate, "\x56\x8B\x31\x83\xE6\x0F", 6) || !bytes_match(kFind, "\x56\x8B\x74\x24\x08", 5) ||
        !remove_ok) {
        LOG_ERROR("[coop] Shared drops: the item manager does not match this build; drops stay with their player");
        return false;
    }
    g_net_create_orig = reinterpret_cast<CreateFn>(detour(kNetCreate, kCreateCode, sizeof(kCreateCode),
                                                         reinterpret_cast<void*>(&hk_net_create)));
    if (!g_net_create_orig) {
        LOG_ERROR("[coop] Shared drops: the shared item maker does not match this build; drops stay with their player");
        return false;
    }
    g_pickup_orig = reinterpret_cast<PickupFn>(
        detour(kPickup, kPickupCode, sizeof(kPickupCode), reinterpret_cast<void*>(&hk_pickup)));
    if (!g_pickup_orig) {
        LOG_ERROR("[coop] Shared drops: the pickup does not match this build; drops stay with their player");
        g_enabled = false;  // the maker is hooked but does nothing then
        return false;
    }
    return true;
}

void set_enabled(bool on) { g_enabled = on && g_pickup_orig; }

void set_session(bool coop, bool host, uint64_t self) {
    const bool on = coop && g_enabled;
    if (!on && g_coop && !g_shared.empty()) {
        // The session is over: what still lies on our floor stays as any item does; nobody else has it now.
        if (++g_lines <= 200) LOG_INFO("[coop] Shared drops: %zu forgotten (the session is over)", g_shared.size());
        for (const Shared& s : g_shared) {
            if (s.granted && ++g_lines <= 200) {
                LOG_INFO("[coop] A shared drop the host gave us (%s) never found room on our floor: lost with the "
                         "session", items_text(s.d.entry).c_str());
            }
        }
        g_shared.clear();
    }
    g_coop = on;
    g_host = host;
    g_self = self;
}

void set_out(void (*drop)(const Drop&), void (*claim)(uint64_t, uint32_t), void (*taken)(uint64_t, uint32_t, uint64_t)) {
    g_out_drop = drop;
    g_out_claim = claim;
    g_out_taken = taken;
}

void remote_drop(const Drop& d) {
    // (One of ours comes back only in the host's list for a newcomer: ours from before we left, if it still lies there.)
    if (!g_coop || !sane(d.entry)) return;
    if (find(d.origin, d.seq)) return;  // heard already
    add(d);
    if (++g_lines <= 200) {
        LOG_INFO("[coop] A drop of another player's (%s, map %08X) is shared with us", items_text(d.entry).c_str(), d.map);
    }
    tick();  // on our floor at once if we are there
}

void remote_claim(uint64_t from, uint64_t origin, uint32_t seq) {
    if (!g_coop || !g_host) return;
    Shared* s = find(origin, seq);
    if (!s) {
        // Not in our list (it should be: every drop comes through us): theirs, then, and nobody else's from now on.
        Drop d;
        d.origin = origin;
        d.seq = seq;
        s = &add(d);
        s->made = true;
    }
    if (!s->taken) {
        s->taken = true;
        s->taker = from;
        drop_copy(*s);
        if (++g_lines <= 200) LOG_INFO("[coop] A guest picked up a shared drop (%s)", items_text(s->d.entry).c_str());
    }
    // Who has it, to everyone (again, if a second request came: that guest's copy goes).
    if (g_out_taken) g_out_taken(origin, seq, s->taker);
}

void remote_taken(uint64_t origin, uint32_t seq, uint64_t taker) {
    if (!g_coop) return;
    Shared* s = find(origin, seq);
    if (!s) {
        Drop d;
        d.origin = origin;
        d.seq = seq;
        s = &add(d);
        s->made = true;  // it never lies here now
    }
    const uint32_t man = manager();
    if (taker == g_self && !s->taken && s->claimed_at > 0) {
        // Ours: picked up now, as the game would have when we asked.
        if (man && !game::loading() && copy_here(*s, man)) {
            if (++g_lines <= 200) LOG_INFO("[coop] The host gave us the shared drop (%s)", items_text(s->d.entry).c_str());
            take_ours(*s, man);
            return;
        }
        // Gone from our floor since we asked (not by a load): made again and taken, so the grant is not lost.
        if (man && !game::loading() && !s->loaded_since && room(man) && make_copy(*s, man)) {
            if (++g_lines <= 200) {
                LOG_INFO("[coop] The host gave us the shared drop (%s), gone from our floor meanwhile: made again and "
                         "taken", items_text(s->d.entry).c_str());
            }
            take_ours(*s, man);
            return;
        }
        // Not now: most likely the cap itself took our copy (0xDC7B30 takes the oldest at 30 and counts the new one,
        // so the count stays at 30 and there is no room). Ours all the same: tick makes and takes it once there is.
        if (!s->loaded_since) {
            s->taken = true;
            s->taker = g_self;
            s->granted = true;
            s->handle = 0;
            if (++g_lines <= 200) {
                LOG_INFO("[coop] The host gave us the shared drop (%s), gone from our floor meanwhile and no room to "
                         "make it again now: it comes once there is", items_text(s->d.entry).c_str());
            }
            return;
        }
        // Gone from our floor meanwhile (a load): nobody has it, and nobody else will.
        if (++g_lines <= 200) LOG_INFO("[coop] The host gave us a shared drop that is no longer on our floor");
    }
    if (!s->taken && ++g_lines <= 200 && s->handle) {
        LOG_INFO("[coop] A shared drop (%s) was picked up by another player: off our floor", items_text(s->d.entry).c_str());
    }
    s->taken = true;
    s->taker = taker;
    drop_copy(*s);
}

std::vector<Drop> live() {
    std::vector<Drop> out;
    for (const Shared& s : g_shared) {
        if (!s.taken && sane(s.d.entry)) out.push_back(s.d);
    }
    return out;
}

void tick() {
    // Two seconds after a load (or the session's start) before anything is made: the map's floor is in by then.
    static double settled_at = 0;
    static double last_call = 0;
    static double checked_at = 0;
    if (g_coop && game::loading()) {
        // A copy missing after this load went with it (as a load takes the dropper's own): not made again.
        for (Shared& s : g_shared) s.loaded_since = true;
    }
    if (!g_coop || !game::in_game() || game::loading()) return;
    if (now() - last_call > 0.5) settled_at = now();
    last_call = now();
    if (g_shared.empty() || now() - settled_at < 2.0) return;
    const uint32_t man = manager();
    if (!man) return;
    // Our copies, once a second: one gone without a load (the kind-1 cap took it, or its block went away) is made again
    // once we are in its map and there is room; one a load took stays gone.
    if (now() - checked_at >= 1.0) {
        checked_at = now();
        for (Shared& s : g_shared) {
            if (!s.handle || s.taken) continue;
            const uint32_t obj = find_object(man, s.handle);
            if (obj && is_copy(obj, s.handle, s.d.entry)) {
                s.loaded_since = false;  // still here after the load
                continue;
            }
            const bool by_load = s.loaded_since;
            s.handle = 0;
            // (An object of another kind of ours under that handle: not ours any more, and not made again either.)
            if (!obj && !by_load && s.remade < kMaxRemade) {
                s.made = false;
                ++s.remade;
            }
            if (++g_lines <= 200) {
                LOG_INFO("[coop] A shared drop (%s) is gone from our floor (%s)", items_text(s.d.entry).c_str(),
                         by_load ? "a load" : obj ? "its handle is another item's" : "not picked up: made again later");
            }
        }
        // The host gave us these while our copy was gone and the floor full (remote_taken): made in its map, where the
        // ones we have not picked up are made, and picked up at once.
        for (Shared& s : g_shared) {
            if (!s.granted || s.d.map != game::map_id() || !room(man)) continue;
            if (!make_copy(s, man)) {
                if (++s.grant_tries >= 5) {
                    s.granted = false;
                    if (++g_lines <= 200) {
                        LOG_ERROR("[coop] A shared drop the host gave us (%s) could not be put on our floor: lost",
                                  items_text(s.d.entry).c_str());
                    }
                }
                continue;
            }
            s.granted = false;
            if (++g_lines <= 200) {
                LOG_INFO("[coop] The shared drop the host gave us (%s) is ours now: made and taken",
                         items_text(s.d.entry).c_str());
            }
            take_ours(s, man);
        }
    }
    const int map = game::map_id();
    game::Vec3 me{};
    if (!game::position(&me, nullptr)) return;
    int made = 0;
    for (Shared& s : g_shared) {
        if (s.made || s.taken || s.d.map != map || made >= 4) continue;
        // Near our player: a block far off may not keep it.
        const float dx = entry_f(s.d.entry, 0x10) - me.x, dz = entry_f(s.d.entry, 0x18) - me.z;
        if (std::sqrt(dx * dx + dz * dz) > kMakeNear) continue;
        // Only while the cap leaves room: making it would take the oldest drop off our floor.
        if (!room(man)) break;
        s.made = true;
        ++made;
        if (!make_copy(s, man)) {
            if (++g_lines <= 200) LOG_ERROR("[coop] A shared drop could not be put on our floor");
            continue;
        }
        if (++g_lines <= 200) {
            LOG_INFO("[coop] A shared drop (%s, dropped by %s) lies on our floor at (%.1f, %.1f, %.1f)",
                     items_text(s.d.entry).c_str(), who(s.d.origin), entry_f(s.d.entry, 0x10),
                     entry_f(s.d.entry, 0x14), entry_f(s.d.entry, 0x18));
        }
    }
}

}  // namespace coop::itemshare
