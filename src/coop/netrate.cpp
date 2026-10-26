// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "netrate.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#include <cmath>
#include <cstdint>
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

// The send test before it (`cmp dword ptr [ebx+234h], 0`, then `jg` past the update). At most one update goes a frame,
// so a game drawing fewer frames a second than the rate (a slow PC, a long frame) sends every frame and, with the
// FPS Unlock's reload added on, the counter sinks further below zero each time (at 30 a second and 25 FPS it wrapped
// round after about two hours, and then no update went for a quarter of an hour). A game that sped up again sent
// every frame until it had made up the debt. The stub holds the counter at minus one reload, so a slow game simply
// sends every frame; a game as fast as the rate or faster never reaches it, and its leftover part of a frame carries
// on as before. (The game's own `mov` reload never goes below zero.)
constexpr uint32_t kSendTest = 0x00E1DDE4;
const uint8_t kSendTestCode[7] = {0x83, 0xBB, 0x34, 0x02, 0x00, 0x00, 0x00};

// The enemies' positions (WorldChrMan+0x2B4, the table 0xE563D0 builds; 0xE57850 per frame): a round of updates
// (0xE56BC0, packet type 3, 8 enemies a packet, one packet every other frame until the round is through) starts when
// +0x1C4 has counted down by the frame time, and +0x1C4 is reloaded from +0x1BC, 0.25 s (from the shared 0x12DF764).
// A round's first call is skipped when the last round's packet went the frame before (+0x1CC), so a round of up to 8
// enemies goes every other frame at best: 30 a second at 60 FPS, 15 at 30 FPS (a gap of 0.05 s: 15 to 20 at 60 FPS).
// The receiver (0xE556F0) keeps only the newest position, which the enemy's copy moves to. (An enemy's actions are
// messages of their own, sent the frame they start: 0xE56AB0, packet type 0x21.)
constexpr uint32_t kChrTable = 0x2B4;
constexpr uint32_t kRoundGap = 0x1BC;
constexpr float kVanillaRoundGap = 0.25f;

// Another player's character (NetworkManipulator update 0xE1F950, edi = manipulator) takes every update that came
// for it, one after another, in the same frame (pop 0xD30140, esi = the manipulator's sample: it copies the update
// in, nothing else). Each is decoded (0xE1EDC0, esi = manipulator) against the time since the one before (+0x2BC,
// zeroed after each), so the second of two that came in one frame has a gap of 0 and the character jumps to it. With
// smoothing on, the pop takes all that are waiting and keeps the newest: each update holds the whole state (position,
// facing, both animation layers with their time, HP), so nothing else is lost.
constexpr uint32_t kPopCall = 0x00E1F989;
constexpr uint32_t kPop = 0x00D30140;

// The decode moves the character toward the update at the speed that covers it in the gap x 1.2 (the double at
// 0x11E84F8), an enemy's (0xE21270, esi = NetAIManipulator; gap +0x288) in the gap x 1.1 (0x11E8500); xmm0 holds the
// gap as a double at these instructions. 20% (10%) of slack is a 33 ms margin at six updates a second, but a few ms
// at thirty, where an update a little late leaves the character standing at the last one until it comes. With
// smoothing on, the time is at least the gap plus NetworkSmoothing: a character is drawn about one update and that
// much behind, plus the ping.
constexpr uint32_t kPlayerCover = 0x00E1F3A1;  // mulsd xmm0, qword ptr [011E84F8h]
constexpr uint32_t kEnemyCover = 0x00E21663;   // mulsd xmm0, qword ptr [011E8500h]
const uint8_t kPlayerCoverCode[8] = {0xF2, 0x0F, 0x59, 0x05, 0xF8, 0x84, 0x1E, 0x01};
const uint8_t kEnemyCoverCode[8] = {0xF2, 0x0F, 0x59, 0x05, 0x00, 0x85, 0x1E, 0x01};

// The same decode sets the turn its character is given every 30 FPS frame (+0x290; the FPS Unlock scales it by the
// frame time) to a fifth of the angle left (the shared 0.2 at 0x1160764, loaded at these two places), which takes the
// 5 frames of a sixth of a second. The stubs below load a 30 FPS frame over the time the turn is to take instead: the
// gap the update took since the one before (+0x2BC) plus NetworkSmoothing, never less than the game's fifth and at
// most the whole turn in one frame.
constexpr uint32_t kTurnLoadXyz = 0x00E1F83C;  // movss xmm5, dword ptr [01160764h]
constexpr uint32_t kTurnLoadW = 0x00E1F8CB;    // movss xmm4, dword ptr [01160764h]
const uint8_t kTurnLoadXyzCode[8] = {0xF3, 0x0F, 0x10, 0x2D, 0x64, 0x07, 0x16, 0x01};
const uint8_t kTurnLoadWCode[8] = {0xF3, 0x0F, 0x10, 0x25, 0x64, 0x07, 0x16, 0x01};

// Both decodes jump the character to the update, instead of moving it there, when the distance over the gap since the
// update before is above a speed (or the gap is not between 0 and 2 s): 20 m/s for players (the double at 0x11E7E50),
// the enemy's own for enemies (NetAIManipulator +0x80). At the game's own rates the gap is a sixth of a second for
// players and about a quarter for enemies, so only a copy metres away jumps. At thirty a second one metre over a
// thirtieth reads as 30 m/s: a copy a frame or two behind (the smoothing keeps it NetworkSmoothing behind on purpose)
// jumped from update to update, and a fast one (the Asylum Demon's leap off its perch, the Taurus Demon's onto the
// bridge) at every update, in steps 30 times a second on a picture drawn 144 times, standing still between them, with
// its run left at a walk (2026-10-25: better at thirty than sixty, which halves the gap again; a good connection does
// not help, the ping is not in it). With smoothing on, the time in the test is at least the game's own gap, so a copy
// jumps only as far off as it would at the game's own rate. xmm1 = the distance and xmm0 = the gap (floats) there.
constexpr uint32_t kPlayerSnap = 0x00E1F370;  // cvtps2pd xmm1,xmm1 ; cvtps2pd xmm2,xmm0 ; divsd xmm1,xmm2
constexpr uint32_t kEnemySnap = 0x00E2162B;   // cvtps2pd xmm2,xmm0 ; cvtps2pd xmm1,xmm1 ; divsd xmm1,xmm2
const uint8_t kPlayerSnapCode[10] = {0x0F, 0x5A, 0xC9, 0x0F, 0x5A, 0xD0, 0xF2, 0x0F, 0x5E, 0xCA};
const uint8_t kEnemySnapCode[10] = {0x0F, 0x5A, 0xD0, 0x0F, 0x5A, 0xC9, 0xF2, 0x0F, 0x5E, 0xCA};
constexpr double kEnemyGap = 0.25;  // the enemies' round at the game's own rate (kVanillaRoundGap)

// Read by the stubs; all of it is the game's own behaviour at the game's own rate (g_frame 0 leaves the fifth,
// g_smooth 0 the gap x 1.2 / 1.1, g_drain 0 one update per pop, the snap floors 0 the gap itself). The game thread
// alone runs this code.
float g_frame = 0.0f;
float g_min_gap = 1.0f / 30.0f;
float g_game_turn = 0.2f;
float g_smooth_f = 0.0f;
double g_smooth = 0.0;
double g_player_slack = 1.2;
double g_enemy_slack = 1.1;
uint8_t g_drain = 0;
uint32_t g_pop = kPop;
float g_gap = 0.0f;   // scratch
double g_gap_d = 0.0;  // scratch
double g_slack_d = 0.0;  // scratch
int32_t g_send_floor = INT32_MIN;  // minus the send reload, once written (INT32_MIN: no floor)
double g_player_snap_floor = 0.0;
double g_enemy_snap_floor = 0.0;

// The send test: the counter is held at g_send_floor or above, then compared with 0 for the game's `jg`.
__declspec(naked) void send_test() {
    __asm {
        push eax
        mov eax, dword ptr [g_send_floor]
        cmp dword ptr [ebx + 234h], eax
        jge keep
        mov dword ptr [ebx + 234h], eax
    keep:
        pop eax
        cmp dword ptr [ebx + 234h], 0
        ret
    }
}

__declspec(naked) void turn_fraction_xmm5() {
    __asm {
        movss xmm5, dword ptr [esi + 2BCh]
        addss xmm5, dword ptr [g_smooth_f]
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
        addss xmm4, dword ptr [g_smooth_f]
        maxss xmm4, dword ptr [g_min_gap]
        movss dword ptr [g_gap], xmm4
        movss xmm4, dword ptr [g_frame]
        divss xmm4, dword ptr [g_gap]
        maxss xmm4, dword ptr [g_game_turn]
        ret
    }
}

// xmm0 = the gap (> 0, double) -> the time to cover the update: max(gap x slack, gap + smoothing).
__declspec(naked) void cover_player() {
    __asm {
        movsd qword ptr [g_gap_d], xmm0
        mulsd xmm0, qword ptr [g_player_slack]
        movsd qword ptr [g_slack_d], xmm0
        movsd xmm0, qword ptr [g_gap_d]
        addsd xmm0, qword ptr [g_smooth]
        maxsd xmm0, qword ptr [g_slack_d]
        ret
    }
}

__declspec(naked) void cover_enemy() {
    __asm {
        movsd qword ptr [g_gap_d], xmm0
        mulsd xmm0, qword ptr [g_enemy_slack]
        movsd qword ptr [g_slack_d], xmm0
        movsd xmm0, qword ptr [g_gap_d]
        addsd xmm0, qword ptr [g_smooth]
        maxsd xmm0, qword ptr [g_slack_d]
        ret
    }
}

// The jump tests: xmm1 = distance / max(gap, floor) as doubles; xmm2 is left the gap, as the game's code leaves it.
// A gap of 0 or below still jumps (distance / 0, or the game's own test after).
__declspec(naked) void snap_player() {
    __asm {
        cvtps2pd xmm1, xmm1
        cvtps2pd xmm2, xmm0
        maxsd xmm2, qword ptr [g_player_snap_floor]
        divsd xmm1, xmm2
        cvtps2pd xmm2, xmm0
        ret
    }
}

__declspec(naked) void snap_enemy() {
    __asm {
        cvtps2pd xmm1, xmm1
        cvtps2pd xmm2, xmm0
        maxsd xmm2, qword ptr [g_enemy_snap_floor]
        divsd xmm1, xmm2
        cvtps2pd xmm2, xmm0
        ret
    }
}

// esi = the sample; al = an update was taken. With g_drain, every waiting update is taken and the last one stays.
__declspec(naked) void pop_newest() {
    __asm {
        push ebx
        xor ebx, ebx
    again:
        call dword ptr [g_pop]
        test al, al
        jz done
        mov bl, 1
        cmp byte ptr [g_drain], 0
        jne again
    done:
        mov al, bl
        pop ebx
        ret
    }
}

bool patch_call(uint32_t at, const uint8_t* expect, size_t n, void* fn) {
    uint8_t code[16];
    std::memset(code, 0x90, sizeof(code));
    code[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(fn) - (at + 5));
    std::memcpy(code + 1, &rel, 4);
    return n >= 5 && n <= sizeof(code) && bytes_match(at, expect, n) && patch(at, expect, code, n);
}

bool g_turn = false;
bool g_newest = false;
bool g_cover = false;
bool g_snap = false;
bool g_floor = false;
int g_smooth_ms = 32;
int g_applied = 0;  // the rate the send reload holds (0: not yet written)
bool g_smoothing = false;
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
    if (static_cast<int32_t>(old) != value && !patch(kSendReload + 6, &old, &value, sizeof(value))) return false;
    g_send_floor = -value;
    return true;
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

// The receiving side follows the session's rate: the game's own at six a second, else newest updates, the turn in
// the update's time and NetworkSmoothing.
void set_receiving(bool on) {
    if (on == g_smoothing) return;
    g_smoothing = on;
    g_frame = on && g_turn ? 1.0f / 30.0f : 0.0f;
    g_drain = on && g_newest ? 1 : 0;
    g_smooth = on && g_cover ? g_smooth_ms / 1000.0 : 0.0;
    g_smooth_f = on && g_turn ? g_smooth_ms / 1000.0f : 0.0f;
    g_player_snap_floor = on && g_snap ? 1.0 / kVanilla : 0.0;
    g_enemy_snap_floor = on && g_snap ? kEnemyGap : 0.0;
}

}  // namespace

bool install() {
    g_turn = bytes_match(kTurnLoadW, kTurnLoadWCode, sizeof(kTurnLoadWCode)) &&
             patch_call(kTurnLoadXyz, kTurnLoadXyzCode, sizeof(kTurnLoadXyzCode),
                        reinterpret_cast<void*>(&turn_fraction_xmm5)) &&
             patch_call(kTurnLoadW, kTurnLoadWCode, sizeof(kTurnLoadWCode), reinterpret_cast<void*>(&turn_fraction_xmm4));
    g_newest = redirect_call(kPopCall, kPop, reinterpret_cast<void*>(&pop_newest));
    g_cover = bytes_match(kEnemyCover, kEnemyCoverCode, sizeof(kEnemyCoverCode)) &&
              patch_call(kPlayerCover, kPlayerCoverCode, sizeof(kPlayerCoverCode), reinterpret_cast<void*>(&cover_player)) &&
              patch_call(kEnemyCover, kEnemyCoverCode, sizeof(kEnemyCoverCode), reinterpret_cast<void*>(&cover_enemy));
    g_floor = patch_call(kSendTest, kSendTestCode, sizeof(kSendTestCode), reinterpret_cast<void*>(&send_test));
    g_snap = bytes_match(kEnemySnap, kEnemySnapCode, sizeof(kEnemySnapCode)) &&
             patch_call(kPlayerSnap, kPlayerSnapCode, sizeof(kPlayerSnapCode), reinterpret_cast<void*>(&snap_player)) &&
             patch_call(kEnemySnap, kEnemySnapCode, sizeof(kEnemySnapCode), reinterpret_cast<void*>(&snap_enemy));
    uint8_t op = 0;
    const bool send = read8(kSendReload, &op) && (op == kReloadMov || op == kReloadAdd) &&
                      bytes_match(kSendReload + 1, kReloadOperand, sizeof(kReloadOperand));
    if (!g_turn) LOG_ERROR("[coop] Network rate: the turn of other players' characters did not match this build");
    if (!g_newest) LOG_ERROR("[coop] Network rate: the other players' update queue did not match this build");
    if (!g_cover) LOG_ERROR("[coop] Network rate: the movement toward updates did not match this build (no smoothing)");
    if (!send) LOG_ERROR("[coop] Network rate: the player update's reload did not match this build");
    if (!g_floor) LOG_ERROR("[coop] Network rate: the player update's send test did not match this build");
    if (!g_snap) LOG_ERROR("[coop] Network rate: the jump test toward updates did not match this build");
    return g_turn && g_newest && g_cover && send && g_floor && g_snap;
}

void set_smoothing(int ms) {
    g_smooth_ms = ms < 0 ? 0 : ms > kMaxSmoothing ? kMaxSmoothing : ms;
}

void tick(int rate) {
    if (rate < kVanilla) rate = kVanilla;
    if (rate > kMax) rate = kMax;
    // The enemies' round, every frame: the table is made again with each map load.
    set_round_gap(kVanillaRoundGap * kVanilla / rate);
    set_receiving(rate > kVanilla);
    if (rate == g_applied || g_reload_bad) return;
    if (!write_reload(rate)) {
        g_reload_bad = true;
        LOG_ERROR("[coop] Network rate: the player update's reload changed under us; left as it is");
        return;
    }
    if (g_applied) {
        LOG_INFO("[coop] Network updates: %d a second (enemy rounds every %.3f s); smoothing %s", rate,
                 kVanillaRoundGap * kVanilla / rate, rate > kVanilla ? "on" : "off (the game's own)");
    }
    g_applied = rate;
}

}  // namespace coop::netrate
