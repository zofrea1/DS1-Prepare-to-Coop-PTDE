// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "knell.h"

#include "game.h"
#include "hook.h"
#include "log.h"
#include "motion.h"
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

#include "death_knell_art.inc"

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
// character in from nothing (TAE event 193) with the summon sound.
constexpr int kArrivals[] = {6951, 6952, 6950};
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
// The game's action prompt, the one "Rest at bonfire" uses: 0xD40310 registers a request with the action manager
// [0x137D7CC] for this frame and answers 1 on the frame the action button is pressed on it ("IF Action Button",
// EcTestActionButton 0xBBD8D1, calls it every frame the player is in range). ecx = the prompt's kind (0xD5D650 makes
// 10 of an id like ours), eax = 0xFF (no check of the player's state, as the talk prompts pass), stdcall(manager, flag
// that disables it or -1, text id, enabled, 0, 0, 0), ret 0x1C. Its text comes through the FMG hook (ids of ours).
constexpr uint32_t kActionRequest = 0x00D40310;
constexpr int kActionKind = 10;
constexpr uint32_t kPromptText = 93129720;  // .. +7: a new id for each teammate shown, so the prompt reads the name anew
constexpr int kPromptSlots = 8;

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

constexpr double kOpenDelay = 1.2;   // the bell's animation and effect play before the view opens
constexpr double kGoDelay = 1.2;     // and again before the player goes
constexpr double kViewLimit = 120.0;
constexpr double kPosFresh = 4.0;    // a teammate's reported position is used up to this old
constexpr uint16_t kPadB = 0x2000;   // XINPUT_GAMEPAD_B
constexpr uint16_t kPadLeft = 0x0004, kPadRight = 0x0008;

// ---------------------------------------------------------------- state
enum class State { Idle, Opening, View, Going, Loading, Arriving };
State g_state = State::Idle;
double g_at = 0;           // Opening / Going: when the next step is due
double g_view_since = 0;
uint64_t g_target_id = 0;  // the teammate the view shows
uint32_t g_shown_chr = 0;  // the character the camera was given (0: ours)
int g_view_hp = 0;         // our HP when the view opened (a hit closes it)
bool g_cancel_down = true; // B or Esc held (only a fresh press closes the view)
struct Destination {
    uint64_t id = 0;
    std::wstring name;
    int map = -1;
    float pos[3] = {};
};
Destination g_dest;
double g_load_since = 0;
bool g_load_seen = false;
double g_stand_since = 0;
// The ring's own animation (the item's use, ChrCtrl+0x350 as the ring lands) and how far into it the ring lands: the
// confirmation plays it again, and the warp waits as long.
int g_use_anim = -1;
double g_use_point = 0;
int g_seen_anim = -1;       // the animation playing, followed every frame
double g_seen_since = 0;
// The arrival playing: its animation, since when, and whether the price follows (a join's arrival has none).
int g_arrival_anim = 0;
double g_arrival_at = 0;
bool g_arrival_price = false;
double g_join_at = 0;       // a join's arrival waits for this (0 none)
wchar_t g_prompt[kPromptSlots][96] = {};
int g_prompt_slot = 0;
// Our request, like a bonfire's, stays with the action manager until it is cancelled: closed without a warp (B, a hit,
// a refusal, the time limit) the view left "Summon yourself to ..." on screen (close_view). Anything still in range
// registers again on the next frame.
bool g_prompt_up = false;  // our request was registered since the last clear
std::mt19937 g_rng(static_cast<uint32_t>(GetTickCount64()));

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

bool start_event_anim(uint32_t me, int anim) {
    const uint32_t start = kEventAnimStart;
    __try {
        __asm {
            push esi
            push edi
            mov esi, me
            mov edi, anim
            push 0
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

// One of the game's summon arrivals at random, which the others see too (1.5.0). Returns it, 0 when none started.
int play_arrival(const char* why) {
    const uint32_t me = game::player();
    if (!me) return 0;
    const int anim = kArrivals[std::uniform_int_distribution<int>(0, 2)(g_rng)];
    motion::send_knell_arrival(anim);
    if (!start_event_anim(me, anim)) {
        LOG_ERROR("[coop] Death Knell: the arrival animation could not be started");
        return 0;
    }
    LOG_INFO("[coop] Summon arrival %d (%s)", anim, why);
    return anim;
}

void arrive() {
    g_arrival_anim = play_arrival("the Death Knell");
    g_arrival_at = now();
    g_arrival_price = true;
    g_state = State::Arriving;
    LOG_INFO("[coop] Death Knell: arrived by %s; the price once the arrival is over", narrow(g_dest.name).c_str());
}

// The poise breaks, the riposte's stab sounds, the HUD says so, and the level goes.
void pay_price() {
    const uint32_t me = game::player();
    if (!me) return;
    motion::send_knell_arrival(kStagger);
    const bool staggered = start_event_anim(me, kStagger);
    const bool sound = play_sound(10000, kRiposteSoundType, kRiposteSound);
    hud_warning(kDrainedText);
    const char* why = nullptr;
    const int stat = drain(&why);
    if (stat < 0) LOG_INFO("[coop] Death Knell: no soul level was taken (%s)", why ? why : "?");
    LOG_INFO("[coop] Death Knell: the price (stagger %d%s, riposte sound c%d%s)", kStagger, staggered ? "" : " FAILED",
             kRiposteSound, sound ? "" : " FAILED");
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

std::vector<const spectate::Teammate*> candidates() {
    std::vector<const spectate::Teammate*> out;
    for (const spectate::Teammate& t : spectate::teammates()) {
        if (t.alive) out.push_back(&t);
    }
    std::sort(out.begin(), out.end(),
              [](const spectate::Teammate* a, const spectate::Teammate* b) { return a->id < b->id; });
    return out;
}

const spectate::Teammate* find(uint64_t id) {
    for (const spectate::Teammate& t : spectate::teammates()) {
        if (t.id == id && t.alive) return &t;
    }
    return nullptr;
}

std::wstring where(const spectate::Teammate& t) {
    if (t.chr && remote_character(t.chr)) return L"";
    const char* area = area_name(t.map);
    return area ? L" (far away, in " + text::widen(area) + L")" : L" (far away)";
}

// Why the Death Knell cannot be rung now, or null.
const char* refusal() {
    if (!session::in_session()) return "Only in a co-operative session";
    if (session::invader()) return "Not while invading";
    if (!game::player() || game::loading()) return "Not while loading";
    if (world::local_dead() || spectate::holding() || world::in_limbo()) return "Not while fallen";
    if (session::trip_slot_busy()) return "Not while the party travels";
    if (world::boss_fight()) return "Not in a boss fight";
    if (candidates().empty()) return "No other co-operator stands";
    return nullptr;
}

void say(const std::wstring& line) { text::show(line); }

// The prompt "Summon yourself to <name>" for this frame. True on the frame the action button is pressed on it.
bool request_action(uint32_t mgr, uint32_t text_id) {
    uint8_t pressed = 0;
    __try {
        __asm {
            push 0
            push 0
            push 0
            push 1
            push text_id
            push -1
            push mgr
            mov ecx, kActionKind
            mov eax, 0xFF
            mov edx, kActionRequest
            call edx
            mov pressed, al
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return pressed != 0;
}

void set_prompt(const std::wstring& text) {
    g_prompt_slot = (g_prompt_slot + 1) % kPromptSlots;
    std::wstring t = text.substr(0, 95);
    std::memcpy(g_prompt[g_prompt_slot], t.c_str(), (t.size() + 1) * sizeof(wchar_t));
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
    g_prompt_up = false;
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
void show_target(const spectate::Teammate& t, bool first) {
    g_target_id = t.id;
    const uint32_t chr = t.chr && remote_character(t.chr) ? t.chr : 0;
    g_shown_chr = chr;
    spectate::knell_view(chr);
    set_prompt(L"Summon yourself to " + t.name + where(t));
    if (first) say(L"Death Knell: choose a co-operator with the D-pad or the arrow keys");
    LOG_INFO("[coop] Death Knell: viewing %s%s", narrow(t.name).c_str(), chr ? "" : " (not in our world: no camera)");
}

void close_view(const char* why) {
    if (g_state == State::View) LOG_INFO("[coop] Death Knell: view closed (%s)", why);
    if (g_prompt_up) {
        g_prompt_up = false;
        clear_action_prompt("the Death Knell's view closed");
    }
    spectate::knell_view(0);
    g_shown_chr = 0;
    InterlockedExchange(&g_mask_dpad, 0);
    if (g_state == State::View || g_state == State::Opening) g_state = State::Idle;
}

void open_view() {
    if (const char* why = refusal()) {
        say(text::widen(why));
        LOG_INFO("[coop] Death Knell: the view does not open (%s)", why);
        g_state = State::Idle;
        return;
    }
    const auto can = candidates();
    // The nearest teammate whose character stands in our world, else the first.
    const spectate::Teammate* best = can.front();
    game::Vec3 me{};
    float best_d = 3.4e38f;
    if (game::position(&me, nullptr)) {
        for (const spectate::Teammate* t : can) {
            game::Vec3 q{};
            if (!t->chr || !remote_character(t->chr) || !game::chr_position(t->chr, &q)) continue;
            const float d = (q.x - me.x) * (q.x - me.x) + (q.y - me.y) * (q.y - me.y) + (q.z - me.z) * (q.z - me.z);
            if (d < best_d) {
                best_d = d;
                best = t;
            }
        }
    }
    g_state = State::View;
    g_view_since = now();
    g_view_hp = chr_int(game::player(), kChrHp);
    g_cancel_down = true;
    InterlockedExchange(&g_mask_dpad, 1);
    LOG_INFO("[coop] Death Knell: view open (%d co-operator(s))", static_cast<int>(can.size()));
    show_target(*best, true);
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
    const auto can = candidates();
    int at = -1;
    for (size_t i = 0; i < can.size(); ++i) {
        if (can[i]->id == g_target_id) at = static_cast<int>(i);
    }
    const int step = spectate::switch_step();
    if (at < 0) {
        show_target(*can.front(), false);  // the one shown fell or left
        return;
    }
    if (step != 0 && can.size() > 1) {
        show_target(*can[(at + step + static_cast<int>(can.size())) % can.size()], false);
        return;
    }
    // Their character came into our world, or left it: the camera follows.
    const uint32_t chr = can[at]->chr && remote_character(can[at]->chr) ? can[at]->chr : 0;
    if (chr != g_shown_chr) {
        g_shown_chr = chr;
        spectate::knell_view(chr);
    }
    uint32_t mgr = 0;
    if (!read32(kActionManVar, &mgr) || !mgr) return;
    g_prompt_up = true;
    if (request_action(mgr, kPromptText + g_prompt_slot)) confirm();
}

// The action button on the prompt: the teammate shown is where we go. The camera stays on them until the warp, so
// it does not swing back to us and out again, and the arrival's sound is heard where we arrive.
void confirm() {
    const spectate::Teammate* t = find(g_target_id);
    if (!t) {
        say(L"The Death Knell finds no one there");
        return;
    }
    if (t->boss_fight) {
        say(t->name + L" is in a boss fight");
        LOG_INFO("[coop] Death Knell: %s is in a boss fight; not going", narrow(t->name).c_str());
        return;
    }
    g_dest = Destination{};
    g_dest.id = t->id;
    g_dest.name = t->name;
    g_dest.map = t->map;
    std::copy(std::begin(t->pos), std::end(t->pos), g_dest.pos);
    InterlockedExchange(&g_mask_dpad, 0);
    g_state = State::Going;
    // The bell rings again: the ring's own animation and effect, and the warp as far into it as the ring landed.
    const uint32_t me = game::player();
    const bool again = g_use_anim > 0 && me && start_event_anim(me, g_use_anim);
    g_at = now() + (again ? std::max(0.6, std::min(2.5, g_use_point)) + 0.3 : kGoDelay);
    LOG_INFO("[coop] Death Knell: going to %s (the ring's animation %d %s, the warp in %.1f s)",
             narrow(t->name).c_str(), g_use_anim, again ? "again" : "not played", g_at - now());
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

void going_tick() {
    if (now() < g_at) return;
    if (const char* why = refusal()) {
        say(text::widen(why));
        LOG_INFO("[coop] Death Knell: not going (%s)", why);
        close_view("not going");
        g_state = State::Idle;
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
    g_dest.map = t->map;
    std::copy(std::begin(t->pos), std::end(t->pos), g_dest.pos);
    float angle = 0;
    game::Vec3 mine{};
    game::position(&mine, &angle);
    const int map = static_cast<int>(static_cast<uint32_t>(t->map) & 0xFFFF0000u);
    rules::place_next_load_at(map, g_dest.pos[0], g_dest.pos[1], g_dest.pos[2], angle);
    world::warp_now(map, -1);
    g_state = State::Loading;
    g_load_since = now();
    g_load_seen = false;
    g_stand_since = 0;
    LOG_INFO("[coop] Death Knell: loading into map %08X (%s) for %s at (%.2f, %.2f, %.2f)", static_cast<uint32_t>(map),
             area_name(map) ? area_name(map) : "?", narrow(t->name).c_str(), g_dest.pos[0], g_dest.pos[1],
             g_dest.pos[2]);
}

void loading_tick() {
    if (game::loading() || !game::player()) {
        g_load_seen = true;
        g_stand_since = 0;
        if (now() - g_load_since > 120.0) {
            LOG_INFO("[coop] Death Knell: the load took over two minutes; no arrival");
            g_state = State::Idle;
        }
        return;
    }
    if (!g_load_seen) {
        if (now() - g_load_since > 8.0) {
            LOG_INFO("[coop] Death Knell: the load into %08X never began", static_cast<uint32_t>(g_dest.map));
            say(L"The Death Knell falls silent");
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

// ---------------------------------------------------------------- the icon
// The atlas the icon's sprite reads (Icon21: menu/menu_4.tpf, 1024 x 512 DXT5, 11 levels), as the game hands it to
// D3DX. EquIcon_2159's cell is empty in it; the bell is laid over the cell's blocks level by level (the patches are
// built by tools/build_death_knell_icon.py, see death_knell_art.inc).
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

bool paint_level(IDirect3DTexture9* t, const KnellLevel& lv, LoadFromMemoryFn load) {
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
    // The bell over what is there (straight alpha).
    const uint8_t* art = kKnellArt + lv.offset;
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

bool paint_level_guarded(IDirect3DTexture9* t, const KnellLevel& lv, LoadFromMemoryFn load) {
    __try {
        return paint_level(t, lv, load);
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
            // The ring's animation, for the confirmation: the one playing as the ring lands, and how far into it.
            const uint32_t me = game::player();
            g_use_anim = playing_anim(me);
            g_use_point = g_use_anim == g_seen_anim && g_seen_since > 0 ? now() - g_seen_since : 0.0;
            LOG_INFO("[coop] Death Knell: rung (animation %d, %.2f s in; the view opens after its effect)", g_use_anim,
                     g_use_point);
            return;
        }
        default:
            LOG_INFO("[coop] Death Knell: rung while it already works; nothing more");
            return;
    }
}

void tick() {
    // Every load (the Homeward Bone, the Darksign, the Homeward miracle, travel, the Knell's own): the prompt of what
    // stood near the player before it goes once the player stands again.
    static bool was_loading = false;
    const bool loading = game::loading() || !game::player();
    if (was_loading && !loading) clear_action_prompt("after a load");
    was_loading = loading;
    const int anim = loading ? -1 : playing_anim(game::player());
    if (anim != g_seen_anim) {
        g_seen_anim = anim;
        g_seen_since = now();
    }
    // A join's arrival: once we stand in the host's world.
    if (g_join_at > 0 && !loading && now() >= g_join_at && g_state == State::Idle) {
        g_join_at = 0;
        if (!world::local_dead()) {
            g_arrival_anim = play_arrival("joined the session");
            g_arrival_at = now();
            g_arrival_price = false;
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
}

bool viewing() { return g_state == State::View; }

const wchar_t* text(uint32_t id) {
    if (id == kDrainedText) return L"! SOUL LEVEL DRAINED !";
    if (id >= kPromptText && id < kPromptText + kPromptSlots) return g_prompt[id - kPromptText];
    return nullptr;
}

bool swallow_use(int32_t goods) {
    if (g_state != State::Going) return false;
    LOG_INFO("[coop] Death Knell: item %d used by the ring's replayed animation: not used", goods);
    return true;
}

void joined() {
    g_join_at = now() + 0.6;
    LOG_INFO("[coop] Joined: the summon arrival plays once we stand");
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
    int painted = 0;
    if (load) {
        for (const KnellLevel& lv : kKnellLevels) {
            if (!paint_level_guarded(t, lv, load)) break;
            ++painted;
        }
    }
    t->Release();
    if (painted == static_cast<int>(sizeof(kKnellLevels) / sizeof(kKnellLevels[0]))) {
        LOG_INFO("[coop] Death Knell: its icon is painted into the menu atlas (%d levels)", painted);
    } else if (!g_icon_failed) {
        g_icon_failed = true;
        LOG_ERROR("[coop] Death Knell: its icon could not be painted (%d level(s) done%s)", painted,
                  load ? "" : "; no D3DXLoadSurfaceFromMemory");
    }
}

}  // namespace coop::knell
