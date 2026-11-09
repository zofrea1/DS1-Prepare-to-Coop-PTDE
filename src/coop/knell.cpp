// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "knell.h"

#include "game.h"
#include "hook.h"
#include "items.h"
#include "log.h"
#include "motion.h"
#include "overlay.h"
#include "rules.h"
#include "session.h"
#include "spectate.h"
#include "text.h"
#include "world.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace coop::knell {

namespace {

#include "icon_art.inc"

// ---------------------------------------------------------------- addresses
// The level-up menu's apply (0xC75DD0, from the confirm at 0xC76328: eax = the menu's stats at +0x148, ecx = the
// menu): writes the nine values into PlayerGameData (Vitality +0x38, Attunement +0x40, Endurance +0x48, Strength
// +0x50, Dexterity +0x58, Resistance +0x80, Intelligence +0x60, Faith +0x68, humanity +0x78 and +0x7C), the souls
// (+0x8C = menu+0x178 - menu+0x174), the soul level (+0x88 = menu+0x16C), then works out everything that follows
// from them the way a level-up does: base HP from Vitality (0xEA5C10 with 100), the character's max HP (0xE6C850)
// and HP by the difference, attunement slots (101), stamina (0xEA44D0), equip load and the attack ratings
// (0xE4ABF0, 0xE46C20, 0xE4BFA0, 0xE47280). With one point less and the soul level one lower, it takes a level away.
constexpr uint32_t kLevelApply = 0x00C75DD0;
constexpr uint32_t kMenuSoulLevel = 0x16C, kMenuCost = 0x174, kMenuSouls = 0x178;
constexpr uint32_t kPgdStat[8] = {0x38, 0x40, 0x48, 0x50, 0x58, 0x80, 0x60, 0x68};  // the menu's order
constexpr uint32_t kPgdHumanity = 0x7C, kPgdHumanity2 = 0x78, kPgdHp = 0x0C, kPgdMaxHp = 0x10;
constexpr uint32_t kPgdLevel = 0x88, kPgdSouls = 0x8C;
constexpr uint32_t kPgdName = 0xA0;   // 16 characters
constexpr uint32_t kPgdClass = 0xC6;  // byte: CharaInitParam row 3000 + class (0xFA4E09)
constexpr uint32_t kSetHp = 0x00E688E0;    // stdcall(character, hp)
constexpr uint32_t kChrHp = 0x2D4, kChrMaxHp = 0x2D8;
// The HUD's warnings ("! WEAPON AT RISK !", 10010773, "! ARMOR AT RISK !", 10010775 ...): six text ids at
// MenuMan+0xFA0 with a timer each at +0xFB8; 0x1012590 writes at +0xFD4 and moves it on, the HUD dialog shows the one
// at +0xFD0, clears it and moves that on (0xC859F2).
constexpr uint32_t kMenuManVar = 0x013786D0;
constexpr uint32_t kHudIds = 0xFA0, kHudTimers = 0xFB8, kHudWrite = 0xFD4;
constexpr int kHudSlots = 6;
// ChrIns event animation (motion.cpp): esi = character, edi = animation, stdcall(loop).
constexpr uint32_t kEventAnimStart = 0x00E69590;
// "Warp Character and Copy Floor" (2004[42], 0xBC3482): 0xD71A90 warps the character with its vtable +0x30C
// (thiscall(pos x y z 1, rotation 0 angle 0 0, 1)) and sets +0x1FD bit 1, then 0xD61950 stands it on the other
// character's floor: 0xE6E580 (ecx = character, stdcall(the other's +0x29C)).
constexpr uint32_t kChrWarpSlot = 0x30C;
constexpr uint32_t kChrWarpFlags = 0x1FD;
constexpr uint32_t kSetFloor = 0x00E6E580;
constexpr uint32_t kChrFloor = 0x29C;
// The summon arrivals (global_event.lua): a summoned phantom or invader plays SummonMotion_wait's GetSummonAnimId
// (0xD60890): 6951, or 6952 when the player is a Warrior of Sunlight (PlayerGameData+0x10B == 3: the arrival with
// "Praise the Sun", Solaire's); a living player arriving plays 6950 (InGameStart's AliveMotion). 6951 and 6952 fade the
// character in from nothing (TAE event 193) with the summon sound. The arrival is one of the two summons: 6950, the
// plain rise at a bonfire, was a third choice until the 2026-11-04 test, where it came up in 9 of 15 arrivals and read
// as the summon effect missing ("just the default get up from bonfire").
constexpr int kArrivals[] = {6951, 6952};
constexpr int kArrivalCount = static_cast<int>(sizeof(kArrivals) / sizeof(kArrivals[0]));
constexpr int kWhiteArrival = 6951;
// The white arrival's glow is TAE event 120 (0xE61580), which picks its effect by the character's type (chr+0x70): the
// event's third value (120, the white glow) for a white phantom (1) or a hollow (8), the fourth (130, red) for a black
// phantom or an intruder (2, 12), and for anyone else what 0xE6A400 says of them (1 white, 2 black: their player
// data's +0x12D), else its first value, 0 for 6951: no effect. So a player arriving human had no glow and a hollow one
// had it (2026-11-04 test: "the white summon effect won't appear", some of the time). While a character plays the
// white arrival, a type with no effect of its own takes the white one (1.5.0).
constexpr uint32_t kChrTypeFxSite = 0x00E615E3;  // mov ecx,edi; call 0xE6A400; sub eax,1; je; sub eax,1; jne
constexpr uint32_t kChrTypeFxBack = 0x00E615FC;  // ebx = the effect, esi = the event's values, edi = the character
constexpr uint32_t kChrTypeFxSide = 0x00E6A400;  // ecx = character: 1 white, 2 black, else 0
// Where control comes back in each (the TAE's first JumpTable window that lets the player act again).
double arrival_length(int anim) { return anim == 6952 ? 5.4 : anim == 6951 ? 4.55 : 2.15; }
// The price, once the arrival has played out: the stagger of a broken poise ("Damage Middle", 2010: the damage voice
// 999999996 at once and the hit c44443 at 0.13 s; control back at about 1.5 s), the sound of a riposte's stab (the
// riposte 3201 of the weapon TAEs plays c999999992 as the blade goes in, the game's weapon-dependent stab), then the HUD
// line and the drain.
constexpr int kStagger = 2010;
constexpr int kRiposteSoundType = 1;  // 'c', the character's
constexpr int kRiposteSound = 999999992;
constexpr uint32_t kPlaySe = 0x00D6EA70;  // "Play SE" (2010[02], 0xBBFD6F): stdcall(Lua events, entity, type, id)
constexpr uint32_t kLuaEventsVar = 0x0137D790;
// The view's prompt is ours (overlay::set_prompt, at the bottom centre): who and where the bell would take us, and the
// buttons. Until the 2026-11-06 evening test it was the game's own action prompt, the one "Rest at bonfire" uses
// (0xD40310, a request with the action manager [0x137D7CC] every frame, "Summon yourself to <name>"), which never
// showed while the ring's crouch was held (the game shows no action prompt during an item's use) and whose button never
// answered (every choice that evening was A on the pad): the player could not tell whose summon sign the view was on,
// nor where it was. A on the pad or Enter chooses (Enter is hidden from the game meanwhile, as the arrows are).
constexpr double kBoxEvery = 0.25;  // the prompt's distances are brought up to date this often
// The way out (1.5.0). The user asked for the rings the eye orb's use shows around the player as they go down into its
// crouch to play when the warp takes them too. Made again on their own they never showed (the 2026-11-06 tests: an
// effect slot asked for from the frame tick, then in the TAE step, then a lasting effect at the character's feet, made
// every time and never seen), and the held crouch put back to its beginning (its animation records' time at 0) only
// rose as before, without them. 6060, taken for the eye orb's use (it is not: that animation is the player's state
// machine's choice from the goods' use animation byte, among 62xx-65xx), is a knockdown: played as an event animation
// it throws the player up into the air with a shake of the camera (6060 to 6067: launched, down, lying, up). The user
// kept that for the release ("the one where you got launched into the air was funny"): the choice plays it, the bell's
// toll taking the player, and the warp comes kReplayWarp on (the user's 1.5 s). The others are sent its start (motion, a
// knell animation) and play it on our character. Its events at 1.2 and 2.8 s are no item use; any use that comes on the
// way is swallowed for kReplaySwallow all the same (swallow_use).
constexpr int kReplayAnim = 6060;
constexpr double kReplayWarp = 1.5;
constexpr double kReplaySwallow = 3.0;

// The class's starting soul level and attributes (CharaInitParam 3000-3009: soulLv, baseVit, baseWil, baseEnd,
// baseStr, baseDex, baseDurability, baseMag, baseFai), in the menu's order.
struct ClassBase {
    const char* name;
    int level;
    int stat[8];
};
constexpr ClassBase kClasses[10] = {
    {"Warrior", 4, {11, 8, 12, 13, 13, 11, 9, 9}},     {"Knight", 5, {14, 10, 10, 11, 11, 10, 9, 11}},
    {"Wanderer", 3, {10, 11, 10, 10, 14, 12, 11, 8}},  {"Thief", 5, {9, 11, 9, 9, 15, 10, 12, 11}},
    {"Bandit", 4, {12, 8, 14, 14, 9, 11, 8, 10}},      {"Hunter", 4, {11, 9, 11, 12, 14, 11, 9, 9}},
    {"Sorcerer", 3, {8, 15, 8, 9, 11, 8, 15, 8}},      {"Pyromancer", 1, {10, 12, 11, 12, 9, 12, 10, 8}},
    {"Cleric", 2, {11, 11, 9, 12, 8, 11, 8, 14}},      {"Deprived", 6, {11, 11, 11, 11, 11, 11, 11, 11}},
};
constexpr const char* kStatName[8] = {"Vitality", "Attunement", "Endurance", "Strength",
                                      "Dexterity", "Resistance", "Intelligence", "Faith"};
constexpr char kStatLetter[8] = {'V', 'A', 'E', 'S', 'D', 'R', 'I', 'F'};

constexpr double kOpenDelay = 1.2;   // the ring's crouch settles before the view opens
constexpr double kGoDelay = 1.2;     // before the player goes, when the crouch was not held
// The ring's crouch, held while the view is open (1.5.0, the user's design): the others saw nothing of a player choosing
// in the view, only someone standing there. The Knell's use is the eye orb's (goodsUseAnim 6: 6060, down into a crouch,
// the use at 1.2 s, up with its light, the player free again at 2.47 s). From the ring, at the bottom of that crouch,
// our character's animations are held where they are (motion::hold_anims: the animation set's speed at 0), and so is
// our character in the others' games (motion kHold). It is still the item's use, an ordinary animation: hits land
// (and close the view), and the game takes no roll and no weapon or item switch during it. B or Esc, a hit or any
// other close lets it go, here and there, and the ring plays on: the player rises with its light. The choice plays the
// bell's knockdown (kReplayAnim; the warp as the player is up, kRiseWarp, only should that not start). The 2026-11-06 test held the crouch with event
// animation 6066 instead, which is no crouch (the character lay on their back, twitching), took no hits, rolled out at
// once (an event animation can be cancelled), and showed no action prompt.
// 6060 lets the player act again 1.27 s after the use. The warp came at 1.5 s until the 2026-11-06 21:20 test, and in
// between the player stood free and their weapons switched ("just moments after the player gets out of the crouch",
// the user, who asked for the warp right as the stand-up ends): it now comes at 1.2 s, before the player is free, and
// the D-pad and the arrow keys stay hidden from the game until the warp.
constexpr double kRiseWarp = 1.2;
constexpr double kViewLimit = 120.0;
constexpr double kPosFresh = 4.0;    // a teammate's reported position is used up to this old
constexpr uint16_t kPadB = 0x2000;   // XINPUT_GAMEPAD_B
constexpr uint16_t kPadA = 0x1000;   // XINPUT_GAMEPAD_A
constexpr uint16_t kPadLeft = 0x0004, kPadRight = 0x0008;

// ---------------------------------------------------------------- state
enum class State { Idle, Opening, View, Going, Loading, Arriving };
State g_state = State::Idle;
double g_at = 0;           // Opening / Going: when the next step is due
double g_view_since = 0;
uint64_t g_target_id = 0;  // the teammate the view shows, or the owner of the summon sign it shows
bool g_target_sign = false;
uint32_t g_shown_chr = 0;  // the character the camera was given (0: ours)
int g_view_hp = 0;         // our HP when the view opened (a hit closes it)
bool g_cancel_down = true; // B or Esc held (only a fresh press closes the view)
bool g_confirm_down = true; // A held (only a fresh press chooses)
struct Destination {
    uint64_t id = 0;
    bool sign = false;  // their summon sign, not the teammate
    std::wstring name;
    int map = -1;
    float pos[3] = {};
    float angle = 0;
};
Destination g_dest;
double g_load_since = 0;
bool g_load_seen = false;
double g_stand_since = 0;
// The ring's crouch: held, since when, and why the view closed (for the log of its end).
bool g_hold_on = false;
double g_hold_at = 0;
const char* g_hold_end = "";
// The arrival playing: its animation, since when, and whether the price follows (a join's arrival has none).
int g_arrival_anim = 0;
double g_arrival_at = 0;
bool g_arrival_price = false;
double g_join_at = 0;       // a join's arrival is due (0 none): when it was armed
double g_join_stood_at = 0;  // when we first stood in the host's world after it was armed (0 not yet)
constexpr int kGameRise = 6950;         // the game's own rise at a bonfire, which a guest's placement plays
constexpr double kJoinRiseWait = 1.5;   // the arrival starts by itself once we stood this long without that rise
                                        // (the rise came 0.15 to 1.2 s after the load in the 2026-11-04 test)
bool g_join_arrival = false;            // the arrival playing is a join's (the game's rise may still come over it)
double g_box_at = 0;                     // the view's prompt was last drawn (0: draw it now)
double g_swallow_until = 0;              // the replay's own item use is swallowed until then
std::mt19937 g_rng(static_cast<uint32_t>(GetTickCount64()));
// DeathKnellSoulLevel (1.5.0, default off): going to a teammate takes a soul level (the bell's first price) instead of
// their souls and humanity.
bool g_soul_level = false;

double now() { return GetTickCount64() / 1000.0; }

std::string narrow(const std::wstring& w) {
    char out[256];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out, sizeof(out), nullptr, nullptr);
    return n > 0 ? std::string(out) : std::string();
}

uint32_t pgd() { return game::player_game_data(); }

int pgd_int(uint32_t off) {
    uint32_t v = 0;
    const uint32_t p = pgd();
    return p && read32(p + off, &v) ? static_cast<int>(v) : 0;
}

int chr_int(uint32_t chr, uint32_t off) {
    uint32_t v = 0;
    return chr && read32(chr + off, &v) ? static_cast<int>(v) : 0;
}

// ---------------------------------------------------------------- the D-pad while the view is open
// D-pad left and right switch the weapons; while the view is open they choose the teammate instead, so the game's
// own reads (XInputGetState, imported by ordinal 2 from XINPUT1_3) do not see them. The view reads the real state.
using XGetStateFn = DWORD(WINAPI*)(DWORD, void*);
XGetStateFn g_xget = nullptr;
volatile LONG g_mask_dpad = 0;

DWORD WINAPI hk_xinput_get_state(DWORD index, void* state) {
    const DWORD r = g_xget(index, state);
    if (r == 0 && state && g_mask_dpad) {
        uint16_t b = 0;
        std::memcpy(&b, static_cast<uint8_t*>(state) + 4, 2);
        b &= static_cast<uint16_t>(~(kPadLeft | kPadRight));
        std::memcpy(static_cast<uint8_t*>(state) + 4, &b, 2);
    }
    return r;
}

// The arrow keys choose too (spectate::switch_step reads them through our own import of GetAsyncKeyState); the game
// reads the keyboard through DirectInput (DirectInput8Create) and USER32's GetAsyncKeyState / GetKeyboardState, and
// switched weapons with them while the view was open (2026-11-04, on the keyboard). While the D-pad is masked, left
// and right are hidden from those reads too: the game's two USER32 imports are wrapped, and so is every DirectInput
// device's GetDeviceState and GetDeviceData (slots 9 and 10, patched in dinput8's own vtables, found through a device
// made here: the game's were made before us), for a keyboard's 256-byte state and its key events.
using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
using DevGetStateFn = HRESULT(WINAPI*)(void*, DWORD, void*);
using DevGetDataFn = HRESULT(WINAPI*)(void*, DWORD, void*, DWORD*, DWORD);
GetAsyncKeyStateFn g_get_async = nullptr;
GetKeyboardStateFn g_get_keyboard = nullptr;
constexpr int kDiSlots = 2;  // the A and the W device vtables
void** g_di_vtables[kDiSlots] = {};
DevGetStateFn g_di_get_state[kDiSlots] = {};
DevGetDataFn g_di_get_data[kDiSlots] = {};
// Enter chooses (1.5.0): it is hidden from the game the same way, both Enter keys.
constexpr uint8_t kDikLeft = 0xCB, kDikRight = 0xCD, kDikReturn = 0x1C, kDikPadEnter = 0x9C;

bool masked_vk(int vk) { return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_RETURN; }
bool masked_dik(DWORD dik) { return dik == kDikLeft || dik == kDikRight || dik == kDikReturn || dik == kDikPadEnter; }

SHORT WINAPI hk_get_async_key_state(int vk) {
    const SHORT r = g_get_async(vk);
    return g_mask_dpad && masked_vk(vk) ? 0 : r;
}

BOOL WINAPI hk_get_keyboard_state(PBYTE keys) {
    const BOOL r = g_get_keyboard(keys);
    if (r && keys && g_mask_dpad) {
        keys[VK_LEFT] = 0;
        keys[VK_RIGHT] = 0;
        keys[VK_RETURN] = 0;
    }
    return r;
}

int di_slot(void* dev) {
    void** vt = dev ? *reinterpret_cast<void***>(dev) : nullptr;
    for (int i = 0; i < kDiSlots; ++i) {
        if (g_di_vtables[i] && g_di_vtables[i] == vt) return i;
    }
    return 0;
}

HRESULT WINAPI hk_di_get_state(void* dev, DWORD size, void* data) {
    const int i = di_slot(dev);
    const HRESULT r = g_di_get_state[i](dev, size, data);
    if (SUCCEEDED(r) && data && size == 256 && g_mask_dpad) {
        for (const uint8_t dik : {kDikLeft, kDikRight, kDikReturn, kDikPadEnter}) static_cast<uint8_t*>(data)[dik] = 0;
    }
    return r;
}

// DIDEVICEOBJECTDATA (dwOfs, dwData, dwTimeStamp, dwSequence, uAppData: 20 bytes in DirectInput 8): a keyboard's
// events carry the DIK code in dwOfs; those of the two arrows and Enter are taken out.
HRESULT WINAPI hk_di_get_data(void* dev, DWORD cb, void* data, DWORD* inout, DWORD flags) {
    const int i = di_slot(dev);
    const HRESULT r = g_di_get_data[i](dev, cb, data, inout, flags);
    if (SUCCEEDED(r) && data && inout && cb >= 8 && g_mask_dpad) {
        uint8_t* p = static_cast<uint8_t*>(data);
        DWORD kept = 0;
        for (DWORD k = 0; k < *inout; ++k) {
            DWORD ofs = 0;
            std::memcpy(&ofs, p + k * cb, 4);
            if (masked_dik(ofs)) continue;
            if (kept != k) std::memmove(p + kept * cb, p + k * cb, cb);
            ++kept;
        }
        *inout = kept;
    }
    return r;
}

// The game's import of `name` from a DLL whose name starts with `dll` points at `hook` (the old target in `*orig`).
bool patch_import(const char* dll, const char* name, void* hook, void** orig) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_strnicmp(reinterpret_cast<const char*>(base + imp->Name), dll, std::strlen(dll)) != 0 ||
            !imp->OriginalFirstThunk) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), name) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) return false;
            *orig = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
            return *orig != nullptr;
        }
    }
    return false;
}

bool patch_vtable_slot(void** vt, int slot, void* hook, void** orig) {
    DWORD old = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = vt[slot];
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    return true;
}

// dinput8's device vtables, through a keyboard device made here with each interface (A and W), released again.
int patch_dinput_devices() {
    HMODULE di = LoadLibraryW(L"dinput8.dll");
    if (!di) return 0;
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, const GUID&, void**, void*);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(di, "DirectInput8Create"));
    if (!create) return 0;
    static const GUID kIidA = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    static const GUID kIidW = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    static const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    const GUID* iids[kDiSlots] = {&kIidA, &kIidW};
    int patched = 0;
    for (int i = 0; i < kDiSlots; ++i) {
        void* dinput = nullptr;
        if (FAILED(create(GetModuleHandleW(nullptr), 0x0800, *iids[i], &dinput, nullptr)) || !dinput) continue;
        void** dvt = *reinterpret_cast<void***>(dinput);
        using CreateDeviceFn = HRESULT(WINAPI*)(void*, const GUID&, void**, void*);
        void* dev = nullptr;
        if (SUCCEEDED(reinterpret_cast<CreateDeviceFn>(dvt[3])(dinput, kSysKeyboard, &dev, nullptr)) && dev) {
            void** vt = *reinterpret_cast<void***>(dev);
            bool seen = false;
            for (int k = 0; k < i; ++k) seen = seen || g_di_vtables[k] == vt;
            if (!seen) {
                g_di_vtables[i] = vt;
                void* o1 = nullptr;
                void* o2 = nullptr;
                if (patch_vtable_slot(vt, 9, reinterpret_cast<void*>(&hk_di_get_state), &o1) &&
                    patch_vtable_slot(vt, 10, reinterpret_cast<void*>(&hk_di_get_data), &o2)) {
                    g_di_get_state[i] = reinterpret_cast<DevGetStateFn>(o1);
                    g_di_get_data[i] = reinterpret_cast<DevGetDataFn>(o2);
                    ++patched;
                }
            }
            using ReleaseFn = ULONG(WINAPI*)(void*);
            reinterpret_cast<ReleaseFn>((*reinterpret_cast<void***>(dev))[2])(dev);
        }
        using ReleaseFn = ULONG(WINAPI*)(void*);
        reinterpret_cast<ReleaseFn>(dvt[2])(dinput);
    }
    return patched;
}

bool patch_xinput_import() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_strnicmp(reinterpret_cast<const char*>(base + imp->Name), "xinput", 6) != 0 || !imp->OriginalFirstThunk) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            bool get_state = false;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                get_state = IMAGE_ORDINAL(names->u1.Ordinal) == 2;
            } else {
                const auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                get_state = std::strcmp(reinterpret_cast<const char*>(by_name->Name), "XInputGetState") == 0;
            }
            if (!get_state) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) return false;
            g_xget = reinterpret_cast<XGetStateFn>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(&hk_xinput_get_state);
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
            return g_xget != nullptr;
        }
    }
    return false;
}

// The pad's own poll (PadDevice, the call at 0x643B5B: XInputGetState through the import's thunk 0xABA542). FPS
// Unlock's controller follow (fps-unlock src/ptde/controller.cpp, built into the same DLL, installed after us) puts its
// own call there, which reads XInput itself: the import patched above is not what the pad's poll reads, so on a
// controller the D-pad reached the game all the while the view was open, and the weapon switches it asked for came out
// the moment the player could act again (2026-11-06: "just moments after the player gets out of the crouch", in the
// 21:20 and 21:46 tests too, with the warp at 1.5 and at 1.2 s). The call is wrapped, whatever it calls (the thunk, or
// the controller follow's), and D-pad left and right are taken out of what it read while the view is open.
constexpr uint32_t kPadPollCall = 0x00643B5B;
using PollGetStateFn = DWORD(WINAPI*)(DWORD, void*);
PollGetStateFn g_poll_next = nullptr;

DWORD WINAPI hk_poll_get_state(DWORD index, void* state) {
    const DWORD r = g_poll_next(index, state);
    if (r == 0 && state && g_mask_dpad) {
        uint16_t b = 0;
        std::memcpy(&b, static_cast<uint8_t*>(state) + 4, 2);
        b &= static_cast<uint16_t>(~(kPadLeft | kPadRight));
        std::memcpy(static_cast<uint8_t*>(state) + 4, &b, 2);
    }
    return r;
}

// 1 wrapped just now, 0 wrapped already, -1 the call is not there.
int wrap_pad_poll() {
    uint8_t cur[5] = {};
    __try {
        std::memcpy(cur, reinterpret_cast<const void*>(kPadPollCall), sizeof(cur));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (cur[0] != 0xE8) return -1;
    int32_t rel = 0;
    std::memcpy(&rel, cur + 1, 4);
    const uint32_t target = kPadPollCall + 5 + static_cast<uint32_t>(rel);
    const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&hk_poll_get_state));
    if (target == mine) return 0;
    g_poll_next = reinterpret_cast<PollGetStateFn>(target);
    uint8_t code[5] = {0xE8};
    const int32_t to = static_cast<int32_t>(mine - (kPadPollCall + 5));
    std::memcpy(code + 1, &to, 4);
    if (!patch(kPadPollCall, cur, code, sizeof(code))) return -1;
    LOG_INFO("[coop] Death Knell: the pad's poll (0x643B5B, it called %08X%s) wrapped: D-pad left/right hidden from the "
             "game while the view is open", target, target == 0x00ABA542 ? ": the game's XInput import" : "");
    return 1;
}

// ---------------------------------------------------------------- the levels a character gained, in order
// The game keeps the attributes and the soul level, not the order the levels came in. Prepare to Co-op writes it
// down from 1.5.0 on: each level-up seen while playing adds its attribute(s) to the character's record
// (PTDE-Coop-Levels.txt beside the game, one line per character: name and class, then the letters VAESDRIF in the
// order gained). A drain takes the last one recorded; a level from before the record is chosen at random among the
// levels gained above the class's start.
struct Snapshot {
    bool valid = false;
    std::wstring key;
    int level = 0;
    int stat[8] = {};
};
Snapshot g_snap;
std::map<std::wstring, std::string> g_records;
bool g_records_loaded = false;
double g_track_at = 0;

std::wstring records_path() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    const size_t slash = dir.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? L"" : dir.substr(0, slash + 1)) + L"PTDE-Coop-Levels.txt";
}

void load_records() {
    g_records_loaded = true;
    FILE* f = nullptr;
    if (_wfopen_s(&f, records_path().c_str(), L"rb") != 0 || !f) return;
    char line[4096];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        const size_t tab = s.rfind('\t');
        if (tab == std::string::npos) continue;
        wchar_t key[128] = {};
        if (MultiByteToWideChar(CP_UTF8, 0, s.substr(0, tab).c_str(), -1, key, 127) <= 0) continue;
        std::string letters;
        for (char c : s.substr(tab + 1)) {
            if (std::strchr("VAESDRIF", c)) letters.push_back(c);
        }
        g_records[key] = letters;
    }
    std::fclose(f);
    LOG_INFO("[coop] Death Knell: the level records of %d character(s) read", static_cast<int>(g_records.size()));
}

void save_records() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, records_path().c_str(), L"wb") != 0 || !f) {
        LOG_ERROR("[coop] Death Knell: PTDE-Coop-Levels.txt could not be written");
        return;
    }
    for (const auto& r : g_records) std::fprintf(f, "%s\t%s\r\n", narrow(r.first).c_str(), r.second.c_str());
    std::fclose(f);
}

bool read_raw(uint32_t p, wchar_t* name, uint8_t* cls) {
    __try {
        std::memcpy(name, reinterpret_cast<const void*>(static_cast<uintptr_t>(p + kPgdName)), 32);
        *cls = *ptr<uint8_t>(p + kPgdClass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool read_character(Snapshot* s) {
    const uint32_t p = pgd();
    if (!p) return false;
    wchar_t name[17] = {};
    uint8_t cls = 0xFF;
    if (!read_raw(p, name, &cls)) return false;
    name[16] = 0;
    if (!name[0] || cls > 9) return false;
    s->key = std::wstring(name) + L" (" + text::widen(kClasses[cls].name) + L")";
    s->level = pgd_int(kPgdLevel);
    for (int i = 0; i < 8; ++i) s->stat[i] = pgd_int(kPgdStat[i]);
    s->valid = s->level > 0;
    return s->valid;
}

// Twice a second while we stand in the world: the levels gained since the last look go on the record.
void track_levels() {
    if (now() - g_track_at < 0.5) return;
    g_track_at = now();
    Snapshot cur;
    if (!game::player() || game::loading() || !read_character(&cur)) return;
    if (!g_records_loaded) load_records();
    if (g_snap.valid && g_snap.key == cur.key && cur.level > g_snap.level) {
        std::string gained;
        int sum = 0;
        for (int i = 0; i < 8; ++i) {
            for (int d = g_snap.stat[i]; d < cur.stat[i]; ++d) gained.push_back(kStatLetter[i]);
            sum += cur.stat[i] - g_snap.stat[i];
        }
        if (!gained.empty() && sum == cur.level - g_snap.level) {
            g_records[cur.key] += gained;
            save_records();
            LOG_INFO("[coop] Death Knell: %s gained level(s) %d -> %d (%s)", narrow(cur.key).c_str(), g_snap.level,
                     cur.level, gained.c_str());
        }
    }
    g_snap = cur;
}

// ---------------------------------------------------------------- the price
void apply_levels(const int32_t* stats, const uint8_t* menu) {
    __asm {
        mov eax, stats
        mov ecx, menu
        mov edx, kLevelApply
        call edx
    }
}

void set_hp(uint32_t chr, int hp) {
    using Fn = void(__stdcall*)(uint32_t, int);
    reinterpret_cast<Fn>(kSetHp)(chr, hp);
}

// A lower Vitality lowers the HP by as much as the max: the HP is filled first and put back after, capped by the new
// max, so the price is a level and never a death. False when the game faulted.
bool apply_drain(uint32_t me, uint32_t p, const int32_t* stats, const uint8_t* menu) {
    __try {
        const int hp = chr_int(me, kChrHp);
        const int humanity2 = pgd_int(kPgdHumanity2);
        set_hp(me, chr_int(me, kChrMaxHp));
        *ptr<int32_t>(p + kPgdHp) = pgd_int(kPgdMaxHp);
        apply_levels(stats, menu);
        *ptr<int32_t>(p + kPgdHumanity2) = humanity2;
        const int back = std::max(1, std::min(hp, chr_int(me, kChrMaxHp)));
        set_hp(me, back);
        *ptr<int32_t>(p + kPgdHp) = std::min(back, pgd_int(kPgdMaxHp));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Takes one soul level. Returns the attribute that went (0..7), or -1 when nothing could go (the class's starting
// level, or no attribute above its start), `why` saying which.
int drain(const char** why) {
    *why = nullptr;
    const uint32_t p = pgd();
    const uint32_t me = game::player();
    Snapshot cur;
    if (!p || !me || !read_character(&cur)) {
        *why = "no character";
        return -1;
    }
    const int cls = *ptr<uint8_t>(p + kPgdClass);
    const ClassBase& base = kClasses[cls];
    if (cur.level <= base.level) {
        *why = "the class's starting level";
        return -1;
    }
    int above[8] = {};
    int total = 0;
    for (int i = 0; i < 8; ++i) {
        above[i] = std::max(0, cur.stat[i] - base.stat[i]);
        total += above[i];
    }
    if (total == 0) {
        *why = "no attribute above the class's start";
        return -1;
    }
    if (!g_records_loaded) load_records();
    int pick = -1;
    const char* how = "a level from before the record, at random";
    std::string& rec = g_records[cur.key];
    while (!rec.empty() && pick < 0) {
        const char c = rec.back();
        rec.pop_back();
        for (int i = 0; i < 8; ++i) {
            if (kStatLetter[i] == c && above[i] > 0) pick = i;
        }
    }
    if (pick >= 0) {
        how = "the last level recorded";
    } else {
        // Every level gained above the start is as likely: an attribute by how far it is above its start.
        int roll = std::uniform_int_distribution<int>(0, total - 1)(g_rng);
        for (int i = 0; i < 8 && pick < 0; ++i) {
            if (roll < above[i]) pick = i;
            roll -= above[i];
        }
    }
    int32_t stats[9];
    for (int i = 0; i < 8; ++i) stats[i] = cur.stat[i];
    stats[8] = pgd_int(kPgdHumanity);
    stats[pick] -= 1;
    alignas(16) uint8_t menu[0x200] = {};
    const int souls = pgd_int(kPgdSouls);
    *reinterpret_cast<int32_t*>(menu + kMenuSoulLevel) = cur.level - 1;
    *reinterpret_cast<int32_t*>(menu + kMenuCost) = 0;
    *reinterpret_cast<int32_t*>(menu + kMenuSouls) = souls;
    if (!apply_drain(me, p, stats, menu)) {
        *why = "the game's level change failed";
        return -1;
    }
    save_records();
    g_snap.valid = false;  // the tracker starts again from the drained character
    LOG_INFO("[coop] Death Knell: %s paid soul level %d -> %d: %s %d -> %d (%s); HP %d, souls %d", narrow(cur.key).c_str(),
             cur.level, pgd_int(kPgdLevel), kStatName[pick], cur.stat[pick], pgd_int(kPgdStat[pick]), how,
             chr_int(me, kChrHp), pgd_int(kPgdSouls));
    return pick;
}

void hud_warning(uint32_t text_id) {
    uint32_t mm = 0, at = 0;
    if (!read32(kMenuManVar, &mm) || !mm || !read32(mm + kHudWrite, &at) || at >= static_cast<uint32_t>(kHudSlots)) {
        LOG_INFO("[coop] Death Knell: the HUD's warning line is not there");
        return;
    }
    *ptr<uint32_t>(mm + kHudIds + at * 4) = text_id;
    *ptr<uint32_t>(mm + kHudTimers + at * 4) = 0;
    *ptr<uint32_t>(mm + kHudWrite) = (at + 1) % kHudSlots;
}

bool start_event_anim(uint32_t me, int anim, uint32_t looped = 0) {
    const uint32_t start = kEventAnimStart;
    __try {
        __asm {
            push esi
            push edi
            mov esi, me
            mov edi, anim
            push looped
            mov eax, start
            call eax
            pop edi
            pop esi
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool play_sound(int entity, int kind, int id) {
    uint32_t lua = 0;
    if (!read32(kLuaEventsVar, &lua) || !lua) return false;
    __try {
        __asm {
            push id
            push kind
            push entity
            push lua
            mov eax, kPlaySe
            call eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// The animation a character plays now (ChrCtrl+0x350, see rules.cpp action_of), -1 none.
int playing_anim(uint32_t chr) {
    uint32_t ctrl = 0, id = 0;
    if (!chr || !read32(chr + 0x28, &ctrl) || !ctrl || !read32(ctrl + 0x350, &id)) return -1;
    return static_cast<int>(id);
}

// The white arrival's glow for any character type (kChrTypeFxSite): asked only when the type gave no effect.
bool __stdcall white_arrival(uint32_t chr) {
    return playing_anim(chr) == kWhiteArrival || motion::held_event_anim(chr) == kWhiteArrival;
}

__declspec(naked) void hk_chr_type_fx() {
    __asm {
        mov ecx, edi
        mov eax, kChrTypeFxSide
        call eax
        cmp eax, 1
        je fx_white
        cmp eax, 2
        je fx_black
        test ebx, ebx         // the event's first value: the effect of a type with none of its own
        jnz fx_back
        push edi
        call white_arrival    // stdcall
        test al, al
        jz fx_back
    fx_white:
        mov ebx, dword ptr [esi + 8]
        jmp fx_back
    fx_black:
        mov ebx, dword ptr [esi + 0Ch]
    fx_back:
        mov eax, kChrTypeFxBack
        jmp eax
    }
}

bool patch_chr_type_fx() {
    static const uint8_t expect[17] = {0x8B, 0xCF, 0xE8, 0x16, 0x8E, 0x00, 0x00, 0x83, 0xE8,
                                       0x01, 0x74, 0x0A, 0x83, 0xE8, 0x01, 0x75, 0x08};
    uint8_t code[17];
    std::memset(code, 0x90, sizeof(code));
    code[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_chr_type_fx) - (kChrTypeFxSite + 5));
    std::memcpy(code + 1, &rel, 4);
    return patch(kChrTypeFxSite, expect, code, sizeof(code));
}

// One of the game's summon arrivals at random, which the others see too (1.5.0). Returns it, 0 when none started. An
// invader arrives as an evil spirit does (the user's design): the summoned phantom's arrival only (never Solaire's), whose
// glow TAE event 120 picks by the character's type, the red one (130) for a black phantom, as an invader is in the
// host's world (the game made them one on arrival; only a co-op guest keeps their own type, rules.cpp hk_arrival), in
// their game and in the others'.
int play_arrival(const char* why) {
    const uint32_t me = game::player();
    if (!me) return 0;
    const int anim = session::invader() ? kWhiteArrival
                                        : kArrivals[std::uniform_int_distribution<int>(0, kArrivalCount - 1)(g_rng)];
    motion::send_knell_arrival(anim);
    if (!start_event_anim(me, anim)) {
        LOG_ERROR("[coop] Death Knell: the arrival animation could not be started");
        return 0;
    }
    LOG_INFO("[coop] Summon arrival %d (%s)", anim, why);
    return anim;
}

// ---------------------------------------------------------------- the ring's crouch
// At the ring (the item's use, the bottom of its crouch): held here and in the others' games.
void start_hold() {
    const uint32_t me = game::player();
    if (!me || world::local_dead() || game::loading()) return;
    g_hold_on = motion::hold_anims(me, true);
    g_hold_at = now();
    if (g_hold_on) motion::send_knell_hold(true);
    LOG_INFO("[coop] Death Knell: the crouch %s", g_hold_on ? "held while the view is open" : "NOT held (no animation set)");
}

// Let go, here and there: the ring plays on and the player rises with its light.
void release_hold(const char* why) {
    if (!g_hold_on) return;
    g_hold_on = false;
    const uint32_t me = game::player();
    const bool done = me && !game::loading() && motion::hold_anims(me, false);
    motion::send_knell_hold(false);
    LOG_INFO("[coop] Death Knell: the crouch let go (%s)%s", why, done ? "" : "; our character was not there");
}

// While the crouch is held: let go once the view is closed (by any way out), and held again when something else
// (the game) set its speed back meanwhile.
void hold_tick() {
    if (!g_hold_on) return;
    if (g_state != State::Opening && g_state != State::View) {
        release_hold(g_hold_end);
        return;
    }
    const uint32_t me = game::player();
    if (!me || game::loading()) {
        // The character is gone. The others still have the hold until they are told.
        g_hold_on = false;
        motion::send_knell_hold(false);
        LOG_INFO("[coop] Death Knell: the crouch let go (our character is gone)");
        return;
    }
    if (!motion::anims_held(me)) {
        motion::hold_anims(me, true);
        static int lines = 0;
        if (++lines <= 20) LOG_INFO("[coop] Death Knell: the crouch's speed was set back by the game; held again");
    }
}

void arrive() {
    g_arrival_anim = play_arrival(session::invader() ? "the Death Knell, as an evil spirit" : "the Death Knell");
    g_arrival_at = now();
    // An invader pays nothing (the user's design): no stagger, no debuffs, no Bloody Mary, and nothing is said.
    g_arrival_price = !session::invader();
    g_join_arrival = false;
    g_state = State::Arriving;
    LOG_INFO("[coop] Death Knell: arrived by %s; %s", narrow(g_dest.name).c_str(),
             g_arrival_price ? "the price once the arrival is over" : "no price (we invade)");
}

// The poise breaks, the riposte's stab sounds, and the bell takes its price (1.5.0, the user's design): the mod's death
// debuffs, a Crimson Blossom for us and a Bloody Mary for the whole session (the host counts it), as a death gives
// them; and going to a teammate also takes every soul and all humanity we carry, as the Darksign does (no bloodstain).
// A summon sign one of them left (Vigil Soapstone) is the co-operative way: the debuffs only. With
// DeathKnellSoulLevel, going to a teammate takes a soul level instead of the souls and humanity (the first price).
// The price's line names only what was taken (Grok dcb9359e): the souls and humanity (going to a teammate), the
// Crimson Blossom when one was given (not while invading, or with five held), and the hardier enemies when the
// session's Bloody Marys went up (not with five already). Both are the price with DeathDebuffs or BloodyMary off too:
// those settings are about deaths. Empty when nothing was.
std::wstring price_line(bool souls, bool blossom, bool mary) {
    std::wstring what;
    if (souls) what = L"your souls, your humanity";
    if (blossom) what += what.empty() ? L"a Crimson Blossom" : L" and a Crimson Blossom";
    if (mary) what += what.empty() ? L"the enemies grow hardier" : L"; the enemies grow hardier";
    return what.empty() ? std::wstring() : L"The Death Knell's price: " + what;
}

// A guest's price line waits for the host's answer about the Bloody Mary (its next State), as long as
// session::knell_bloody_mary_answer allows.
struct PendingPrice {
    bool waiting = false;
    bool souls = false;
    bool blossom = false;
};
PendingPrice g_pending_price;

void show_price(bool souls, bool blossom, bool mary) {
    const std::wstring line = price_line(souls, blossom, mary);
    if (!line.empty()) text::show(line);
}

void pending_price_tick() {
    if (!g_pending_price.waiting) return;
    const int answer = session::knell_bloody_mary_answer();
    if (answer == 0) return;
    g_pending_price.waiting = false;
    show_price(g_pending_price.souls, g_pending_price.blossom, answer > 0);
    LOG_INFO("[coop] Death Knell: the host's game %s the Bloody Mary", answer > 0 ? "counted" : "did not count");
}

void pay_price() {
    const uint32_t me = game::player();
    if (!me) return;
    motion::send_knell_arrival(kStagger);
    const bool staggered = start_event_anim(me, kStagger);
    const bool sound = play_sound(10000, kRiposteSoundType, kRiposteSound);
    const bool sign = g_dest.sign;
    const bool blossom = world::give_knell_blossom();
    const session::KnellMary mary = session::knell_bloody_mary(sign);
    bool souls = false;
    if (!sign && g_soul_level) {
        hud_warning(kDrainedText);
        const char* why = nullptr;
        const int stat = drain(&why);
        if (stat < 0) LOG_INFO("[coop] Death Knell: no soul level was taken (%s)", why ? why : "?");
    } else {
        if (!sign) {
            const int was_souls = game::souls(), was_humanity = game::humanity();
            game::set_souls(0);
            game::set_humanity(0);
            souls = true;
            LOG_INFO("[coop] Death Knell: %d souls and %d humanity lost, as to the Darksign", was_souls, was_humanity);
        }
        if (souls || blossom || mary != session::KnellMary::None) hud_warning(kDebuffedText);
        if (mary == session::KnellMary::Asked) {
            g_pending_price = PendingPrice{true, souls, blossom};
        } else {
            show_price(souls, blossom, mary == session::KnellMary::Added);
        }
    }
    LOG_INFO("[coop] Death Knell: the price for %s (stagger %d%s, riposte sound c%d%s, Crimson Blossom %s, Bloody Mary "
             "%s)", sign ? "a summon sign" : g_soul_level ? "a teammate, a soul level" : "a teammate",
             kStagger, staggered ? "" : " FAILED", kRiposteSound, sound ? "" : " FAILED",
             blossom ? "given" : "not given (invading, or five held)",
             mary == session::KnellMary::Added   ? "counted"
             : mary == session::KnellMary::Asked ? "asked of the host's game"
                                                 : "not counted (five already)");
}

// Until the arrival lets the player act again: its animation leaves ChrCtrl+0x350, or its length passes.
void arriving_tick() {
    const uint32_t me = game::player();
    if (!me || game::loading()) {
        g_state = State::Idle;
        LOG_INFO("[coop] Arrival: interrupted by a load; no price");
        return;
    }
    if (world::local_dead()) {
        g_state = State::Idle;
        LOG_INFO("[coop] Arrival: we fell; no price");
        return;
    }
    const double t = now() - g_arrival_at;
    // A join's arrival that came before the game's own rise: the rise comes over it a moment later, so the arrival
    // starts again on our character (the others play it already).
    if (g_join_arrival && g_arrival_anim != kGameRise && t < 2.0 && motion::held_event_anim(me) == kGameRise) {
        g_join_arrival = false;
        const bool again = start_event_anim(me, g_arrival_anim);
        g_arrival_at = now();
        LOG_INFO("[coop] Joined: the game's rise came over the arrival %.2f s in: arrival %d %s", t, g_arrival_anim,
                 again ? "started again" : "could not be started again");
        return;
    }
    if (t < 0.8) return;
    const bool playing = g_arrival_anim && playing_anim(me) == g_arrival_anim;
    if (playing && t < arrival_length(g_arrival_anim) + 1.0) return;
    if (!playing && g_arrival_anim && t < arrival_length(g_arrival_anim) - 0.3) return;  // not yet where it lets go
    g_state = State::Idle;
    if (g_arrival_price) pay_price();
}

// ---------------------------------------------------------------- where the others are
const char* area_name(int map) {
    switch (static_cast<uint32_t>(map) & 0xFFFF0000u) {
        case 0x0A000000: return "the Depths";
        case 0x0A010000: return "the Undead Burg";
        case 0x0A020000: return "Firelink Shrine";
        case 0x0B000000: return "the Painted World of Ariamis";
        case 0x0C000000: return "Darkroot";
        case 0x0C010000: return "Oolacile";
        case 0x0D000000: return "the Catacombs";
        case 0x0D010000: return "the Tomb of the Giants";
        case 0x0D020000: return "the Great Hollow";
        case 0x0E000000: return "Blighttown";
        case 0x0E010000: return "the Demon Ruins";
        case 0x0F000000: return "Sen's Fortress";
        case 0x0F010000: return "Anor Londo";
        case 0x10000000: return "New Londo";
        case 0x11000000: return "the Duke's Archives";
        case 0x12000000: return "the Kiln of the First Flame";
        case 0x12010000: return "the Undead Asylum";
        default: return nullptr;
    }
}

bool valid_map(int map) {
    const int area = (static_cast<uint32_t>(map) >> 24) & 0xFF;
    return map != -1 && area >= 10 && area <= 18;
}

// Still one of WorldChrMan's player characters (slots 1 on).
bool remote_character(uint32_t chr) {
    if (!chr) return false;
    for (uint32_t c : game::remote_player_chrs()) {
        if (c == chr) return true;
    }
    return false;
}

// The players the view shows: the co-op teammates, or, while we invade, the co-op players of the world, whom an
// invader may watch but not go to (1.5.0, the user's design: "to spy on their targets").
const std::vector<spectate::Teammate>& people() {
    return session::invader() ? spectate::targets() : spectate::teammates();
}

std::vector<const spectate::Teammate*> candidates() {
    std::vector<const spectate::Teammate*> out;
    for (const spectate::Teammate& t : people()) {
        if (t.alive) out.push_back(&t);
    }
    std::sort(out.begin(), out.end(),
              [](const spectate::Teammate* a, const spectate::Teammate* b) { return a->id < b->id; });
    return out;
}

const spectate::Teammate* find(uint64_t id) {
    for (const spectate::Teammate& t : people()) {
        if (t.id == id && t.alive) return &t;
    }
    return nullptr;
}

// The places the bell can take us (1.5.0): every co-op teammate standing, then every summon sign the others left
// (session::summon_signs, their Vigil Soapstones), each by player. Good for the frame they are made in.
struct Choice {
    uint64_t id = 0;
    bool sign = false;
    const spectate::Teammate* mate = nullptr;  // the teammate (sign false)
    session::SummonSign at;                    // the sign (sign true)
    std::wstring name;
    int map = -1;
};
std::vector<Choice> choices() {
    std::vector<Choice> out;
    for (const spectate::Teammate* t : candidates()) {
        Choice c;
        c.id = t->id;
        c.mate = t;
        c.name = t->name;
        c.map = t->map;
        out.push_back(c);
    }
    std::vector<session::SummonSign> signs = session::summon_signs();
    std::sort(signs.begin(), signs.end(),
              [](const session::SummonSign& a, const session::SummonSign& b) { return a.owner < b.owner; });
    for (const session::SummonSign& sg : signs) {
        Choice c;
        c.id = sg.owner;
        c.sign = true;
        c.at = sg;
        c.name = sg.name;
        c.map = sg.map;
        out.push_back(c);
    }
    return out;
}

const Choice* find_choice(const std::vector<Choice>& can, uint64_t id, bool sign) {
    for (const Choice& c : can) {
        if (c.id == id && c.sign == sign) return &c;
    }
    return nullptr;
}

std::wstring where(const spectate::Teammate& t) {
    if (t.chr && remote_character(t.chr)) return L"";
    const char* area = area_name(t.map);
    return area ? L" (far away, in " + text::widen(area) + L")" : L" (far away)";
}

// "<name>" for a teammate (and where), "<name>'s summon sign (<area>)" for a sign.
std::wstring label(const Choice& c) {
    if (!c.sign) return c.name + where(*c.mate);
    const char* area = area_name(c.map);
    return c.name + L"'s summon sign" + (area ? L" (" + text::widen(area) + L")" : std::wstring());
}

// Why the Death Knell cannot be rung now, or null.
const char* refusal() {
    if (!session::in_session()) return "Only in a co-operative session";
    if (!game::player() || game::loading()) return "Not while loading";
    if (world::local_dead() || spectate::holding() || world::in_limbo()) return "Not while fallen";
    if (session::trip_slot_busy()) return "Not while the party travels";
    if (world::boss_fight()) return "Not in a boss fight";
    if (choices().empty()) return session::invader() ? "No one to watch" : "No co-operator or summon sign to go to";
    return nullptr;
}

void say(const std::wstring& line) { text::show(line); }

// " 14 m away" (and how far up or down when that is more than a storey), from our character to `q`.
std::wstring distance_text(const game::Vec3& q) {
    game::Vec3 me{};
    if (!game::position(&me, nullptr)) return L"";
    const float dx = q.x - me.x, dy = q.y - me.y, dz = q.z - me.z;
    const long d = std::lround(std::sqrt(dx * dx + dy * dy + dz * dz));
    std::wstring s = std::to_wstring(std::max(d, 1L)) + L" m away";
    if (std::fabs(dy) >= 3.0f) {
        s += L", " + std::to_wstring(std::lround(std::fabs(dy))) + (dy > 0 ? L" m above you" : L" m below you");
    }
    return s;
}

// The prompt's second line: where the choice is.
std::wstring where_line(const Choice& c) {
    const char* area = area_name(c.map);
    const std::wstring in = area ? text::widen(area) : std::wstring();
    if (c.sign) {
        // Drawn here (gold while shown) when its block is in use here, and then in our world's coordinates.
        if (rules::map_in_use(c.map)) {
            const game::Vec3 q{c.at.pos[0], c.at.pos[1], c.at.pos[2]};
            const std::wstring d = distance_text(q);
            return (in.empty() ? L"" : in + L", ") + d + L" (the sign glowing gold)";
        }
        return in.empty() ? L"Far away" : L"Far away, in " + in;
    }
    game::Vec3 q{};
    if (c.mate->chr && remote_character(c.mate->chr) && game::chr_position(c.mate->chr, &q)) {
        return (in.empty() ? L"" : in + L", ") + distance_text(q);
    }
    return in.empty() ? L"Far away" : L"Far away, in " + in;
}

// The view's prompt (bottom centre): the choice, where it is, and the buttons.
// While we invade, a co-op player is only watched (no A); their summon signs can be gone to.
void show_box(const Choice& c, int at, int count) {
    const bool watch_only = !c.sign && session::invader();
    std::vector<std::wstring> lines;
    if (watch_only) {
        lines.push_back(L"Watching " + c.name);
    } else {
        lines.push_back(L"Summon yourself to " + (c.sign ? c.name + L"'s summon sign" : c.name));
    }
    lines.push_back(where_line(c));
    std::wstring keys;
    if (count > 1) keys = L"Left / Right: " + std::to_wstring(at + 1) + L" of " + std::to_wstring(count) + L"        ";
    if (!watch_only) keys += L"A or Enter: go        ";
    keys += L"B or Esc: back";
    lines.push_back(keys);
    overlay::set_prompt(lines);
    g_box_at = now();
}

void hide_box() {
    overlay::set_prompt({});
    g_box_at = 0;
}

// The action prompt at the bottom of the screen ("Rest at bonfire"). A talk object (a bonfire) registers its request
// with the action manager [0x137D7CC] while the player is in range, and cancels it itself (0x456C40, from 0xD4E04B)
// when the player walks out of range. After a teleport, or a load that takes the player away, that out-of-range pass
// never runs, so the prompt stayed until another object of the same kind replaced it. Cancelled here as the game
// does, and MenuMan's prompt (+0x814 text, +0x820 state) cleared as 0xC2A010 does; anything still in range
// registers again on the next frame.
constexpr uint32_t kActionManVar = 0x0137D7CC;
constexpr uint32_t kActionCancel = 0x00456C40;  // eax = the action manager

bool cancel_action(uint32_t mgr) {
    __try {
        __asm {
            mov eax, mgr
            mov ecx, kActionCancel
            call ecx
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void clear_action_prompt(const char* why) {
    uint32_t mgr = 0, mm = 0;
    if (read32(kActionManVar, &mgr) && mgr) cancel_action(mgr);
    if (read32(kMenuManVar, &mm) && mm) {
        *ptr<int32_t>(mm + 0x820) = -1;
        *ptr<int32_t>(mm + 0x814) = -1;
    }
    static int lines = 0;
    if (++lines <= 50) LOG_INFO("[coop] Action prompt cleared (%s)", why);
}

// ---------------------------------------------------------------- the view
void show_target(const Choice& c) {
    g_target_id = c.id;
    g_target_sign = c.sign;
    const uint32_t chr = !c.sign && c.mate->chr && remote_character(c.mate->chr) ? c.mate->chr : 0;
    g_shown_chr = chr;
    spectate::knell_view(chr);
    // A summon sign: the camera turns around it (spectate.cpp), and it glows gold.
    if (c.sign) {
        spectate::knell_point(c.at.pos, c.map, c.at.floor_map, c.at.floor_index);
    } else {
        spectate::knell_point(nullptr, -1, -1, -1);
    }
    session::set_knell_sign(c.sign ? c.id : 0);
    g_box_at = 0;  // the prompt is drawn anew on this frame (view_tick)
    LOG_INFO("[coop] Death Knell: viewing %s%s", narrow(label(c)).c_str(),
             chr ? "" : c.sign ? " (a summon sign: the camera goes to it; it glows gold)" : " (not in our world: no camera)");
}

void close_view(const char* why) {
    if (g_state == State::View) LOG_INFO("[coop] Death Knell: view closed (%s)", why);
    g_hold_end = why;
    if (g_state == State::View || g_state == State::Opening) release_hold(why);
    hide_box();
    session::set_knell_sign(0);
    spectate::knell_view(0);
    spectate::knell_point(nullptr, -1, -1, -1);
    g_shown_chr = 0;
    InterlockedExchange(&g_mask_dpad, 0);
    if (g_state == State::View || g_state == State::Opening) g_state = State::Idle;
}

void open_view() {
    if (const char* why = refusal()) {
        say(text::widen(why));
        LOG_INFO("[coop] Death Knell: the view does not open (%s)", why);
        g_hold_end = "the view did not open";
        g_state = State::Idle;
        return;
    }
    const auto can = choices();
    // The nearest teammate whose character stands in our world, else the first.
    const Choice* best = &can.front();
    game::Vec3 me{};
    float best_d = 3.4e38f;
    if (game::position(&me, nullptr)) {
        for (const Choice& c : can) {
            game::Vec3 q{};
            if (c.sign || !c.mate->chr || !remote_character(c.mate->chr) || !game::chr_position(c.mate->chr, &q)) {
                continue;
            }
            const float d = (q.x - me.x) * (q.x - me.x) + (q.y - me.y) * (q.y - me.y) + (q.z - me.z) * (q.z - me.z);
            if (d < best_d) {
                best_d = d;
                best = &c;
            }
        }
    }
    g_state = State::View;
    g_view_since = now();
    g_view_hp = chr_int(game::player(), kChrHp);
    g_cancel_down = true;
    g_confirm_down = true;
    InterlockedExchange(&g_mask_dpad, 1);
    LOG_INFO("[coop] Death Knell: view open (%d co-operator(s) and summon sign(s))", static_cast<int>(can.size()));
    show_target(*best);
}

void confirm();

bool cancel_pressed() {
    DWORD pid = 0;
    const HWND fg = GetForegroundWindow();
    const bool focus = fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
    const bool down = focus && ((spectate::pad_buttons() & kPadB) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000));
    const bool pressed = down && !g_cancel_down;
    g_cancel_down = down;
    return pressed;
}

// A on the pad or Enter (read through our own import: the game's is masked meanwhile). Null when neither was pressed
// on this frame.
const char* confirm_pressed() {
    DWORD pid = 0;
    const HWND fg = GetForegroundWindow();
    const bool focus = fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
    const bool pad = focus && (spectate::pad_buttons() & kPadA);
    const bool key = focus && (GetAsyncKeyState(VK_RETURN) & 0x8000);
    const bool down = pad || key;
    const bool pressed = down && !g_confirm_down;
    g_confirm_down = down;
    return pressed ? (pad ? "A on the pad" : "Enter") : nullptr;
}

void view_tick() {
    const char* why = refusal();
    const uint32_t me = game::player();
    if (!why && me && chr_int(me, kChrHp) < g_view_hp) why = "we were hit";
    if (!why && now() - g_view_since > kViewLimit) why = "time limit";
    if (!why && cancel_pressed()) why = "B or Esc";
    if (why) {
        close_view(why);
        if (std::strcmp(why, "B or Esc") != 0 && std::strcmp(why, "time limit") != 0) say(L"The Death Knell falls silent");
        return;
    }
    g_view_hp = std::max(g_view_hp, chr_int(me, kChrHp));  // healing is no hit
    const auto can = choices();
    int at = -1;
    for (size_t i = 0; i < can.size(); ++i) {
        if (can[i].id == g_target_id && can[i].sign == g_target_sign) at = static_cast<int>(i);
    }
    const int step = spectate::switch_step();
    if (at < 0) {
        show_target(can.front());  // the one shown fell or left, or the sign went
        return;
    }
    if (step != 0 && can.size() > 1) {
        show_target(can[(at + step + static_cast<int>(can.size())) % can.size()]);
        return;
    }
    // Their character came into our world, or left it: the camera follows.
    const Choice& shown = can[at];
    // An invader watching a co-op player: that player hears their own knell toll (1.7.0, session.cpp).
    if (!shown.sign && session::invader()) session::knell_watching(shown.id);
    const uint32_t chr = !shown.sign && shown.mate->chr && remote_character(shown.mate->chr) ? shown.mate->chr : 0;
    if (chr != g_shown_chr) {
        g_shown_chr = chr;
        spectate::knell_view(chr);
        g_box_at = 0;
    }
    if (shown.sign) spectate::knell_point(shown.at.pos, shown.map, shown.at.floor_map, shown.at.floor_index);  // moved?
    if (g_box_at == 0 || now() - g_box_at >= kBoxEvery) show_box(shown, at, static_cast<int>(can.size()));
    if (const char* how = confirm_pressed()) {
        LOG_INFO("[coop] Death Knell: chosen (%s)", how);
        confirm();
    }
}

// On our way (the choice): the crouch is let go and the bell's knockdown throws the player up (kReplayAnim), here and in
// the others' games, and the warp takes them kReplayWarp on. Should it not start, the ring plays on as before (the
// player rises with its light) and the warp comes as they are up.
// The prompt goes; the sign shown stays gold, the camera stays where it is, and the D-pad and the arrows stay hidden
// from the game until the warp (close_view).
void set_going(const char* what) {
    hide_box();
    g_state = State::Going;
    const bool held = g_hold_on;
    release_hold("chosen: the bell's knockdown");
    const uint32_t me = game::player();
    bool replay = false;
    if (me && !game::loading()) {
        motion::send_knell_arrival(kReplayAnim);  // the others play it on our character
        g_swallow_until = now() + kReplaySwallow;
        replay = start_event_anim(me, kReplayAnim);
    }
    g_at = now() + (replay ? kReplayWarp : held ? kRiseWarp : kGoDelay);
    LOG_INFO("[coop] Death Knell: going to %s (the warp in %.1f s, %s)", what, g_at - now(),
             replay ? "thrown up by the bell's knockdown" : "the knockdown did not start: as the player rises");
}

// A or Enter on the prompt: the teammate shown is where we go. The camera stays on them until the warp, so it does
// not swing back to us and out again, and the arrival's sound is heard where we arrive.
void confirm() {
    const auto can = choices();
    const Choice* c = find_choice(can, g_target_id, g_target_sign);
    if (!c) {
        say(L"The Death Knell finds no one there");
        return;
    }
    // An invader goes to no one, only to their summon signs (the prompt offers no A for a person).
    if (!c->sign && session::invader()) {
        LOG_INFO("[coop] Death Knell: we invade; %s is only watched", narrow(c->name).c_str());
        return;
    }
    if (c->sign) {
        g_dest = Destination{};
        g_dest.id = c->id;
        g_dest.sign = true;
        g_dest.name = c->name;
        g_dest.map = c->map;
        std::copy(std::begin(c->at.pos), std::end(c->at.pos), g_dest.pos);
        g_dest.angle = c->at.angle;
        set_going((narrow(c->name) + "'s summon sign").c_str());
        return;
    }
    const spectate::Teammate* t = c->mate;
    if (t->boss_fight) {
        say(t->name + L" is in a boss fight");
        LOG_INFO("[coop] Death Knell: %s is in a boss fight; not going", narrow(t->name).c_str());
        return;
    }
    // Where they stand and the map they report do not go together during a cutscene (the crow's flight: Firelink's
    // landing point in the Asylum's map; the load put us there in the Asylum, and we fell, 2026-11-04), nor during a
    // load (no character of theirs: no position yet).
    if (t->cutscene || t->loading) {
        say(t->name + (t->cutscene ? L" is in a cutscene" : L" is travelling"));
        LOG_INFO("[coop] Death Knell: %s is %s; not going", narrow(t->name).c_str(),
                 t->cutscene ? "in a cutscene" : "in a load");
        return;
    }
    g_dest = Destination{};
    g_dest.id = t->id;
    g_dest.name = t->name;
    g_dest.map = t->map;
    std::copy(std::begin(t->pos), std::end(t->pos), g_dest.pos);
    set_going(narrow(t->name).c_str());
}

bool do_warp(uint32_t me, uint32_t fn, const float* pp, const float* pr, uint32_t floor) {
    __try {
        __asm {
            push 1
            push pr
            push pp
            mov ecx, me
            mov eax, fn
            call eax
        }
        *ptr<uint8_t>(me + kChrWarpFlags) |= 2;
        __asm {
            push floor
            mov ecx, me
            mov eax, kSetFloor
            call eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// In our world: warped onto them and stood on their floor, as "Warp Character and Copy Floor" does.
bool warp_here(uint32_t target) {
    const uint32_t me = game::player();
    game::Vec3 q{};
    float angle = 0;
    game::Vec3 mine{};
    if (!me || !game::chr_position(target, &q) || !game::position(&mine, &angle)) return false;
    uint32_t floor = 0, vt = 0, fn = 0;
    if (!read32(target + kChrFloor, &floor) || !floor || rules::draw_from(target) ||
        !read32(me, &vt) || !read32(vt + kChrWarpSlot, &fn) || !fn) {
        return false;
    }
    // Their map is loaded here only because the view keeps it (spectate.cpp): the load takes us there, as before the
    // view loaded far maps, and as the game itself takes a player into a map it has not loaded around them.
    if (rules::view_only_floor(target)) {
        LOG_INFO("[coop] Death Knell: %s stands in a map loaded only for the view: going by a load",
                 narrow(g_dest.name).c_str());
        return false;
    }
    // Nor onto a floor whose collision is not loaded here (we would fall through it): a load places us properly.
    if (!rules::floor_ready(floor)) {
        LOG_INFO("[coop] Death Knell: the floor under %s is not loaded here: going by a load", narrow(g_dest.name).c_str());
        return false;
    }
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) return false;
    alignas(16) float pos[4] = {q.x, q.y, q.z, 1.0f};
    alignas(16) float rot[4] = {0.0f, angle, 0.0f, 0.0f};
    if (!do_warp(me, fn, pos, rot, floor)) {
        LOG_ERROR("[coop] Death Knell: the warp failed");
        return false;
    }
    clear_action_prompt("the Death Knell's warp");
    LOG_INFO("[coop] Death Knell: warped to (%.2f, %.2f, %.2f) beside %s, from (%.2f, %.2f, %.2f)", q.x, q.y, q.z,
             narrow(g_dest.name).c_str(), mine.x, mine.y, mine.z);
    return true;
}

// An invader's far Death Knell is a load in another player's world, and every such load empties the key array the
// inventory uses there (MoveMapStep 0xEFC470 -> 0xEFC6C3: 0x51C520 with clear 1). A co-op guest gets its keys back
// from the session's key poll (session.cpp, guest_carry_keys), which an invader does not run: a key item it held in
// the invasion was gone for the rest of it (Grok review 44169466). The world's keys it holds when the load starts go
// back once it stands; nothing of the host's is lent. The co-op key items (the Crimson Blossom, the Bloody Mary) have
// their own counts.
// The give-back follows the load itself, not the knell's arrival (invader_keys_tick, every frame): a load that took over
// two minutes gave the arrival up after MoveMapStep had already emptied the array, and the keys were lost with it. And
// it goes only into the visit array: the invasion can end, or the trip home win, before we have stood in that world,
// and the next stand is then at home, where the array in use is the own world's; a key, an Ember or a Lord Soul held
// only in the visit array would have been written into the own save (Grok review 212423ba).
std::vector<int32_t> g_invader_keys;
bool g_keys_load_seen = false;   // the load began after the snapshot: the visit array has been emptied
double g_keys_stand_since = 0;   // standing (a player, no load) since, 0 not

void drop_invader_keys(const char* why) {
    LOG_INFO("[coop] Death Knell (invading): the key items held before the load are not given back (%s)", why);
    g_invader_keys.clear();
}

void give_back_invader_keys() {
    if (g_invader_keys.empty()) return;
    const std::vector<int32_t> held = items::key_items();
    std::string back, kept;
    for (int32_t id : g_invader_keys) {
        if (std::find(held.begin(), held.end(), id) != held.end()) {
            kept += " " + std::to_string(id);
        } else if (items::give(id, 1)) {
            back += " " + std::to_string(id);
        }
    }
    LOG_INFO("[coop] Death Knell (invading): key items after the load: given back:%s; still held:%s",
             back.empty() ? " none" : back.c_str(), kept.empty() ? " none" : kept.c_str());
    g_invader_keys.clear();
}

// Once the load has begun and we have stood 0.5 s after it, still invading and in that world.
void invader_keys_tick() {
    if (g_invader_keys.empty()) return;
    if (game::loading() || !game::player()) {
        g_keys_load_seen = true;
        g_keys_stand_since = 0;
        return;
    }
    if (!g_keys_load_seen) return;  // not begun yet (loading_tick drops the snapshot if it never does)
    if (!items::keys_are_visit()) {
        drop_invader_keys("we are home: the key items of that world stay there");
        return;
    }
    if (!session::invader()) {
        drop_invader_keys("the invasion is over");
        return;
    }
    if (g_keys_stand_since == 0) g_keys_stand_since = now();
    if (now() - g_keys_stand_since < 0.5) return;
    give_back_invader_keys();
}

// An invader's chr type through the knell's load (1.6.0). The game turns a summoned player into a phantom of the join
// type on arrival (0xEA15B0: chr type 2 for a black phantom, set with 0x455FC0) and keeps their own type pending in
// GameDataMan+0x60, which the next load applies: in vanilla that load is always the trip home. The Death Knell's load
// into a far summon sign is a load in the host's world, and it made the invader human (chr 2/2 before, 0/0 after,
// 2026-11-07 13:31): no red glow, and the others saw a human character there. So the invader's own type is the pending
// one for that load, and once they stand it is set back on the character if it changed anyway, and the pending type is
// put back for the trip home.
constexpr uint32_t kGdmNextChrType = 0x60;
int g_invader_type = -1;        // our chr type before the load (-1: no invader's knell load pending)
int g_invader_pending = -1;     // GameDataMan+0x60 then
bool g_type_load_seen = false;
double g_type_armed_at = 0;

void put_back_pending_type(const char* why) {
    if (const uint32_t gdm = game::game_data_man()) *ptr<int>(gdm + kGdmNextChrType) = g_invader_pending;
    LOG_INFO("[coop] Death Knell (invading): the pending chr type is %d again (%s)", g_invader_pending, why);
    g_invader_type = -1;
}

void invader_type_tick() {
    if (g_invader_type < 0) return;
    if (game::loading() || !game::player()) {
        g_type_load_seen = true;
        return;
    }
    if (!g_type_load_seen) {
        if (now() - g_type_armed_at > 10.0) put_back_pending_type("the load never began");
        return;
    }
    if (!session::invader() || game::own_world()) {
        put_back_pending_type("we are home, or the invasion is over");
        return;
    }
    const int type = game::chr_type();
    if (type != g_invader_type) {
        const bool ok = rules::set_player_chr_type(g_invader_type);
        LOG_INFO("[coop] Death Knell (invading): chr type %d after the load, set back to %d%s", type, g_invader_type,
                 ok ? "" : " (failed)");
    } else {
        LOG_INFO("[coop] Death Knell (invading): still chr type %d after the load", type);
    }
    put_back_pending_type("after the load");
}

// A load into `map`, stood at `pos` facing `angle` (rules::place_next_load_at).
void load_into(int map_id, const float* pos, float angle, const std::wstring& what) {
    const int map = static_cast<int>(static_cast<uint32_t>(map_id) & 0xFFFF0000u);
    g_invader_keys.clear();
    g_keys_load_seen = false;
    g_keys_stand_since = 0;
    if (g_invader_type >= 0) put_back_pending_type("another load");
    if (session::invader()) {
        const int type = game::chr_type();
        const uint32_t gdm = game::game_data_man();
        // A phantom's type only (an invader is a black phantom, 2): 0 or 8 is a body of its own, and none to keep.
        if (gdm && type != 0 && type != 8 && type >= 0) {
            g_invader_type = type;
            g_invader_pending = *ptr<int>(gdm + kGdmNextChrType);
            g_type_load_seen = false;
            g_type_armed_at = now();
            *ptr<int>(gdm + kGdmNextChrType) = g_invader_type;
            LOG_INFO("[coop] Death Knell (invading): chr type %d kept for the load (the pending type was %d)",
                     g_invader_type, g_invader_pending);
        }
        for (int32_t id : items::key_items()) {
            if (id < items::kFirst || id > items::kLast) g_invader_keys.push_back(id);
        }
        if (!g_invader_keys.empty()) {
            std::string ids;
            for (int32_t id : g_invader_keys) ids += " " + std::to_string(id);
            LOG_INFO("[coop] Death Knell (invading): key items held before the load:%s", ids.c_str());
        }
    }
    rules::place_next_load_at(map, pos[0], pos[1], pos[2], angle);
    world::warp_now(map, -1);
    g_state = State::Loading;
    g_load_since = now();
    g_load_seen = false;
    g_stand_since = 0;
    LOG_INFO("[coop] Death Knell: loading into map %08X (%s) for %s at (%.2f, %.2f, %.2f)", static_cast<uint32_t>(map),
             area_name(map) ? area_name(map) : "?", narrow(what).c_str(), pos[0], pos[1], pos[2]);
}

// A summon sign in a block in use here: onto the collision under it (found as the game finds our own floor, by a cast
// from the sign's point; rules::floor_at), as the warp onto a teammate stands us on theirs.
bool warp_to_point(const float* p, float angle) {
    const uint32_t me = game::player();
    uint32_t vt = 0, fn = 0;
    if (!me || !read32(me, &vt) || !read32(vt + kChrWarpSlot, &fn) || !fn) return false;
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    const uint32_t floor = rules::floor_at(p);
    if (!floor) return false;
    // A map loaded only because the view showed the sign there: the load takes us, as for a far teammate.
    if (rules::view_only_hit(floor)) {
        LOG_INFO("[coop] Death Knell: %s's summon sign is in a map loaded only for the view: going by a load",
                 narrow(g_dest.name).c_str());
        return false;
    }
    if (!rules::floor_ready(floor)) {
        LOG_INFO("[coop] Death Knell: the floor under %s's summon sign is not loaded here: going by a load",
                 narrow(g_dest.name).c_str());
        return false;
    }
    game::Vec3 mine{};
    game::position(&mine, nullptr);
    alignas(16) float pos[4] = {p[0], p[1], p[2], 1.0f};
    alignas(16) float rot[4] = {0.0f, angle, 0.0f, 0.0f};
    if (!do_warp(me, fn, pos, rot, floor)) {
        LOG_ERROR("[coop] Death Knell: the warp failed");
        return false;
    }
    clear_action_prompt("the Death Knell's warp");
    LOG_INFO("[coop] Death Knell: warped to %s's summon sign at (%.2f, %.2f, %.2f), from (%.2f, %.2f, %.2f)",
             narrow(g_dest.name).c_str(), p[0], p[1], p[2], mine.x, mine.y, mine.z);
    return true;
}

void go_to_sign() {
    const std::vector<session::SummonSign> signs = session::summon_signs();
    const session::SummonSign* sg = nullptr;
    for (const session::SummonSign& x : signs) {
        if (x.owner == g_dest.id) sg = &x;
    }
    if (!sg) {
        say(L"The summon sign is gone");
        LOG_INFO("[coop] Death Knell: %s's summon sign is gone", narrow(g_dest.name).c_str());
        close_view("the sign is gone");
        g_state = State::Idle;
        return;
    }
    g_dest.map = sg->map;  // it may have moved since
    std::copy(std::begin(sg->pos), std::end(sg->pos), g_dest.pos);
    g_dest.angle = sg->angle;
    if (warp_to_point(g_dest.pos, g_dest.angle)) {
        close_view("arrived");
        arrive();
        return;
    }
    close_view("loading");
    if (!valid_map(g_dest.map)) {
        say(L"The Death Knell cannot find the summon sign");
        LOG_INFO("[coop] Death Knell: %s's summon sign is in no map we know (%08X)", narrow(g_dest.name).c_str(),
                 static_cast<uint32_t>(g_dest.map));
        g_state = State::Idle;
        return;
    }
    load_into(g_dest.map, g_dest.pos, g_dest.angle, g_dest.name + L"'s summon sign");
}

void going_tick() {
    if (now() < g_at) return;
    if (const char* why = refusal()) {
        say(text::widen(why));
        LOG_INFO("[coop] Death Knell: not going (%s)", why);
        close_view("not going");
        g_state = State::Idle;
        return;
    }
    if (g_dest.sign) {
        go_to_sign();
        return;
    }
    const spectate::Teammate* t = find(g_dest.id);
    if (!t) {
        say(L"The Death Knell finds no one there");
        LOG_INFO("[coop] Death Knell: %s is gone", narrow(g_dest.name).c_str());
        close_view("they are gone");
        g_state = State::Idle;
        return;
    }
    if (t->cutscene || t->loading) {
        say(t->name + (t->cutscene ? L" is in a cutscene" : L" is travelling"));
        LOG_INFO("[coop] Death Knell: %s went into a %s; not going", narrow(t->name).c_str(),
                 t->cutscene ? "cutscene" : "load");
        close_view(t->cutscene ? "their cutscene" : "their load");
        g_state = State::Idle;
        return;
    }
    if (t->chr && remote_character(t->chr) && warp_here(t->chr)) {
        close_view("arrived");  // the camera comes back to us, beside them
        arrive();
        return;
    }
    close_view("loading");
    // Elsewhere: a load into their map, stood where they last said they stand.
    if (!valid_map(t->map) || t->pos_at <= 0 || now() - t->pos_at > kPosFresh || !std::isfinite(t->pos[0]) ||
        !std::isfinite(t->pos[1]) || !std::isfinite(t->pos[2])) {
        say(L"The Death Knell cannot find " + t->name);
        close_view("no position");
        LOG_INFO("[coop] Death Knell: no fresh position of %s (map %08X, %.1f s old)", narrow(t->name).c_str(),
                 static_cast<uint32_t>(t->map), t->pos_at > 0 ? now() - t->pos_at : -1.0);
        g_state = State::Idle;
        return;
    }
    // The map of the floor their game has them on when they report it: on a map boundary the game's own map can still
    // name the one they came from (the elevator between Firelink and the Undead Burg).
    g_dest.map = valid_map(t->floor_map) ? t->floor_map : t->map;
    if (g_dest.map != t->map) {
        LOG_INFO("[coop] Death Knell: %s stands on a floor of map %08X (their game's map is %08X): loading into that "
                 "one", narrow(t->name).c_str(), static_cast<uint32_t>(g_dest.map), static_cast<uint32_t>(t->map));
    }
    std::copy(std::begin(t->pos), std::end(t->pos), g_dest.pos);
    float angle = 0;
    game::Vec3 mine{};
    game::position(&mine, &angle);
    load_into(g_dest.map, g_dest.pos, angle, t->name);
}

void loading_tick() {
    if (game::loading() || !game::player()) {
        g_load_seen = true;
        g_stand_since = 0;
        if (now() - g_load_since > 120.0) {
            // The arrival is given up; key items held before the load still come back once we stand (invader_keys_tick).
            LOG_INFO("[coop] Death Knell: the load took over two minutes; no arrival");
            g_state = State::Idle;
        }
        return;
    }
    if (!g_load_seen) {
        if (now() - g_load_since > 8.0) {
            LOG_INFO("[coop] Death Knell: the load into %08X never began", static_cast<uint32_t>(g_dest.map));
            say(L"The Death Knell falls silent");
            g_invader_keys.clear();  // nothing was emptied
            g_state = State::Idle;
        }
        return;
    }
    // Standing in the new map: the arrival plays once the world is up.
    if (g_stand_since == 0) g_stand_since = now();
    if (now() - g_stand_since < 0.5) return;
    if (world::local_dead()) {
        LOG_INFO("[coop] Death Knell: arrived fallen; no arrival");
        g_state = State::Idle;
        return;
    }
    game::Vec3 at{};
    if (game::position(&at, nullptr)) {
        LOG_INFO("[coop] Death Knell: standing in map %08X at (%.2f, %.2f, %.2f)",
                 static_cast<uint32_t>(game::map_id()), at.x, at.y, at.z);
    }
    arrive();
}

// ---------------------------------------------------------------- the icons
// The atlas the icons' sprites read (Icon21: menu/menu_4.tpf, 1024 x 512 DXT5, 11 levels), as the game hands it to
// D3DX. EquIcon_2159's and EquIcon_2160's cells are empty in it; the Death Knell's bell and the Vigil Soapstone are laid
// over their cells' blocks level by level, one after the other (the patches are built by tools/build_icons.py, see
// icon_art.inc; at the smaller levels the two share a block, each laid over what is there).
constexpr unsigned kAtlasSize = 699216;
constexpr uint32_t kAtlasHash = 0xB4985C7C;  // FNV-1a
constexpr DWORD kFilterNone = 1;            // D3DX_FILTER_NONE
using LoadFromMemoryFn = HRESULT(WINAPI*)(IDirect3DSurface9*, const PALETTEENTRY*, const RECT*, const void*, D3DFORMAT,
                                          UINT, const PALETTEENTRY*, const RECT*, DWORD, D3DCOLOR);
std::mutex g_icon_mutex;
IDirect3DTexture9* g_icon_atlas = nullptr;  // one reference held until it is painted
bool g_icon_failed = false;

uint32_t fnv1a(const uint8_t* p, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

void decode_block(const uint8_t* b, uint8_t out[16][4]) {
    uint8_t alpha[8];
    alpha[0] = b[0];
    alpha[1] = b[1];
    if (alpha[0] > alpha[1]) {
        for (int i = 1; i < 7; ++i) alpha[1 + i] = static_cast<uint8_t>(((7 - i) * alpha[0] + i * alpha[1]) / 7);
    } else {
        for (int i = 1; i < 5; ++i) alpha[1 + i] = static_cast<uint8_t>(((5 - i) * alpha[0] + i * alpha[1]) / 5);
        alpha[6] = 0;
        alpha[7] = 255;
    }
    uint64_t abits = 0;
    for (int i = 0; i < 6; ++i) abits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
    const uint16_t c[2] = {static_cast<uint16_t>(b[8] | b[9] << 8), static_cast<uint16_t>(b[10] | b[11] << 8)};
    int rgb[4][3];
    for (int k = 0; k < 2; ++k) {
        rgb[k][0] = ((c[k] >> 11) & 31) * 255 / 31;
        rgb[k][1] = ((c[k] >> 5) & 63) * 255 / 63;
        rgb[k][2] = (c[k] & 31) * 255 / 31;
    }
    for (int j = 0; j < 3; ++j) {
        rgb[2][j] = (2 * rgb[0][j] + rgb[1][j]) / 3;
        rgb[3][j] = (rgb[0][j] + 2 * rgb[1][j]) / 3;
    }
    const uint32_t cbits = static_cast<uint32_t>(b[12]) | b[13] << 8 | b[14] << 16 | static_cast<uint32_t>(b[15]) << 24;
    for (int p = 0; p < 16; ++p) {
        const int ci = (cbits >> (2 * p)) & 3;
        out[p][0] = static_cast<uint8_t>(rgb[ci][2]);  // B G R A
        out[p][1] = static_cast<uint8_t>(rgb[ci][1]);
        out[p][2] = static_cast<uint8_t>(rgb[ci][0]);
        out[p][3] = alpha[(abits >> (3 * p)) & 7];
    }
}

bool is_atlas(const void* data, unsigned size) {
    if (!data || size != kAtlasSize) return false;
    __try {
        return fnv1a(static_cast<const uint8_t*>(data), size) == kAtlasHash;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool paint_level(IDirect3DTexture9* t, const IconLevel& lv, const unsigned char* icon_art, LoadFromMemoryFn load) {
    D3DSURFACE_DESC d{};
    if (FAILED(t->GetLevelDesc(lv.level, &d)) || d.Format != D3DFMT_DXT5 || d.Width != (1024u >> lv.level) ||
        d.Height != (512u >> lv.level)) {
        return false;
    }
    RECT r{lv.left, lv.top, lv.right, lv.bottom};
    const int w = lv.right - lv.left, h = lv.bottom - lv.top;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    D3DLOCKED_RECT lr{};
    if (FAILED(t->LockRect(lv.level, &lr, &r, D3DLOCK_READONLY)) || !lr.pBits) return false;
    for (int by = 0; by < h / 4; ++by) {
        for (int bx = 0; bx < w / 4; ++bx) {
            uint8_t blk[16][4];
            decode_block(static_cast<const uint8_t*>(lr.pBits) + by * lr.Pitch + bx * 16, blk);
            for (int p = 0; p < 16; ++p) {
                std::memcpy(&px[((by * 4 + p / 4) * w + bx * 4 + p % 4) * 4], blk[p], 4);
            }
        }
    }
    t->UnlockRect(lv.level);
    // The art over what is there (straight alpha).
    const uint8_t* art = icon_art + lv.offset;
    for (int i = 0; i < w * h; ++i) {
        const float sa = art[i * 4 + 3] / 255.0f, da = px[i * 4 + 3] / 255.0f;
        const float oa = sa + da * (1 - sa);
        for (int k = 0; k < 3; ++k) {
            const float v = oa > 0 ? (art[i * 4 + k] * sa + px[i * 4 + k] * da * (1 - sa)) / oa : 0.0f;
            px[i * 4 + k] = static_cast<uint8_t>(std::min(255.0f, v + 0.5f));
        }
        px[i * 4 + 3] = static_cast<uint8_t>(std::min(255.0f, oa * 255.0f + 0.5f));
    }
    IDirect3DSurface9* s = nullptr;
    if (FAILED(t->GetSurfaceLevel(lv.level, &s)) || !s) return false;
    RECT src{0, 0, w, h};
    const HRESULT hr = load(s, nullptr, &r, px.data(), D3DFMT_A8R8G8B8, w * 4, nullptr, &src, kFilterNone, 0);
    s->Release();
    return SUCCEEDED(hr);
}

bool paint_level_guarded(IDirect3DTexture9* t, const IconLevel& lv, const unsigned char* art, LoadFromMemoryFn load) {
    __try {
        return paint_level(t, lv, art, load);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool install() {
    if (!patch_xinput_import()) {
        LOG_ERROR("[coop] Death Knell: the game's XInput import was not found; D-pad left/right also switch weapons "
                  "while its view is open");
    }
    void* o = nullptr;
    const bool async = patch_import("user32", "GetAsyncKeyState", reinterpret_cast<void*>(&hk_get_async_key_state), &o);
    if (async) g_get_async = reinterpret_cast<GetAsyncKeyStateFn>(o);
    const bool state = patch_import("user32", "GetKeyboardState", reinterpret_cast<void*>(&hk_get_keyboard_state), &o);
    if (state) g_get_keyboard = reinterpret_cast<GetKeyboardStateFn>(o);
    const int devices = patch_dinput_devices();
    LOG_INFO("[coop] Death Knell: the arrow keys hidden from the game while its view is open (USER32 %s/%s, DirectInput "
             "%d device table(s))", async ? "yes" : "no", state ? "yes" : "no", devices);
    if (patch_chr_type_fx()) {
        LOG_INFO("[coop] Summon arrivals: the white arrival glows on every character (human ones too)");
    } else {
        LOG_ERROR("[coop] Summon arrivals: the effect-by-type code did not match this build (the white arrival glows "
                  "only on hollow characters)");
    }
    return true;
}

bool can_use() {
    // One ring: while it works (the view, the way there, the arrival) the bell is greyed out.
    if (g_state != State::Idle) return false;
    return refusal() == nullptr;
}

void use() {
    switch (g_state) {
        case State::Idle: {
            if (const char* why = refusal()) {
                say(text::widen(why));
                LOG_INFO("[coop] Death Knell: rung, but %s", why);
                return;
            }
            g_state = State::Opening;
            g_at = now() + kOpenDelay;
            g_view_hp = chr_int(game::player(), kChrHp);
            g_cancel_down = true;
            LOG_INFO("[coop] Death Knell: rung (the view opens in %.1f s)", kOpenDelay);
            start_hold();  // the use is at the bottom of the ring's crouch
            return;
        }
        default:
            LOG_INFO("[coop] Death Knell: rung while it already works; nothing more");
            return;
    }
}

void tick() {
    // The pad's poll, wrapped once the controller follow has put its own call in (it installs after us), and again
    // (a few times at most) should anything put another there.
    static double pad_at = -100;
    static int wraps = 0;
    static bool pad_told = false;
    if (now() - pad_at >= 5.0 && wraps < 3) {
        pad_at = now();
        const int w = wrap_pad_poll();
        if (w > 0) ++wraps;
        if (w < 0 && !pad_told) {
            pad_told = true;
            LOG_ERROR("[coop] Death Knell: the pad's poll call did not match this build (on a controller, D-pad "
                      "left/right also switch weapons while the view is open)");
        }
    }
    // Every load (the Homeward Bone, the Darksign, the Homeward miracle, travel, the Knell's own): the prompt of what
    // stood near the player before it goes once the player stands again.
    static bool was_loading = false;
    const bool loading = game::loading() || !game::player();
    if (was_loading && !loading) clear_action_prompt("after a load");
    was_loading = loading;
    invader_keys_tick();
    invader_type_tick();
    pending_price_tick();
    // A join's arrival: the moment the game starts its own rise at the host's bonfire (6950, 0.2 to 0.8 s after the
    // load in the 2026-11-04 test), it is replaced by the arrival, or after kJoinRiseWait standing if no rise comes.
    // Only while we are still a co-op guest there (a leave meanwhile is a load: the arrival would start at home, with
    // nobody to send it to).
    if (g_join_at > 0 && loading) g_join_stood_at = 0;
    if (g_join_at > 0 && !loading && g_join_stood_at == 0) g_join_stood_at = now();
    const bool rising = g_join_at > 0 && !loading && motion::held_event_anim(game::player()) == kGameRise;
    if (g_join_at > 0 && !loading && g_state == State::Idle && (rising || now() - g_join_stood_at >= kJoinRiseWait)) {
        LOG_INFO("[coop] Joined: the arrival %s (%.2f s after we stood)",
                 rising ? "replaces the game's own rise" : "starts (no rise of the game's came)", now() - g_join_stood_at);
        g_join_at = 0;
        g_join_stood_at = 0;
        if (!session::coop_guest_in_world()) {
            LOG_INFO("[coop] Joined: the summon arrival is not played (we are no longer a co-op guest in the host's "
                     "world)");
        } else if (!world::local_dead()) {
            g_arrival_anim = play_arrival("joined the session");
            g_arrival_at = now();
            g_arrival_price = false;
            g_join_arrival = true;
            if (g_arrival_anim) g_state = State::Arriving;
        }
    }
    switch (g_state) {
        case State::Idle:
            track_levels();
            break;
        case State::Opening:
            if (world::local_dead() || game::loading() || !game::player()) {
                close_view("interrupted");
            } else if (chr_int(game::player(), kChrHp) < g_view_hp) {
                close_view("we were hit");
                say(L"The Death Knell falls silent");
            } else if (cancel_pressed()) {
                close_view("B or Esc");
            } else if (now() >= g_at) {
                open_view();
            }
            break;
        case State::View:
            view_tick();
            break;
        case State::Going:
            going_tick();
            break;
        case State::Loading:
            loading_tick();
            break;
        case State::Arriving:
            arriving_tick();
            break;
    }
    hold_tick();
}

bool viewing() { return g_state == State::View; }

void set_soul_level_price(bool on) {
    g_soul_level = on;
    LOG_INFO("[coop] Death Knell: going to a teammate costs %s (DeathKnellSoulLevel)",
             on ? "a soul level and the debuffs" : "souls, humanity and the debuffs");
}

const wchar_t* text(uint32_t id) {
    if (id == kDrainedText) return L"! SOUL LEVEL DRAINED !";
    if (id == kDebuffedText) return L"! DEBUFFED !";
    return nullptr;
}

bool swallow_use(int32_t goods) {
    if (g_state != State::Going && now() >= g_swallow_until) return false;
    LOG_INFO("[coop] Death Knell: item %d used on the way: not used", goods);
    return true;
}

// The arrival takes the place of the game's own rise at the host's bonfire (the game plays 6950 on every guest it
// places, 0.2 to 0.8 s after the load): no delay of our own (1.5.0 first waited 2 s, standing, which showed). The
// others' games make our character 0.5 to 0.9 s after we join (the time our player data takes to reach them,
// 0xD2A360 builds it the frame it is complete; 2026-11-04 test): they hold the arrival until it is built and start it
// then, and end it with ours (session.cpp, knell_anims_tick). Their game making our character while we play an event
// animation is no trouble: it did so during the game's own rise at every join of the 2026-11-04 test.
void joined() {
    g_join_at = now();
    g_join_stood_at = 0;
    LOG_INFO("[coop] Joined: the summon arrival takes the place of the game's rise at the bonfire");
}

void left_session(const char* why) {
    rules::view_loaded_clear();  // the loads that follow are ours, whatever the view had loaded
    if (g_join_at <= 0) return;
    g_join_at = 0;
    LOG_INFO("[coop] Joined: the summon arrival is dropped (%s)", why);
}

void note_texture(IDirect3DTexture9* texture, const void* data, unsigned size) {
    if (!texture || !is_atlas(data, size)) return;
    std::lock_guard<std::mutex> lock(g_icon_mutex);
    texture->AddRef();
    if (g_icon_atlas) g_icon_atlas->Release();
    g_icon_atlas = texture;
}

void render() {
    if (!g_icon_atlas) return;
    std::lock_guard<std::mutex> lock(g_icon_mutex);
    if (!g_icon_atlas) return;
    IDirect3DTexture9* t = g_icon_atlas;
    g_icon_atlas = nullptr;
    static LoadFromMemoryFn load = nullptr;
    if (!load) {
        const HMODULE d3dx = GetModuleHandleW(L"d3dx9_43.dll");
        load = d3dx ? reinterpret_cast<LoadFromMemoryFn>(GetProcAddress(d3dx, "D3DXLoadSurfaceFromMemory")) : nullptr;
    }
    for (const IconArt& icon : kIcons) {
        int painted = 0;
        if (load) {
            for (int i = 0; i < icon.count; ++i) {
                if (!paint_level_guarded(t, icon.levels[i], icon.art, load)) break;
                ++painted;
            }
        }
        if (painted == icon.count) {
            LOG_INFO("[coop] %s: its icon is painted into the menu atlas (%d levels)", icon.name, painted);
        } else if (!g_icon_failed) {
            g_icon_failed = true;
            LOG_ERROR("[coop] %s: its icon could not be painted (%d level(s) done%s)", icon.name, painted,
                      load ? "" : "; no D3DXLoadSurfaceFromMemory");
        }
    }
    t->Release();
}

}  // namespace coop::knell
