// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
//
// A stress test for sessions of many players (2026-11-24, the 64-player work): with no 64 games to join, the world's
// own characters stand in for players. `npcs N` turns the N living characters nearest our player into white phantoms
// the game's own way (0xE407F0, as a summon sign does) and adds each to the party list as an NPC member (0xEA2510, a
// live summon: the summon-in animation and the notice), then places them in rings around our player: each takes a
// party entry (partylist.cpp, past the game's three NPC handles while the test runs) and a floating health bar with its
// name (players.cpp makes the gauges in the HUD). `npcs 0` sends them home (0xEA2A10). `perf S` measures the game
// thread's frames for S seconds (frames, mean and worst frame time, the 99th percentile) into the log and the reply
// file. Diagnostics only: a release build's remote control is a stub, so neither command ever comes.
#include "stress.h"
#include "bells.h"

#include "game.h"
#include "handout.h"
#include "handout_pair.h"
#include "hook.h"
#include "log.h"
#include "partylist.h"
#include "players.h"
#include "session.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace coop::stress {

namespace {

constexpr uint32_t kWorldChrManVar = 0x0137D644;
constexpr uint32_t kGameManVar = 0x013784A0;
constexpr uint32_t kEnemyVtable = 0x0118979C;  // EnemyIns (NPCs too)
constexpr uint32_t kMakePhantom = 0x00E407F0;  // edx = handle, stdcall(type, kind) -> character or 0
constexpr uint32_t kPartyAdd = 0x00EA2510;     // stdcall(party, handle, chr type, 2 = NPC, f1, f2, f3)
constexpr uint32_t kPartyLeave = 0x00EA2A10;   // eax = handle, stdcall(party)

volatile LONG g_want = -1;       // npcs N (-1: no request)
volatile LONG g_perf_req = 0;    // perf S
volatile LONG g_spread = 0;
volatile LONG g_session_req = 0;  // coophost 1 / coopleave 2 (the hotkeys' calls, for a game not in front)      // npcs N R: the first ring's radius in tenths of a metre (0: 2.5 m)
std::vector<uint32_t> g_mine;    // the handles we made phantoms of
struct Pending {
    uint32_t handle;
    double since;
};
std::vector<Pending> g_pending;  // brought next to our player, made phantoms once their map entity is active
int g_want_now = 0;
double g_perf_until = 0;
std::vector<double> g_frames;    // frame times (ms) while measuring
LARGE_INTEGER g_last = {};
double g_freq = 0;

uint32_t rd(uint32_t a) {
    uint32_t v = 0;
    return read32(a, &v) ? v : 0;
}

double now_s() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / g_freq;
}

// A character handle's character (0x10000000 | block << 14 | place), as 0xE3E340.
uint32_t chr_by_handle(uint32_t handle) {
    const uint32_t wcm = rd(kWorldChrManVar);
    const uint32_t set = wcm ? rd(wcm + 0x240 + ((handle >> 14) & 0x3F) * 4) : 0;
    const uint32_t at = handle & 0x3FFF;
    if (!set || at >= rd(set)) return 0;
    const uint32_t entries = rd(set + 4);
    return entries ? rd(entries + at * 0x20) : 0;
}

bool position(uint32_t chr, float* xyz) {
    const uint32_t body = rd(rd(chr + 0x28) + 0x1C);
    if (!body) return false;
    for (int i = 0; i < 3; ++i) {
        if (!read32(body + 0x10 + i * 4, reinterpret_cast<uint32_t*>(&xyz[i]))) return false;
    }
    return true;
}

// The event command Warp Character's own move (0xD71A90): the character's vt+0x30C(position, rotation, 1) with the
// rotation its body has, then +0x1FD bit 1 (warped).
void place(uint32_t chr, const float* xyz) {
    const uint32_t body = rd(rd(chr + 0x28) + 0x1C);
    if (!body) return;
    __try {
        alignas(16) float pos[4] = {xyz[0], xyz[1], xyz[2], *reinterpret_cast<float*>(body + 0x1C)};
        alignas(16) float rot[4];
        std::memcpy(rot, reinterpret_cast<const void*>(body), sizeof(rot));
        using WarpFn = void(__thiscall*)(uint32_t chr, float* pos, float* rot, int32_t);
        reinterpret_cast<WarpFn>(rd(rd(chr) + 0x30C))(chr, pos, rot, 1);
        *reinterpret_cast<uint8_t*>(chr + 0x1FD) |= 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

uint32_t party() {
    const uint32_t gm = rd(kGameManVar);
    return gm ? rd(gm + 0xBEC) : 0;
}

bool in_party(uint32_t p, uint32_t handle) {
    for (int i = 0; i < partylist::entries(); ++i) {
        if (rd(p + 0x18 + i * 0x14) == handle) return true;
    }
    return false;
}

uint32_t make_phantom(uint32_t handle) {
    uint32_t chr = 0;
    __try {
        __asm {
            push 1        // kind: a white sign's
            push 1        // chr type 1: a white phantom
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

// The steps of 0xE407F0 past its test (vt+0x9C, true only for the human characters NPC summons are): enemies stand in
// as phantoms too. Chr type 1 (+0x70) and the type's handler (vt+0x98), the team for it (0xE66AB0: ecx = the sign's
// kind, edx = the type), then 0xE4FB50 (esi = the character).
constexpr uint32_t kTeamForType = 0x00E66AB0;
constexpr uint32_t kAfterType = 0x00E4FB50;

bool force_phantom(uint32_t chr) {
    __try {
        __asm {
            push esi
            push ebx
            mov esi, chr
            mov ebx, 1
            mov dword ptr [esi + 0x70], ebx
            mov edx, dword ptr [esi]
            mov eax, dword ptr [edx + 0x98]
            mov ecx, esi
            call eax
            cmp dword ptr [esi + 0x74], 5
            je no_team
            mov ecx, 1
            mov edx, ebx
            mov eax, kTeamForType
            call eax
            mov dword ptr [esi + 0x74], eax
        no_team:
            mov eax, kAfterType
            call eax
            pop ebx
            pop esi
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool party_add(uint32_t p, uint32_t handle) {
    using AddFn = char(__stdcall*)(uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
    __try {
        return reinterpret_cast<AddFn>(kPartyAdd)(p, handle, 1, 2, -1, -1, -1) != 0;
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

struct Candidate {
    uint32_t handle;
    uint32_t chr;
    float d2;
};

// The living, enabled characters of the loaded map blocks that are not phantoms yet, nearest first.
std::vector<Candidate> candidates(uint32_t me, const float* at) {
    std::vector<Candidate> out;
    const uint32_t wcm = rd(kWorldChrManVar);
    if (!wcm) return out;
    for (uint32_t block = 0; block < 64; ++block) {
        const uint32_t set = rd(wcm + 0x240 + block * 4);
        if (!set) continue;
        const uint32_t n = rd(set), entries = rd(set + 4);
        if (!entries || n > 4096) continue;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t chr = rd(entries + i * 0x20);
            if (!chr || chr == me || rd(chr) != kEnemyVtable) continue;
            uint8_t flags = 0;
            if (!read8(rd(chr + 0xC) + 0x14, &flags) || (flags & 1)) continue;  // disabled
            const int32_t hp = static_cast<int32_t>(rd(chr + 0x2D4));
            const int32_t type = static_cast<int32_t>(rd(chr + 0x70));
            if (hp <= 0 || type == 1 || type == 2 || type == 0xC) continue;
            float p[3];
            if (!position(chr, p)) continue;
            const float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
            out.push_back({0x10000000 | block << 14 | i, chr, dx * dx + dy * dy + dz * dz});
        }
    }
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.d2 < b.d2; });
    return out;
}

void dismiss_all() {
    const uint32_t p = party();
    int gone = 0;
    for (uint32_t h : g_mine) {
        if (p && in_party(p, h)) {
            party_leave(p, h);
            ++gone;
        }
    }
    g_mine.clear();
    players::want_bars(0);
    partylist::set_npc_test(false);
    LOG_INFO("[coop] Stress test: %d NPC phantoms sent home", gone);
}

// The place of stand-in i: rings around our player, 12 to a ring, 1.5 m apart, the first at the asked radius.
void ring_place(size_t i, const float* at, float* out) {
    const float r0 = g_spread > 0 ? g_spread / 10.0f : 2.5f;
    const int ring = static_cast<int>(i / 12), k = static_cast<int>(i % 12);
    const float r = r0 + 1.5f * ring, a = (k + 0.5f * ring) * 6.2831853f / 12.0f;
    out[0] = at[0] + r * std::cos(a);
    out[1] = at[1] + 0.3f;
    out[2] = at[2] + r * std::sin(a);
}

void summon(int want) {
    const uint32_t me = game::player();
    const uint32_t p = party();
    float at[3];
    if (!me || !p || !position(me, at)) {
        LOG_INFO("[coop] Stress test: no player or party yet");
        return;
    }
    // Drop the ones that left (died, or a load took them).
    g_mine.erase(std::remove_if(g_mine.begin(), g_mine.end(), [&](uint32_t h) { return !in_party(p, h); }),
                 g_mine.end());
    if (want <= 0) {
        g_pending.clear();
        g_want_now = 0;
        dismiss_all();
        return;
    }
    g_want_now = want;
    players::want_bars(want);
    partylist::set_npc_test(true);
    // Each stand-in is brought next to our player first: one far away is not updated (its map entity inactive,
    // chr+0xF4 = -1), and the phantom's steps fault on it. It becomes a phantom once its entity is active (tick).
    int brought = 0;
    const int missing = want - static_cast<int>(g_mine.size() + g_pending.size());
    if (missing > 0) {
        for (const Candidate& c : candidates(me, at)) {
            if (brought >= missing) break;
            if (in_party(p, c.handle)) continue;
            bool queued = false;
            for (const Pending& q : g_pending) queued = queued || q.handle == c.handle;
            if (queued) continue;
            float xyz[3];
            ring_place(g_mine.size() + g_pending.size(), at, xyz);
            place(c.chr, xyz);
            g_pending.push_back({c.handle, now_s()});
            ++brought;
        }
    }
    // The ones made already go back to their places (they wander off with their AI).
    for (size_t i = 0; i < g_mine.size(); ++i) {
        const uint32_t chr = chr_by_handle(g_mine[i]);
        if (!chr) continue;
        float xyz[3];
        ring_place(i, at, xyz);
        place(chr, xyz);
    }
    LOG_INFO("[coop] Stress test: %d NPC phantoms, %d brought over to become phantoms; party +0x10 = %d, HUD gauges %d",
             static_cast<int>(g_mine.size()), brought, static_cast<int>(rd(p + 0x10)), players::gauges());
}

// The stand-ins brought over: a phantom each once its entity is active (0.3 s after the move at the earliest), given
// up after 5 s.
void pending_tick() {
    if (g_pending.empty()) return;
    const uint32_t p = party();
    if (!p) return;
    int made = 0, refused = 0, waited_out = 0;
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        const double age = now_s() - it->since;
        const uint32_t chr = chr_by_handle(it->handle);
        if (!chr) {
            it = g_pending.erase(it);
            continue;
        }
        const int32_t rate = static_cast<int32_t>(rd(chr + 0xF4));
        if (age < 0.3 || (rate < 0 && age < 5.0)) {
            ++it;
            continue;
        }
        if (rate < 0) {
            ++waited_out;
        } else if ((make_phantom(it->handle) || force_phantom(chr)) && party_add(p, it->handle)) {
            g_mine.push_back(it->handle);
            ++made;
        } else {
            ++refused;
        }
        it = g_pending.erase(it);
    }
    if (made || refused || waited_out) {
        LOG_INFO("[coop] Stress test: %d NPC phantoms (%d new, %d refused, %d never active); party +0x10 = %d",
                 static_cast<int>(g_mine.size()), made, refused, waited_out, static_cast<int>(rd(p + 0x10)));
    }
}

void perf_report() {
    if (g_frames.empty()) return;
    std::vector<double> f = g_frames;
    std::sort(f.begin(), f.end());
    double sum = 0;
    for (double v : f) sum += v;
    const double mean = sum / f.size();
    const double p99 = f[(std::min)(f.size() - 1, static_cast<size_t>(f.size() * 0.99))];
    const double p50 = f[f.size() / 2];
    LOG_INFO("[coop] Stress test perf: %d phantoms, %d frames, mean %.3f ms (%.1f FPS), median %.3f ms, 99th %.3f ms, "
             "worst %.3f ms", static_cast<int>(g_mine.size()), static_cast<int>(f.size()), mean, 1000.0 / mean, p50,
             p99, f.back());
}


// ---------------------------------------------------------------- the hand-out, in the game (handouttest N)
// The game's own pair step (0x46FF00 for every pair in order, as its loop at 0xE559DD) against handout::settle_pass,
// on random bid lists made in the game's network heap (vt+0x28 allocates, vt+0x34 frees: the heap the hand-out's
// lists come from), on the game thread: every list and count compared. Then both timed on lists of players in one
// place and in many. Records and sentinels are ours (the pair step reads only +0x04, +0x10, +0x14 of a record).
constexpr uint32_t kNetHeapVar = 0x013787F8;
constexpr uint32_t kGamePairStep = 0x0046FF00;
volatile LONG g_handout_req = 0;  // handouttest N

using NetAllocFn = void*(__thiscall*)(uint32_t heap, uint32_t size, uint32_t align);
using NetFreeFn = void(__thiscall*)(uint32_t heap, void* block);
using PairStepFn = void(__stdcall*)(handout::Member*, handout::Member*);

struct HoWorld {
    std::vector<handout::Member> recs;
    std::vector<handout::Node> heads;
    std::vector<handout::MemberNode> mnodes;
    handout::MemberNode mhead{};
    std::vector<handout::Node*> made;
    HoWorld() = default;
    HoWorld(const HoWorld&) = delete;
    // lists[i] = record i's values; order = the member list (record per member)
    bool build(uint32_t heap, const std::vector<std::vector<uint32_t>>& lists) {
        const uint32_t vt = *ptr<uint32_t>(heap);
        const NetAllocFn alloc = *ptr<NetAllocFn>(vt + 0x28);
        const size_t m = lists.size();
        recs.assign(m, handout::Member{});
        heads.assign(m, handout::Node{});
        mnodes.assign(m, handout::MemberNode{});
        mhead.next = mhead.prev = &mhead;
        for (size_t i = 0; i < m; ++i) {
            handout::Node* h = &heads[i];
            h->next = h->prev = h;
            recs[i].id = static_cast<uint32_t>(i);
            recs[i].heap = heap;
            recs[i].head = h;
            recs[i].count = static_cast<int32_t>(lists[i].size());
            handout::MemberNode* mn = &mnodes[i];
            mn->rec = &recs[i];
            mn->next = &mhead;
            mn->prev = mhead.prev;
            mhead.prev->next = mn;
            mhead.prev = mn;
        }
        size_t most = 0;
        for (const auto& l : lists) most = (std::max)(most, l.size());
        for (size_t k = 0; k < most; ++k) {  // made interleaved, as reports arrive
            for (size_t i = 0; i < m; ++i) {
                if (k >= lists[i].size()) continue;
                auto* n = static_cast<handout::Node*>(alloc(heap, sizeof(handout::Node), 4));
                if (!n) return false;
                made.push_back(n);
                n->value = lists[i][k];
                handout::Node* h = &heads[i];
                n->next = h;
                n->prev = h->prev;
                h->prev->next = n;
                h->prev = n;
            }
        }
        return true;
    }
    // record i's list as places in `made` (the same in both worlds when the same entries are left)
    std::vector<int> list_of(size_t i) const {
        std::vector<int> out;
        for (const handout::Node* n = heads[i].next; n != &heads[i]; n = n->next) {
            out.push_back(static_cast<int>(std::find(made.begin(), made.end(), n) - made.begin()));
        }
        return out;
    }
    void release(uint32_t heap) {  // what the pass left
        const NetFreeFn fr = *ptr<NetFreeFn>(*ptr<uint32_t>(heap) + 0x34);
        for (size_t i = 0; i < recs.size(); ++i) {
            handout::Node* n = heads[i].next;
            while (n != &heads[i]) {
                handout::Node* next = n->next;
                fr(heap, n);
                n = next;
            }
            heads[i].next = heads[i].prev = &heads[i];
        }
    }
};

double ho_ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

void game_pass(HoWorld& w) {
    const PairStepFn step = reinterpret_cast<PairStepFn>(static_cast<uintptr_t>(kGamePairStep));
    for (handout::MemberNode* a = w.mhead.next; a != &w.mhead; a = a->next)
        for (handout::MemberNode* b = a->next; b != &w.mhead; b = b->next) step(a->rec, b->rec);
}

// Places: each member's enemies are its place's (places = 1: everyone together; = members: everyone apart).
std::vector<std::vector<uint32_t>> ho_lists(uint32_t& seed, int members, int enemies, int places, bool random_bids) {
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    std::vector<std::vector<uint32_t>> lists(members);
    for (int i = 0; i < members; ++i) {
        const int place = i % places;
        std::vector<uint32_t>& v = lists[i];
        for (int k = 0; k < enemies; ++k) v.push_back(static_cast<uint32_t>(((place % 32) << 11) | ((k * 3 + place / 32 * 700) & 0x7FF)));
        for (size_t k = v.size(); k > 1; --k) std::swap(v[k - 1], v[rnd() % k]);
        for (uint32_t& e : v) {
            const uint32_t bid = random_bids ? (rnd() % 8 == 0 ? 0 : rnd() % 4) : 256 + rnd() % 256;
            e |= bid << 16;
        }
    }
    return lists;
}

void handout_test(int cases) {
    const uint32_t heap = *ptr<uint32_t>(kNetHeapVar);
    if (!heap) {
        LOG_INFO("[stress] handouttest: no network heap");
        return;
    }
    uint32_t seed = 20261125;
    int same = 0, differ = 0;
    long long pairs = 0, pairs_run = 0;
    for (int c = 0; c < cases; ++c) {
        uint32_t s2 = seed;
        const int members = 2 + static_cast<int>((seed = seed * 1664525u + 1013904223u) >> 8) % 63;
        const int enemies = static_cast<int>((seed = seed * 1664525u + 1013904223u) >> 8) % 120;
        const int places = 1 + static_cast<int>((seed = seed * 1664525u + 1013904223u) >> 8) % members;
        s2 = seed;
        const auto lists = ho_lists(seed, members, enemies, places, true);
        // a few enemies twice in a list
        auto lists2 = lists;
        if (c % 4 == 0) {
            for (auto& l : lists2) {
                if (l.size() > 2) l.push_back(l[(s2 >> 8) % l.size()]);
            }
        }
        HoWorld w1, w2;
        if (!w1.build(heap, lists2) || !w2.build(heap, lists2)) {
            LOG_INFO("[stress] handouttest: the network heap is full");
            w1.release(heap);
            w2.release(heap);
            return;
        }
        game_pass(w1);
        const handout::PassStats st = handout::settle_pass(&w2.mhead);
        bool ok = true;
        for (int i = 0; ok && i < members; ++i) {
            ok = w1.recs[i].count == w2.recs[i].count && w1.list_of(i) == w2.list_of(i);
        }
        ok ? ++same : ++differ;
        if (!ok && differ <= 3) {
            LOG_INFO("[stress] handouttest: case %d DIFFERS (%d members, %d enemies, %d places)", c, members, enemies,
                     places);
        }
        pairs += st.pairs;
        pairs_run += st.pairs_run;
        w1.release(heap);
        w2.release(heap);
    }
    LOG_INFO("[stress] handouttest: %d random hand-outs in the network heap, %d identical, %d different (%lld pairs, "
             "%lld shared an enemy)", cases, same, differ, pairs, pairs_run);
    struct Shape {
        int members, enemies, places;
    };
    for (const Shape sh : {Shape{8, 160, 1}, Shape{8, 160, 8}, Shape{32, 160, 1}, Shape{32, 160, 32},
                           Shape{64, 160, 1}, Shape{64, 160, 8}, Shape{64, 160, 64}}) {
        const auto lists = ho_lists(seed, sh.members, sh.enemies, sh.places, false);
        HoWorld w1, w2;
        if (!w1.build(heap, lists) || !w2.build(heap, lists)) {
            LOG_INFO("[stress] handouttest: the network heap is full");
            w1.release(heap);
            w2.release(heap);
            return;
        }
        LARGE_INTEGER t0, t1, t2;
        QueryPerformanceCounter(&t0);
        game_pass(w1);
        QueryPerformanceCounter(&t1);
        handout::settle_pass(&w2.mhead);
        QueryPerformanceCounter(&t2);
        bool ok = true;
        for (int i = 0; ok && i < sh.members; ++i) ok = w1.recs[i].count == w2.recs[i].count;
        LOG_INFO("[stress] handouttest: %2d players, %d enemies each, %2d place(s): game %.3f ms, ours %.3f ms (%s)",
                 sh.members, sh.enemies, sh.places, ho_ms(t0, t1), ho_ms(t1, t2), ok ? "same counts" : "COUNTS DIFFER");
        w1.release(heap);
        w2.release(heap);
    }
}

}  // namespace

bool remote(const char* line, char* reply, size_t size) {
    char cmd[16] = {};
    if (sscanf_s(line, "%15s", cmd, static_cast<unsigned>(sizeof(cmd))) != 1) return false;
    if (std::strcmp(cmd, "npcs") == 0) {
        int n = 0, spread = 0;
        sscanf_s(line, "%*s %d %d", &n, &spread);
        n = (std::max)(0, (std::min)(players::kMaxBars, n));
        InterlockedExchange(&g_spread, spread);
        InterlockedExchange(&g_want, n);
        std::snprintf(reply, size, "npcs %d: in the log", n);
        return true;
    }
    if (std::strcmp(cmd, "coophost") == 0 || std::strcmp(cmd, "coopleave") == 0) {
        InterlockedExchange(&g_session_req, cmd[4] == 'h' ? 1 : 2);
        std::snprintf(reply, size, "%s", cmd);
        return true;
    }
    if (std::strcmp(cmd, "bars") == 0) {
        int on = 1;
        sscanf_s(line, "%*s %d", &on);
        players::show_all_bars(on != 0);
        std::snprintf(reply, size, "bars %d", on != 0);
        return true;
    }
    if (std::strcmp(cmd, "handout") == 0) {
        handout::describe(reply, size);
        return true;
    }
    if (std::strcmp(cmd, "handouttest") == 0) {
        int n = 200;
        sscanf_s(line, "%*s %d", &n);
        InterlockedExchange(&g_handout_req, (std::max)(1, n));
        std::snprintf(reply, size, "handouttest %d: in the log", (std::max)(1, n));
        return true;
    }
    if (std::strcmp(cmd, "bellsound") == 0) {
        int entity = 1401110, sound = 130300002;
        sscanf_s(line, "%*s %d %d", &entity, &sound);
        bells::play_sound(entity, sound);
        std::snprintf(reply, size, "bellsound %d %d: in the log", entity, sound);
        return true;
    }
    if (std::strcmp(cmd, "bellrecv") == 0 || std::strcmp(cmd, "bellsend") == 0) {
        int id = 10020;
        sscanf_s(line, "%*s %d", &id);
        if (cmd[4] == 'r') bells::received(id); else bells::send_event(id);
        std::snprintf(reply, size, "%s %d: in the log", cmd, id);
        return true;
    }
    if (std::strcmp(cmd, "perf") == 0) {
        int s = 10;
        sscanf_s(line, "%*s %d", &s);
        InterlockedExchange(&g_perf_req, (std::max)(1, s));
        std::snprintf(reply, size, "perf %d s: in the log", (std::max)(1, s));
        return true;
    }
    return false;
}

void tick() {
    if (g_freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = static_cast<double>(f.QuadPart);
        QueryPerformanceCounter(&g_last);
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const double dt_ms = static_cast<double>(t.QuadPart - g_last.QuadPart) * 1000.0 / g_freq;
    g_last = t;
    if (const LONG n = InterlockedExchange(&g_handout_req, 0)) handout_test(n);
    if (const LONG s = InterlockedExchange(&g_perf_req, 0)) {
        g_frames.clear();
        g_perf_until = now_s() + s;
    } else if (g_perf_until > 0) {
        g_frames.push_back(dt_ms);
        if (now_s() >= g_perf_until) {
            g_perf_until = 0;
            perf_report();
        }
    }
    if (const LONG s = InterlockedExchange(&g_session_req, 0)) {
        if (s == 1) session::host();
        else session::leave();
    }
    const LONG want = InterlockedExchange(&g_want, -1);
    if (want >= 0 && !game::loading()) summon(static_cast<int>(want));
    if (!game::loading()) pending_tick();
}

}  // namespace coop::stress
