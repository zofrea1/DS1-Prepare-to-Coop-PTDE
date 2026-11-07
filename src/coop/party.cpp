// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "party.h"

#include "game.h"
#include "partylist.h"
#include "hook.h"
#include "log.h"
#include "players.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace coop::party {

namespace {

// Session members (FrpgSessionManagerImp): +0x12C list of {next, prev, member id (0x24 bytes) at +8, slot at
// +0x2C}; +0x118 list of members waiting for their character (member id at +8).
constexpr uint32_t kSlotList = 0x12C;
constexpr uint32_t kWaitList = 0x118;
// Remote player data (GameDataMan+0xC, 0x4C0 per slot; +8 = -1 unused, +0xA0 character name) and its
// completeness (GameDataMan+0x10, one byte per slot, 0x1F once every part arrived).
constexpr uint32_t kRemoteData = 0xC;
constexpr uint32_t kRemoteState = 0x10;
constexpr uint32_t kRemoteSize = 0x4C0;
constexpr uint32_t kNameOffset = 0xA0;
constexpr int kNameChars = 16;

// The other players' slots: the game's three, or more with MaxPlayers (players.cpp).
int remote_slots() { return players::max_players() - 1; }

// Game functions that take a member id by value (0x24 bytes, copied with 0x6F80F0, destroyed by the callee).
constexpr uint32_t kMemberCopy = 0x006F80F0;   // thiscall(dst, const src*), ret 4
constexpr uint32_t kMemberEqual = 0x006F8250;  // thiscall(a, const b*) -> bool, ret 4
constexpr uint32_t kFindPlayer = 0x00E3D930;   // (WorldChrMan, member) -> PlayerIns or 0, ret 0x28
constexpr uint32_t kWaitPush = 0x00D2AFD0;     // ecx = SessMan, (member), ret 0x24: put on the waiting list
// Removing a member's character, as the member-left handler (0xD2E5C0) does after taking it out of
// PartyMemberInfo (0xEA2A10, not used here: see remove_character).
constexpr uint32_t kChrRemove = 0x00E3E600;    // eax = PlayerIns, push WorldChrMan; ret 4

double now() { return GetTickCount64() / 1000.0; }

uint32_t sess_man() { return game::sess_man(); }

uint32_t world_chr_man() {
    uint32_t v = 0;
    return read32(game::kWorldChrManVar, &v) ? v : 0;
}

uint32_t find_player(uint32_t member) {
    const uint32_t wcm = world_chr_man();
    if (!wcm) return 0;
    uint32_t result = 0;
    __try {
        __asm {
            sub esp, 0x24
            mov ecx, esp
            push member
            mov eax, kMemberCopy
            call eax
            push wcm
            mov eax, kFindPlayer
            call eax
            mov result, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return result;
}

bool members_equal(uint32_t a, uint32_t b) {
    bool same = false;
    __try {
        __asm {
            push b
            mov ecx, a
            mov eax, kMemberEqual
            call eax
            mov same, al
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return same;
}

bool waiting(uint32_t member) {
    const uint32_t sm = sess_man();
    uint32_t head = 0, node = 0;
    if (!sm || !read32(sm + kWaitList, &head) || !head || !read32(head, &node)) return false;
    for (int guard = 0; node && node != head && guard < 16; ++guard) {
        if (members_equal(node + 8, member)) return true;
        if (!read32(node, &node)) return false;
    }
    return false;
}

bool push_waiting(uint32_t member) {
    const uint32_t sm = sess_man();
    if (!sm) return false;
    __try {
        __asm {
            sub esp, 0x24
            mov ecx, esp
            push member
            mov eax, kMemberCopy
            call eax
            mov ecx, sm
            mov eax, kWaitPush
            call eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Only the character goes; the player's PartyMemberInfo entry stays. Taking the entry out (0xEA2A10, as the
// member-left handler does) is the game's "a player left" path: for the host on a guest's side it shows
// "Disconnected from master of world" and five seconds later warps the guest home (the 2026-10-07 fall loop).
// The rebuilt character takes the freed player slot again, so its handle (slot | map << 14) matches the entry.
bool remove_character(uint32_t chr) {
    const uint32_t wcm = world_chr_man();
    if (!wcm) return false;
    __try {
        __asm {
            push wcm
            mov eax, chr
            mov edx, kChrRemove
            call edx
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// A member id's player: +0 a DLNRD::SteamPlayerData (vtable 0x10FB604; the id's compare 0x6F8250 calls its slot 10,
// IsEqual 0x716450, which compares the online ids at +0xC). Its Steam id (slot 15, 0x715EE0, under the lock at +0x10):
// +0xC a DLNRD::SteamOnlineIDData (vtable 0x10FD7FC, "SteamID(0x%I64x) UserTag(%s)"), whose +0x28 is the 64-bit
// Steam id (0x71F960, under the lock at +8). Read as the game reads it, without the locks: the id is set when the
// member joins and never changes after. 0 when the objects are not what this build should hold.
constexpr uint32_t kSteamPlayerDataVt = 0x010FB604;
constexpr uint32_t kSteamOnlineIdVt = 0x010FD7FC;
constexpr uint32_t kPlayerOnlineId = 0xC;
constexpr uint32_t kOnlineIdSteamId = 0x28;

uint64_t member_steam_id(uint32_t member) {
    uint32_t pd = 0, vt = 0, oid = 0, lo = 0, hi = 0;
    if (!read32(member, &pd) || !pd || !read32(pd, &vt)) return 0;
    static bool told = false;
    if (vt != kSteamPlayerDataVt) {
        if (!told) {
            told = true;
            LOG_INFO("[coop] A session member's player is not a SteamPlayerData (vtable %08X): players are matched by "
                     "name", vt);
        }
        return 0;
    }
    if (!read32(pd + kPlayerOnlineId, &oid) || !oid || !read32(oid, &vt)) return 0;
    if (vt != kSteamOnlineIdVt) {
        if (!told) {
            told = true;
            LOG_INFO("[coop] A session member's online id is not a SteamOnlineIDData (vtable %08X): players are "
                     "matched by name", vt);
        }
        return 0;
    }
    if (!read32(oid + kOnlineIdSteamId, &lo) || !read32(oid + kOnlineIdSteamId + 4, &hi)) return 0;
    return static_cast<uint64_t>(hi) << 32 | lo;
}

std::wstring read_name(uint32_t at) {
    std::wstring s;
    for (int i = 0; i < kNameChars; ++i) {
        uint32_t w = 0;
        if (!read32(at + i * 2, &w)) break;
        const wchar_t c = static_cast<wchar_t>(w & 0xFFFF);
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

int hp(uint32_t chr) {
    uint32_t v = 0;
    return read32(chr + 0x2D4, &v) ? static_cast<int>(v) : 0;
}

bool being_removed(uint32_t chr) {
    uint8_t f = 0;
    return read8(chr + 0x1FF, &f) && (f & 0x10);
}

struct Node {
    uint32_t member;  // address of the member id inside the session's list
    int slot;
};

std::vector<Node> slot_nodes() {
    std::vector<Node> out;
    const uint32_t sm = sess_man();
    uint32_t head = 0, node = 0;
    if (!sm || !read32(sm + kSlotList, &head) || !head || !read32(head, &node)) return out;
    for (int guard = 0; node && node != head && guard < 16; ++guard) {
        uint32_t slot = 0;
        if (read32(node + 0x2C, &slot) && static_cast<int>(slot) < remote_slots()) {
            out.push_back({node + 8, static_cast<int>(slot)});
        }
        if (!read32(node, &node)) break;
    }
    return out;
}

// Per player slot: the other players, players::kMaxPlayers - 1 of them (remote_slots() in use).
constexpr int kSlots = players::kMaxPlayers - 1;
static_assert(kSlots == 7, "g_queued_at's initializer");
double g_queued_at[kSlots] = {-100, -100, -100, -100, -100, -100, -100};
int g_deferred[kSlots] = {};  // a missing character waits: 1 for its player to be in the world, 2 to be up (logged once)
bool g_was_loading = true;
double g_loaded_at = 0;
double g_last_check = 0;
// How soon the characters are looked at, and how often (1.5.0; until then 3 s after a load and once a second). Each
// wait added to the time before a player was seen again after a death, theirs or ours: the others' characters came
// 3.4 s after our own respawn, and ours in their games 5.3 s after we stood (2026-11-03 test). A request still goes
// only for a member whose data is complete and who is not waiting already, so asking sooner asks nothing twice.
constexpr double kAfterLoad = 0.5;
constexpr double kCheckEvery = 0.25;
constexpr double kRequestGap = 5.0;  // a slot's next request, when the last one brought no character
// The character we asked the game to remove from each player slot, and when (it lingers for a frame or two). The
// slot's next character can be made at the same address: on 2026-10-28 the host's rebuilt copy on the guest took
// the old one's, was taken for the old one on its way out, and so was never checked again. It lived through two
// more deaths of the host unrebuilt, invisible but for its health bar, until a team wipe reloaded the guest's world.
// The record is dropped once the slot is seen empty, or after kRemoveLinger s.
constexpr double kRemoveLinger = 5.0;
uint32_t g_removed[kSlots] = {};
double g_removed_at[kSlots] = {};
// What each player slot's character was made with: a death since then leaves it stale. The host died in the
// Undead Asylum and came back in Firelink Shrine, where the guest already was; the guest's character of the host
// had its health back from the network at once and was never rebuilt, so only its health bar showed (2026-10-13
// test). A rebuild after a jump into our map (respawn, warp) was tried as well and crashed the host: see
// party_reads_character.
// Kept per slot and per character, and for the player it was made for: a slot's next player can get a character
// at the same address (2026-10-14: the guest's character took the one of a player who had left, the record kept
// that player's death count, and the guest's death went unseen, so their dead copy stayed pushable on the host).
struct Made {
    uint32_t chr = 0;
    uint64_t peer = 0;  // the player's Steam id, 0 not known yet
    int deaths = -1;
    int death_events = -1;
    bool seen_dead = false;
};

// PartyMemberInfo ([GameMan+0xBEC]) keeps its members (eight: partylist.cpp) from +0x18, 0x14 bytes each: +0 character handle (-1 none),
// +4 kind, +8 state. Its update (0xEA2FD0) reads the member's character with no null check in states 1, 3 (alive
// in the world) and 4 (dying); 2, 5 (dead), 6 and 7 do not, or check. Only the character goes in a rebuild (see
// remove_character), so a member in state 1, 3 or 4 must keep it: the host took out a living guest's character
// right after the crow's flight and crashed at 0xEA3107 reading its HP (2026-10-13 test).
constexpr uint32_t kGmParty = 0xBEC;

uint32_t chr_of_handle(uint32_t handle) {
    const uint32_t wcm = world_chr_man();
    if (!wcm || handle == 0xFFFFFFFF) return 0;
    uint32_t table = 0, count = 0, entries = 0, chr = 0;
    const uint32_t index = (handle >> 14) & 0x3F, slot = handle & 0x3FFF;
    if (!read32(wcm + 0x240 + index * 4, &table) || !table || !read32(table, &count) || slot >= count ||
        !read32(table + 4, &entries) || !read32(entries + slot * 0x20, &chr)) {
        return 0;
    }
    return chr;
}

// The state of the party entry that holds this character (1, 3 alive; 4 dying; 2, 5 dead; 6, 7), -1 none.
int party_state_of(uint32_t chr) {
    uint32_t gm = 0, pmi = 0;
    if (!read32(game::kGameManVar, &gm) || !gm || !read32(gm + kGmParty, &pmi) || !pmi) return -1;
    for (int i = 0; i < partylist::entries(); ++i) {
        uint32_t handle = 0, state = 0;
        if (!read32(pmi + 0x18 + i * 0x14, &handle) || !read32(pmi + 0x20 + i * 0x14, &state)) return -1;
        if (handle != 0xFFFFFFFF && chr_of_handle(handle) == chr) return static_cast<int>(state);
    }
    return -1;
}

bool party_reads_character(uint32_t chr) {
    uint32_t gm = 0, pmi = 0;
    if (!read32(game::kGameManVar, &gm) || !gm || !read32(gm + kGmParty, &pmi) || !pmi) return true;
    for (int i = 0; i < partylist::entries(); ++i) {
        uint32_t handle = 0, state = 0;
        if (!read32(pmi + 0x18 + i * 0x14, &handle) || !read32(pmi + 0x20 + i * 0x14, &state)) return true;
        if (handle == 0xFFFFFFFF || chr_of_handle(handle) != chr) continue;
        if (state == 1 || state == 3 || state == 4) return true;
    }
    return false;
}
Made g_made[kSlots];

}  // namespace

std::wstring local_name() {
    const uint32_t pgd = game::player_game_data();
    return pgd ? read_name(pgd + kNameOffset) : std::wstring();
}

std::vector<Remote> remotes() {
    std::vector<Remote> out;
    const uint32_t gdm = game::game_data_man();
    uint32_t data = 0, state = 0;
    if (!gdm || !read32(gdm + kRemoteData, &data) || !data || !read32(gdm + kRemoteState, &state) || !state) return out;
    for (const Node& n : slot_nodes()) {
        Remote r;
        r.slot = n.slot;
        const uint32_t rec = data + n.slot * kRemoteSize;
        uint32_t used = 0;
        if (!read32(rec + 8, &used) || used == 0xFFFFFFFF) continue;
        r.name = read_name(rec + kNameOffset);
        uint8_t st = 0;
        r.data_ready = read8(state + n.slot, &st) && st == 0x1F;
        r.chr = find_player(n.member);
        r.steam_id = member_steam_id(n.member);
        out.push_back(r);
    }
    return out;
}

void tick(bool active, PeerState (*peer)(uint32_t chr, const std::wstring& name, uint64_t steam_id)) {
    const bool loading = game::loading() || !game::in_game();
    if (loading) {
        g_was_loading = true;
        for (uint32_t& c : g_removed) c = 0;
        for (Made& m : g_made) m = Made{};
        for (int& d : g_deferred) d = 0;
        return;
    }
    if (g_was_loading) {
        g_was_loading = false;
        g_loaded_at = now();
    }
    if (!active || now() - g_loaded_at < kAfterLoad || now() - g_last_check < kCheckEvery) return;
    g_last_check = now();
    const int state = game::session_state();
    if (!(state == 3 || (state == 6 && game::join_step() == 4))) return;

    const std::vector<Node> nodes = slot_nodes();
    const std::vector<Remote> list = remotes();
    for (const Remote& r : list) {
        if (!r.data_ready || r.slot < 0 || r.slot >= remote_slots()) continue;
        if (now() - g_queued_at[r.slot] < kRequestGap) continue;
        uint32_t member = 0;
        for (const Node& n : nodes) {
            if (n.slot == r.slot) member = n.member;
        }
        if (!member) continue;
        const PeerState ps = peer ? peer(r.chr, r.name, r.steam_id) : PeerState{};
        char name[64];
        WideCharToMultiByte(CP_UTF8, 0, r.name.c_str(), -1, name, sizeof(name), nullptr, nullptr);
        if (g_removed[r.slot] && (!r.chr || now() - g_removed_at[r.slot] >= kRemoveLinger)) g_removed[r.slot] = 0;
        if (!r.chr || r.chr == g_removed[r.slot] || being_removed(r.chr)) {
            if (r.chr) continue;  // the old character is on its way out
            if (waiting(member)) continue;
            // Only once they stand in the world again. After a team wipe the guest's game asked for the host's
            // character while the host was still loading after its own death: it came out invisible and could be
            // walked through, and stayed so after the host was back (2026-10-17 test). A living load (a Homeward Bone,
            // bonfire travel) or a cutscene is waited out the same way: they report it (1.5.0, Grok dcb9359e), and
            // no time limit applies, as no character of theirs stands anywhere meanwhile.
            if (ps.known && !ps.invader && (!ps.alive || ps.away)) {
                const int why = ps.alive ? 3 : 1;
                if (g_deferred[r.slot] != why) {
                    g_deferred[r.slot] = why;
                    if (ps.alive) {
                        LOG_INFO("[coop] %s's character is missing: it is created once they are back in the world "
                                 "(they load or watch a cutscene: event animation %d)", name, ps.event_anim);
                    } else {
                        LOG_INFO("[coop] %s's character is missing: it is created once they are back in the world", name);
                    }
                }
                continue;
            }
            // Nor while they still get up at their bonfire (event animation 6950 after a respawn) or hold another
            // event animation: the character made then could be walked through and showed none of their attacks
            // for as long as it lived (the host's copy on the guest, made 2.3 s after the host stood again,
            // 2026-10-19 twice). Made anyway 20 s after they stood, an event animation or not.
            if (ps.known && !ps.invader && !ps.settled && ps.up_for < 20.0) {
                if (g_deferred[r.slot] != 2) {
                    g_deferred[r.slot] = 2;
                    LOG_INFO("[coop] %s's character is missing: it is created once they are up (event animation %d)",
                             name, ps.event_anim);
                }
                continue;
            }
            if (g_deferred[r.slot] == 2) {
                LOG_INFO("[coop] %s is up %.1f s after standing again (event animation %d)", name, ps.up_for,
                         ps.event_anim);
            }
            g_deferred[r.slot] = 0;
            if (push_waiting(member)) {
                g_queued_at[r.slot] = now();
                LOG_INFO("[coop] %s's character is missing: asked the game to create it again", name);
            }
            continue;
        }
        // An invader's character missing after our own load (we died and respawned) is asked for like anyone's:
        // the host and the guests saw no invader after their respawn (2026-10-16/17 invasions, 1.0.0 players).
        Made& made = g_made[r.slot];
        if (made.chr != r.chr || (ps.known && made.peer != ps.id)) {
            made = Made{};
            made.chr = r.chr;
            made.peer = ps.known ? ps.id : 0;
            made.deaths = ps.deaths;
            made.death_events = ps.death_events;
            // Made while its player was dead or loading, it shows them as they were then: rebuilt once they are
            // back, as after a death.
            made.seen_dead = ps.known && ps.dead;
        }
        if (hp(r.chr) <= 0) made.seen_dead = true;
        // The party can hold the character dead (state 2 or 5) after its HP came back from a moment at 0 between two
        // of these checks: an invader's Death Knell to a summon sign by a load (their game frees its character and
        // makes it again) showed 0 HP here for half a second, the party entry went to 5 and stayed there, and the
        // invader was a dead copy without their phantom glow, gone from sight, for the rest of the invasion while
        // alive in their own game (2026-11-07 13:31, host and guest).
        const int state = party_state_of(r.chr);
        if (state == 2 || state == 5) made.seen_dead = true;
        // A death shows as 0 HP here, a dead party entry, a higher death count, or the player's own death message (the
        // copy's HP is not always seen at 0 in the 1 s checks).
        const bool counted = (ps.known && made.deaths >= 0 && ps.deaths > made.deaths) ||
                             (ps.known && made.death_events >= 0 && ps.death_events > made.death_events);
        const bool died = made.seen_dead || counted;
        if (made.deaths < 0) made.deaths = ps.deaths;
        if (made.death_events < 0) made.death_events = ps.death_events;
        if (!died) continue;
        // An invader who really dies goes home (or spectates a standing invader, still dead to us): no rebuild for
        // them. A copy held dead while its invader never died is their own load (above): rebuilt as a co-op player's.
        if (ps.invader && counted) continue;
        // Their character here died. Rebuild it once that player reports being alive (they respawned) and the
        // party is done with it; a character no player can be matched to stays as it is. The old character goes
        // first; a later check finds the slot empty and asks for the new one, which then takes the same slot.
        if (!ps.known || !ps.alive || ps.away) continue;
        if (party_reads_character(r.chr)) {
            static int told = 0;
            if (++told <= 10) LOG_INFO("[coop] %s's character is still in use by the party: not rebuilt", name);
            continue;
        }
        if (remove_character(r.chr)) {
            g_removed[r.slot] = r.chr;
            g_removed_at[r.slot] = now();
            made = Made{};
            g_queued_at[r.slot] = now() - kRequestGap;  // the new character on the first check that finds the slot empty
            LOG_INFO("[coop] %s is back after %s: their character is created again", name,
                     ps.invader ? "a load (an invader's copy held dead)" : counted ? "dying" : "being held dead");
        }
    }
}

}  // namespace coop::party
