// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "netrate.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#include <cmath>
#include <cstring>

namespace coop::netrate {

namespace {

// The player's character (PadManipulator 0xE1DC60, ebx = manipulator): [ebx+0x234] counts down and the update goes
// (packet type 1 through 0xD31B40) when it runs out, then it is reloaded at 0xE1DF7F. The game's reload is
// `mov dword ptr [ebx+234h], 5` (5 frames); the FPS Unlock's ghost fix makes it `add dword ptr [ebx+234h], 5 << 16`
// and counts 1/65536 of a 30 FPS frame, so the rate holds at any frame rate. Only the immediate is written here.
constexpr uint32_t kSendReload = 0x00E1DF7F;
constexpr uint8_t kReloadMov = 0xC7, kReloadAdd = 0x81;
const uint8_t kReloadOperand[5] = {0x83, 0x34, 0x02, 0x00, 0x00};  // [ebx+234h]
constexpr int32_t kUnit = 65536;

// The enemies' positions (WorldChrMan+0x2B4, the table 0xE563D0 builds; 0xE57850 per frame): a round of updates
// (0xE56BC0, packet type 3, 8 enemies a packet, one packet every other frame until the round is through) starts when
// +0x1C4 has counted down by the frame time, and +0x1C4 is reloaded from +0x1BC, 0.25 s (from the shared 0x12DF764).
// The receiver (0xE556F0) keeps only the newest position, which the enemy's copy moves to.
constexpr uint32_t kChrTable = 0x2B4;
constexpr uint32_t kRoundGap = 0x1BC;
constexpr float kVanillaRoundGap = 0.25f;

// Another player's character (the sample decode 0xE1EDC0, esi = NetworkManipulator): each update sets the turn its
// character is given every 30 FPS frame (+0x290; the FPS Unlock scales it by the frame time) to a fifth of the angle
// left (the shared 0.2 at 0x1160764, loaded at these two places), which takes the 5 frames of a sixth of a second.
// The stubs below load the time the update took since the one before (+0x2BC) as the fraction instead: 1/30 s over
// that gap, never less than the game's fifth and at most the whole turn in one frame.
constexpr uint32_t kTurnLoadXyz = 0x00E1F83C;  // movss xmm5, dword ptr [01160764h]
constexpr uint32_t kTurnLoadW = 0x00E1F8CB;    // movss xmm4, dword ptr [01160764h]
const uint8_t kTurnLoadXyzCode[8] = {0xF3, 0x0F, 0x10, 0x2D, 0x64, 0x07, 0x16, 0x01};
const uint8_t kTurnLoadWCode[8] = {0xF3, 0x0F, 0x10, 0x25, 0x64, 0x07, 0x16, 0x01};

// Read by the stubs. g_frame is 0 at the game's own rate, which leaves the game's fifth.
float g_frame = 0.0f;
float g_min_gap = 1.0f / 30.0f;
float g_game_turn = 0.2f;
float g_gap = 0.0f;  // scratch: the game thread alone runs the decode

__declspec(naked) void turn_fraction_xmm5() {
    __asm {
        movss xmm5, dword ptr [esi + 2BCh]
        maxss xmm5, dword ptr [g_min_gap]  // also 0 (two updates in one frame) and NaN
        movss dword ptr [g_gap], xmm5
        movss xmm5, dword ptr [g_frame]
        divss xmm5, dword ptr [g_gap]
        maxss xmm5, dword ptr [g_game_turn]
        ret
    }
}

__declspec(naked) void turn_fraction_xmm4() {
    __asm {
        movss xmm4, dword ptr [esi + 2BCh]
        maxss xmm4, dword ptr [g_min_gap]
        movss dword ptr [g_gap], xmm4
        movss xmm4, dword ptr [g_frame]
        divss xmm4, dword ptr [g_gap]
        maxss xmm4, dword ptr [g_game_turn]
        ret
    }
}

bool patch_call(uint32_t at, const uint8_t* expect, size_t n, void* fn) {
    uint8_t code[8];
    std::memset(code, 0x90, sizeof(code));
    code[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(fn) - (at + 5));
    std::memcpy(code + 1, &rel, 4);
    return n == sizeof(code) && patch(at, expect, code, n);
}

bool g_turn = false;
int g_applied = 0;  // the rate the send reload holds (0: not yet written)
bool g_reload_bad = false;

// The send reload for `rate`; false when the instruction is neither the game's nor the FPS Unlock's.
bool write_reload(int rate) {
    uint8_t op = 0;
    if (!read8(kSendReload, &op) || !bytes_match(kSendReload + 1, kReloadOperand, sizeof(kReloadOperand))) return false;
    int32_t value = 0;
    if (op == kReloadAdd) {
        value = static_cast<int32_t>(std::lround(30.0 * kUnit / rate));
    } else if (op == kReloadMov) {
        value = static_cast<int32_t>(std::lround(30.0 / rate));
        if (value < 1) value = 1;
    } else {
        return false;
    }
    uint32_t old = 0;
    if (!read32(kSendReload + 6, &old)) return false;
    return static_cast<int32_t>(old) == value || patch(kSendReload + 6, &old, &value, sizeof(value));
}

void set_round_gap(float gap) {
    uint32_t wcm = 0, table = 0, now = 0;
    if (!read32(game::kWorldChrManVar, &wcm) || !wcm || !read32(wcm + kChrTable, &table) || !table ||
        !read32(table + kRoundGap, &now)) {
        return;
    }
    float current = 0.0f;
    std::memcpy(&current, &now, sizeof(current));
    if (current != gap) *ptr<float>(table + kRoundGap) = gap;
}

}  // namespace

bool install() {
    g_turn = bytes_match(kTurnLoadXyz, kTurnLoadXyzCode, sizeof(kTurnLoadXyzCode)) &&
             bytes_match(kTurnLoadW, kTurnLoadWCode, sizeof(kTurnLoadWCode)) &&
             patch_call(kTurnLoadXyz, kTurnLoadXyzCode, sizeof(kTurnLoadXyzCode),
                        reinterpret_cast<void*>(&turn_fraction_xmm5)) &&
             patch_call(kTurnLoadW, kTurnLoadWCode, sizeof(kTurnLoadWCode), reinterpret_cast<void*>(&turn_fraction_xmm4));
    uint8_t op = 0;
    const bool send = read8(kSendReload, &op) && (op == kReloadMov || op == kReloadAdd) &&
                      bytes_match(kSendReload + 1, kReloadOperand, sizeof(kReloadOperand));
    if (!g_turn) LOG_ERROR("[coop] Network rate: the turn of other players' characters did not match this build");
    if (!send) LOG_ERROR("[coop] Network rate: the player update's reload did not match this build");
    return g_turn && send;
}

void tick(int rate) {
    if (rate < kVanilla) rate = kVanilla;
    if (rate > kMax) rate = kMax;
    // The enemies' round, every frame: the table is made again with each map load.
    set_round_gap(kVanillaRoundGap * kVanilla / rate);
    if (rate == g_applied || g_reload_bad) return;
    if (!write_reload(rate)) {
        g_reload_bad = true;
        LOG_ERROR("[coop] Network rate: the player update's reload changed under us; left as it is");
        return;
    }
    g_frame = g_turn && rate > kVanilla ? 1.0f / 30.0f : 0.0f;
    if (g_applied) {
        LOG_INFO("[coop] Network updates: %d a second (enemy rounds every %.3f s)", rate, kVanillaRoundGap * kVanilla / rate);
    }
    g_applied = rate;
}

}  // namespace coop::netrate
