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
    if (g_replaying || !g_enabled || !chr || chr != game::player()) return;
    const bool looped = (loop & 0xFF) != 0;
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

int local_loop_anim() { return g_enabled ? g_local_loop : 0; }

void set_bonfire_animations(bool on) { g_enabled = on; }

bool play_remote_anim(uint32_t chr, const Anim& a) {
    // A stop is always carried out: it only ever puts a character back to normal.
    if (!chr || chr == game::player() || (!g_enabled && a.kind == 0)) return false;
    if (a.kind == 0 && !synced_anim(a.anim)) return false;
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
