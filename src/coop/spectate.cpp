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
#include <cmath>
#include <cstdio>
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
// The area's look: later in the same step (0xEFE4C3) the frame's draw parameters are taken from the floor of the same
// character, the debug "player" or else the local player: 0xEAF340 (eax = its floor, chr+0x29C) sets the lens flare,
// shadow, depth of field, tone map and tone correction (the colour grading) of that floor's MSB part (HitIns+8: its
// part's ids at +0x58 +7..+0xB, made area << 8 | id with the area at HitIns+0xE by 0x42E430), each blended in over
// 0.5 s (0x10002F0); and the draw state's +0x38 takes the character's +0x294 (SetFloor's value of the floor,
// 0xE6E68A). A floor in another area takes that area's DrawParam (every area's a%02d_DrawParam.parambnd is loaded
// with the world, 0xDE7AA0), so the far teammate's floor gives their area's grading. While we follow a teammate both
// reads take them, as the camera does: the look of their area was ours before (2026-11-05: "the color grading / the
// lighting looks incorrect when spectating someone from far away").
constexpr uint32_t kDrawChrSite = 0x00EFE4C9;     // mov eax,[esi+0xEC] (esi = [0x137D648]): the floor's character
constexpr uint32_t kDrawChrSite2 = 0x00EFE4EF;    // mov esi,[esi+0xEC]: the same for +0x294
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

// The world around a teammate we follow: drawn as from their floor (rules::draw_from), and their character updated
// every frame. Far from the local player the game updates a character less often (its rate chr+0xF4, set by
// WorldChrMan's update at 0xE3F8D7 unless chr+0x1FD bit 0 fixes it), so a far teammate moved in jerks; the event
// command "Set Network Update Rate" (2004[34], 0xD6D780) fixes it at 0 (always), as here while we follow them.
constexpr uint32_t kChrRateFlags = 0x1FD;  // bit 0: the rate is fixed
constexpr uint32_t kChrRate = 0xF4;
// The world around a far teammate, as if we stood there (SpectateStreaming, 1.5.0):
//  * Their map: a teammate in a block our game has not loaded had no floor here, so nothing around them was drawn
//    (2026-11-03: "no floor under them" in the Undead Burg, seen from the Asylum). Their block is asked for on every
//    WorldRes update while we follow them (rules::view_map), as a floor under them connecting to it would; once it is
//    in use their character finds its floor there and the world is drawn from it (above).
//  * What is there to draw: the game streams map pieces, objects and characters in by the groups our own floor shows
//    (FieldArea+0x3C), and a map piece outside them is not in the scene at all, whatever is drawn from. Their floor
//    is added to that streaming while we follow them, as the cutscenes add theirs (rules::draw_from with `stream`):
//    without it large parts of the world around them were missing, in our own block too (2026-11-04).
//  * The characters around them: WorldChrMan's per-frame pass (0xE3F630) decides every character's update rate from
//    a camera frustum test and a rank by distance, both taken from the local player's character ([WorldChrMan+0x3C]:
//    its position, and its draw groups chr+0x274 that the frustum test is limited to, 0x456B70). The enemies around a
//    far teammate failed the draw group test (not drawn, or drawn at rate -2's LOD) and ranked behind everything near
//    us. While we follow a teammate the pass takes their character instead (0xE3F68A: mov ecx,[edi+3Ch]; mov eax,
//    [ecx+28h]), checked to be one of WorldChrMan's players every time.
constexpr uint32_t kChrPassOriginSite = 0x00E3F68A;
//  * The enemies there (2026-11-05: around a teammate in another map they were "not there at all"). A character is
//    updated, and its model placed (0xE6AA30 hands the model its display groups), only while its map entity is active
//    ([chr+0xC]+8 == 2; the pass gives any other rate -1). Every frame the streaming update (0xDE7C30) builds each
//    block's streaming mask (the floors' draw groups, 0xDE2AE0: the floors of the extra slots at FieldArea+0x44 and our
//    own, else what the back-read around us wants), writes it to that block's WorldBlockChr (WorldChrMan+0x20, 0x98
//    each, +0x1C), and asks for each character's entity (0xE3E870: [chr+0xC]+0xC = 2 when its display groups chr+0x264
//    meet its block's mask, else 0; the entity's state machine 0xE6AC50, run for every character from the field
//    update 0xE3EB40, follows the ask). The teammate's floor is in an extra slot while we follow them, so their block's
//    characters should be asked for: after the game's own asks (its call at 0xDE81F0) the same rule is applied once
//    more with their floor's draw groups (HitIns+0x20) to the characters of their floor's block, and the log counts
//    what that changed. And the pass gives a character that fails its draw group test the rate of the block we stand in
//    only (FieldArea+0x14, compared at 0xE3F966 with chr+0x5C: every 5th frame near the origin, every 30th farther,
//    else none): the teammate's block counts as ours there while the pass takes their character. While we follow a
//    teammate the log says, every few seconds, what the characters of their block are doing here.
constexpr uint32_t kActivationCallSite = 0x00DE81F0;  // call 0xE3E870: stdcall(WorldChrMan) -> al, every entity ready
constexpr uint32_t kActivation = 0x00E3E870;
constexpr uint32_t kBlockTestSite = 0x00E3F966;       // mov ecx,[esp+0xA0] ; cmp [esi+0x5C],ecx (then jne 0xE3FC48)
constexpr uint32_t kNoBlock = 0x7FFFFFFF;             // no character's block (chr+0x5C)

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
uint32_t g_rate_chr = 0;      // the teammate whose update rate we fixed (0 none)
bool g_rate_was_fixed = false;
uint32_t g_drawn_for = 0;     // the teammate the view's last "drawn from" line was about
const char* g_drawn_why = "";
bool g_drawn_streamed = false;  // and whether that floor added to what is streamed
uint32_t g_knell_chr = 0;     // the Death Knell's view: the teammate it shows (knell.cpp), 0 none
bool g_streaming = true;      // SpectateStreaming
bool g_origin_patched = false;
volatile uint32_t g_pass_origin = 0;  // the character WorldChrMan's pass takes instead of ours (0: ours)
// The pass's visibility test is a draw group overlap with that character (0x456B70 copies its chr+0x274, 0x42DB10 /
// 0x42DAA0 test ours against it), not the camera: far from the teammate our own character failed it, and the pass gave
// it rate 5 (its LOD path, vt+0xA0), so it was updated every fifth frame. The camera, which turns by our character's
// input, then barely turned, and the Death Knell's prompt missed the button (2026-11-04: "only worked in the vicinity
// of the player"). Our own rate is fixed at every frame while the pass takes the teammate (as the teammate's is).
uint32_t g_own_fixed_chr = 0;   // our character whose rate we fixed (0 none)
bool g_own_was_fixed = false;
int g_view_map = -1;          // the followed teammate's map kept loaded for the view (-1 none)
// Their floor, found by a cast (rules::find_floor) where their character never touches the collision: in a map loaded
// for the view, their character had no floor at all (2026-11-04). The character given one, to take it back from when
// the view lets that map go.
uint32_t g_cast_chr = 0;
double g_cast_at = 0;
const char* g_cast_why = "";
const char* g_give_why = "";  // why the reported floor was last not given (logged once per change)
// The reported floor we last gave and when: when their character's own physics takes another floor back within
// kTakenBackWithin, it touches that collision here (a seam between two maps, both loaded), and the view leaves its floor
// to the physics for kSeamHold unless it has none at all. Both games' floors were right there, and the view and the
// physics took turns every frame for 0.1 s (2026-11-04 22:04:15, New Londo and Darkroot).
uint32_t g_gave_chr = 0;
uint32_t g_gave_hit = 0;
double g_gave_at = 0;
double g_seam_until = 0;
uint32_t g_gave_logged = 0;  // the floor the last "given" line was about
constexpr double kTakenBackWithin = 0.5;
constexpr double kSeamHold = 2.0;
constexpr double kCastEvery = 0.2;

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

// A boss fight is on: ours (our boss gauge), or a living teammate's far from us. A death of ours during a teammate's
// fight would reload our world in the middle of it, and the session host's world decides every enemy's HP: its
// fresh, unscaled boss went to the fighter's screen at half its health (2026-10-18).
bool fight_on() {
    const auto& st = rules::state();
    return world::boss_fight() || st.teammate_boss_fight || st.invader_present;
}

int living_teammates() {
    int n = 0;
    for (const Teammate& t : g_team) n += t.alive ? 1 : 0;
    return n;
}

bool g_trip_release = false;  // the host's trip home came while we were down (trip_release): no hold until we live

bool hold_wanted() {
    const auto& st = rules::state();
    if (!st.active || st.leaving || g_trip_release || !world::local_dead() || living_teammates() == 0) return false;
    // We invade: our teammates are the other invaders, and the invasion is the fight (session's team_invasion).
    return st.invader ? st.team_invasion : fight_on();
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
    const uint32_t want = g_hold ? g_target : g_knell_chr;
    if (!want || !player_character(want)) return 0;
    g_followed = want;
    return want;
}

// The character whose floor gives the frame's draw parameters (kDrawChrSite): the game's debug choice as before, else
// the teammate the camera follows while they have a floor, else 0 (the game then takes the local player, as it did).
uint32_t __cdecl draw_chr(uint32_t debug_chr) {
    if (debug_chr) return debug_chr;
    const uint32_t want = g_hold ? g_target : g_knell_chr;
    uint32_t hit = 0;
    uint8_t area = 0;
    const bool floor = want && player_character(want) && read32(want + 0x29C, &hit) && hit && read8(hit + 0xE, &area);
    const uint32_t chr = floor ? want : 0;
    // The log notes a change (this runs twice a frame).
    static uint32_t s_chr = 0;
    static int s_area = -1;
    if (chr != s_chr || (chr && area != s_area)) {
        if (chr) {
            LOG_INFO("[coop] Spectating: the colour grading and lighting are the followed teammate's (area %d, from "
                     "their floor)", area);
        } else if (s_chr) {
            LOG_INFO("[coop] Spectating: the colour grading and lighting are ours again");
        }
        s_chr = chr;
        s_area = chr ? area : -1;
    }
    return chr;
}

// mov eax,[esi+0xEC]
__declspec(naked) void hk_draw_chr() {
    __asm {
        push ecx
        push edx
        push dword ptr [esi + 0xEC]
        call draw_chr
        add esp, 4
        pop edx
        pop ecx
        ret
    }
}

// mov esi,[esi+0xEC]
__declspec(naked) void hk_draw_chr2() {
    __asm {
        push eax
        push ecx
        push edx
        push dword ptr [esi + 0xEC]
        call draw_chr
        add esp, 4
        mov esi, eax
        pop edx
        pop ecx
        pop eax
        ret
    }
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

uint32_t __stdcall pass_origin(uint32_t wcm) {
    uint32_t local = 0;
    read32(wcm + 0x3C, &local);
    const uint32_t chr = g_pass_origin;
    uint32_t ctrl = 0, phys = 0;
    if (!chr || chr == local || !player_character(chr) || !read32(chr + 0x28, &ctrl) || !ctrl ||
        !read32(ctrl + 0x1C, &phys) || !phys) {
        return local;
    }
    return chr;
}

__declspec(naked) void hk_chr_pass_origin() {
    __asm {
        push edx
        push eax
        push edi            // WorldChrMan
        call pass_origin    // stdcall
        mov ecx, eax
        pop eax
        pop edx
        mov eax, dword ptr [ecx + 0x28]
        ret
    }
}

// ---------------------------------------------------------------- the enemies around a far teammate
volatile uint32_t g_view_block = kNoBlock;  // their floor's block while the pass takes their character
bool g_block_test = false;                  // the pass's block test is patched (kBlockTestSite)
int g_asked_more = 0;                       // characters of their block our rule asked for this frame (the game had not)

// WorldChrMan's character sets: {set, block, WorldBlockChr} at +0xE4, the count at +0xE0; a set is {count, entries of
// 0x20 with the character first}.
constexpr uint32_t kWcmSets = 0xE4;
constexpr uint32_t kWcmSetCount = 0xE0;

// The characters of `block` (their set), 0 when the block has none here.
uint32_t block_set(uint32_t wcm, uint32_t block) {
    uint32_t n = 0;
    if (!wcm || !read32(wcm + kWcmSetCount, &n) || n > 64) return 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t set = 0, b = 0;
        if (read32(wcm + kWcmSets + i * 12, &set) && read32(wcm + kWcmSets + i * 12 + 4, &b) && b == block) return set;
    }
    return 0;
}

bool overlaps(uint32_t a, uint32_t b) {
    for (uint32_t k = 0; k < 16; k += 4) {
        uint32_t x = 0, y = 0;
        if (read32(a + k, &x) && read32(b + k, &y) && (x & y)) return true;
    }
    return false;
}

// After the game's own asks: the characters of the followed teammate's block whose display groups meet their floor's
// draw groups are asked for, as the game asks for them when their floor is ours (0xE3E870: not one it asks for
// whatever the masks, chr+0x15 / chr+0x54 / its vt+0xA0 and +0xA4, nor one it never asks for, chr+0x14).
void ask_view_block(uint32_t wcm) {
    g_asked_more = 0;
    const uint32_t chr = g_pass_origin;
    uint32_t hit = 0, count = 0, arr = 0;
    uint8_t b = 0xFF;
    if (!chr || !read32(chr + 0x29C, &hit) || !hit || !read8(hit + 0xF, &b) || b >= 64) return;
    const uint32_t set = block_set(wcm, b);
    if (!set || !read32(set, &count) || !read32(set + 4, &arr) || !arr || count > 4096) return;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t c = 0, ent = 0, ask = 0, attached = 0;
        uint8_t never = 1, always = 1;
        if (!read32(arr + i * 0x20, &c) || !c || !read8(c + 0x14, &never) || !read8(c + 0x15, &always) ||
            !read32(c + 0x54, &attached) || !read32(c + 0xC, &ent) || !ent || !read32(ent + 0xC, &ask)) {
            continue;
        }
        if (never || always || !attached || ask == 2 || !overlaps(c + 0x264, hit + 0x20)) continue;
        *ptr<uint32_t>(ent + 0xC) = 2;
        ++g_asked_more;
    }
}

using ActivationFn = uint8_t(__stdcall*)(uint32_t);

uint8_t __stdcall hk_activation(uint32_t wcm) {
    const uint8_t ready = reinterpret_cast<ActivationFn>(kActivation)(wcm);
    __try {
        ask_view_block(wcm);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_asked_more = 0;
    }
    return ready;
}

// mov ecx,[esp+0xA0] ; cmp [esi+0x5C],ecx: the block the pass counts as ours, or the followed teammate's. The jne after
// it takes ZF.
__declspec(naked) void hk_block_test() {
    __asm {
        mov ecx, dword ptr [esp + 0xA4]  // the game's [esp+0xA0], past our return address
        push eax
        mov eax, dword ptr [esi + 0x5C]
        cmp eax, ecx
        je done
        cmp eax, g_view_block
    done:
        pop eax
        ret
    }
}

// What the characters of the followed teammate's block are doing here, every few seconds while we follow them (the
// log's answer to "the enemies around them are not there").
void view_block_report() {
    static double last = 0;
    static int lines = 0;
    const uint32_t chr = g_pass_origin;
    if (!chr || lines >= 40 || now() - last < 4.0) return;
    last = now();
    uint32_t wcm = 0, hit = 0, fa = 0, ours = 0xFFFFFFFF, count = 0, arr = 0;
    uint8_t b = 0xFF, flags = 0;
    if (!read32(kWorldChrManVar, &wcm) || !wcm || !read32(chr + 0x29C, &hit) || !hit || !read8(hit + 0xF, &b)) return;
    read8(hit + 0x78, &flags);
    if (read32(0x01378520, &fa) && fa) read32(fa + 0x14, &ours);
    const uint32_t set = block_set(wcm, b);
    ++lines;
    if (!set || !read32(set, &count) || !read32(set + 4, &arr) || !arr) {
        LOG_INFO("[coop] Spectating: the followed teammate's block %u has no characters here (our block %d)", b,
                 static_cast<int>(ours));
        return;
    }
    game::Vec3 them{};
    game::chr_position(chr, &them);
    int alive = 0, active = 0, asked = 0, updated = 0, none = 0, seen = 0, model = 0, meet = 0, close = 0;
    float best = 1e9f;
    uint32_t best_chr = 0;
    for (uint32_t i = 0; i < count && i < 4096; ++i) {
        uint32_t c = 0, ent = 0, state = 0, ask = 0, hp = 0, mdl = 0;
        int32_t rate = 0;
        uint8_t f1fd = 0;
        if (!read32(arr + i * 0x20, &c) || !c || !read32(c + 0xC, &ent) || !ent) continue;
        read32(ent + 8, &state);
        read32(ent + 0xC, &ask);
        read32(c + 0x2D4, &hp);
        read32(c + 0x24, &mdl);
        read32(c + 0xF4, reinterpret_cast<uint32_t*>(&rate));
        read8(c + 0x1FD, &f1fd);
        alive += static_cast<int>(hp) > 0;
        active += state == 2;
        asked += ask == 2;
        updated += rate >= 0;
        none += rate == -1;
        seen += (f1fd & 8) != 0;
        model += mdl != 0;
        meet += overlaps(c + 0x264, hit + 0x20);
        game::Vec3 at{};
        if (static_cast<int>(hp) > 0 && game::chr_position(c, &at)) {
            const float dx = at.x - them.x, dy = at.y - them.y, dz = at.z - them.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            close += d < 40.0f;
            if (d < best) {
                best = d;
                best_chr = c;
            }
        }
    }
    char nearest[160] = "none alive";
    if (best_chr) {
        uint32_t ent = 0, state = 0, ask = 0;
        int32_t rate = 0;
        read32(best_chr + 0xC, &ent);
        if (ent) {
            read32(ent + 8, &state);
            read32(ent + 0xC, &ask);
        }
        read32(best_chr + 0xF4, reinterpret_cast<uint32_t*>(&rate));
        std::snprintf(nearest, sizeof(nearest), "%.0f m from them: entity %u (asked %u), rate %d, groups %s their floor's",
                      best, state, ask, rate, overlaps(best_chr + 0x264, hit + 0x20) ? "meet" : "miss");
    }
    LOG_INFO("[coop] Spectating: the followed teammate's block %u here (our block %d; their floor streams: %s): %u "
             "characters, %d alive, %d within 40 m; entity active %d, asked for %d (%d by the view's rule this frame), "
             "groups meeting their floor's %d; updated %d, never %d, seen %d, with a model %d; nearest: %s",
             b, static_cast<int>(ours), (flags & 4) ? "yes" : "no", count, alive, close, active, asked, g_asked_more,
             meet, updated, none, seen, model, nearest);
}

// Puts back the update rate of the teammate we stop following (if they are still here) and fixes the new one's.
void fix_rate(uint32_t chr) {
    if (chr == g_rate_chr) return;
    if (g_rate_chr && !g_rate_was_fixed && player_character(g_rate_chr)) {
        uint8_t flags = 0;
        if (read8(g_rate_chr + kChrRateFlags, &flags)) *ptr<uint8_t>(g_rate_chr + kChrRateFlags) = flags & ~1;
    }
    g_rate_chr = 0;
    uint8_t flags = 0;
    if (!chr || !read8(chr + kChrRateFlags, &flags)) return;
    g_rate_was_fixed = (flags & 1) != 0;
    *ptr<uint8_t>(chr + kChrRateFlags) = flags | 1;
    *ptr<int32_t>(chr + kChrRate) = 0;
    g_rate_chr = chr;
}

// Our own character's update rate fixed at every frame while `on` (put back after, as it was).
void fix_own_rate(bool on) {
    const uint32_t me = game::player();
    if (g_own_fixed_chr && (!on || g_own_fixed_chr != me)) {
        if (g_own_fixed_chr == me && !g_own_was_fixed) {
            uint8_t flags = 0;
            if (read8(me + kChrRateFlags, &flags)) *ptr<uint8_t>(me + kChrRateFlags) = flags & ~1;
        }
        g_own_fixed_chr = 0;
    }
    if (!on || !me) return;
    uint8_t flags = 0;
    if (!read8(me + kChrRateFlags, &flags)) return;
    if (g_own_fixed_chr != me) {
        g_own_was_fixed = (flags & 1) != 0;
        g_own_fixed_chr = me;
    }
    *ptr<uint8_t>(me + kChrRateFlags) = flags | 1;  // every frame: the game clears it at times (a death)
    *ptr<int32_t>(me + kChrRate) = 0;
}

// Every frame while we follow a teammate: the world is drawn from their floor (the reason when not is logged once
// per change).
void view_extras() {
    const uint32_t want = g_hold ? g_target : g_knell_chr;
    const uint32_t chr = want && player_character(want) ? want : 0;
    fix_rate(chr);
    // Their map, as they report it, kept loaded while we follow them: the one of the floor their game has them on when
    // they report it (on a map boundary the game's map and the floor's can differ for a while).
    int map = -1;
    const Teammate* mate = nullptr;
    if (chr && g_streaming) {
        for (const Teammate& t : g_team) {
            if (t.chr == chr) mate = &t;
        }
        if (mate) map = mate->floor_map != -1 ? mate->floor_map : mate->map;
    }
    // The floor we gave a character goes again before the view lets its map go (the game unloads a block nobody wants
    // two frames later), as the game takes the party's floors at a map switch (0xDE36E0): nothing else would.
    if (g_cast_chr && (g_cast_chr != chr || map != g_view_map)) {
        if (player_character(g_cast_chr) && rules::view_only_floor(g_cast_chr)) {
            rules::clear_floor(g_cast_chr);
            LOG_INFO("[coop] Spectating: the floor given to the character we followed taken back (its map is let go)");
        }
        g_cast_chr = 0;
    }
    if (map != g_view_map || map >= 0) {
        g_view_map = map;
        rules::view_map(map);
    }
    if (!chr) {
        g_pass_origin = 0;
        g_view_block = kNoBlock;
        fix_own_rate(false);
        g_drawn_for = 0;
        return;
    }
    bool streamed = false;
    const char* why = rules::draw_from(chr, g_streaming, &streamed);
    // The floor their own game has them on (1.5.0 reports it), given to their character here when it has none, or one
    // from another map than theirs, or one whose collision is not loaded here (their character touches no collision
    // there, so its floor stays where it last did): across a map boundary their character had no floor here at all,
    // so nothing around them was drawn or streamed in (2026-11-04, "no floor under them" for 46 s in Firelink).
    const uint32_t reported = mate && mate->floor_map != -1 ? rules::floor_by_id(mate->floor_map, mate->floor_index) : 0;
    if (g_streaming && reported) {
        uint32_t have = 0;
        uint8_t have_block = 0xFF, their_block = 0xFF;
        read32(chr + 0x29C, &have);
        const bool other_map = have && read8(have + 0xF, &have_block) && read8(reported + 0xF, &their_block) &&
                               have_block != their_block;
        if (g_gave_chr == chr && g_gave_hit && have && have != g_gave_hit && now() - g_gave_at < kTakenBackWithin) {
            if (now() >= g_seam_until) {
                LOG_INFO("[coop] Spectating: the followed teammate's character took its own floor back (its physics "
                         "touches it here): left to it for %.0f s", kSeamHold);
            }
            g_seam_until = now() + kSeamHold;
            g_gave_hit = 0;
        }
        const bool stale = other_map || !rules::floor_ready(have) || rules::view_only_floor(chr);
        if (have != reported && (why || (stale && now() >= g_seam_until))) {
            bool changed = false;
            const char* give_why = rules::give_floor(chr, reported, &changed);
            if (changed) {
                g_cast_chr = chr;
                g_gave_chr = chr;
                g_gave_hit = reported;
                g_gave_at = now();
                const char* was = why;
                why = rules::draw_from(chr, g_streaming, &streamed);
                static int lines = 0;
                const bool tell = reported != g_gave_logged;
                g_gave_logged = reported;
                if ((was || other_map) && tell && ++lines <= 60) {
                    LOG_INFO("[coop] Spectating: the followed teammate's character given the floor their game reports "
                             "(map %08X, collision %d; %s)", static_cast<uint32_t>(mate->floor_map), mate->floor_index,
                             was ? "it had none here" : "its own was in another map");
                }
            } else if (give_why && std::strcmp(give_why, g_give_why) != 0) {
                g_give_why = give_why;
                LOG_INFO("[coop] Spectating: the floor the followed teammate's game reports not given (%s)", give_why);
            }
        }
    }
    // No floor of theirs here, or one in the view's own map (their character never touches its collision, so the floor
    // does not follow them there), and none reported (an older game): the one under them, by a cast.
    if (g_streaming && !reported && (why || rules::view_only_floor(chr)) && now() - g_cast_at >= kCastEvery) {
        g_cast_at = now();
        bool changed = false;
        const char* cast_why = rules::find_floor(chr, &changed);
        if (changed) {
            g_cast_chr = chr;
            g_cast_why = "";
            const char* was = why;
            why = rules::draw_from(chr, g_streaming, &streamed);
            static int lines = 0;
            if (was && ++lines <= 40) {
                LOG_INFO("[coop] Spectating: the floor under the followed teammate found by a cast (%s)",
                         why ? why : "the world is drawn from it");
            }
        } else if (why && cast_why && std::strcmp(cast_why, g_cast_why) != 0) {
            g_cast_why = cast_why;
            LOG_INFO("[coop] Spectating: no floor found under the followed teammate (%s)", cast_why);
        }
    }
    // Only from a floor of theirs that is loaded here: their draw groups come from it.
    g_pass_origin = g_streaming && g_origin_patched && !why ? chr : 0;
    uint32_t their_floor = 0;
    uint8_t their_block = 0xFF;
    g_view_block = g_pass_origin && g_block_test && read32(chr + 0x29C, &their_floor) && their_floor &&
                           read8(their_floor + 0xF, &their_block) && their_block < 64
                       ? their_block
                       : kNoBlock;
    fix_own_rate(g_pass_origin != 0);
    if (chr != g_drawn_for || (why ? why : "") != std::string(g_drawn_why) || (!why && streamed != g_drawn_streamed)) {
        g_drawn_for = chr;
        g_drawn_why = why ? why : "";
        g_drawn_streamed = streamed;
        if (why) {
            LOG_INFO("[coop] Spectating: the world stays drawn from our own floor (%s)", why);
        } else {
            LOG_INFO("[coop] Spectating: the world is drawn from the followed teammate's floor, and %s",
                     streamed       ? "what it shows is streamed in"
                     : g_streaming ? "only what our own floor streams is there (their floor adds nothing to it)"
                                   : "only what our own floor streams is there (SpectateStreaming off)");
        }
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

bool focused() {
    DWORD pid = 0;
    const HWND fg = GetForegroundWindow();
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

// Every connected controller's buttons. An empty controller slot is slow to ask (XInput looks for the device each
// time): the slots in use are read every 30 ms, the others every 2 s.
uint16_t poll_buttons() {
    static double polled = 0;
    static uint16_t buttons = 0;
    static bool connected[4] = {};
    static double scanned = -100;
    if (now() - polled >= 0.03) {
        polled = now();
        const bool scan = now() - scanned >= 2.0;
        if (scan) scanned = now();
        buttons = 0;
        for (DWORD i = 0; i < 4; ++i) {
            if (!connected[i] && !scan) continue;
            uint8_t st[16] = {};
            connected[i] = xinput_get_state(i, st) == 0;
            if (connected[i]) {
                uint16_t b = 0;
                std::memcpy(&b, st + 4, 2);
                buttons |= b;
            }
        }
    }
    return buttons;
}

// D-pad left or right (any controller), or the arrow keys, while the game has the focus: the previous or next
// teammate.
int switch_input() {
    static bool was_left = false, was_right = false;
    if (!focused()) {
        was_left = was_right = false;
        return 0;
    }
    const uint16_t buttons = poll_buttons();
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
    const Teammate* best = can.front();  // when no distance can be read
    float best_d = 3.4e38f;
    for (const Teammate* t : can) {
        game::Vec3 p{};
        if (!have_me) break;
        if (!game::chr_position(t->chr, &p)) continue;  // one unreadable character does not end the search
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
    g_view_at = now() + kViewDelay;
    g_told_waiting = false;
    const char* during = rules::state().invader             ? "our invasion"
                         : world::boss_fight()                ? "a boss fight"
                         : rules::state().teammate_boss_fight ? "a teammate's boss fight"
                                                              : "an invasion";
    LOG_INFO("[coop] We fell during %s with %d teammate(s) still standing: our death waits", during,
             living_teammates());
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
    if (g_rested) {  // a rest came between our fall and this event (about 3 s): the world has reset already
        release("someone rested at a bonfire");
        return true;
    }
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
    static const uint8_t draw_k[6] = {0x8B, 0x86, 0xEC, 0x00, 0x00, 0x00};   // mov eax,[esi+0xEC]
    static const uint8_t draw2_k[6] = {0x8B, 0xB6, 0xEC, 0x00, 0x00, 0x00};  // mov esi,[esi+0xEC]
    const bool look = g_view && bytes_match(kDrawChrSite, draw_k, sizeof(draw_k)) &&
                      bytes_match(kDrawChrSite2, draw2_k, sizeof(draw2_k)) &&
                      patch_call(kDrawChrSite, draw_k, sizeof(draw_k), reinterpret_cast<void*>(&hk_draw_chr)) &&
                      patch_call(kDrawChrSite2, draw2_k, sizeof(draw2_k), reinterpret_cast<void*>(&hk_draw_chr2));
    if (g_view && !look) {
        LOG_ERROR("[coop] Spectating: the draw parameter pick did not match this build (a far teammate's area keeps "
                  "our colour grading)");
    }
    static const uint8_t tint_k[5] = {0x8A, 0x40, 0x14, 0x24, 0x01};
    if (!patch_call(kDeathTintTest, tint_k, sizeof(tint_k), reinterpret_cast<void*>(&hk_death_tint))) {
        LOG_ERROR("[coop] Spectating: the death tint test did not match this build (the picture greys while spectating)");
    }
    static const uint8_t origin_k[6] = {0x8B, 0x4F, 0x3C, 0x8B, 0x41, 0x28};  // mov ecx,[edi+3Ch]; mov eax,[ecx+28h]
    g_origin_patched = g_view && patch_call(kChrPassOriginSite, origin_k, sizeof(origin_k),
                                            reinterpret_cast<void*>(&hk_chr_pass_origin));
    if (g_view && !g_origin_patched) {
        LOG_ERROR("[coop] Spectating: the character update pass did not match this build (the characters around a far "
                  "teammate stay as seen from us)");
    }
    // The enemies around a far teammate: their block's characters asked for after the game's asks, and their block
    // counted as ours by the pass.
    static const uint8_t block_k[10] = {0x8B, 0x8C, 0x24, 0xA0, 0x00, 0x00, 0x00, 0x39, 0x4E, 0x5C};
    if (g_origin_patched) {
        const bool asks = redirect_call(kActivationCallSite, kActivation, reinterpret_cast<void*>(&hk_activation));
        g_block_test = patch_call(kBlockTestSite, block_k, sizeof(block_k), reinterpret_cast<void*>(&hk_block_test));
        if (!asks || !g_block_test) {
            LOG_ERROR("[coop] Spectating: the character activation (%s) or the pass's block test (%s) did not match this "
                      "build (the enemies around a far teammate may stay out)",
                      asks ? "in" : "no match", g_block_test ? "in" : "no match");
        }
    }
    return true;
}

void set_streaming(bool on) {
    g_streaming = on;
    LOG_INFO("[coop] Spectating a far teammate loads and draws the world around them: %s", on ? "yes" : "no");
}

void set_teammates(const std::vector<Teammate>& teammates) { g_team = teammates; }

void tick() {
    if (!g_installed) return;
    view_extras();
    view_block_report();
    if (game::player() && !world::local_dead()) {
        if (g_hold) {
            g_hold = false;
            g_no_tint = 0;
            follow(nullptr);
            LOG_INFO("[coop] We live again while our death waited");
        }
        g_passed = false;
        g_rested = false;
        g_trip_release = false;
        return;
    }
    if (!g_hold) return;
    const auto& st = rules::state();
    if (st.invader ? st.team_invasion : fight_on()) {
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
        why = st.invader ? "every invader has fallen" : "every co-op player has fallen";
    } else if (st.invader && !st.team_invasion) {
        why = "the invasion is decided";
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
    if (g_hold || (world::local_dead() && !g_passed)) g_rested = true;
}

void trip_release() {
    g_trip_release = true;
    if (g_hold) release("the host's trip home");
}

bool holding() { return g_hold; }

bool installed() { return g_installed; }

const std::vector<Teammate>& teammates() { return g_team; }

void knell_view(uint32_t chr) {
    if (chr != g_knell_chr) g_drawn_for = 0;
    g_knell_chr = chr;
}

int switch_step() { return switch_input(); }

uint16_t pad_buttons() { return focused() ? poll_buttons() : 0; }

}  // namespace coop::spectate
