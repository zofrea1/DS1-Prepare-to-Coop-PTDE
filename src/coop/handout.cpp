// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "handout.h"

#include "handout_pair.h"
#include "hook.h"
#include "log.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace coop::handout {

namespace {

// The session host's game hands each enemy to the member that bids the most for it (0xE57850 -> 0xE558F0, every
// second in a session: netrate.cpp). 0xE558F0 drops the members that left, then settles every pair of members' bid
// lists, at 0xE559DD:
//   mov eax,[ebx+174h] / mov edi,[eax] / cmp edi,eax / je end / (for each member, each later member)
//   push [esi+8] / push ebp / call 0x46FF00 / ... / end: pop edi / pop esi / pop ebp / pop ebx / ret 4
// with ebx the hand-out (WorldChrMan+0x2B4) and +0x174 its member list. The pair step walks the later member's list
// for every entry of the earlier one's: with players in different places (lists that share nothing, the walks never
// stop early) 32 players with 160 enemies each took 12 ms a hand-out, 64 took 50 ms (measured in the game,
// 2026-11-25). settle_pass (handout_pair.h) gives the same results, in the same order, in time linear in the entries:
// under 0.5 ms for 64 players with 160 enemies each in any arrangement. Checked against the game's own machine code of
// the pair step on a million random hand-outs (every list, count and freed entry identical), and in the game on 300
// more made in its network heap (stress.cpp, handouttest).
constexpr uint32_t kPassSite = 0x00E559DD;
constexpr uint32_t kPassEnd = 0x00E55A20;
const uint8_t kPassCode[74] = {
    0x8B, 0x83, 0x74, 0x01, 0x00, 0x00, 0x8B, 0x38, 0x3B, 0xF8, 0x74, 0x37, 0x8D, 0xA4, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x8B, 0x37, 0x8B, 0x6F, 0x08, 0x3B, 0xF0, 0x74, 0x1B, 0x8D, 0xA4, 0x24, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x4E, 0x08,
    0x51, 0x55, 0xE8, 0xF6, 0xA4, 0x61, 0xFF, 0x8B, 0x36, 0x3B, 0xB3, 0x74, 0x01, 0x00, 0x00, 0x75, 0xEC, 0x8B, 0x3F,
    0x8B, 0x83, 0x74, 0x01, 0x00, 0x00, 0x3B, 0xF8, 0x75, 0xD0, 0x5F, 0x5E, 0x5D, 0x5B, 0xC2, 0x04, 0x00};
// The pair step and the walk that removes an enemy from B (0x46FEB0..0x46FFD2), whose logic settle_pass repeats.
constexpr uint32_t kPairCode = 0x0046FEB0, kPairCodeSize = 290, kPairCodeHash = 0x5A208EA9;  // FNV-1a

int g_most_members = 1;  // the most members a pass had so far (logged when it grows)
bool g_plain_logged = false;
volatile LONG g_passes = 0;
PassStats g_last;
double g_last_ms = 0, g_most_ms = 0;

uint32_t fnv1a(const uint8_t* p, uint32_t n) {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

void __cdecl run_pass(MemberNode* head) {
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    const PassStats st = settle_pass(head);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
    g_last = st;
    g_last_ms = ms;
    if (ms > g_most_ms) g_most_ms = ms;
    InterlockedIncrement(&g_passes);
    if (st.members > g_most_members) {
        g_most_members = st.members;
        LOG_INFO("[coop] Enemy hand-out: %d players, %d bids, %d of %d pairs shared an enemy, %.3f ms", st.members,
                 st.entries, st.pairs_run, st.pairs, ms);
    }
    if (st.plain && !g_plain_logged) {
        g_plain_logged = true;
        LOG_INFO("[coop] Enemy hand-out: a member was listed twice; that pass ran the game's own pairwise walk");
    }
}

__declspec(naked) void hk_pass() {
    __asm {
        mov eax, [ebx + 0x174]      // the member list (ebx = the hand-out, as the game's loop has it)
        push eax
        call run_pass
        add esp, 4
        mov eax, kPassEnd           // the function's own end: pop edi / esi / ebp / ebx, ret 4
        jmp eax
    }
}

}  // namespace

void describe(char* out, size_t size) {
    std::snprintf(out, size, "handout passes %ld, last: %d members %d bids %d/%d pairs %.4f ms, slowest %.4f ms",
                  g_passes, g_last.members, g_last.entries, g_last.pairs_run, g_last.pairs, g_last_ms, g_most_ms);
}

bool install() {
    if (!bytes_match(kPassSite, kPassCode, sizeof(kPassCode))) {
        LOG_ERROR("[coop] Enemy hand-out: the game's pair loop at 0x%08X is not this build's; left as it is", kPassSite);
        return false;
    }
    if (fnv1a(ptr<const uint8_t>(kPairCode), kPairCodeSize) != kPairCodeHash) {
        LOG_ERROR("[coop] Enemy hand-out: the game's pair step at 0x%08X is not this build's; left as it is", kPairCode);
        return false;
    }
    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_pass)) - static_cast<int32_t>(kPassSite + 5);
    std::memcpy(jmp + 1, &rel, 4);
    if (!patch(kPassSite, kPassCode, jmp, sizeof(jmp))) {
        LOG_ERROR("[coop] Enemy hand-out: the pair loop could not be patched; left as it is");
        return false;
    }
    LOG_INFO("[coop] Enemy hand-out: pairs settled by lookups (the game's results, linear time)");
    return true;
}

}  // namespace coop::handout
