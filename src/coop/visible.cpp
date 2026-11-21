// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "visible.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <vector>

namespace coop::visible {

namespace {

// 0xE6AA30 (a character's model update): movq xmm0,[esi+264h] / [esi+26Ch] onto 16 bytes of stack, eax = the model
// (chr+0x24), then this call: the model takes those as its display groups.
constexpr uint32_t kModelGroupsCall = 0x00E6AAF3;
constexpr uint32_t kModelGroups = 0x00468DF0;

constexpr int kMaxOthers = 8;
constexpr int kMaxExtra = 192;
// The other players' characters this frame (the first g_count), then the enemies farenemies.cpp shows (to g_total).
uint32_t g_others[kMaxOthers + kMaxExtra] = {};
volatile int g_count = 0;
volatile int g_total = 0;
uint32_t g_model_groups = kModelGroups;
bool g_installed = false;

// In place of the call: for one of the other players' characters (esi) the groups on the stack become every group.
__declspec(naked) void model_groups_stub() {
    __asm {
        push ecx
        xor ecx, ecx
    next:
        cmp ecx, dword ptr [g_total]
        jae done
        cmp esi, dword ptr [g_others + ecx * 4]
        je other
        inc ecx
        jmp next
    other:
        mov dword ptr [esp + 8], 0xFFFFFFFF   // [esp+4] our return address, [esp+8..] the 16 bytes of groups
        mov dword ptr [esp + 12], 0xFFFFFFFF
        mov dword ptr [esp + 16], 0xFFFFFFFF
        mov dword ptr [esp + 20], 0xFFFFFFFF
    done:
        pop ecx
        jmp dword ptr [g_model_groups]
    }
}

}  // namespace

bool install() {
    g_installed = redirect_call(kModelGroupsCall, kModelGroups, reinterpret_cast<void*>(&model_groups_stub));
    if (!g_installed) {
        LOG_ERROR("[coop] Other players' characters: the model update did not match this build (they may still vanish "
                  "across map boundaries)");
    }
    return g_installed;
}

void tick() {
    if (!g_installed) return;
    const std::vector<uint32_t> others = game::loading() ? std::vector<uint32_t>{} : game::remote_player_chrs();
    int n = 0;
    for (uint32_t c : others) {
        if (n < kMaxOthers) g_others[n++] = c;
    }
    // The shown enemies stay after the players (set_extra writes them from g_count on): moved up or down with them.
    const int extra = g_total - g_count;
    if (n != g_count && extra > 0) std::memmove(&g_others[n], &g_others[g_count], extra * sizeof(uint32_t));
    g_count = n;
    g_total = n + (extra > 0 ? extra : 0);
}

void set_extra(const uint32_t* chrs, int n) {
    if (n < 0 || !chrs) n = 0;
    if (n > kMaxExtra) n = kMaxExtra;
    const int base = g_count;
    if (n > 0) std::memcpy(&g_others[base], chrs, n * sizeof(uint32_t));
    g_total = base + n;
}

int keep_players_asked() {
    int changed = 0;
    for (int i = 0; i < g_count && i < kMaxOthers; ++i) {
        const uint32_t chr = g_others[i];
        uint32_t ent = 0, ask = 0;
        uint8_t never = 1;
        if (!chr || !read8(chr + 0x14, &never) || never || !read32(chr + 0xC, &ent) || !ent || !read32(ent + 0xC, &ask)) {
            continue;
        }
        if (ask == 2) continue;
        *ptr<uint32_t>(ent + 0xC) = 2;
        ++changed;
    }
    if (changed) {
        static int lines = 0;
        if (++lines <= 30) {
            LOG_INFO("[coop] Other players' characters: %d kept asked for after the game let them go (across a map "
                     "boundary)", changed);
        }
    }
    return changed;
}

}  // namespace coop::visible
