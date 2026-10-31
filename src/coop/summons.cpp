// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "summons.h"

#include "game.h"
#include "hook.h"
#include "partylist.h"
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
// 0xEA2510: stdcall(party, handle, chr type, 2 = NPC, f1, f2, f3). f3 < 0 (a live summon: the sign table 0x12DED30
// gives -1 for a white sign, -3 for a black one) is state 1: the summon-in event animation 6951/6952 (0xE69590) and
// the "summoned" notice; f3 >= 0 is state 2 (sets flag f1) or 3 at once, with no animation.
constexpr uint32_t kPartyAdd = 0x00EA2510;
// Only the animation shows the copy in a guest's game: Solaire's own load event (m10_01 11015100) disables him there
// (0xE69A40 through 2004[05]) unless his summon flag is already on, and his sign, whose fade enables him in the host's
// game (0xDB4209), is only placed for the host. The resend to a joining player (0xEA2900) carries f3 = 0, so a guest
// who joined after the summon had him in its party, a phantom, and never drawn (2026-10-24, and so in vanilla).
constexpr uint32_t kSetDisabled = 0x00E69A40;  // edi = character, stdcall(disabled): bit 0 of [[chr+0xC]+0x14]
// 0xEA2A10: eax = handle, stdcall(party): the member leaves; for an NPC (entry type 2) its character goes back to
// what it was before the summon (+0x70 = 9, a full heal, the event notice 0xDA0770). Reads the character without a
// check: only called with a character in this world.
constexpr uint32_t kPartyLeave = 0x00EA2A10;
constexpr uint32_t kPartyRecount = 0x00EA1A70;  // edx = party: the counts +4..+0x10 from the entries
// 0xEA2E90: eax = handle, edi = party, stdcall(notice variant): the host's Black Separation Crystal menu's "send
// home" (0xC3FCF0). For an NPC entry: the "returned home" notice, 0xEA2A10, then packet 49 {handle, 0} to the
// session, whose receivers (0xEA2DD0, kRecvState here) take the NPC out of their parties the same way.
constexpr uint32_t kSendHome = 0x00EA2E90;
// PartyMemberInfo ([GameMan+0xBEC]): entries of 0x14 bytes from +0x18 {handle (-1 none), type (1 player, 2 NPC),
// state, f1, f2}, eight of them (partylist.cpp; the game's four if that does not fit this build); +0x10 and +0x14
// count the party (the probe's "party a/b").
constexpr uint32_t kEntries = 0x18, kEntrySize = 0x14;
constexpr uint32_t kTypeNpc = 2;
// The packet loop (0xEE2F8B, the in-game step): packet 48 = {handle, f1, f2, chr type, f3} -> 0xE407F0 at kRecvMake,
// then 0xEA2510 with the character it returned (read without a check: the crash). Packet 49 = {handle, value} ->
// 0xEA2DD0(party, handle, value) at kRecvState, which looks the character up again for an NPC entry.
constexpr uint32_t kRecvMake = 0x00EE3023;
constexpr uint32_t kRecvAdd = 0x00EE3042;   // its 0xEA2510 call: f3 from the packet
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
// WorldChrMan's blocks: +0x70 count, from +0x74 a pointer per block, in WorldChrMan itself (0xE3DC1F: lea edi,
// [ebp+0x74]); each block's entity table +0x50 count, +0x54 pairs {entity, handle} sorted by entity (0xE41E50
// searches it). Until the 2026-10-24 test this read +0x74 as a pointer to the list: no NPC phantom was ever named,
// the host's list went out empty, and a guest's copy of Solaire left its party 3 s after his summon.
constexpr uint32_t kBlockCount = 0x70, kBlocks = 0x74, kEntityCount = 0x50, kEntityPairs = 0x54;
// SosSignMan's summon of an NPC sign's phantom (0xDB3BF0, the pending summons): 0xE407F0, then 0xEA2510 here
// (stdcall(party, chr+4, sign table +0xC = chr type, 2, f1, f2, table +8 = f3)), then packet 48 to the session.
// The game summons so for whoever touched the sign (a black sign: when its time comes); a co-op guest's game too,
// now that NPC signs are placed for co-op guests (rules.cpp, hk_sign_session).
constexpr uint32_t kSignAdd = 0x00DB3E2F;
// Packet 48 follows (0xDB3E6E, the session's broadcast 0xD2D940): every game that has the NPC loaded makes its copy
// the phantom too (kRecvMake), the host's among them, whose party then holds it like one of its own summons.

bool g_visitor = false;
std::map<uint32_t, int32_t> g_entity_of;   // our NPC party entries (handle) -> the entity they are
std::vector<Phantom> g_host;               // the host's NPC phantoms (we visit)
std::map<int32_t, uint32_t> g_made;        // entity -> the character we turned into its phantom
std::map<uint32_t, double> g_gone_since;   // our NPC party entries (handle) the host no longer lists, since when
std::set<uint32_t> g_told_waiting;         // characters we said could not become a phantom yet
std::map<uint32_t, double> g_listed_since; // characters of the host's phantoms in our party, since when (show_late)
std::set<uint32_t> g_shown;                // characters show_late enabled
bool g_heard = false;                      // a list came from the host (a 1.2.0 host sends none)
// A co-op guest's own summons: the NPC phantoms our game summoned from a sign (kSignAdd). A summon is the world's,
// whoever made it: the host's game takes it into its party as one of its own (by packet 48, or by our word,
// kNpcSummons, once that NPC is in its world), and from then on it goes by the host's list like any of the host's
// summons. It stays through anyone's loads and leaving, and ends as the host's own do (its death, a team wipe, the
// host's Black Separation Crystal). Until the host's list names it, it is kept here as ours (a host before 1.4.0
// that did not have the NPC loaded at the summon never names it), and a load of ours ends it.
std::map<uint32_t, Phantom> g_mine;       // handle -> what it is
std::set<uint32_t> g_mine_listed;         // those the host's list has named: the host's list decides from then on
std::map<uint32_t, double> g_mine_ended;  // handle -> when it left us (our load, our party): not made again at once
uint32_t g_mine_player = 0;               // our character when g_mine was last brought up to date
// After one of our summons left us, the host's list can still name it for a moment (our word that it is over is on
// its way): long enough for that list to catch up, short enough that one the host's world keeps is back soon.
constexpr double kMineEndedHold = 3.0;
// Host: the NPC summons the co-op guests' games report (kNpcSummons, set_guest_summons). adopt_tick takes each into
// our party once its NPC is in our world, and own_refresh then has it as ours; until then our list names it as
// reported, so the other games keep their copies.
std::vector<Phantom> g_reported;
// Host: the NPCs whose summon ended in our game (its death, a dismissal, a team wipe), since when: a guest's
// report, which lags behind, does not take them in again meanwhile, and our list leaves them out.
std::vector<std::pair<Phantom, double>> g_ended;
constexpr double kEndedHold = 10.0;
// A visitor: the host's NPC phantoms its game sent home (packet 49: its Black Separation Crystal, a team wipe), as
// the host's list named them, since when. The list we hold can still name them (the host's next one follows the
// packet, on another channel, and none comes while the host loads): they are not summoned in again from it, here or
// onto a fresh copy after our own reload, until a list of the host's leaves them out (it decides then) or 30 s.
std::vector<std::pair<Phantom, double>> g_dismissed;
constexpr double kDismissedHold = 30.0;
// Our own world: characters of our NPC phantoms summoned in again (after our load, or their block's), since when:
// one still disabled 8 s on (its map's load event disables it; the summon-in animation takes a few) is enabled.
std::map<uint32_t, double> g_own_shown_at;
// Our own world: the NPC phantoms of our party as we last saw them loaded. Solaire follows his summoner anywhere in
// a session (no walls keep phantoms in an area, and the party is not reset at a map-block change), so his own map
// block can unload under him while he is still summoned: his character is gone until the block streams back, and
// comes back as a fresh, unsummoned copy. The summon goes on through that: it is still announced to the others,
// still counts and stays through the session's end, and the fresh copy is summoned in again. Our own loads (a
// death, a rest, travel) are the same in a session, where the party is kept through them (rules.cpp,
// allow_party_reset): the host is not special, and only the NPC's death, a team wipe (dismiss_all) or the host's
// Black Separation Crystal end a summon. Outside a session the game's own party reset at a load ends it.
struct Own {
    Phantom ph;
    uint32_t chr = 0;
    bool away = false;  // its block has unloaded since we last saw it as a phantom
    double back_at = 0; // when its block came back with a copy that would not become the phantom yet
};
std::map<uint32_t, Own> g_own;  // handle -> what it is
uint32_t g_own_player = 0;      // our character when g_own was last brought up to date (another one: a load)
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
    uint32_t n = 0;
    if (!w || !read32(w + kBlockCount, &n) || n > 64) return -1;
    for (uint32_t b = 0; b < n; ++b) {
        uint32_t block = 0, count = 0, pairs = 0;
        if (!read32(w + kBlocks + b * 4, &block) || !block || !read32(block + kEntityCount, &count) ||
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
    for (int i = 0; i < partylist::entries(); ++i) {
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

// The f3 of a live summon of a phantom of this type (white sign -1, black sign -3).
int32_t summon_f3(int32_t type) { return type == 2 ? -3 : -1; }

bool party_add(uint32_t p, uint32_t handle, int32_t type, int32_t f1, int32_t f2) {
    using AddFn = char(__stdcall*)(uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
    __try {
        return reinterpret_cast<AddFn>(kPartyAdd)(p, handle, type, static_cast<int32_t>(kTypeNpc), f1, f2,
                                                   summon_f3(type)) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool chr_disabled(uint32_t chr) {
    uint32_t c = 0;
    uint8_t flags = 0;
    return chr && read32(chr + 0xC, &c) && read8(c + 0x14, &flags) && (flags & 1) != 0;
}

void set_enabled(uint32_t chr) {
    __try {
        __asm {
            push edi
            mov edi, chr
            push 0
            mov eax, kSetDisabled
            call eax
            pop edi
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Packet 48 with f3 >= 0 is the resend to a joining player: it gets a live summon's f3, so its copy is summoned in.
int32_t __cdecl recv_f3(uint32_t handle, int32_t type, int32_t f3) {
    if (f3 < 0) return f3;
    if (++g_lines <= 40) {
        LOG_INFO("[coop] NPC phantom %08X was summoned before we came: it is summoned in here (f3 %d -> %d)", handle,
                 f3, summon_f3(type));
    }
    return summon_f3(type);
}

// At kRecvAdd: [esp] = return, [esp+4] party, +8 handle, +0xC chr type, +0x10 2, +0x14 f1, +0x18 f2, +0x1C f3.
__declspec(naked) void hk_recv_add() {
    __asm {
        pushad
        push dword ptr [esp + 0x3C]  // f3
        push dword ptr [esp + 0x30]  // chr type
        push dword ptr [esp + 0x30]  // handle
        call recv_f3
        add esp, 12
        mov [esp + 0x3C], eax
        popad
        mov eax, kPartyAdd
        jmp eax
    }
}

void send_home(uint32_t p, uint32_t handle) {
    __try {
        __asm {
            push edi
            mov edi, p
            push 0
            mov eax, handle
            mov edx, kSendHome
            call edx
            pop edi
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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

// The members are kept together from entry 0, as the game's own leave (0xEA2A10) and reset (0xEA1CB0) keep them:
// the game's readers outside the party's own code go only as far as the count (+0x10), so an entry behind a hole
// left by blank_entry was not seen (the Black Separation Crystal's menu, 0xC3F0B0, the HUD, the sign lookups).
void compact(uint32_t p) {
    int to = 0;
    for (int i = 0; i < partylist::entries(); ++i) {
        const uint32_t from = p + kEntries + i * kEntrySize;
        if (*ptr<int32_t>(from) == -1) continue;
        if (i != to) {
            std::memcpy(ptr<uint8_t>(p + kEntries + to * kEntrySize), ptr<uint8_t>(from), kEntrySize);
            *ptr<int32_t>(from) = -1;
            *ptr<int32_t>(from + 4) = 1;
            *ptr<int32_t>(from + 8) = 0;
            *ptr<int32_t>(from + 0xC) = -1;
            *ptr<int32_t>(from + 0x10) = -1;
        }
        ++to;
    }
}

void recount(uint32_t p) {
    __try {
        compact(p);
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

// The party's three NPC handles (+0x68 .. +0x70) and their count (+0x74), which the touch summon and the add go
// by: an NPC's handle leaves them as the game's own leave takes it out (0xEA1720), or it stays there for good
// (the party is kept through loads in a session, so the reset that would drop it does not run) and three stale
// handles refuse every NPC's sign (partylist.cpp).
void drop_npc_handle(uint32_t p, uint32_t handle) {
    int n = 0;
    for (int off = 0x68; off <= 0x70; off += 4) {
        int32_t& h = *ptr<int32_t>(p + partylist::field(off));
        if (h == -1) continue;
        if (static_cast<uint32_t>(h) == handle) {
            h = -1;
        } else {
            ++n;
        }
    }
    *ptr<int32_t>(p + partylist::field(0x74)) = n;
}

// The entry goes as 0xEA1900 empties one: handle -1, type 1, state 0, -1, -1 (an NPC's handle out of the NPCs').
void blank_entry(uint32_t p, int i) {
    const uint32_t e = p + kEntries + i * kEntrySize;
    if (*ptr<int32_t>(e + 4) == static_cast<int32_t>(kTypeNpc) && *ptr<int32_t>(e) != -1) {
        drop_npc_handle(p, *ptr<uint32_t>(e));
    }
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

bool same_npc(const Phantom& a, const Phantom& b) {
    return a.entity >= 0 ? a.entity == b.entity : a.handle == b.handle;
}

bool dismissed(const Phantom& ph) {
    for (const auto& d : g_dismissed) {
        if ((same_npc(d.first, ph) || d.first.handle == ph.handle) && now() - d.second < kDismissedHold) return true;
    }
    return false;
}

// A visitor got packet 49 (an NPC left the host's party) for the host's `host_handle`. Only one the list we hold
// still names needs keeping home (a list that came first and left it out already does).
void note_dismissed(uint32_t host_handle) {
    const int32_t entity = entity_of(host_handle);  // the same handle while our game has its block
    const Phantom* named = nullptr;
    for (const Phantom& l : g_host) {
        if (l.handle == host_handle || (entity >= 0 && l.entity == entity)) {
            named = &l;
            break;
        }
    }
    if (!named) return;
    const Phantom ph = *named;
    for (auto& d : g_dismissed) {
        if (same_npc(d.first, ph)) {
            d.second = now();
            return;
        }
    }
    g_dismissed.emplace_back(ph, now());
    if (++g_lines <= 40) {
        LOG_INFO("[coop] The host sent NPC phantom %d (%08X) home: it is not summoned in again from an older list",
                 ph.entity, host_handle);
    }
}

uint32_t __cdecl our_state_handle(uint32_t host_handle) {
    if (g_visitor) note_dismissed(host_handle);
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

bool own_kept(uint32_t handle) {
    const auto it = g_own.find(handle);
    return it != g_own.end() && it->second.away && !chr_by_handle(handle);
}

// Host: the summon of this NPC ended in our game lately.
bool ended_here(const Phantom& ph) {
    for (const auto& e : g_ended) {
        if (same_npc(e.first, ph) && now() - e.second < kEndedHold) return true;
    }
    return false;
}

void note_ended(const Phantom& ph) {
    for (auto& e : g_ended) {
        if (same_npc(e.first, ph)) {
            e.second = now();
            return;
        }
    }
    g_ended.emplace_back(ph, now());
}

// Our game summoned an NPC sign's phantom (kSignAdd). Kept as ours while we are a co-op guest, until the host's
// list names it.
void __cdecl note_sign_summon(uint32_t handle, int32_t type, int32_t f1, int32_t f2) {
    if (!g_visitor) return;
    Phantom ph;
    ph.entity = entity_of(handle);
    ph.handle = handle;
    ph.kind = type;
    ph.f1 = f1;
    ph.f2 = f2;
    g_mine[handle] = ph;
    g_mine_listed.erase(handle);
    g_mine_ended.erase(handle);
    LOG_INFO("[coop] We summoned NPC %d (character %08X, type %d) as a co-op guest: the host's world takes it in",
             ph.entity, handle, static_cast<int>(type));
}

// At kSignAdd: [esp] = return, [esp+4] party, +8 handle, +0xC chr type, +0x10 2, +0x14 f1, +0x18 f2, +0x1C f3.
__declspec(naked) void hk_sign_add() {
    __asm {
        pushad
        push dword ptr [esp + 0x38]  // f2
        push dword ptr [esp + 0x38]  // f1
        push dword ptr [esp + 0x34]  // chr type
        push dword ptr [esp + 0x34]  // handle
        call note_sign_summon
        add esp, 16
        popad
        mov eax, kPartyAdd
        jmp eax
    }
}

// A co-op guest: our summons as the party has them. A load ends those the host's list never named (the others come
// back by that list, as any of the host's summons do after our loads); one whose entry left the party is over here
// (dead, or the host's list let it go).
void mine_refresh() {
    const uint32_t me = game::player();
    if (game::loading() || !me || me != g_mine_player) {
        if (!g_mine.empty()) {
            int listed = 0;
            for (const auto& m : g_mine) listed += g_mine_listed.count(m.first) ? 1 : 0;
            LOG_INFO("[coop] A load: of the NPC phantoms we summoned, %d the host's world has (back by its list), "
                     "%d it never had (over)", listed, static_cast<int>(g_mine.size()) - listed);
        }
        for (const auto& m : g_mine) g_mine_ended[m.first] = now();
        g_mine.clear();
        g_mine_listed.clear();
        g_mine_player = me;
        return;
    }
    const uint32_t p = party();
    for (auto it = g_mine.begin(); it != g_mine.end();) {
        if (p && entry_of(p, it->first) >= 0) {
            ++it;
            continue;
        }
        LOG_INFO("[coop] NPC phantom %d we summoned has left our party", it->second.entity);
        g_mine_ended[it->first] = now();
        g_mine_listed.erase(it->first);
        it = g_mine.erase(it);
    }
    for (auto it = g_mine_ended.begin(); it != g_mine_ended.end();) {
        it = now() - it->second > kMineEndedHold ? g_mine_ended.erase(it) : std::next(it);
    }
}

bool mine_ended(uint32_t handle) {
    const auto it = g_mine_ended.find(handle);
    return it != g_mine_ended.end() && now() - it->second < kMineEndedHold;
}

bool own_has(const Phantom& ph) {
    for (const auto& o : g_own) {
        if (o.first == ph.handle || same_npc(o.second.ph, ph)) return true;
    }
    return false;
}

// Host: a co-op guest's summon that our party does not hold yet (its packet 48 found no copy of the NPC here, or
// came from no 1.4.0 game) joins it once the NPC is in our world, the way a visitor's game takes ours in; own_refresh
// then has it as ours, for good.
void adopt_tick() {
    static double last = 0;
    if (now() - last < 0.5) return;
    last = now();
    const uint32_t p = party();
    if (!p || game::loading() || !game::player() || !game::own_world()) return;
    for (const Phantom& ph : g_reported) {
        if (ended_here(ph) || own_has(ph)) continue;
        const uint32_t chr = ph.entity >= 0 ? chr_by_entity(ph.entity) : chr_by_handle(ph.handle);
        uint32_t h = 0;
        if (!chr || !read32(chr + 4, &h) || h == 0xFFFFFFFF) continue;
        const bool phantom = chr_type(chr) == ph.kind;
        const int entry = entry_of(p, h);
        if (phantom && entry >= 0) continue;  // in our party already: own_refresh takes it for ours
        if (!phantom && !make_phantom(h, ph.kind)) {
            if (g_told_waiting.insert(chr).second && ++g_lines <= 40) {
                LOG_INFO("[coop] A guest's NPC summon %d (character %08X) cannot become a phantom here yet", ph.entity,
                         h);
            }
            continue;
        }
        if (entry >= 0) {
            blank_entry(p, entry);
            recount(p);
        }
        const bool added = party_add(p, h, chr_type(chr), ph.f1, ph.f2);
        if (++g_lines <= 40) {
            LOG_INFO("[coop] A guest's NPC summon %d joins our party, the world's now (character %08X, type %d, "
                     "summoned in%s)", ph.entity, h, static_cast<int>(chr_type(chr)),
                     added ? "" : "; our party has no room");
        }
    }
    for (auto it = g_ended.begin(); it != g_ended.end();) {
        it = now() - it->second > kEndedHold ? g_ended.erase(it) : std::next(it);
    }
}

// Brings g_own up to date (our own world, every frame and before each list we send).
void own_refresh() {
    const uint32_t me = game::player();
    if (game::loading() || !me || me != g_own_player) {
        // Our load makes every character anew: each summon waits for its fresh copy, as after its block's unload.
        // An entry the game's own party reset took (outside a session) ends it below.
        int waiting = 0;
        for (auto& o : g_own) {
            if (!o.second.away) ++waiting;
            o.second.away = true;
            o.second.back_at = 0;
            o.second.chr = 0;
        }
        if (waiting) LOG_INFO("[coop] A load: our %d NPC phantom(s) stay summoned (summoned in again here)", waiting);
        g_own_shown_at.clear();
        // The hold counts from the load's end: we send no list while loading, so a summoner's game, and its report
        // of a summon that ended, only learn of the end afterwards.
        for (auto& e : g_ended) e.second = now();
        g_own_player = me;
        return;
    }
    const uint32_t p = party();
    if (!p || !game::own_world()) {
        g_own.clear();
        return;
    }
    std::set<uint32_t> present;
    bool revived = false;
    for (int i = 0; i < partylist::entries(); ++i) {
        const uint32_t e = p + kEntries + i * kEntrySize;
        uint32_t h = 0, type = 0, f1 = 0, f2 = 0;
        if (!read32(e, &h) || h == 0xFFFFFFFF || !read32(e + 4, &type) || type != kTypeNpc) continue;
        present.insert(h);
        read32(e + 0xC, &f1);
        read32(e + 0x10, &f2);
        const uint32_t chr = chr_by_handle(h);
        const int32_t t = chr_type(chr);
        auto it = g_own.find(h);
        if (chr && (t == 1 || t == 2)) {
            const int32_t entity = entity_of(h);  // -1 if no table names it: the others go by the handle
            Own& o = g_own[h];
            o.ph.entity = entity;
            o.ph.handle = h;
            o.ph.kind = t;
            o.ph.f1 = static_cast<int32_t>(f1);
            o.ph.f2 = static_cast<int32_t>(f2);
            o.chr = chr;
            o.away = false;
            const auto shown = g_own_shown_at.find(chr);
            if (shown != g_own_shown_at.end() && now() - shown->second >= 8.0) {
                if (chr_disabled(chr)) {
                    set_enabled(chr);
                    LOG_INFO("[coop] Our NPC phantom %d (character %08X) was still disabled 8 s after it was summoned "
                             "in again: enabled", entity, chr);
                }
                g_own_shown_at.erase(shown);
            }
            continue;
        }
        if (it == g_own.end()) continue;  // not one we knew as our phantom: own_world_sweep decides
        if (!chr) {
            if (!it->second.away) {
                LOG_INFO("[coop] Our NPC phantom %d: its map block has unloaded; the summon goes on",
                         it->second.ph.entity);
            }
            it->second.away = true;
            it->second.back_at = 0;  // the 10 s below count one visit of a copy that will not take the summon
            continue;
        }
        // A character that is not a phantom: the same one stopped being one (the game dismissed it), or its block
        // came back with a fresh copy.
        if (!it->second.away) {
            note_ended(it->second.ph);
            g_own.erase(it);
            continue;
        }
        if (make_phantom(h, it->second.ph.kind)) {
            // Summoned in as a visitor's game does it: the kept entry is in state 3, which never shows a fresh copy,
            // and the NPC's map's load event disabled it (m10_01 11015100 disables 6540 in a host's game).
            blank_entry(p, i);
            recount(p);
            const bool added = party_add(p, h, chr_type(chr), static_cast<int32_t>(f1), static_cast<int32_t>(f2));
            g_own_shown_at[chr] = now();
            it->second.chr = chr;
            it->second.away = false;
            it->second.back_at = 0;
            revived = true;
            LOG_INFO("[coop] Our NPC phantom %d: its new character %08X is the phantom again, summoned in%s",
                     it->second.ph.entity, chr, added ? "" : "; the party has no room, so the summon is over");
        } else if (it->second.back_at == 0) {
            it->second.back_at = now();
        } else if (now() - it->second.back_at > 10.0) {
            // The copy never took the summon (its events keep it out): the summon ends, as own_world_sweep sees.
            LOG_INFO("[coop] Our NPC phantom %d: its new character %08X did not become the phantom again; the summon "
                     "is over", it->second.ph.entity, chr);
            note_ended(it->second.ph);
            g_own.erase(it);
        }
    }
    if (revived) recount(p);
    for (auto it = g_own.begin(); it != g_own.end();) {
        if (present.count(it->first)) {
            ++it;
            continue;
        }
        note_ended(it->second.ph);  // its entry left the party (its death, its going home)
        it = g_own.erase(it);
    }
}

// Our game's handle for a co-op guest's summon: its character here (by the NPC's entity), else the one its
// summoner's game reported (the same handle while both games have its map block).
uint32_t local_handle(const Phantom& ph) {
    const uint32_t chr = ph.entity >= 0 ? chr_by_entity(ph.entity) : 0;
    uint32_t h = 0;
    return chr && read32(chr + 4, &h) && h != 0xFFFFFFFF ? h : ph.handle;
}

// A guest's or an invader's copy of a host's phantom, in our party: said once, and if it is still disabled 8 s on
// (the summon-in animation takes a few) it is enabled as the host's sign does, once per character.
void show_late(uint32_t p, int entry, int32_t entity, uint32_t chr) {
    const auto it = g_listed_since.find(chr);
    if (it == g_listed_since.end()) {
        g_listed_since[chr] = now();
        uint32_t state = 0;
        read32(p + kEntries + entry * kEntrySize + 8, &state);
        if (++g_lines <= 40) {
            LOG_INFO("[coop] The host's NPC phantom %d is in our party (character %08X, entry state %u, %s)", entity,
                     chr, state, chr_disabled(chr) ? "disabled" : "enabled");
        }
        return;
    }
    if (now() - it->second < 8.0 || !chr_disabled(chr) || !g_shown.insert(chr).second) return;
    set_enabled(chr);
    LOG_INFO("[coop] The host's NPC phantom %d (character %08X) was still disabled here 8 s after its summon: "
             "enabled (%s)", entity, chr, chr_disabled(chr) ? "still disabled" : "now enabled");
}

}  // namespace

std::vector<Phantom> ours() {
    std::vector<Phantom> out;
    own_refresh();
    const uint32_t p = party();
    if (!p || game::loading()) return out;
    for (int i = 0; i < partylist::entries(); ++i) {
        uint32_t h = 0, type = 0;
        if (!read32(p + kEntries + i * kEntrySize, &h) || h == 0xFFFFFFFF ||
            !read32(p + kEntries + i * kEntrySize + 4, &type) || type != kTypeNpc) {
            continue;
        }
        const auto it = g_own.find(h);
        if (it != g_own.end()) out.push_back(it->second.ph);
    }
    return out;
}

std::vector<Phantom> mine() {
    std::vector<Phantom> out;
    if (!g_visitor) return out;
    for (const auto& m : g_mine) out.push_back(m.second);
    return out;
}

void set_guest_summons(const std::vector<Phantom>& list) { g_reported = list; }

std::vector<Phantom> guest_summons_pending() {
    std::vector<Phantom> out;
    for (Phantom ph : g_reported) {
        if (ended_here(ph) || own_has(ph)) continue;
        ph.handle = local_handle(ph);
        out.push_back(ph);
    }
    return out;
}

void dismiss_all(const char* why) {
    const uint32_t p = party();
    if (!p || !game::own_world()) return;
    int sent = 0, blanked = 0;
    // The NPC members first: the game's send home takes each out and moves the members behind it up.
    std::vector<uint32_t> npcs;
    for (int i = 0; i < partylist::entries(); ++i) {
        uint32_t h = 0, type = 0;
        if (read32(p + kEntries + i * kEntrySize, &h) && h != 0xFFFFFFFF &&
            read32(p + kEntries + i * kEntrySize + 4, &type) && type == kTypeNpc) {
            npcs.push_back(h);
        }
    }
    for (uint32_t h : npcs) {
        const int i = entry_of(p, h);
        if (i < 0) continue;
        const uint32_t chr = chr_by_handle(h);
        const int32_t t = chr_type(chr);
        if (chr && (t == 1 || t == 2)) {
            send_home(p, h);  // the game's own: the notice, the party leave, packet 49 to every other game
            ++sent;
        } else {
            blank_entry(p, i);  // its map is not loaded here: nothing to send home in this game
            recount(p);
            ++blanked;
        }
    }
    for (const auto& o : g_own) note_ended(o.second.ph);
    for (const Phantom& ph : g_reported) note_ended(ph);  // a guest's summon our party never held ends too
    g_own.clear();
    g_own_shown_at.clear();
    if (sent || blanked) {
        LOG_INFO("[coop] %s: every NPC phantom goes home (%d sent home, %d not loaded here)", why, sent, blanked);
    }
}

bool white_summoned() {
    for (const auto& o : g_own) {
        if (o.second.ph.kind == 1) return true;
    }
    const uint32_t p = party();
    for (int i = 0; p && i < partylist::entries(); ++i) {
        uint32_t h = 0, type = 0;
        if (read32(p + kEntries + i * kEntrySize, &h) && h != 0xFFFFFFFF &&
            read32(p + kEntries + i * kEntrySize + 4, &type) && type == kTypeNpc && chr_type(chr_by_handle(h)) == 1) {
            return true;
        }
    }
    return false;
}

bool npc_summoned(int32_t entity) {
    if (entity < 0) return false;
    for (const auto& o : g_own) {
        if (o.second.ph.entity == entity) return true;
    }
    for (const auto& m : g_mine) {
        if (m.second.entity == entity) return true;
    }
    if (g_visitor) {
        for (const Phantom& ph : g_host) {
            if (ph.entity == entity) return true;
        }
    }
    const uint32_t chr = chr_by_entity(entity);
    const int32_t t = chr_type(chr);
    uint32_t h = 0;
    return chr && (t == 1 || t == 2) && read32(chr + 4, &h) && entry_of(party(), h) >= 0;
}

int count() {
    const uint32_t p = party();
    int n = 0;
    for (int i = 0; p && i < partylist::entries(); ++i) {
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
    // A phantom the host sent home stays home until a list of the host's leaves it out (that list keeps it home
    // from then on, and a later one that names it again is a new summon).
    for (auto it = g_dismissed.begin(); it != g_dismissed.end();) {
        const bool named = std::any_of(list.begin(), list.end(), [&](const Phantom& ph) {
            return same_npc(it->first, ph) || it->first.handle == ph.handle;
        });
        it = !named || now() - it->second >= kDismissedHold ? g_dismissed.erase(it) : std::next(it);
    }
    g_host = list;
    g_heard = true;
}

// Our own world: an NPC entry whose character is no longer a phantom goes. The party is kept through our own loads
// in a session (rules.cpp keeps it for the guests), and a load makes the NPC anew, unsummoned: after the host's death
// Solaire's entry would have stayed with his health bar while he stood at his sign again. Vanilla clears the party
// at that load. The scripted summon (+0x7C, 0xEA1F40) keeps its own type and is left alone.
void own_world_sweep() {
    own_refresh();
    static double last = 0;
    if (now() - last < 1.0) return;
    last = now();
    const uint32_t p = party();
    uint32_t special = 0;
    if (!p || game::loading() || !game::player() || !game::own_world() || !read32(p + partylist::field(0x7C), &special) ||
        special != 0xFFFFFFFF) {
        return;
    }
    for (int i = 0; i < partylist::entries(); ++i) {
        uint32_t h = 0, type = 0;
        if (!read32(p + kEntries + i * kEntrySize, &h) || h == 0xFFFFFFFF ||
            !read32(p + kEntries + i * kEntrySize + 4, &type) || type != kTypeNpc) {
            continue;
        }
        const uint32_t chr = chr_by_handle(h);
        const int32_t t = chr_type(chr);
        if (!chr || t == 1 || t == 2) continue;
        const auto known = g_own.find(h);
        if (known != g_own.end() && known->second.away) continue;  // back from an unload: own_refresh revives it
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
        g_listed_since.clear();
        g_shown.clear();
        g_entity_of.clear();
        g_heard = false;
        g_mine.clear();
        g_mine_listed.clear();
        g_mine_ended.clear();
        g_dismissed.clear();
        own_world_sweep();
        adopt_tick();
        return;
    }
    mine_refresh();
    static double last = 0;
    if (now() - last < 0.5) return;
    last = now();
    const uint32_t p = party();
    if (!p || game::loading() || !game::player()) {
        g_made.clear();  // a load makes every character anew
        g_told_waiting.clear();
        g_listed_since.clear();
        g_shown.clear();
        return;
    }
    std::set<int32_t> listed;
    std::set<uint32_t> listed_handles;
    for (const Phantom& ph : g_host) {
        listed.insert(ph.entity);
        listed_handles.insert(ph.handle);
        if (dismissed(ph)) continue;  // the host sent it home after this list: packet 49 took our copy out
        const uint32_t chr = ph.entity >= 0 ? chr_by_entity(ph.entity) : chr_by_handle(ph.handle);
        uint32_t h = 0;
        if (!chr || !read32(chr + 4, &h) || h == 0xFFFFFFFF || mine_ended(h)) continue;
        if (g_mine.count(h) && g_mine_listed.insert(h).second) {
            LOG_INFO("[coop] NPC phantom %d we summoned is in the host's world now: it goes by the host's list",
                     ph.entity);
        }
        const bool phantom = chr_type(chr) == ph.kind;
        const int entry = entry_of(p, h);
        if (phantom && entry >= 0) {
            show_late(p, entry, ph.entity, chr);
            continue;
        }
        const bool was_hidden = chr_disabled(chr);
        if (!phantom && !make_phantom(h, ph.kind)) {
            if (g_told_waiting.insert(chr).second && ++g_lines <= 40) {
                LOG_INFO("[coop] The host's NPC phantom %d (character %08X) cannot become one here yet", ph.entity, h);
            }
            continue;
        }
        // A fresh copy (our own reload, its block back) under an entry the party kept: that entry is in state 3,
        // which never shows the copy, so it goes and the copy is summoned in like any other.
        if (entry >= 0) {
            blank_entry(p, entry);
            recount(p);
        }
        const bool added = party_add(p, h, chr_type(chr), ph.f1, ph.f2);
        if (added) g_entity_of[h] = ph.entity;
        auto it = g_made.find(ph.entity);
        if ((it == g_made.end() || it->second != chr) && ++g_lines <= 40) {
            LOG_INFO("[coop] The host's NPC phantom %d is a phantom here too (character %08X, type %d, %s, summoned "
                     "in%s)", ph.entity, h, static_cast<int>(chr_type(chr)), was_hidden ? "was disabled" : "was enabled",
                     added ? "" : "; our party has no room");
        }
        g_made[ph.entity] = chr;
    }
    // Our NPC entries the host no longer lists (the NPC went home, died, or the host closed its world and the
    // NPC is gone there): after a moment they go here too, as the game's own departure does.
    for (int i = 0; i < partylist::entries(); ++i) {
        const uint32_t e = p + kEntries + i * kEntrySize;
        uint32_t h = 0, type = 0;
        if (!read32(e, &h) || h == 0xFFFFFFFF || !read32(e + 4, &type) || type != kTypeNpc ||
            (g_mine.count(h) && !g_mine_listed.count(h))) {
            continue;  // one of our own summons the host's list has not named yet: ours to keep meanwhile
        }
        const uint32_t chr = chr_by_handle(h);
        const auto known = g_entity_of.find(h);
        const int32_t entity = chr ? entity_of(h) : known != g_entity_of.end() ? known->second : -1;
        // Listed (by its entity, or by the host's handle, which names the same character here), or not known while
        // its block is away: it stays (its update waits, hk_update_state).
        const bool on_list = (entity >= 0 && listed.count(entity)) || listed_handles.count(h);
        if (on_list || (entity < 0 && !chr) || !g_heard) {
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
    for (int i = 0; i < partylist::entries(); ++i) {
        uint32_t h = 0, type = 0;
        if (!read32(p + kEntries + i * kEntrySize, &h) || h == 0xFFFFFFFF) continue;
        read32(p + kEntries + i * kEntrySize + 4, &type);
        const int32_t t = chr_type(chr_by_handle(h));
        if (type == kTypeNpc && (t == 1 || t == 2 || own_kept(h))) kept = true;
    }
    if (!kept) return false;
    int npcs = 0;
    bool changed = false;
    for (int i = 0; i < partylist::entries(); ++i) {
        uint32_t h = 0, type = 0;
        if (!read32(p + kEntries + i * kEntrySize, &h) || h == 0xFFFFFFFF) continue;
        read32(p + kEntries + i * kEntrySize + 4, &type);
        const int32_t t = chr_type(chr_by_handle(h));
        if (type == kTypeNpc && (t == 1 || t == 2 || own_kept(h))) {
            ++npcs;
            continue;
        }
        if (h == local && local) continue;
        blank_entry(p, i);
        changed = true;
    }
    // +0x14 is the game's: every party update (0xEA3381 -> 0xEA18A0) makes it the session's members (at least 1)
    // plus the NPC summons (+0x74). Writing +0x10 into it lasted one frame (the log said so every second).
    uint32_t before10 = 0, n = 0;
    read32(p + 0x10, &before10);
    recount(p);
    read32(p + 0x10, &n);
    if (changed || n != before10) {
        LOG_INFO("[coop] The session is over: the party keeps our %d NPC phantom(s) (party entries %u -> %u)", npcs,
                 before10, n);
    }
    return true;
}

// Quit destroys the characters and then counts whoever the party's handles still name, so the other players'
// handles go. Until the 2026-10-24 test this was 0xEA1900, which blanks all four, ours too, for 20 s after every
// session: a reload in that time lost our entry for good (the game makes it only with our character), the next
// session counted one short, and after its disband Solaire's entry alone counted 1. HasPartyMembers (0xD2A4B0:
// session members, or +0x10 >= 2) was false and the Black Separation Crystal stayed grey with him still here.
void solo_party(uint32_t p) {
    if (p < 0x10000) return;
    uint32_t local = 0;
    const uint32_t me = game::player();
    if (me) read32(me + 4, &local);
    bool changed = false;
    for (int i = 0; i < partylist::entries(); ++i) {
        uint32_t h = 0;
        if (!read32(p + kEntries + i * kEntrySize, &h) || h == 0xFFFFFFFF || (local && h == local)) {
            continue;
        }
        blank_entry(p, i);
        changed = true;
    }
    if (changed) recount(p);
}

bool install() {
    const bool make = redirect_call(kRecvMake, kMakePhantom, reinterpret_cast<void*>(&hk_recv_make)) &&
                      redirect_call(kRecvAdd, kPartyAdd, reinterpret_cast<void*>(&hk_recv_add));
    const bool state = redirect_call(kRecvState, kPartyState, reinterpret_cast<void*>(&hk_recv_state)) &&
                       redirect_call(kSignAdd, kPartyAdd, reinterpret_cast<void*>(&hk_sign_add));
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
