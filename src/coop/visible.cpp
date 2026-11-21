// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "visible.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vector>

namespace coop::visible {

namespace {

// 0xE6AA30 (a character's model update): movq xmm0,[esi+264h] / [esi+26Ch] onto 16 bytes of stack, eax = the model
// (chr+0x24), then this call: the model takes those as its display groups.
constexpr uint32_t kModelGroupsCall = 0x00E6AAF3;
constexpr uint32_t kModelGroups = 0x00468DF0;

constexpr int kMaxOthers = 8;
uint32_t g_others[kMaxOthers] = {};  // the other players' characters this frame
volatile int g_count = 0;
uint32_t g_model_groups = kModelGroups;
bool g_installed = false;

// In place of the call: for one of the other players' characters (esi) the groups on the stack become every group.
__declspec(naked) void model_groups_stub() {
    __asm {
        push ecx
        xor ecx, ecx
    next:
        cmp ecx, dword ptr [g_count]
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
    g_count = n;
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
