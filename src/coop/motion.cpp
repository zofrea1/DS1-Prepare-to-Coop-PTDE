#include "motion.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstring>

namespace coop::motion {

namespace {

// ---------------------------------------------------------------- gait
// NetworkManipulator move (0xE1E6A0): the gait class is in al at 0xE1E935, before the 0.2 s hysteresis
// (+0x2C0 timer, +0x2C4 current class). The stub keeps the replaced load of the timer and swaps al for the
// sender's gait when the character moves at all (al != 0) and that player reported one.
constexpr uint32_t kGaitSite = 0x00E1E935;    // movss xmm0, dword ptr [ecx+2C0h]       (8 bytes)
constexpr uint32_t kSprintSite = 0x00E1E924;  // movss xmm2, dword ptr [011603ACh] (6.0) (8 bytes)
constexpr uint32_t kNetworkManipulatorVtable = 0x0118DDEC;

struct GaitEntry {
    uint32_t manipulator;
    int32_t gait;  // -1 none
};
GaitEntry g_gaits[4] = {{0, -1}, {0, -1}, {0, -1}, {0, -1}};
alignas(4) float g_sprint_speed = 4.5f;  // Remastered's fallback threshold (PTDE 6.0)

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
        cmp esi, 4
        jb next_entry
    gait_done:
        pop esi
        pop edx
    done:
        ret
    }
}

// Our own gait: the horizontal speed (smoothed over about 0.12 s) and the sprint flag the PadManipulator sets
// while the dash button is held (+0x1E4, set at 0xE17FE3).
LARGE_INTEGER g_freq{};
LARGE_INTEGER g_prev_t{};
game::Vec3 g_prev_pos{};
bool g_have_prev = false;
double g_speed = 0;
Gait g_local = kStand;

// ---------------------------------------------------------------- event animations
constexpr uint32_t kPlayCore = 0x00D71A20;   // eax = anim, stdcall(entity, a2, a3)
constexpr uint32_t kForceCore = 0x00D719C0;  // the same for ForcePlayAnimation
constexpr int kLocalPlayer = 10000;
constexpr int kNetPlayer = 10001;
void* g_play_orig = nullptr;
void* g_force_orig = nullptr;
bool g_replaying = false;
std::vector<Anim> g_pending;

bool synced_anim(int anim) {
    return (anim >= 7590 && anim <= 7730) ||  // bonfire: sit down, sitting loop, stand up, kindle, warp
           (anim >= 8280 && anim <= 8290);    // bonfire warp out
}

void __stdcall note_anim(int kind, int anim, int entity, int a2, int a3) {
    if (g_replaying || entity != kLocalPlayer || !synced_anim(anim) || g_pending.size() >= 32) return;
    g_pending.push_back(Anim{static_cast<uint8_t>(kind), anim, a2, a3});
}

__declspec(naked) void hk_play_core() {
    __asm {
        pushad
        push dword ptr [esp + 44]
        push dword ptr [esp + 44]
        push dword ptr [esp + 44]
        push eax
        push 0
        call note_anim
        popad
        jmp g_play_orig
    }
}

__declspec(naked) void hk_force_core() {
    __asm {
        pushad
        push dword ptr [esp + 44]
        push dword ptr [esp + 44]
        push dword ptr [esp + 44]
        push eax
        push 1
        call note_anim
        popad
        jmp g_force_orig
    }
}

}  // namespace

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
        static const uint8_t k[5] = {0x53, 0x8A, 0x5C, 0x24, 0x10};  // push ebx ; mov bl,[esp+10h]
        g_play_orig = detour(kPlayCore, k, sizeof(k), reinterpret_cast<void*>(&hk_play_core));
        g_force_orig = detour(kForceCore, k, sizeof(k), reinterpret_cast<void*>(&hk_force_core));
        if (!g_play_orig || !g_force_orig) ++failed;
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
    Gait g = kWalk;
    if (g_speed < 0.3) {
        g = kStand;
    } else if ((dash && g_speed > 3.2) || g_speed > 5.2) {
        g = kSprint;
    } else if (g_speed > 2.2) {
        g = kRun;
    }
    if (g != g_local) {
        static int lines = 0;
        if (++lines <= 300) LOG_INFO("[coop] gait me %d (%.2f m/s, dash %d)", g, g_speed, dash);
    }
    return g_local = g;
}

std::vector<Anim> take_local_anims() {
    std::vector<Anim> out;
    out.swap(g_pending);
    return out;
}

void set_remote_gait(uint32_t chr, Gait gait) {
    const uint32_t m = game::manipulator(chr);
    uint32_t vt = 0;
    if (!m || !read32(m, &vt) || vt != kNetworkManipulatorVtable) return;
    int free_slot = -1;
    for (int i = 0; i < 4; ++i) {
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

void play_remote_anim(uint32_t chr, const Anim& a) {
    uint32_t netno = 0;
    if (!chr || !read32(chr + 0x410, &netno) || netno > 8) return;
    const int entity = kNetPlayer + static_cast<int>(netno);
    const int anim = a.anim, a2 = a.a2, a3 = a.a3;
    const uint32_t core = a.kind ? kForceCore : kPlayCore;
    g_replaying = true;
    __try {
        __asm {
            push a3
            push a2
            push entity
            mov eax, anim
            mov edx, core
            call edx
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[coop] Playing animation %d on another player's character failed", anim);
    }
    g_replaying = false;
}

}  // namespace coop::motion
