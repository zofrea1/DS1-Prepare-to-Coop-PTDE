// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "summons.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace coop::summons {

namespace {

// Character handles: 0x10000000 | block << 14 | index, block = the map block's place in the world's block list (the
// same in every game), index = the character's place in that block's set. WorldChrMan+0x240 + block * 4 -> set
// {count, entries of 0x20 bytes, character first}; a block that is not loaded has an empty set.
constexpr uint32_t kChrByHandle = 0x00E3E340;  // edx = handle, esi = WorldChrMan -> character or 0
constexpr uint32_t kChrByEntity = 0x00D6C360;  // eax = entity id -> character or 0
// 0xE407F0: edx = handle, stdcall(type, kind): the character becomes a phantom of that type (+0x70, team through
// 0xE66AB0), or 0 when it cannot (vt+0x9C).
constexpr uint32_t kMakePhantom = 0x00E407F0;
// 0xEA2510: stdcall(party, handle, chr type, 2 = NPC, f1, f2, f3); f3 < 0 shows "summoned" and the summon effect.
constexpr uint32_t kPartyAdd = 0x00EA2510;
// 0xEA2A10: eax = handle, stdcall(party): the member leaves; for an NPC (entry type 2) its character goes back to
// what it was before the summon (+0x70 = 9, a full heal, the event notice 0xDA0770). Reads the character without a
// check: only called with a character in this world.
constexpr uint32_t kPartyLeave = 0x00EA2A10;
constexpr uint32_t kPartyRecount = 0x00EA1A70;  // edx = party: the counts +4..+0x10 from the entries
// PartyMemberInfo ([GameMan+0xBEC]): four entries of 0x14 bytes from +0x18 {handle (-1 none), type (1 player,
// 2 NPC), state, f1, f2}; +0x10 and +0x14 count the party (the probe's "party a/b").
constexpr uint32_t kEntries = 0x18, kEntrySize = 0x14;
constexpr int kEntryCount = 4;
constexpr uint32_t kTypeNpc = 2;
// The packet loop (0xEE2F8B, the in-game step): packet 48 = {handle, f1, f2, chr type, f3} -> 0xE407F0 at kRecvMake,
// then 0xEA2510 with the character it returned (read without a check: the crash). Packet 49 = {handle, value} ->
// 0xEA2DD0(party, handle, value) at kRecvState, which looks the character up again for an NPC entry.
constexpr uint32_t kRecvMake = 0x00EE3023;
constexpr uint32_t kRecvNext = 0x00EE3047;  // test esi, esi: the loop's next packet
constexpr uint32_t kRecvState = 0x00EE30C0;
constexpr uint32_t kPartyState = 0x00EA2DD0;
// The party's update (0xEA2FD0, every frame) runs each entry's state (+8): 1 (being summoned), 3 (in the world) and 4
// (dying) look the character up and read it with no check. A phantom NPC of the host's party lives in a map block of
// ours: a guest who walks away from it unloads its block, and the next update read through the empty lookup
// (0xEA3107 in state 3). Vanilla never unloads the host's map under a phantom. An entry whose character is not here
// waits (the loop's next entry) until its block is back; tick then makes the fresh copy a phantom again.
constexpr uint32_t kUpdateState = 0x00EA2FFC;  // mov eax,[ebp] ; dec eax ; cmp eax,5 (ebp = entry + 8, edi = handle)
constexpr uint32_t kUpdateNext = 0x00EA3340;   // add ebp,14h: the next entry
// WorldChrMan's blocks: +0x70 count, +0x74 the blocks; each block's entity table +0x50 count, +0x54 pairs
// {entity, handle} sorted by entity (0xE41E50 searches it).
constexpr uint32_t kBlockCount = 0x70, kBlocks = 0x74, kEntityCount = 0x50, kEntityPairs = 0x54;

bool g_visitor = false;
std::map<uint32_t, int32_t> g_entity_of;   // our NPC party entries (handle) -> the entity they are
std::vector<Phantom> g_host;               // the host's NPC phantoms (we visit)
std::map<int32_t, uint32_t> g_made;        // entity -> the character we turned into its phantom
std::map<uint32_t, double> g_gone_since;   // our NPC party entries (handle) the host no longer lists, since when
std::set<uint32_t> g_told_waiting;         // characters we said could not become a phantom yet
bool g_heard = false;                      // a list came from the host (a 1.2.0 host sends none)
int g_lines = 0;

double now() { return GetTickCount64() / 1000.0; }

uint32_t wcm() {
    uint32_t v = 0;
    return read32(game::kWorldChrManVar, &v) ? v : 0;
}

uint32_t party() {
    uint32_t p = 0;
    const uint32_t gm = game::game_man();
    return gm && read32(gm + game::kGmParty, &p) && p > 0x10000 ? p : 0;
}

uint32_t chr_by_handle(uint32_t handle) {
    const uint32_t w = wcm();
    if (!w || handle == 0xFFFFFFFF) return 0;
    uint32_t chr = 0;
    __try {
        __asm {
            push esi
            mov esi, w
            mov edx, handle
            mov eax, kChrByHandle
            call eax
            pop esi
            mov chr, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return chr;
}

uint32_t chr_by_entity(int32_t entity) {
    if (entity < 0) return 0;
    uint32_t chr = 0;
    __try {
        __asm {
            mov eax, entity
            mov edx, kChrByEntity
            call edx
            mov chr, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return chr;
}

// The entity id the loaded blocks give `handle`, -1 none.
int32_t entity_of(uint32_t handle) {
    const uint32_t w = wcm();
    uint32_t n = 0, blocks = 0;
    if (!w || !read32(w + kBlockCount, &n) || !read32(w + kBlocks, &blocks) || n > 64) return -1;
    for (uint32_t b = 0; b < n; ++b) {
        uint32_t block = 0, count = 0, pairs = 0;
        if (!read32(blocks + b * 4, &block) || !block || !read32(block + kEntityCount, &count) ||
            !read32(block + kEntityPairs, &pairs) || !pairs || count > 0x4000) {
            continue;
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t entity = 0, h = 0;
            if (!read32(pairs + i * 8, &entity) || !read32(pairs + i * 8 + 4, &h)) break;
            if (h == handle) return static_cast<int32_t>(entity);
        }
    }
    return -1;
}

int entry_of(uint32_t p, uint32_t handle) {
    for (int i = 0; i < kEntryCount; ++i) {
        uint32_t h = 0;
        if (read32(p + kEntries + i * kEntrySize, &h) && h == handle) return i;
    }
    return -1;
}

int32_t chr_type(uint32_t chr) {
    uint32_t v = 0;
    return chr && read32(chr + 0x70, &v) ? static_cast<int32_t>(v) : -1;
}

bool __cdecl entry_waits(uint32_t state, uint32_t handle) {
    if ((state != 1 && state != 3 && state != 4) || chr_by_handle(handle)) return false;
    static uint32_t told = 0xFFFFFFFF;
    if (told != handle) {
        told = handle;
        LOG_INFO("[coop] Party member %08X (state %u) has no character here now: its update waits", handle, state);
    }
    return true;
}

// At kUpdateState: [esp] = return (kUpdateState + 5, two nops, then ja). The flags of the cmp go back to the ja.
__declspec(naked) void hk_update_state() {
    __asm {
        pushad
        push edi
        push dword ptr [ebp]
        call entry_waits
        add esp, 8
        test al, al
        popad
        jnz later
        mov eax, [ebp]
        dec eax
        cmp eax, 5
        ret
    later:
        add esp, 4
        push kUpdateNext
        ret
    }
}

// The packet's second argument, from the type (0xEE300A).
int32_t kind_of(int32_t type) { return type == 1 ? 1 : type == 2 ? 3 : 0; }

uint32_t make_phantom(uint32_t handle, int32_t as_type) {
    const int32_t kind = kind_of(as_type);
    uint32_t chr = 0;
    __try {
        __asm {
            push kind
            push as_type
            mov edx, handle
            mov eax, kMakePhantom
            call eax
            mov chr, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return chr;
}

bool party_add(uint32_t p, uint32_t handle, int32_t type, int32_t f1, int32_t f2) {
    using AddFn = char(__stdcall*)(uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
    __try {
        return reinterpret_cast<AddFn>(kPartyAdd)(p, handle, type, static_cast<int32_t>(kTypeNpc), f1, f2, 0) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void party_leave(uint32_t p, uint32_t handle) {
    __try {
        __asm {
            push p
            mov eax, handle
            mov edx, kPartyLeave
            call edx
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void recount(uint32_t p) {
    __try {
        __asm {
            push ebx
            push esi
            push edi
            mov edx, p
            mov eax, kPartyRecount
            call eax
            pop edi
            pop esi
            pop ebx
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The entry goes as 0xEA1900 empties one: handle -1, type 1, state 0, -1, -1.
void blank_entry(uint32_t p, int i) {
    const uint32_t e = p + kEntries + i * kEntrySize;
    *ptr<int32_t>(e) = -1;
    *ptr<int32_t>(e + 4) = 1;
    *ptr<int32_t>(e + 8) = 0;
    *ptr<int32_t>(e + 0xC) = -1;
    *ptr<int32_t>(e + 0x10) = -1;
}

// The handle in this game of the NPC the host's game calls `host_handle`, 0 when this game does not have it now
// (its map block is not loaded here). The block numbers come from the world's block list (WorldChrMan is made once,
// with room for 27 blocks), so a handle names the same character in every game that has its block; the NPC the
// host named (kNpcPhantoms) is looked up by its entity id first all the same.
uint32_t __cdecl our_handle(uint32_t host_handle) {
    if (g_visitor) {
        for (const Phantom& ph : g_host) {
            if (ph.handle != host_handle) continue;
            const uint32_t chr = chr_by_entity(ph.entity);
            uint32_t h = 0;
            if (chr && read32(chr + 4, &h)) {
                if (h != host_handle && ++g_lines <= 40) {
                    LOG_INFO("[coop] The host's NPC phantom %d is character %08X here (the host's %08X)", ph.entity,
                             h, host_handle);
                }
                return h;
            }
            break;
        }
    }
    if (chr_by_handle(host_handle)) return host_handle;
    if (++g_lines <= 40) {
        LOG_INFO("[coop] NPC phantom %08X of another game is not in our world yet: it becomes one here once it is",
                 host_handle);
    }
    return 0;
}

// At kRecvMake: [esp] = return (0xEE3028), [esp+4] = type, [esp+8] = kind, edx = the host's handle; esi (the
// packet's size) and ebx (the party) are the loop's. With no character the packet is dropped and the loop goes on.
__declspec(naked) void hk_recv_make() {
    __asm {
        pushad
        push edx
        call our_handle
        add esp, 4
        mov [esp + 0x14], eax  // edx after popad
        test eax, eax
        popad
        jz drop
        push dword ptr [esp + 8]
        push dword ptr [esp + 8]
        mov eax, kMakePhantom
        call eax
        test eax, eax
        jz drop
        ret 8
    drop:
        add esp, 12
        push kRecvNext
        ret
    }
}

uint32_t __cdecl our_state_handle(uint32_t host_handle) {
    const uint32_t h = our_handle(host_handle);
    return h && chr_by_handle(h) ? h : 0;
}

// At kRecvState: [esp] = return, [esp+4] = party, [esp+8] = the host's handle, [esp+0xC] = value.
__declspec(naked) void hk_recv_state() {
    __asm {
        pushad
        push dword ptr [esp + 0x28]
        call our_state_handle
        add esp, 4
        test eax, eax
        jz drop
        mov [esp + 0x28], eax
        popad
        mov eax, kPartyState
        jmp eax
    drop:
        popad
        ret 0xC
    }
}

}  // namespace

std::vector<Phantom> ours() {
    std::vector<Phantom> out;
    const uint32_t p = party();
    if (!p || game::loading()) return out;
    for (int i = 0; i < kEntryCount; ++i) {
        const uint32_t e = p + kEntries + i * kEntrySize;
        uint32_t h = 0, type = 0, f1 = 0, f2 = 0;
        if (!read32(e, &h) || h == 0xFFFFFFFF || !read32(e + 4, &type) || type != kTypeNpc) continue;
        const uint32_t chr = chr_by_handle(h);
        const int32_t entity = chr ? entity_of(h) : -1;
        const int32_t t = chr_type(chr);
        if (entity < 0 || (t != 1 && t != 2)) continue;
        read32(e + 0xC, &f1);
        read32(e + 0x10, &f2);
        Phantom ph;
        ph.entity = entity;
        ph.handle = h;
        ph.kind = chr_type(chr);
        ph.f1 = static_cast<int32_t>(f1);
        ph.f2 = static_cast<int32_t>(f2);
        out.push_back(ph);
    }
    return out;
}

int count() {
    const uint32_t p = party();
    int n = 0;
    for (int i = 0; p && i < kEntryCount; ++i) {
        uint32_t h = 0, type = 0;
        if (read32(p + kEntries + i * kEntrySize, &h) && h != 0xFFFFFFFF &&
            read32(p + kEntries + i * kEntrySize + 4, &type) && type == kTypeNpc) {
            ++n;
        }
    }
    return n;
}

void note_host_list(const std::vector<Phantom>& list) {
    std::set<int32_t> before, after;
    for (const Phantom& ph : g_host) before.insert(ph.entity);
    for (const Phantom& ph : list) after.insert(ph.entity);
    if (before != after) {
        std::string ids;
        for (int32_t e : after) ids += (ids.empty() ? "" : " ") + std::to_string(e);
        LOG_INFO("[coop] The host's NPC phantoms: %s", ids.empty() ? "none" : ids.c_str());
    }
    g_host = list;
    g_heard = true;
}

// Our own world: an NPC entry whose character is no longer a phantom goes. The party is kept through our own loads
// in a session (rules.cpp keeps it for the guests), and a load makes the NPC anew, unsummoned: after the host's death
// Solaire's entry would have stayed with his health bar while he stood at his sign again. Vanilla clears the party
// at that load. The scripted summon (+0x7C, 0xEA1F40) keeps its own type and is left alone.
void own_world_sweep() {
    static double last = 0;
    if (now() - last < 1.0) return;
    last = now();
    const uint32_t p = party();
    uint32_t special = 0;
    if (!p || game::loading() || !game::player() || !game::own_world() || !read32(p + 0x7C, &special) ||
        special != 0xFFFFFFFF) {
        return;
    }
    for (int i = 0; i < kEntryCount; ++i) {
        uint32_t h = 0, type = 0;
        if (!read32(p + kEntries + i * kEntrySize, &h) || h == 0xFFFFFFFF ||
            !read32(p + kEntries + i * kEntrySize + 4, &type) || type != kTypeNpc) {
            continue;
        }
        const uint32_t chr = chr_by_handle(h);
        const int32_t t = chr_type(chr);
        if (!chr || t == 1 || t == 2) continue;
        blank_entry(p, i);
        recount(p);
        LOG_INFO("[coop] NPC %08X is no longer a phantom here (type %d after a load): out of our party", h,
                 static_cast<int>(t));
    }
}

void tick(bool visitor) {
    g_visitor = visitor;
    if (!visitor) {
        g_host.clear();
        g_made.clear();
        g_gone_since.clear();
        g_told_waiting.clear();
        g_entity_of.clear();
        g_heard = false;
        own_world_sweep();
        return;
    }
    static double last = 0;
    if (now() - last < 0.5) return;
    last = now();
    const uint32_t p = party();
    if (!p || game::loading() || !game::player()) {
        g_made.clear();  // a load makes every character anew
        g_told_waiting.clear();
        return;
    }
    std::set<int32_t> listed;
    for (const Phantom& ph : g_host) {
        listed.insert(ph.entity);
        const uint32_t chr = chr_by_entity(ph.entity);
        uint32_t h = 0;
        if (!chr || !read32(chr + 4, &h) || h == 0xFFFFFFFF) continue;
        const bool phantom = chr_type(chr) == ph.kind;
        const bool in_party = entry_of(p, h) >= 0;
        if (phantom && in_party) continue;
        if (!phantom && !make_phantom(h, ph.kind)) {
            if (g_told_waiting.insert(chr).second && ++g_lines <= 40) {
                LOG_INFO("[coop] The host's NPC phantom %d (character %08X) cannot become one here yet", ph.entity, h);
            }
            continue;
        }
        const bool added = in_party || party_add(p, h, chr_type(chr), ph.f1, ph.f2);
        if (added) g_entity_of[h] = ph.entity;
        auto it = g_made.find(ph.entity);
        if ((it == g_made.end() || it->second != chr) && ++g_lines <= 40) {
            LOG_INFO("[coop] The host's NPC phantom %d is a phantom here too (character %08X, type %d%s)", ph.entity, h,
                     static_cast<int>(chr_type(chr)), added ? "" : "; our party has no room");
        }
        g_made[ph.entity] = chr;
    }
    // Our NPC entries the host no longer lists (the NPC went home, died, or the host closed its world and the
    // NPC is gone there): after a moment they go here too, as the game's own departure does.
    for (int i = 0; i < kEntryCount; ++i) {
        const uint32_t e = p + kEntries + i * kEntrySize;
        uint32_t h = 0, type = 0;
        if (!read32(e, &h) || h == 0xFFFFFFFF || !read32(e + 4, &type) || type != kTypeNpc) continue;
        const uint32_t chr = chr_by_handle(h);
        const auto known = g_entity_of.find(h);
        const int32_t entity = chr ? entity_of(h) : known != g_entity_of.end() ? known->second : -1;
        // Listed, or not known while its block is away: it stays (its update waits, hk_update_state).
        if ((entity >= 0 && listed.count(entity)) || (entity < 0 && !chr) || !g_heard) {
            g_gone_since.erase(h);
            continue;
        }
        auto it = g_gone_since.find(h);
        if (it == g_gone_since.end()) {
            g_gone_since[h] = now();
            continue;
        }
        if (now() - it->second < 3.0) continue;
        g_gone_since.erase(it);
        const int32_t t = chr_type(chr);
        if (chr && (t == 1 || t == 2)) {
            party_leave(p, h);  // the game's own departure of an NPC phantom
        } else {
            blank_entry(p, i);
            recount(p);
        }
        g_entity_of.erase(h);
        if (++g_lines <= 40) LOG_INFO("[coop] NPC phantom %d (character %08X) left our party: the host's has gone", entity, h);
    }
}

bool settle_party(uint32_t p) {
    if (!p || !game::own_world()) return false;
    uint32_t local = 0;
    const uint32_t me = game::player();
    if (me) read32(me + 4, &local);
    bool kept = false;
    for (int i = 0; i < kEntryCount; ++i) {
        uint32_t h = 0, type = 0;
        if (!read32(p + kEntries + i * kEntrySize, &h) || h == 0xFFFFFFFF) continue;
        read32(p + kEntries + i * kEntrySize + 4, &type);
        const int32_t t = chr_type(chr_by_handle(h));
        if (type == kTypeNpc && (t == 1 || t == 2)) kept = true;
    }
    if (!kept) return false;
    int npcs = 0;
    bool changed = false;
    for (int i = 0; i < kEntryCount; ++i) {
        uint32_t h = 0, type = 0;
        if (!read32(p + kEntries + i * kEntrySize, &h) || h == 0xFFFFFFFF) continue;
        read32(p + kEntries + i * kEntrySize + 4, &type);
        const int32_t t = chr_type(chr_by_handle(h));
        if (type == kTypeNpc && (t == 1 || t == 2)) {
            ++npcs;
            continue;
        }
        if (h == local && local) continue;
        blank_entry(p, i);
        changed = true;
    }
    uint32_t before10 = 0, before14 = 0, n = 0;
    read32(p + 0x10, &before10);
    read32(p + 0x14, &before14);
    recount(p);
    read32(p + 0x10, &n);
    *ptr<uint32_t>(p + 0x14) = n;
    if (changed || n != before10 || n != before14) {
        LOG_INFO("[coop] The session is over: the party keeps our %d NPC phantom(s) (party %u/%u -> %u)", npcs,
                 before10, before14, n);
    }
    return true;
}

bool install() {
    const bool make = redirect_call(kRecvMake, kMakePhantom, reinterpret_cast<void*>(&hk_recv_make));
    const bool state = redirect_call(kRecvState, kPartyState, reinterpret_cast<void*>(&hk_recv_state));
    static const uint8_t k[7] = {0x8B, 0x45, 0x00, 0x48, 0x83, 0xF8, 0x05};
    uint8_t code[7] = {0xE8, 0, 0, 0, 0, 0x90, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_update_state) - (kUpdateState + 5));
    std::memcpy(code + 1, &rel, 4);
    const bool update = patch(kUpdateState, k, code, sizeof(code));
    if (!make || !state || !update) {
        LOG_ERROR("[coop] NPC phantoms: the game's code does not match this build (%d/%d/%d)", make, state, update);
    }
    return make && state && update;
}

}  // namespace coop::summons
