// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "motion.h"

#include "game.h"
#include "hook.h"
#include "log.h"
#include "players.h"
#include "rules.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstring>

namespace coop::motion {

namespace {

// ---------------------------------------------------------------- gait
// NetworkManipulator move (0xE1E6A0): the gait class is in al at 0xE1E935, before the 0.2 s hysteresis
// (+0x2C0 timer, +0x2C4 current class). The stub keeps the replaced load of the timer and swaps al for the
// sender's gait when the character moves at all (al != 0) and that player reported one. The class becomes the
// stick the character is given (0.5 walk, 1.0 run) and, for a sprint, the dash flag +0x1E4: the copy is driven as if
// its player held the stick and the dash button. Invaders and the players they invade report theirs too (1.4.0; until
// then invasions were left to the guess). With no gait reported (a player of an older version, or a report gone stale)
// the game's own guess decides: its run from 2.6 m/s instead of the game's 2 (kRunSite), its sprint from Remastered's 4.5
// m/s instead of the game's 6. A player walks at up to 2.2 m/s, jogs at 4.0 and sprints at 5.15 (the 2026-11-02
// evening test, where the speeds over 6 were rolls), so at 6 no sprint would ever show. (The afternoon's build tried
// 6, from a jog thought to reach 5.3 that was the end of a sprint.)
constexpr uint32_t kGaitSite = 0x00E1E935;    // movss xmm0, dword ptr [ecx+2C0h]       (8 bytes)
constexpr uint32_t kSprintSite = 0x00E1E924;  // movss xmm2, dword ptr [011603ACh] (6.0) (8 bytes)
// The guess's run line, 2 m/s (the shared 2.0 at 0x115F588, read here only for this): a walk goes up to 2.2 and showed
// as a run; ours is 2.6, between a walk and a heavy jog's 3.0, as for our own gait (kRunFrom).
constexpr uint32_t kRunSite = 0x00E1E913;     // movss xmm2, dword ptr [0115F588h] (2.0) (8 bytes)
constexpr uint32_t kNetworkManipulatorVtable = 0x0118DDEC;

struct GaitEntry {
    uint32_t manipulator;
    int32_t gait;  // -1 none
};
// One for each other player (players::kMaxPlayers - 1); gait_stub's loop counts to the same.
constexpr int kGaitEntries = players::kMaxPlayers - 1;
static_assert(kGaitEntries == 7, "gait_stub compares with 7");
GaitEntry g_gaits[kGaitEntries] = {{0, -1}, {0, -1}, {0, -1}, {0, -1}, {0, -1}, {0, -1}, {0, -1}};
alignas(4) float g_sprint_speed = 4.5f;  // Remastered's fallback threshold (PTDE 6.0)
alignas(4) float g_run_speed = 2.6f;     // (PTDE 2.0)

__declspec(naked) void gait_stub() {
    __asm {
        movss xmm0, dword ptr [ecx + 0x2C0]
        test al, al
        jz done
        push edx
        push esi
        xor esi, esi
    next_entry:
        cmp ecx, dword ptr [g_gaits + esi * 8]
        jne skip
        mov edx, dword ptr [g_gaits + esi * 8 + 4]
        test edx, edx
        js gait_done
        mov al, dl
        jmp gait_done
    skip:
        inc esi
        cmp esi, 7
        jb next_entry
    gait_done:
        pop esi
        pop edx
    done:
        ret
    }
}

// Our own gait: the horizontal speed (smoothed over about 0.12 s), and whether the character really sprints. The
// PadManipulator's dash flag (+0x1E4, set at 0xE17FE3) only says the button has been held a moment: with no stamina
// left the character jogs on with it held, and once the button was held again during the regeneration it jogged until
// there was stamina. Each such jog went out as a sprint (dash and over 3.2 m/s), and the other games, which drive the
// copy with the dash flag, played the sprint at a jog's speed until the button was let go (2026-11-02). The character
// itself knows: its walk animation controller ([ChrCtrl+0x164], WalkAnim_Twist for a player) answers vtable +0x20
// (0xE0DC40: the walk state is the dash state), which the stamina drain asks every frame (0xE80AA8) before it takes
// a point off. The walk and run come from the speed: a walk goes up to 2.2 m/s and a jog runs at 4.0. The line was
// 2.2, the top of a walk, and a full walk switched between walking and running on the other screens several times a
// second (2026-11-02 evening); then 3 m/s, but a heavy player's jog (over half the equip load, the fat roll) runs at
// 3.0 m/s, right on it, and went walk, run, walk every 0.2 to 0.7 s: on the other screens their character walked at
// half the stick part of the time ("half slow-walking, half walking", 2026-11-06). It is 2.6 m/s now, with a margin
// (a run stays a run down to 2.4), between a walk's 2.2 and a heavy jog's 3.0.
constexpr uint32_t kWalkAnimTwistDash = 0x00E0DC40;
constexpr double kRunFrom = 2.6;
constexpr double kRunUntil = 2.4;
LARGE_INTEGER g_freq{};
LARGE_INTEGER g_prev_t{};
game::Vec3 g_prev_pos{};
bool g_have_prev = false;
double g_speed = 0;
Gait g_local = kStand;

// 1 sprinting, 0 not, -1 the test is not there (another walk controller): then the dash flag and the speed decide.
int sprinting(uint32_t chr) {
    uint32_t ctrl = 0, walk = 0, vt = 0, fn = 0;
    if (!read32(chr + 0x28, &ctrl) || !ctrl || !read32(ctrl + 0x164, &walk) || !walk || !read32(walk, &vt) || !vt ||
        !read32(vt + 0x20, &fn) || fn != kWalkAnimTwistDash) {
        return -1;
    }
    uint32_t r = 0;
    __try {
        __asm {
            mov ecx, walk
            mov eax, kWalkAnimTwistDash
            call eax
            movzx eax, al
            mov r, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return r ? 1 : 0;
}

// The speeds each gait of ours reached, for the log (what the game's own guess on the other side sees): the fastest
// (a roll counts as a run) and the time-weighted mean.
struct GaitSpeeds {
    double peak[4] = {};
    double sum[4] = {};   // speed x time
    double time[4] = {};  // time in each gait
    double since = 0;
    double last = 0;
};
GaitSpeeds g_gait_speeds;

// ---------------------------------------------------------------- event animations
// ChrIns event animation: 0xE69590 starts one (esi = character, edi = animation, stdcall(loop)); it asks the
// controller for the two animation slots, keeps their handles at +0xFC/+0x104 and the request at [+0x10]+4
// (animation) and +8 (loop). 0xE69470 stops it (edi = character): both slots are cancelled, the handles and
// the request are cleared and the animation state goes back to normal.
constexpr uint32_t kEventAnimStart = 0x00E69590;
constexpr uint32_t kEventAnimStop = 0x00E69470;
// Remastered Seamless Co-op's list: kindle, first light, sit down / sitting loop / stand up (three sets),
// bonfire warp, and the ceremonies of the bonfire menu.
constexpr int kSyncedAnims[] = {7697, 7698, 7699, 7700, 7701, 7702, 7710, 7711, 7712, 7720,
                                7721, 7722, 7725, 7895, 7896, 7897, 7898, 7905, 7913};
void* g_anim_start_orig = nullptr;
void* g_anim_stop_orig = nullptr;
bool g_replaying = false;
std::vector<Anim> g_pending;
bool g_enabled = true;      // [SeamlessCoop] BonfireAnimations
// The Death Knell's arrivals (knell.cpp): the living player's rise (6950), the summoned phantom's (6951) and the
// Warrior of Sunlight's (6952, "Praise the Sun"). Passed on only when the Knell starts one (send_knell_arrival). And
// the price's stagger, and the ring's animation played again for the choice (6060, with its rings).
constexpr int kKnellAnims[] = {6950, 6951, 6952, 2010, 6060};
int g_knell_send = 0;
ULONGLONG g_knell_send_at = 0;

bool knell_anim(int anim) {
    for (int a : kKnellAnims) {
        if (a == anim) return true;
    }
    return false;
}
bool g_local_open = false;  // a synced animation of ours was passed on and not stopped since
int g_local_loop = 0;       // the synced animation we are looping (0 none)

// The game may start an animation from its loading thread (a character being rebuilt), so the list is guarded.
SRWLOCK g_pending_lock = SRWLOCK_INIT;

void queue(const Anim& a) {
    AcquireSRWLockExclusive(&g_pending_lock);
    if (g_pending.size() < 32) g_pending.push_back(a);
    ReleaseSRWLockExclusive(&g_pending_lock);
}

void __stdcall note_anim_start(uint32_t chr, int anim, uint32_t loop) {
    if (chr && !g_replaying && chr != game::player()) rules::note_event_anim(chr, anim, (loop & 0xFF) != 0);
    if (g_replaying || !chr || chr != game::player()) return;
    const bool looped = (loop & 0xFF) != 0;
    // The Death Knell's arrival and its price are the knell's, not a bonfire's: shown to the others whatever
    // BonfireAnimations says.
    if (anim == g_knell_send && knell_anim(anim) && GetTickCount64() - g_knell_send_at < 2000) {
        g_knell_send = 0;
        queue(Anim{0, anim, static_cast<uint8_t>(looped ? 1 : 0)});
        return;
    }
    if (!g_enabled) return;
    if (!synced_anim(anim)) {
        // Something else replaces our ceremony: the others let go of it too.
        if (g_local_open) queue(Anim{1, 0, 0});
        g_local_open = false;
        g_local_loop = 0;
        return;
    }
    g_local_open = true;
    g_local_loop = looped ? anim : 0;
    queue(Anim{0, anim, static_cast<uint8_t>(looped ? 1 : 0)});
}

void __stdcall note_anim_stop(uint32_t chr) {
    if (g_replaying || !g_enabled || !chr || chr != game::player()) return;
    g_local_loop = 0;
    if (!g_local_open) return;
    g_local_open = false;
    queue(Anim{1, 0, 0});
}

// The game's own callers pass arguments in registers here (link-time code generation), so nothing is assumed
// about which registers they keep across the call: the general and the SSE registers are all put back.
__declspec(naked) void hk_anim_start() {
    __asm {
        pushad
        sub esp, 0x80
        movdqu xmmword ptr [esp], xmm0
        movdqu xmmword ptr [esp + 0x10], xmm1
        movdqu xmmword ptr [esp + 0x20], xmm2
        movdqu xmmword ptr [esp + 0x30], xmm3
        movdqu xmmword ptr [esp + 0x40], xmm4
        movdqu xmmword ptr [esp + 0x50], xmm5
        movdqu xmmword ptr [esp + 0x60], xmm6
        movdqu xmmword ptr [esp + 0x70], xmm7
        push dword ptr [esp + 0xA4]  // loop: [esp+4] at entry; pushad added 0x20, the SSE block 0x80
        push edi
        push esi
        call note_anim_start
        movdqu xmm0, xmmword ptr [esp]
        movdqu xmm1, xmmword ptr [esp + 0x10]
        movdqu xmm2, xmmword ptr [esp + 0x20]
        movdqu xmm3, xmmword ptr [esp + 0x30]
        movdqu xmm4, xmmword ptr [esp + 0x40]
        movdqu xmm5, xmmword ptr [esp + 0x50]
        movdqu xmm6, xmmword ptr [esp + 0x60]
        movdqu xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        popad
        jmp g_anim_start_orig
    }
}

__declspec(naked) void hk_anim_stop() {
    __asm {
        pushad
        sub esp, 0x80
        movdqu xmmword ptr [esp], xmm0
        movdqu xmmword ptr [esp + 0x10], xmm1
        movdqu xmmword ptr [esp + 0x20], xmm2
        movdqu xmmword ptr [esp + 0x30], xmm3
        movdqu xmmword ptr [esp + 0x40], xmm4
        movdqu xmmword ptr [esp + 0x50], xmm5
        movdqu xmmword ptr [esp + 0x60], xmm6
        movdqu xmmword ptr [esp + 0x70], xmm7
        push edi
        call note_anim_stop
        movdqu xmm0, xmmword ptr [esp]
        movdqu xmm1, xmmword ptr [esp + 0x10]
        movdqu xmm2, xmmword ptr [esp + 0x20]
        movdqu xmm3, xmmword ptr [esp + 0x30]
        movdqu xmm4, xmmword ptr [esp + 0x40]
        movdqu xmm5, xmmword ptr [esp + 0x50]
        movdqu xmm6, xmmword ptr [esp + 0x60]
        movdqu xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        popad
        jmp g_anim_stop_orig
    }
}

}  // namespace

bool synced_anim(int anim) {
    for (int a : kSyncedAnims) {
        if (a == anim) return true;
    }
    return false;
}

bool install() {
    int failed = 0;
    {
        static const uint8_t k[8] = {0xF3, 0x0F, 0x10, 0x81, 0xC0, 0x02, 0x00, 0x00};
        uint8_t code[8] = {0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&gait_stub) - (kGaitSite + 5));
        std::memcpy(code + 1, &rel, 4);
        if (!patch(kGaitSite, k, code, sizeof(code))) ++failed;
    }
    {
        static const uint8_t k[8] = {0xF3, 0x0F, 0x10, 0x15, 0xAC, 0x03, 0x16, 0x01};
        uint8_t code[8];
        std::memcpy(code, k, 4);
        const uint32_t operand = reinterpret_cast<uint32_t>(&g_sprint_speed);
        std::memcpy(code + 4, &operand, 4);
        if (!patch(kSprintSite, k, code, sizeof(code))) ++failed;
    }
    {
        static const uint8_t k[8] = {0xF3, 0x0F, 0x10, 0x15, 0x88, 0xF5, 0x15, 0x01};
        uint8_t code[8];
        std::memcpy(code, k, 4);
        const uint32_t operand = reinterpret_cast<uint32_t>(&g_run_speed);
        std::memcpy(code + 4, &operand, 4);
        if (!patch(kRunSite, k, code, sizeof(code))) ++failed;
    }
    {
        // mov ecx,[esi+28h] ; fld dword ptr [011E7CA0h]
        static const uint8_t k[9] = {0x8B, 0x4E, 0x28, 0xD9, 0x05, 0xA0, 0x7C, 0x1E, 0x01};
        g_anim_start_orig = detour(kEventAnimStart, k, sizeof(k), reinterpret_cast<void*>(&hk_anim_start));
        // mov ecx,[edi+28h] ; fldz
        static const uint8_t k2[5] = {0x8B, 0x4F, 0x28, 0xD9, 0xEE};
        g_anim_stop_orig = detour(kEventAnimStop, k2, sizeof(k2), reinterpret_cast<void*>(&hk_anim_stop));
        if (!g_anim_start_orig || !g_anim_stop_orig) ++failed;
    }
    QueryPerformanceFrequency(&g_freq);
    if (failed) LOG_ERROR("[coop] %d motion sync patch(es) did not match this build", failed);
    return failed == 0;
}

Gait local_gait() {
    const uint32_t me = game::player();
    game::Vec3 pos{};
    if (!me || game::loading() || !game::position(&pos, nullptr)) {
        g_have_prev = false;
        g_speed = 0;
        // No character: whatever ceremony it was in is over.
        g_local_open = false;
        g_local_loop = 0;
        return g_local = kStand;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (g_have_prev) {
        const double dt = static_cast<double>(t.QuadPart - g_prev_t.QuadPart) / static_cast<double>(g_freq.QuadPart);
        if (dt > 0.0005 && dt < 0.5) {
            const double dx = pos.x - g_prev_pos.x, dz = pos.z - g_prev_pos.z;
            const double v = std::sqrt(dx * dx + dz * dz) / dt;
            if (v < 30.0) g_speed += (v - g_speed) * (1.0 - std::exp(-dt / 0.12));  // larger: a warp
        }
    }
    g_prev_t = t;
    g_prev_pos = pos;
    g_have_prev = true;
    uint8_t dash = 0;
    if (const uint32_t pad = game::manipulator(me)) read8(pad + 0x1E4, &dash);
    const int real = sprinting(me);
    Gait g = kWalk;
    if (g_speed < 0.3) {
        g = kStand;
    } else if (real == 1 ? g_speed > kRunFrom : real < 0 && dash && g_speed > 4.5) {
        g = kSprint;
    } else if (g_speed > (g_local >= kRun ? kRunUntil : kRunFrom)) {
        g = kRun;
    }
    if (g != g_local) {
        static int lines = 0;
        if (++lines <= 120) {
            LOG_INFO("[coop] gait me %d (%.2f m/s, dash button %d, sprinting %s)", g, g_speed, dash,
                     real == 1 ? "yes" : real == 0 ? "no" : "unknown");
        }
    }
    // Every 60 s of moving: the mean and the fastest of each gait (without our report, the other side's guess runs
    // from 3 m/s and sprints from 4.5).
    const double now_s = static_cast<double>(t.QuadPart) / static_cast<double>(g_freq.QuadPart);
    if (g_gait_speeds.since == 0) g_gait_speeds.since = g_gait_speeds.last = now_s;
    if (g > kStand && g_speed > g_gait_speeds.peak[g]) g_gait_speeds.peak[g] = g_speed;
    if (const double step = now_s - g_gait_speeds.last; g > kStand && step > 0 && step < 0.5) {
        g_gait_speeds.sum[g] += g_speed * step;
        g_gait_speeds.time[g] += step;
    }
    g_gait_speeds.last = now_s;
    if (now_s - g_gait_speeds.since >= 60.0) {
        static int lines = 0;
        const GaitSpeeds& s = g_gait_speeds;
        auto mean = [&s](int i) { return s.time[i] > 0 ? s.sum[i] / s.time[i] : 0.0; };
        if ((s.peak[kWalk] > 0 || s.peak[kRun] > 0 || s.peak[kSprint] > 0) && ++lines <= 30) {
            LOG_INFO("[coop] Our speeds, last 60 s (mean / fastest): walk %.2f / %.2f m/s over %.0f s, run %.2f / %.2f "
                     "over %.0f s, sprint %.2f / %.2f over %.0f s", mean(kWalk), s.peak[kWalk], s.time[kWalk],
                     mean(kRun), s.peak[kRun], s.time[kRun], mean(kSprint), s.peak[kSprint], s.time[kSprint]);
        }
        g_gait_speeds = GaitSpeeds{};
        g_gait_speeds.since = g_gait_speeds.last = now_s;
    }
    return g_local = g;
}

std::vector<Anim> take_local_anims() {
    std::vector<Anim> out;
    AcquireSRWLockExclusive(&g_pending_lock);
    out.swap(g_pending);
    ReleaseSRWLockExclusive(&g_pending_lock);
    return out;
}

void set_remote_gait(uint32_t chr, Gait gait) {
    const uint32_t m = game::manipulator(chr);
    uint32_t vt = 0;
    if (!m || !read32(m, &vt) || vt != kNetworkManipulatorVtable) return;
    int free_slot = -1;
    for (int i = 0; i < kGaitEntries; ++i) {
        if (g_gaits[i].manipulator == m) {
            g_gaits[i].gait = gait;
            return;
        }
        if (free_slot < 0 && g_gaits[i].manipulator == 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_gaits[free_slot].gait = gait;
        g_gaits[free_slot].manipulator = m;
    }
}

void clear_remote_gaits() {
    for (auto& e : g_gaits) {
        e.manipulator = 0;
        e.gait = -1;
    }
}

int local_loop_anim() { return g_enabled ? g_local_loop : 0; }

bool knell_arrival_anim(int anim) { return knell_anim(anim); }

double knell_anim_seconds(int anim) {
    switch (anim) {
        case 6951: return 4.55;
        case 6952: return 5.4;
        case 6950: return 2.15;
        default: return 1.5;  // 2010, the price's stagger
    }
}

void send_knell_arrival(int anim) {
    g_knell_send = anim;
    g_knell_send_at = GetTickCount64();
}

void send_knell_hold(bool on) { queue(Anim{static_cast<uint8_t>(on ? kHold : kRelease), 0, 0}); }

// The speed a character's animation set (ChrCtrl+0x14) runs at: its update (0xD98A50, from the ChrCtrl update
// 0xE368C0) multiplies the frame's time by +0x64 when that is not 1, else by +0x48 (both 1 when it is made,
// 0xD98FC0), before it moves each animation's time on (the 0x60-byte records from +4, their time at +0x14; the TAE
// events and the Havok controls follow that time). With +0x64 at 0 the pose, the TAE and the action they drive stand
// still; the character is otherwise as it was (hits land, its physics runs). The value before is put back.
constexpr uint32_t kChrCtrl = 0x28, kCtrlAnimSet = 0x14, kSetSpeedOverride = 0x64;
struct Held {
    uint32_t chr;
    float was;
};
std::vector<Held> g_held;

bool write_float(uint32_t addr, float v) {
    __try {
        *ptr<float>(addr) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool hold_anims(uint32_t chr, bool on) {
    uint32_t ctrl = 0, set = 0, bits = 0;
    if (!chr || !read32(chr + kChrCtrl, &ctrl) || ctrl < 0x10000 || !read32(ctrl + kCtrlAnimSet, &set) ||
        set < 0x10000 || !read32(set + kSetSpeedOverride, &bits)) {
        return false;
    }
    float speed = 0.0f;
    std::memcpy(&speed, &bits, 4);
    size_t at = g_held.size();
    for (size_t i = 0; i < g_held.size(); ++i) {
        if (g_held[i].chr == chr) at = i;
    }
    if (on) {
        if (at == g_held.size()) g_held.push_back(Held{chr, speed > 0.0f ? speed : 1.0f});
        return write_float(set + kSetSpeedOverride, 0.0f);
    }
    const float was = at < g_held.size() ? g_held[at].was : 1.0f;
    if (at < g_held.size()) g_held.erase(g_held.begin() + static_cast<std::ptrdiff_t>(at));
    return write_float(set + kSetSpeedOverride, was);
}

bool anims_held(uint32_t chr) {
    uint32_t ctrl = 0, set = 0, speed = 0;
    return chr && read32(chr + kChrCtrl, &ctrl) && ctrl >= 0x10000 && read32(ctrl + kCtrlAnimSet, &set) &&
           set >= 0x10000 && read32(set + kSetSpeedOverride, &speed) && speed == 0;
}

void forget_held() { g_held.clear(); }

void set_bonfire_animations(bool on) { g_enabled = on; }

bool play_remote_anim(uint32_t chr, const Anim& a) {
    // A stop is always carried out: it only ever puts a character back to normal.
    if (!chr || chr == game::player() || (!g_enabled && a.kind == 0 && !knell_anim(a.anim))) return false;
    if (a.kind == 0 && !synced_anim(a.anim) && !knell_anim(a.anim)) return false;
    // The functions need the character's controller and its animation object.
    uint32_t ctrl = 0, request = 0, hp = 0;
    if (!read32(chr + 0x28, &ctrl) || ctrl < 0x10000 || !read32(chr + 0x10, &request) || request < 0x10000 ||
        !read32(chr + 0x2D4, &hp) || static_cast<int>(hp) <= 0) {
        return false;
    }
    const int anim = a.anim;
    const uint32_t looped = a.loop ? 1 : 0;
    const uint32_t start = reinterpret_cast<uint32_t>(g_anim_start_orig);
    const uint32_t stop = reinterpret_cast<uint32_t>(g_anim_stop_orig);
    if (!start || !stop) return false;
    bool done = true;
    g_replaying = true;
    __try {
        if (a.kind == 0) {
            __asm {
                push esi
                push edi
                mov esi, chr
                mov edi, anim
                push looped
                mov eax, start
                call eax
                pop edi
                pop esi
            }
        } else {
            __asm {
                push edi
                mov edi, chr
                mov eax, stop
                call eax
                pop edi
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[coop] Event animation %d on another player's character failed", anim);
        done = false;
    }
    g_replaying = false;
    return done;
}

int held_event_anim(uint32_t chr) {
    uint32_t request = 0, anim = 0, h0 = 0, h1 = 0, none0 = 0, none1 = 0;
    if (!chr || !read32(chr + 0xFC, &h0) || !read32(chr + 0x100, &h1) || !read32(0x012E0C64, &none0) ||
        !read32(0x012E0C68, &none1) || (h0 == none0 && h1 == none1)) {
        return 0;
    }
    if (!read32(chr + 0x10, &request) || request < 0x10000 || !read32(request + 4, &anim)) return -1;
    const int id = static_cast<int>(anim);
    return id > 0 ? id : -1;
}

int remote_open_anim(uint32_t chr) {
    // The slot handle at +0xFC/+0x100 is the pair at 0x12E0C64/0x12E0C68 when there is none.
    uint32_t request = 0, anim = 0, h0 = 0, h1 = 0, none0 = 0, none1 = 0;
    if (!chr || !read32(chr + 0xFC, &h0) || !read32(chr + 0x100, &h1) || !read32(0x012E0C64, &none0) ||
        !read32(0x012E0C68, &none1) || (h0 == none0 && h1 == none1)) {
        return 0;
    }
    if (!read32(chr + 0x10, &request) || request < 0x10000 || !read32(request + 4, &anim)) return 0;
    const int id = static_cast<int>(anim);
    return (id >= 0 && synced_anim(id)) ? id : 0;
}

int remote_loop_anim(uint32_t chr) {
    uint32_t request = 0, anim = 0;
    uint8_t loop = 0;
    if (!chr || !read32(chr + 0x10, &request) || request < 0x10000 || !read32(request + 4, &anim) ||
        !read8(request + 8, &loop)) {
        return 0;
    }
    const int id = static_cast<int>(anim);
    return (loop && id >= 0 && synced_anim(id)) ? id : 0;
}

}  // namespace coop::motion
