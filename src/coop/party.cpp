// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "party.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <set>

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
        if (read32(node + 0x2C, &slot) && slot < 3) out.push_back({node + 8, static_cast<int>(slot)});
        if (!read32(node, &node)) break;
    }
    return out;
}

double g_queued_at[3] = {-100, -100, -100};
bool g_was_loading = true;
double g_loaded_at = 0;
double g_last_check = 0;
std::set<uint32_t> g_removed;  // characters we asked the game to remove (they linger for a frame or two)
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

// PartyMemberInfo ([GameMan+0xBEC]) keeps four members from +0x18, 0x14 bytes each: +0 character handle (-1 none),
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

bool party_reads_character(uint32_t chr) {
    uint32_t gm = 0, pmi = 0;
    if (!read32(game::kGameManVar, &gm) || !gm || !read32(gm + kGmParty, &pmi) || !pmi) return true;
    for (int i = 0; i < 4; ++i) {
        uint32_t handle = 0, state = 0;
        if (!read32(pmi + 0x18 + i * 0x14, &handle) || !read32(pmi + 0x20 + i * 0x14, &state)) return true;
        if (handle == 0xFFFFFFFF || chr_of_handle(handle) != chr) continue;
        if (state == 1 || state == 3 || state == 4) return true;
    }
    return false;
}
Made g_made[3];

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
        out.push_back(r);
    }
    return out;
}

void tick(bool active, PeerState (*peer)(uint32_t chr, const std::wstring& name)) {
    const bool loading = game::loading() || !game::in_game();
    if (loading) {
        g_was_loading = true;
        g_removed.clear();
        for (Made& m : g_made) m = Made{};
        return;
    }
    if (g_was_loading) {
        g_was_loading = false;
        g_loaded_at = now();
    }
    if (!active || now() - g_loaded_at < 3.0 || now() - g_last_check < 1.0) return;
    g_last_check = now();
    const int state = game::session_state();
    if (!(state == 3 || (state == 6 && game::join_step() == 4))) return;

    const std::vector<Node> nodes = slot_nodes();
    const std::vector<Remote> list = remotes();
    for (const Remote& r : list) {
        if (!r.data_ready || r.slot < 0 || r.slot > 2) continue;
        if (now() - g_queued_at[r.slot] < 5.0) continue;
        uint32_t member = 0;
        for (const Node& n : nodes) {
            if (n.slot == r.slot) member = n.member;
        }
        if (!member) continue;
        const PeerState ps = peer ? peer(r.chr, r.name) : PeerState{};
        char name[64];
        WideCharToMultiByte(CP_UTF8, 0, r.name.c_str(), -1, name, sizeof(name), nullptr, nullptr);
        if (!r.chr || g_removed.count(r.chr) || being_removed(r.chr)) {
            if (r.chr) continue;  // the old character is on its way out
            if (waiting(member)) continue;
            if (push_waiting(member)) {
                g_queued_at[r.slot] = now();
                LOG_INFO("[coop] %s's character is missing: asked the game to create it again", name);
            }
            continue;
        }
        // An invader's character missing after our own load (we died and respawned) is asked for like anyone's:
        // the host and the guests saw no invader after their respawn (2026-10-16/17 invasions, 1.0.0 players). An
        // invader who dies goes home, so nothing below (the rebuild after a death) is for them.
        if (ps.invader) continue;
        Made& made = g_made[r.slot];
        if (made.chr != r.chr || (ps.known && made.peer != ps.id)) {
            made = Made{};
            made.chr = r.chr;
            made.peer = ps.known ? ps.id : 0;
            made.deaths = ps.deaths;
            made.death_events = ps.death_events;
        }
        if (hp(r.chr) <= 0) made.seen_dead = true;
        // A death shows as 0 HP here, a higher death count, or the player's own death message (the copy's HP is
        // not always seen at 0 in the 1 s checks).
        const bool died = made.seen_dead || (ps.known && made.deaths >= 0 && ps.deaths > made.deaths) ||
                          (ps.known && made.death_events >= 0 && ps.death_events > made.death_events);
        if (made.deaths < 0) made.deaths = ps.deaths;
        if (made.death_events < 0) made.death_events = ps.death_events;
        if (!died) continue;
        // Their character here died. Rebuild it once that player reports being alive (they respawned) and the
        // party is done with it; a character no player can be matched to stays as it is. The old character goes
        // first; a later check finds the slot empty and asks for the new one, which then takes the same slot.
        if (!ps.known || !ps.alive) continue;
        if (party_reads_character(r.chr)) {
            static int told = 0;
            if (++told <= 10) LOG_INFO("[coop] %s's character is still in use by the party: not rebuilt", name);
            continue;
        }
        if (remove_character(r.chr)) {
            g_removed.insert(r.chr);
            made = Made{};
            g_queued_at[r.slot] = now() - 4.0;  // ask for the new character on the next check
            LOG_INFO("[coop] %s is back after dying: their character is created again", name);
        }
    }
}

}  // namespace coop::party
