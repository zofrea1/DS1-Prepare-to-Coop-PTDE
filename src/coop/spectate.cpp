// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "spectate.h"

#include "game.h"
#include "hook.h"
#include "log.h"
#include "rules.h"
#include "text.h"
#include "world.h"
#include "xinput_proxy.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace coop::spectate {

namespace {

// ---------------------------------------------------------------- addresses
// The Lua event system's HP test (0xD53B70: OnCharacterHP kind 0xD, OnCharacterDead kind 0xE, HP drop kind 0x2D)
// hands every event it fires to one call. The global script registers OnCharacterDead(99999, LOCAL_PLAYER,
// 'OnEvent_4000', everytime): the test fires every frame while the local player is dead (OnEvent_4000 ignores the
// repeats through flag 4000), so an event that is not passed on is simply asked for again on the next frame.
// Remastered Seamless Co-op holds the same call (DSR 0x1404A7740 from 0x1404CFAF0).
constexpr uint32_t kDeathEventCall = 0x00D53CF0;
constexpr uint32_t kLuaDispatch = 0x00D66130;  // stdcall(kind, event, entity, extra), esi = event system; ret 0x10
constexpr int kKindDead = 0xE;
constexpr int kLocalPlayer = 10000;            // LOCAL_PLAYER
// The camera. The in-game step (0xEFD2F0) hands ChrCam's update (0xF14320) the character to follow: the debug
// "player" ([0x137D648]+0xEC, read by the menus and the pad as well, so it is not used here) or else the local
// player. The update turns to the death camera while WorldChrMan+0x40 is set (the local player died: 0xE8332E
// sets it, a revival clears it). While we spectate, the follow camera gets the teammate, and the death camera's
// flag reads clear for that character only; the game's own value stays as it is for when the death goes ahead.
constexpr uint32_t kCamChrSite = 0x00EFDB1F;   // mov edx,[0x137D648] ; mov ecx,[edx+0xEC]
constexpr uint32_t kCamDeadSite = 0x00F1457A;  // mov al,[esi+0x40] ; setne [esp+0x6F]  (esi = WorldChrMan)
constexpr uint32_t kDebugChrVar = 0x0137D648;
constexpr uint32_t kWorldChrManVar = 0x0137D644;  // +0x24 player characters, +0x28 their array (0x20 each)
// The cameras turn by the followed character's controller input (ChrCtrl+0x244, else +0x54: the vector at +0x40),
// as Remastered Seamless Co-op's camera hook (DSR 0x140237517) shows. A remote player's controller is the network's,
// so the camera kept turning by itself while spectating (2026-10-17 test; locking on stopped it). While a teammate
// is followed these three reads take the local player's controller instead, as Remastered does:
constexpr uint32_t kCamInputFollow = 0x00F04B2C;  // ChrExFollowCam 0xF02720: mov eax,[ebp+10h]; mov eax,[eax+28h]; cmp [eax+244h],0
constexpr uint32_t kCamInputAim = 0x00F18AFA;     // the aim camera 0xF18860: mov eax,[edi+28h]; mov ecx,[eax+244h]; test ecx,ecx
constexpr uint32_t kCamInputBack = 0x00F1B2AE;    // its end writes its direction back: mov edi,[edi+28h]; mov eax,[edi+244h]
// The picture greys out while the local player is dead: the in-game step (0xEFE74B) fades FadeMan's channel 6
// ([0x13786B8]+0x130, 7 channels of 0x30 bytes from +0x10) to 1 whenever the player's dead flag (chr+0xC -> +0x1C
// -> +0x14 bit 0, the same the death event tests) is set, and the tone correction (0xEAE930) takes that much
// saturation away. While our death waits the test reads "alive" and the channel is cleared; the death that goes
// ahead fades to grey as usual.
constexpr uint32_t kDeathTintTest = 0x00EFE76B;   // mov al,[eax+14h]; and al,1 (je 0xEFE7E4 follows)
constexpr uint32_t kFadeManVar = 0x013786B8;

constexpr double kViewDelay = 3.0;      // the death animation plays before the camera moves on
constexpr double kFightOffFor = 1.5;    // the fight counts as over once it has been over this long
constexpr double kHoldLimit = 600.0;
constexpr uint16_t kPadLeft = 0x0004, kPadRight = 0x0008;  // XINPUT_GAMEPAD_DPAD_LEFT / RIGHT

// ---------------------------------------------------------------- state
bool g_installed = false;
bool g_view = false;          // the camera patches are in
uint8_t g_no_tint = 0;        // the picture keeps its colour (our death waits)
uint32_t g_dispatch = kLuaDispatch;
bool g_hold = false;          // our death event is held
bool g_passed = false;        // this death went ahead; it is not held again until we live
double g_hold_since = 0;
double g_fight_off_since = 0;
bool g_rested = false;
std::vector<Teammate> g_team;
// The view.
uint32_t g_target = 0;        // the character the camera is to follow (checked again on every camera update)
uint64_t g_target_id = 0;
double g_view_at = 0;
bool g_told_waiting = false;
uint32_t g_followed = 0;      // what the camera follows this frame instead of us (0 = us)

double now() { return GetTickCount64() / 1000.0; }

int hp(uint32_t chr) {
    uint32_t v = 0;
    return read32(chr + 0x2D4, &v) ? static_cast<int>(v) : 0;
}

std::string narrow(const std::wstring& w) {
    char out[128];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out, sizeof(out), nullptr, nullptr);
    return n > 0 ? std::string(out) : std::string();
}

bool fight_on() { return world::boss_fight() || rules::state().invader_present; }

int living_teammates() {
    int n = 0;
    for (const Teammate& t : g_team) n += t.alive ? 1 : 0;
    return n;
}

bool hold_wanted() {
    const auto& st = rules::state();
    return st.active && !st.invader && !st.leaving && world::local_dead() && fight_on() && living_teammates() > 0;
}

// ---------------------------------------------------------------- the camera
// Still one of WorldChrMan's player characters (a player who left, or a load, frees it).
bool player_character(uint32_t chr) {
    uint32_t wcm = 0, n = 0, arr = 0;
    if (!chr || !read32(kWorldChrManVar, &wcm) || !wcm || !read32(wcm + 0x24, &n) || !read32(wcm + 0x28, &arr) ||
        !arr) {
        return false;
    }
    for (uint32_t i = 1; i < n && i < 8; ++i) {
        uint32_t c = 0;
        if (read32(arr + i * 0x20, &c) && c == chr) return true;
    }
    return false;
}

// The character ChrCam follows this frame: the game's debug choice as before, else our target, else 0 (the game
// then takes the local player).
uint32_t __cdecl camera_chr() {
    g_followed = 0;
    uint32_t dbg = 0, v = 0;
    if (read32(kDebugChrVar, &dbg) && dbg && read32(dbg + 0xEC, &v) && v) return v;
    if (!g_target || !player_character(g_target)) return 0;
    g_followed = g_target;
    return g_target;
}

__declspec(naked) void hk_cam_chr() {
    __asm {
        push eax
        push edx
        call camera_chr
        mov ecx, eax
        pop edx
        pop eax
        ret
    }
}

// The flags of the cmp before the patch reach the setne, which runs first. edi = the character followed.
__declspec(naked) void hk_cam_dead() {
    __asm {
        setne byte ptr [esp + 0x73]  // the game's [esp+0x6F], past our return address
        mov al, byte ptr [esi + 0x40]
        cmp edi, g_followed
        jne done
        xor al, al
    done:
        ret
    }
}

// The local player's character, else the one the camera follows.
__declspec(naked) void hk_cam_input_follow() {
    __asm {
        mov eax, dword ptr [ebp + 0x10]
        cmp eax, g_followed
        jne keep
        mov eax, 0x0137D644
        mov eax, [eax]
        mov eax, [eax + 0x3C]
        test eax, eax
        jnz keep
        mov eax, dword ptr [ebp + 0x10]
    keep:
        mov eax, [eax + 0x28]
        cmp dword ptr [eax + 0x244], 0
        ret
    }
}

__declspec(naked) void hk_cam_input_aim() {
    __asm {
        mov eax, edi
        cmp eax, g_followed
        jne keep
        mov eax, 0x0137D644
        mov eax, [eax]
        mov eax, [eax + 0x3C]
        test eax, eax
        jnz keep
        mov eax, edi
    keep:
        mov eax, [eax + 0x28]
        mov ecx, [eax + 0x244]
        test ecx, ecx
        ret
    }
}

__declspec(naked) void hk_cam_input_back() {
    __asm {
        cmp edi, g_followed
        jne keep
        push eax
        mov eax, 0x0137D644
        mov eax, [eax]
        mov eax, [eax + 0x3C]
        test eax, eax
        jz none
        mov edi, eax
    none:
        pop eax
    keep:
        mov edi, [edi + 0x28]
        mov eax, [edi + 0x244]
        ret
    }
}

// The je after the patch takes ZF: set = alive (no grey).
__declspec(naked) void hk_death_tint() {
    __asm {
        mov al, byte ptr [eax + 0x14]
        and al, 1
        jz done
        cmp g_no_tint, 0
        je dead
        xor al, al
        ret
    dead:
        test al, al
    done:
        ret
    }
}

// Channel 6 back to nothing, as the map load does (0xEFC77B).
void clear_death_tint() {
    uint32_t fm = 0;
    if (!read32(kFadeManVar, &fm) || !fm) return;
    __try {
        std::memset(ptr<uint8_t>(fm + 0x130), 0, 0x24);  // value, its pair, target, its pair, time
        *ptr<uint8_t>(fm + 0x154) = 0;
        *ptr<uint8_t>(fm + 0x155) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void follow(const Teammate* t) {
    g_target = t ? t->chr : 0;
    g_target_id = t ? t->id : 0;
    if (!t) return;
    g_told_waiting = false;
    text::show(L"Spectating " + t->name);
    LOG_INFO("[coop] Spectating %s", narrow(t->name).c_str());
}

// D-pad left or right (any controller), or the arrow keys, while the game has the focus: the previous or next
// teammate.
int switch_input() {
    static bool was_left = false, was_right = false;
    static double polled = 0;
    static uint16_t buttons = 0;
    DWORD pid = 0;
    const HWND fg = GetForegroundWindow();
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || pid != GetCurrentProcessId()) {
        was_left = was_right = false;
        return 0;
    }
    if (now() - polled >= 0.03) {
        polled = now();
        buttons = 0;
        for (DWORD i = 0; i < 4; ++i) {
            uint8_t st[16] = {};
            if (xinput_get_state(i, st) == 0) {
                uint16_t b = 0;
                std::memcpy(&b, st + 4, 2);
                buttons |= b;
            }
        }
    }
    const bool left = (buttons & kPadLeft) || (GetAsyncKeyState(VK_LEFT) & 0x8000);
    const bool right = (buttons & kPadRight) || (GetAsyncKeyState(VK_RIGHT) & 0x8000);
    const int step = left && !was_left ? -1 : right && !was_right ? 1 : 0;
    was_left = left;
    was_right = right;
    return step;
}

void view_tick() {
    if (!g_view || now() < g_view_at) return;
    std::vector<const Teammate*> can;
    for (const Teammate& t : g_team) {
        if (t.alive && t.chr && hp(t.chr) > 0 && player_character(t.chr)) can.push_back(&t);
    }
    std::sort(can.begin(), can.end(), [](const Teammate* a, const Teammate* b) { return a->id < b->id; });
    int at = -1;
    for (size_t i = 0; i < can.size(); ++i) {
        if (can[i]->id == g_target_id && can[i]->chr == g_target) at = static_cast<int>(i);
    }
    const int step = switch_input();
    if (can.empty()) {
        if (g_target) follow(nullptr);
        if (!g_told_waiting) {
            g_told_waiting = true;
            text::show(L"Waiting for the fight to be decided");
            LOG_INFO("[coop] No living teammate in this area to spectate: waiting for the fight to be decided");
        }
        return;
    }
    if (at >= 0 && step == 0) return;
    if (at >= 0) {
        if (can.size() == 1) return;
        follow(can[(at + step + static_cast<int>(can.size())) % can.size()]);
        return;
    }
    // A first target, or the last one fell or left: the nearest one standing.
    game::Vec3 me{};
    const bool have_me = game::position(&me, nullptr);
    const Teammate* best = can.front();
    float best_d = 3.4e38f;
    for (const Teammate* t : can) {
        game::Vec3 p{};
        if (!have_me || !game::chr_position(t->chr, &p)) break;
        const float d = (p.x - me.x) * (p.x - me.x) + (p.y - me.y) * (p.y - me.y) + (p.z - me.z) * (p.z - me.z);
        if (d < best_d) {
            best_d = d;
            best = t;
        }
    }
    follow(best);
}

// ---------------------------------------------------------------- the death event
void start_hold() {
    g_hold = true;
    g_no_tint = 1;
    clear_death_tint();
    g_hold_since = now();
    g_fight_off_since = 0;
    g_rested = false;
    g_view_at = now() + kViewDelay;
    g_told_waiting = false;
    LOG_INFO("[coop] We fell during %s with %d teammate(s) still standing: our death waits",
             world::boss_fight() ? "a boss fight" : "an invasion", living_teammates());
}

void release(const char* why) {
    g_hold = false;
    g_no_tint = 0;
    g_passed = true;
    follow(nullptr);
    LOG_INFO("[coop] Our death goes ahead (%s)", why);
}

bool __stdcall death_event_passes(int kind, int entity) {
    if (kind != kKindDead || entity != kLocalPlayer) return true;
    if (g_hold) return false;
    if (g_passed) return true;
    if (!hold_wanted()) {
        g_passed = true;
        return true;
    }
    start_hold();
    return false;
}

__declspec(naked) void hk_death_event() {
    __asm {
        pushad
        push dword ptr [esp + 44]  // entity: [esp+0Ch] before pushad
        push dword ptr [esp + 40]  // kind: [esp+4]
        call death_event_passes    // stdcall
        test al, al
        popad                      // leaves the flags alone
        jz held
        jmp g_dispatch
    held:
        ret 16
    }
}

bool patch_call(uint32_t at, const uint8_t* expect, size_t n, void* fn) {
    uint8_t code[16];
    std::memset(code, 0x90, sizeof(code));
    code[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(fn) - (at + 5));
    std::memcpy(code + 1, &rel, 4);
    return n >= 5 && n <= sizeof(code) && patch(at, expect, code, n);
}

}  // namespace

bool install() {
    g_installed = redirect_call(kDeathEventCall, kLuaDispatch, reinterpret_cast<void*>(&hk_death_event));
    if (!g_installed) {
        LOG_ERROR("[coop] Spectating: the death event call did not match this build (deaths wait at the respawn instead)");
        return false;
    }
    static const uint8_t dead_k[8] = {0x8A, 0x46, 0x40, 0x0F, 0x95, 0x44, 0x24, 0x6F};
    static const uint8_t chr_k[12] = {0x8B, 0x15, 0x48, 0xD6, 0x37, 0x01, 0x8B, 0x8A, 0xEC, 0x00, 0x00, 0x00};
    static const uint8_t follow_k[13] = {0x8B, 0x45, 0x10, 0x8B, 0x40, 0x28, 0x83, 0xB8, 0x44, 0x02, 0x00, 0x00, 0x00};
    static const uint8_t aim_k[11] = {0x8B, 0x47, 0x28, 0x8B, 0x88, 0x44, 0x02, 0x00, 0x00, 0x85, 0xC9};
    static const uint8_t back_k[9] = {0x8B, 0x7F, 0x28, 0x8B, 0x87, 0x44, 0x02, 0x00, 0x00};
    // The input patches first: the camera never follows a teammate without them.
    g_view = bytes_match(kCamInputFollow, follow_k, sizeof(follow_k)) && bytes_match(kCamInputAim, aim_k, sizeof(aim_k)) &&
             bytes_match(kCamInputBack, back_k, sizeof(back_k)) &&
             patch_call(kCamInputFollow, follow_k, sizeof(follow_k), reinterpret_cast<void*>(&hk_cam_input_follow)) &&
             patch_call(kCamInputAim, aim_k, sizeof(aim_k), reinterpret_cast<void*>(&hk_cam_input_aim)) &&
             patch_call(kCamInputBack, back_k, sizeof(back_k), reinterpret_cast<void*>(&hk_cam_input_back)) &&
             patch_call(kCamDeadSite, dead_k, sizeof(dead_k), reinterpret_cast<void*>(&hk_cam_dead)) &&
             patch_call(kCamChrSite, chr_k, sizeof(chr_k), reinterpret_cast<void*>(&hk_cam_chr));
    if (!g_view) LOG_ERROR("[coop] Spectating: the camera code did not match this build (the camera stays on us)");
    static const uint8_t tint_k[5] = {0x8A, 0x40, 0x14, 0x24, 0x01};
    if (!patch_call(kDeathTintTest, tint_k, sizeof(tint_k), reinterpret_cast<void*>(&hk_death_tint))) {
        LOG_ERROR("[coop] Spectating: the death tint test did not match this build (the picture greys while spectating)");
    }
    return true;
}

void set_teammates(const std::vector<Teammate>& teammates) { g_team = teammates; }

void tick() {
    if (!g_installed) return;
    if (game::player() && !world::local_dead()) {
        if (g_hold) {
            g_hold = false;
            g_no_tint = 0;
            follow(nullptr);
            LOG_INFO("[coop] We live again while our death waited");
        }
        g_passed = false;
        return;
    }
    if (!g_hold) return;
    const auto& st = rules::state();
    if (fight_on()) {
        g_fight_off_since = 0;
    } else if (g_fight_off_since == 0) {
        g_fight_off_since = now();
    }
    const char* why = nullptr;
    if (game::loading() || !game::player()) {
        why = "a map load";
    } else if (!st.active || st.leaving) {
        why = "the session ended";
    } else if (g_rested) {
        why = "someone rested at a bonfire";
    } else if (living_teammates() == 0) {
        why = "every co-op player has fallen";
    } else if (g_fight_off_since && now() - g_fight_off_since >= kFightOffFor) {
        why = "the fight is over";
    } else if (now() - g_hold_since > kHoldLimit) {
        why = "time limit";
    }
    if (why) {
        release(why);
    } else {
        view_tick();
    }
}

void someone_rested() {
    if (g_hold) g_rested = true;
}

bool holding() { return g_hold; }

bool installed() { return g_installed; }

}  // namespace coop::spectate
