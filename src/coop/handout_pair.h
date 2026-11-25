// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The session host's hand-out of enemies: the pass that settles every pair of members' bid lists (the game's loop at
// 0xE559DD in 0xE558F0, calling 0x46FF00 for each pair), with the same results found faster. No game headers here:
// the test harness (scratchpad p64/handout_fuzz.cpp) compiles this file against the game's own machine code.
//
// The host's game keeps a record per session member (0xE561C0) with the list of enemies that member's game bids for:
// every enemy of its loaded map blocks it may move (0xE40A20), so players in one area list the same enemies. A list
// is doubly linked, sentinel at +0x10, count at +0x14, nodes {next, prev, value} from the heap at +0x04, value = the
// enemy's net handle (low 16 bits) and the bid (high 16 bits). Every hand-out (every second in a session, netrate.cpp)
// the loop calls 0x46FF00(A, B) for every pair of members, A before B in the member list. For each entry of A, in A's
// order, it walks B for B's first entry of the same enemy and, when B's bid is not 0:
//   - B's bid higher, or the same with B's list no longer than A's (the counts as they stand then): A's entry goes
//     (unlinked, freed to A's heap, A's count less one);
//   - else every entry of that enemy in B goes (0x46FEB0, esi = B, a walk of all of B).
// So each entry of A walks B, members^2 x enemies^2 a hand-out, and most of all when the lists share nothing (players
// in different places: nothing is removed, every walk goes to the end). Measured in the game (2026-11-25, 160 enemies
// a player): 64 players all apart 49.9 ms a hand-out, 32 apart 12.2 ms, 64 in one place 1.7 ms; this file's pass
// 0.19, 0.04 and 0.43 ms.
//
// Here the pass is settled as one (settle_pass), in the game's order, with two facts of that order:
//   - During the pass a list only loses entries. A pair whose lists share no enemy when A's row starts shares none
//     later and changes nothing in the game's loop, so it is skipped: each enemy has a mask of the members that listed
//     it when the pass began, and a row visits only the later members in the masks of A's enemies.
//   - A member is B in all of its pairs before it is ever A (rows go in member order), and as B its list changes only
//     by the removals of the pair step. So each member's list is indexed once (first entry of each enemy, and whether
//     the list holds it more than once) and the index is kept by those removals: a lookup instead of a walk.
// Everything else is the game's, in the game's order: the same entries leave the same lists in the same order, freed by
// the same heaps, and the counts change at the same moments, so the count tie-break reads the same values. A list
// holding an enemy twice (the receiver, 0x46FE50, does not merge them) has that enemy's entries removed by the game's
// walk; a record listed twice (never seen) runs the game's loop as it is.

#include <intrin.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace coop::handout {

struct Node {
    Node* next;
    Node* prev;
    uint32_t value;  // low 16 bits: the enemy's net handle; high 16: the bid
};

struct Member {         // the host's record of a member (0xE561C0)
    uint32_t id;        // +0x00 the member's id in the session (-1: gone)
    uint32_t heap;      // +0x04 the list's heap: vt+0x34 frees a node (thiscall)
    uint32_t list_8;    // +0x08
    uint32_t list_c;    // +0x0C
    Node* head;         // +0x10 sentinel
    int32_t count;      // +0x14
};
static_assert(sizeof(Node) == 12 && sizeof(Member) == 0x18, "hand-out record layout");

struct MemberNode {  // the hand-out's member list (+0x174 its sentinel)
    MemberNode* next;
    MemberNode* prev;
    Member* rec;
};

using HeapFreeFn = void(__thiscall*)(uint32_t heap, void* block);

inline void erase(Member* m, Node* n) {  // the game's list erase, as inlined in 0x46FF00 / 0x46FEB0
    if (n == m->head) return;
    n->prev->next = n->next;
    n->next->prev = n->prev;
    const uint32_t vt = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(m->heap));
    (*reinterpret_cast<HeapFreeFn*>(static_cast<uintptr_t>(vt + 0x34)))(m->heap, n);
    --m->count;
}

// 0x46FEB0: every entry of the handle leaves the list, in the list's order.
inline void erase_all(Member* m, uint32_t handle) {
    Node* n = m->head->next;
    while (n != m->head) {
        Node* const next = n->next;
        if ((n->value & 0xFFFF) == handle) erase(m, n);
        n = next;
    }
}

// 0x46FF00 as the game runs it: the reference, and the pass for a record listed twice.
inline void settle_pair_plain(Member* a, Member* b) {
    if (!a || !b) return;  // the game's: no B, nothing (no A, a fault; never seen)
    Node* x = a->head->next;
    while (x != a->head) {
        const uint32_t handle = x->value & 0xFFFF;
        const int32_t bid_a = static_cast<int32_t>(x->value >> 16);
        Node* y = b->head->next;
        while (y != b->head && (y->value & 0xFFFF) != handle) y = y->next;
        if (y != b->head) {
            const int32_t bid_b = static_cast<int32_t>(y->value >> 16);
            if (bid_b > 0) {
                if (bid_b > bid_a || (bid_b == bid_a && a->count >= b->count)) {
                    Node* const next = x->next;
                    erase(a, x);
                    x = next;
                    continue;
                }
                erase_all(b, handle);
            }
        }
        x = x->next;
    }
}

namespace detail {

// A member's index: open addressing in g_slots. key = handle + 1 (0: empty); node = the list's first entry of the
// handle, bit 0 set when the list holds it more than once (nodes are heap blocks, at least 4-aligned), 0 once the list
// holds it no more.
struct Slot {
    uint32_t key;
    uint32_t node;
};
struct Index {
    uint32_t at;    // first slot
    uint32_t bits;  // log2 of its slots
};
inline std::vector<Slot> g_slots;
inline std::vector<Index> g_index;
inline std::vector<Member*> g_recs;
// per handle: the members (bit = place in the member list, up to 64) that listed it when the pass began
inline uint32_t g_handle_pass[0x10000];
inline uint64_t g_handle_members[0x10000];
inline uint32_t g_pass = 0;

inline uint32_t slot_of(uint32_t handle, uint32_t bits) { return (handle * 0x9E3779B1u) >> (32 - bits); }

inline void build_index(const Member* m, Index& ix) {
    uint32_t n = 0;
    for (const Node* y = m->head->next; y != m->head; y = y->next) ++n;
    uint32_t bits = 3;
    while ((1u << bits) < n * 2) ++bits;
    ix.at = static_cast<uint32_t>(g_slots.size());
    ix.bits = bits;
    g_slots.resize(g_slots.size() + (size_t{1} << bits), Slot{0, 0});
    Slot* const s = g_slots.data() + ix.at;
    const uint32_t mask = (1u << bits) - 1;
    for (Node* y = m->head->next; y != m->head; y = y->next) {
        const uint32_t handle = y->value & 0xFFFF;
        uint32_t k = slot_of(handle, bits);
        while (s[k].key != 0 && s[k].key != handle + 1) k = (k + 1) & mask;
        if (s[k].key == 0) {
            s[k].key = handle + 1;
            s[k].node = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(y));
        } else {
            s[k].node |= 1;
        }
    }
}

inline Slot* find(const Index& ix, uint32_t handle) {
    Slot* const s = g_slots.data() + ix.at;
    const uint32_t mask = (1u << ix.bits) - 1;
    for (uint32_t k = slot_of(handle, ix.bits);; k = (k + 1) & mask) {
        if (s[k].key == handle + 1) return &s[k];
        if (s[k].key == 0) return nullptr;
    }
}

// 0x46FF00 with B's index for the walks.
inline void settle_pair_indexed(Member* a, Member* b, const Index& ix) {
    Node* x = a->head->next;
    while (x != a->head) {
        const uint32_t handle = x->value & 0xFFFF;
        Slot* const s = find(ix, handle);
        if (s && s->node) {
            Node* const y = reinterpret_cast<Node*>(static_cast<uintptr_t>(s->node & ~1u));
            const int32_t bid_a = static_cast<int32_t>(x->value >> 16);
            const int32_t bid_b = static_cast<int32_t>(y->value >> 16);
            if (bid_b > 0) {
                if (bid_b > bid_a || (bid_b == bid_a && a->count >= b->count)) {
                    Node* const next = x->next;
                    erase(a, x);
                    x = next;
                    continue;
                }
                if (s->node & 1) {
                    erase_all(b, handle);
                } else {
                    erase(b, y);
                }
                s->node = 0;
            }
        }
        x = x->next;
    }
}

}  // namespace detail

struct PassStats {
    int members = 0;
    int entries = 0;     // entries in all lists when the pass began
    int pairs = 0;       // pairs the game's loop calls the pair step for
    int pairs_run = 0;   // pairs that shared an enemy (the rest change nothing)
    bool plain = false;  // a record listed twice: the game's loop as it is
};

// The pair loop of 0xE558F0: every member's list settled against every later member's, as the game does.
inline PassStats settle_pass(MemberNode* head) {
    using namespace detail;
    PassStats st;
    g_recs.clear();
    for (MemberNode* m = head->next; m != head; m = m->next) g_recs.push_back(m->rec);
    const int n = static_cast<int>(g_recs.size());
    st.members = n;
    st.pairs = n * (n - 1) / 2;
    if (n < 2) return st;
    for (int i = 0; i < n && !st.plain; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (g_recs[i] == g_recs[j]) {
                st.plain = true;
                break;
            }
        }
    }
    if (st.plain) {
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) settle_pair_plain(g_recs[i], g_recs[j]);
        return st;
    }
    if (++g_pass == 0) {
        std::memset(g_handle_pass, 0, sizeof(g_handle_pass));
        g_pass = 1;
    }
    const bool masks = n <= 64;
    g_slots.clear();
    g_index.assign(n, Index{0, 0});
    for (int j = 0; j < n; ++j) {
        const Member* m = g_recs[j];
        if (!m) continue;
        if (j > 0) build_index(m, g_index[j]);
        for (const Node* y = m->head->next; y != m->head; y = y->next) {
            ++st.entries;
            if (!masks) continue;
            const uint32_t handle = y->value & 0xFFFF;
            if (g_handle_pass[handle] != g_pass) {
                g_handle_pass[handle] = g_pass;
                g_handle_members[handle] = 0;
            }
            g_handle_members[handle] |= uint64_t{1} << j;
        }
    }
    for (int i = 0; i + 1 < n; ++i) {
        Member* const a = g_recs[i];
        if (!a || a->head->next == a->head) continue;  // nothing to settle in this row
        if (masks) {
            uint64_t later = 0;
            for (const Node* x = a->head->next; x != a->head; x = x->next) later |= g_handle_members[x->value & 0xFFFF];
            later &= ~((uint64_t{2} << i) - 1);  // members after A
            while (later) {
                unsigned long j;
                _BitScanForward(&j, static_cast<unsigned long>(later));
                if (!static_cast<uint32_t>(later)) {
                    _BitScanForward(&j, static_cast<unsigned long>(later >> 32));
                    j += 32;
                }
                later &= later - 1;
                Member* const b = g_recs[j];
                if (!b) continue;
                ++st.pairs_run;
                settle_pair_indexed(a, b, g_index[j]);
                if (a->head->next == a->head) break;  // A has nothing left: the rest of the row changes nothing
            }
        } else {
            for (int j = i + 1; j < n; ++j) {
                Member* const b = g_recs[j];
                if (!b) continue;
                ++st.pairs_run;
                settle_pair_indexed(a, b, g_index[j]);
                if (a->head->next == a->head) break;
            }
        }
    }
    return st;
}

}  // namespace coop::handout
