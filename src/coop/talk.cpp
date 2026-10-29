// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "talk.h"

#include "game.h"
#include "hook.h"
#include "log.h"
#include "session.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <map>

namespace coop::talk {

namespace {

// ---------------------------------------------------------------- the game's talk player
// Every NPC with a talk script has a talker (0xA8 bytes, made per map by 0xD43400). [0x137D7C8] keeps groups (+0x14
// count, +0x18 array of 0x1C), each group its maps' talk managers (+8 count, +0xC array of 0x3C), each manager its
// NPCs (+0x18 count, +0x14 array of 0x2C: +0x10 the NPC's entity id, +0x14 its talker). The talker's player at +4
// ("TP" below): +0xC the entity id, +0x10 the TalkParam row of the line (-1 none), +0x14 the page of it on screen
// (row +0x10 + +0x14), +0x28/+0x2C a line paused part way, +0x44 the NPC's handle (-1: found by its entity id).
// TalkToPlayer (talk command 1:1, 0xFCEB24 -> 0xD46BC0, which follows the TalkParam chain) starts a line with
// 0xD455C0 (eax = row, stdcall(TP, a2, a3)); the TP's tick (0xD45430) turns the page with 0xD453D0 (esi = TP; false
// when there is none: the line ran out) once the voice ends or the player presses on; 0xD44410 (stdcall(TP)) stops
// a line that ran out (from 0xD453D0), was paused (TP +0x20) or was cut short.
constexpr uint32_t kTalkMan = 0x0137D7C8;
constexpr uint32_t kLineStart = 0x00D455C0;
constexpr uint32_t kPageNext = 0x00D453D0;
constexpr uint32_t kPageCall = 0x00D45521;  // the tick's call of kPageNext
constexpr uint32_t kLineStop = 0x00D44410;
constexpr uint32_t kRanOut = 0x00D453FF;  // kLineStop's return address when kPageNext found no page left
constexpr uint32_t kChrByEntity = 0x00D6C360;  // eax = entity id -> character, or 0
constexpr float kHearReach = 30.0f;  // another game's line is played for a player this near the NPC
constexpr int kLogLines = 60;

void (*g_listener)(int, int, int, int, bool) = nullptr;
void* g_start_orig = nullptr;
void* g_stop_orig = nullptr;
bool g_playing = false;  // our own call of the talk player: not told to the others
int g_lines = 0;

// Lines of ours the others were told of, by TP: the NPC and TalkToPlayer's other two arguments, for its pages.
struct Sent {
    int entity;
    int a2;
    int a3;
};
std::map<uint32_t, Sent> g_sent;
// Lines another game told us of, playing here, by TP: when they began.
std::map<uint32_t, double> g_heard;

double now() { return GetTickCount64() / 1000.0; }

int rd(uint32_t addr) {
    uint32_t v = 0;
    return read32(addr, &v) ? static_cast<int>(v) : -1;
}

// The TalkParam row on screen, or -1.
int row_of(uint32_t tp) {
    const int line = rd(tp + 0x10);
    if (line == -1) return -1;
    return line + std::max(0, rd(tp + 0x14));
}

uint32_t chr_by_entity(int entity) {
    uint32_t chr = 0;
    __try {
        __asm {
            mov eax, entity
            mov edx, kChrByEntity
            call edx
            mov chr, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        chr = 0;
    }
    return chr;
}

// An NPC's character in our game (the entities 10000-14095 are the players').
bool npc(int entity) { return entity > 0 && (entity < 10000 || entity > 14095) && chr_by_entity(entity) != 0; }

uint32_t find_tp(int entity) {
    uint32_t man = 0, groups = 0, group_n = 0;
    if (!read32(kTalkMan, &man) || !man || !read32(man + 0x14, &group_n) || !read32(man + 0x18, &groups) || !groups) {
        return 0;
    }
    for (uint32_t g = 0; g < group_n && g < 64; ++g) {
        uint32_t maps = 0, map_n = 0;
        const uint32_t group = groups + g * 0x1C;
        if (!read32(group + 8, &map_n) || !read32(group + 0xC, &maps) || !maps) continue;
        for (uint32_t m = 0; m < map_n && m < 64; ++m) {
            uint32_t entries = 0, entry_n = 0;
            const uint32_t mgr = maps + m * 0x3C;
            if (!read32(mgr + 0x18, &entry_n) || !read32(mgr + 0x14, &entries) || !entries) continue;
            for (uint32_t e = 0; e < entry_n && e < 1024; ++e) {
                const uint32_t entry = entries + e * 0x2C;
                uint32_t talker = 0;
                if (rd(entry + 0x10) == entity && read32(entry + 0x14, &talker) && talker) return talker + 4;
            }
        }
    }
    return 0;
}

// The line starts on our copy of the NPC. Without its handle the start turns no character towards our player:
// the NPC keeps facing whoever it talks to in the other game.
bool play(uint32_t tp, int row, int a2, int a3) {
    uint32_t handle = 0;
    if (!read32(tp + 0x44, &handle)) return false;
    bool ok = true;
    g_playing = true;
    *ptr<uint32_t>(tp + 0x44) = 0xFFFFFFFF;
    __try {
        __asm {
            push a3
            push a2
            push tp
            mov eax, row
            mov ecx, kLineStart
            call ecx
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    *ptr<uint32_t>(tp + 0x44) = handle;
    g_playing = false;
    return ok;
}

void stop(uint32_t tp) {
    g_playing = true;
    __try {
        __asm {
            push tp
            mov eax, kLineStop
            call eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_playing = false;
}

// ---------------------------------------------------------------- our game's lines
// True: the line is not started (the NPC says it already, from another game).
bool __stdcall line_started(int row, uint32_t tp, int a2, int a3) {
    if (g_playing) return false;
    g_sent.erase(tp);  // a talker of a map unloaded since can have the same address
    const auto heard = g_heard.find(tp);
    if (heard != g_heard.end()) {
        // Our own game makes the NPC speak while it says another game's line: ours goes on. Each game's script can
        // react to the same blow or death; the line it says already is not started again.
        const bool same = row_of(tp) == row && now() - heard->second < 10.0;
        g_heard.erase(heard);
        if (same) return true;
    }
    if (!g_listener || !session::active()) return false;
    const int entity = rd(tp + 0xC);
    if (!npc(entity)) return false;
    // A paused line goes on from its page (0xD455C0's own test).
    const int from = rd(tp + 0x28) == row ? row + std::max(0, rd(tp + 0x2C)) : row;
    g_sent[tp] = Sent{entity, a2, a3};
    if (++g_lines <= kLogLines) LOG_INFO("[coop] NPC %d says line %d here: told the others", entity, from);
    g_listener(entity, from, a2, a3, false);
    return false;
}

void __stdcall page_turned(uint32_t tp, int more) {
    if (g_playing || !more || !g_listener || g_heard.count(tp)) return;
    const auto s = g_sent.find(tp);
    if (s == g_sent.end() || !session::active()) return;
    g_listener(s->second.entity, row_of(tp), s->second.a2, s->second.a3, false);
}

void __stdcall line_stopped(uint32_t ret, uint32_t tp) {
    if (g_playing) return;
    g_heard.erase(tp);
    const auto s = g_sent.find(tp);
    if (s == g_sent.end()) return;
    const int entity = s->second.entity;
    const int row = row_of(tp);
    g_sent.erase(s);
    // A line that ran out runs out in the other games too.
    if (ret == kRanOut || !g_listener || !session::active()) return;
    g_listener(entity, row, 0, 0, true);
}

__declspec(naked) void hk_line_start() {
    __asm {
        pushad
        push dword ptr [esp + 44]  // a3
        push dword ptr [esp + 44]  // a2
        push dword ptr [esp + 44]  // TP
        push eax                   // row
        call line_started
        test al, al
        popad
        jnz said
        jmp g_start_orig
    said:
        ret 0xC
    }
}

__declspec(naked) void hk_page_next() {
    __asm {
        mov eax, kPageNext
        call eax
        pushad
        movzx eax, al
        push eax
        push esi                   // TP
        call page_turned
        popad
        ret
    }
}

__declspec(naked) void hk_line_stop() {
    __asm {
        pushad
        push dword ptr [esp + 36]  // TP
        push dword ptr [esp + 36]  // return address
        call line_stopped
        popad
        jmp g_stop_orig
    }
}

}  // namespace

bool install() {
    static const uint8_t start_k[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};  // push ebp ; mov ebp,esp ; and esp,-8
    static const uint8_t stop_k[5] = {0xA1, 0xC8, 0xD7, 0x37, 0x01};          // mov eax,[0137D7C8h]
    g_start_orig = detour(kLineStart, start_k, sizeof(start_k), reinterpret_cast<void*>(&hk_line_start));
    g_stop_orig = detour(kLineStop, stop_k, sizeof(stop_k), reinterpret_cast<void*>(&hk_line_stop));
    const bool page = redirect_call(kPageCall, kPageNext, reinterpret_cast<void*>(&hk_page_next));
    if (!g_start_orig || !g_stop_orig || !page) {
        LOG_ERROR("[coop] NPC voice lines: the talk player did not match this build (lines stay with their speaker)");
        return false;
    }
    LOG_INFO("[coop] NPC voice lines are heard by every player near the NPC");
    return true;
}

void set_listener(void (*fn)(int, int, int, int, bool)) { g_listener = fn; }

void remote(int entity, int row, int a2, int a3, bool end) {
    if (game::loading() || !game::player()) return;
    const uint32_t chr = chr_by_entity(entity);
    const uint32_t tp = chr ? find_tp(entity) : 0;
    if (!tp) return;
    const auto heard = g_heard.find(tp);
    if (end) {
        if (heard != g_heard.end()) {
            if (rd(tp + 0x10) != -1) stop(tp);
            g_heard.erase(heard);
        }
        return;
    }
    game::Vec3 me{}, at{};
    if (!game::position(&me, nullptr) || !game::chr_position(chr, &at)) return;
    const float dx = me.x - at.x, dy = me.y - at.y, dz = me.z - at.z;
    if (dx * dx + dy * dy + dz * dz > kHearReach * kHearReach) return;
    if (rd(tp + 0x10) != -1) {
        if (heard == g_heard.end()) return;  // the NPC says a line of our own game's: it is not cut short
        if (row_of(tp) == row) return;      // on that page already
        stop(tp);
        g_heard.erase(tp);
    }
    if (!play(tp, row, a2, a3)) return;
    g_heard[tp] = now();
    if (++g_lines <= kLogLines) LOG_INFO("[coop] NPC %d says line %d from another game here", entity, row);
}

}  // namespace coop::talk
