// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "coop.h"

#include "game.h"
#include "hook.h"
#include "items.h"
#include "log.h"
#include "motion.h"
#include "rules.h"
#include "session.h"
#include "spectate.h"
#include "steam.h"
#include "text.h"
#include "world.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <cwchar>

namespace {

CoopConfig g_cfg;
bool g_enabled = false;
bool g_installed = false;
bool g_items = false;
bool g_steam_hooked = false;
std::string g_save_report;

// ---------------------------------------------------------------- the co-op save file
// The save manager's constructor (0x733E00) names the file DRAKS0005 + ".sl2": `push 4 ; push L".sl2"` at
// 0x733FC0 (the extension and its length). Co-op characters use their own extension instead.
wchar_t g_ext[20] = {};

void patch_save_extension(const char* ext) {
    if (!ext[0] || _stricmp(ext, "sl2") == 0) {
        g_save_report = "Save file: DRAKS0005.sl2 (shared with vanilla play; SaveFileExtension = sl2).";
        return;
    }
    swprintf_s(g_ext, L".%hs", ext);
    const size_t len = wcslen(g_ext);
    static const uint8_t kExpect[7] = {0x6A, 0x04, 0x68, 0x6C, 0x40, 0x10, 0x01};
    uint8_t code[7] = {0x6A, static_cast<uint8_t>(len), 0x68, 0, 0, 0, 0};
    const uint32_t p = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_ext));
    std::memcpy(code + 3, &p, 4);
    if (coop::patch(0x00733FC0, kExpect, code, sizeof(code))) {
        g_save_report = std::string("Save file: DRAKS0005.") + ext + " (co-op characters are kept apart from DRAKS0005.sl2).";
    } else {
        g_save_report = "Save file: the save name code does not match this build; using DRAKS0005.sl2.";
    }
}

// ---------------------------------------------------------------- items
// Areas where the session items are refused, by map id (area << 24 | block << 16). Kept for later; off for now,
// so the items work anywhere the player carries them (the game's own animation and state rules still apply).
constexpr bool kAreaRulesOn = false;
struct AreaRule {
    int map;
    int32_t item;  // 0 = every session item
};
const AreaRule kAreaRules[] = {
    {0x12010000, coop::items::kInvade},  // e.g. no invading from the Undead Asylum
};

bool area_allows(int32_t id) {
    if (!kAreaRulesOn) return true;
    const int map = coop::game::map_id() & 0xFFFF0000;
    for (const AreaRule& r : kAreaRules) {
        if (r.map == map && (r.item == 0 || r.item == id)) return false;
    }
    return true;
}

// Souls needed to reach the next soul level (the Parchment of Deliverance costs one level's worth).
int level_up_cost(int level) {
    static const int kLow[] = {673, 690, 707, 724, 741, 758, 775, 793, 811, 829};  // to reach levels 2..11
    const int next = level + 1;
    if (next < 2) return kLow[0];
    if (next <= 11) return kLow[next - 2];
    const double x = next;
    return static_cast<int>(0.02 * x * x * x + 3.06 * x * x + 105.6 * x - 895.0 + 0.5);
}

wchar_t g_dialog[256];
const wchar_t* parchment_dialog() {
    swprintf_s(g_dialog, L"Spend %d souls to revive certain fallen allies of the world?\nYou return to your last bonfire.",
               level_up_cost(coop::game::soul_level()));
    return g_dialog;
}

// The Parchment of Deliverance (Remastered Seamless Co-op's NPC repair): pay a level's worth of souls, then
// flag 755 on and the flags of NPCs that can be lost while visiting other worlds off, in the player's own world
// (and in the host's world for a guest), and return to the last bonfire.
void parchment() {
    using namespace coop;
    const int cost = level_up_cost(game::soul_level());
    const int held = game::souls();
    if (held < cost) {
        text::show(L"Nothing interesting happens");
        LOG_INFO("[coop] Parchment: %d souls needed, %d held", cost, held);
        return;
    }
    game::set_souls(held - cost);
    static const int kOn[] = {755};
    static const int kOff[] = {1322, 1402, 11010520, 11010521, 11010523, 11010524, 11010526, 11010527};
    const bool away = !game::own_world();
    for (int id : kOn) {
        game::set_own_world_flag(id, true);
        if (away) game::set_flag(id, true);
    }
    for (int id : kOff) {
        game::set_own_world_flag(id, false);
        if (away) game::set_flag(id, false);
    }
    const int bonfire = game::last_bonfire();
    LOG_INFO("[coop] Parchment: paid %d souls, NPC flags repaired, back to bonfire %d", cost, bonfire);
    if (bonfire >= 0) world::warp_now(game::bonfire_map(bonfire), -1);
}

// Remastered Seamless Co-op's rules for when an item can be used (it replaces the game's check for these ids).
int can_use(int32_t id) {
    using namespace coop;
    const bool in_session = session::in_session();
    const bool host = session::hosting();
    const bool invader = session::invader();
    const bool invader_present = rules::state().invader_present;
    switch (id) {
        case items::kHost:
        case items::kJoin:
        case items::kInvade:
        case items::kParchment:
            return !in_session && area_allows(id) ? 1 : 0;
        case items::kLeave:
            return in_session && !invader_present ? 1 : 0;
        case items::kRules:
        case items::kTaunt:
            return host && area_allows(id) ? 1 : 0;
        case items::kInfection:  // our own world, with or without a session, but not while joining another
            return (host || !session::active()) && game::own_world() && area_allows(id) ? 1 : 0;
        case items::kBlossom:
            return 0;
        case 230:  // Elizabeth's Mushroom
        case 240:  // Divine Blessing
        case 390: case 391: case 392: case 393: case 394: case 395: case 396:  // Fire Keeper Souls
        case 500:  // Humanity
        case 501:  // Twin Humanities
            if (!in_session) return -1;
            return !invader && !invader_present ? 1 : 0;
        case 330:  // Homeward Bone: only the host, and not while an invader is in the world
            if (!in_session) return -1;
            return host && !invader_present ? 1 : 0;
        case 115:  // Black Eye Orb
        case 116:
            if (!in_session) return -1;
            return host && !invader_present ? 2 : 0;
        default:
            return -1;
    }
}

bool use_item(int32_t id) {
    using namespace coop;
    switch (id) {
        case items::kHost: session::host(); return true;
        case items::kJoin: session::join(); return true;
        case items::kInvade: session::invade(); return true;
        case items::kLeave: session::leave(); return true;
        case items::kRules: session::cycle_rules(); return true;
        case items::kTaunt: session::taunt(); return true;
        case items::kInfection: session::curse_world(); return true;
        case items::kParchment: parchment(); return true;
        case 330:
            // In a session the host's bone takes everyone to the host's last bonfire (session tick).
            if (!session::in_session()) return false;
            session::host_homeward();
            return true;
        default:
            return false;
    }
}

int blossom_count() { return coop::items::count(coop::items::kBlossom); }

// A bonfire rest: the co-op items the player lacks, the Crimson Blossoms wither, and the host remembers where
// guests arrive.
void before_rest() {
    if (g_items) {
        coop::items::grant_missing();
        coop::items::remove_all(coop::items::kBlossom);
        coop::world::clear_death_debuff();
    }
    coop::session::remember_rest_point();
}

// ---------------------------------------------------------------- per frame
bool game_has_focus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void hotkeys() {
    static bool down[4] = {};
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const int keys[4] = {VK_F1, VK_F2, VK_F3, VK_F4};
    static const char* const kNames[4] = {"host", "join", "leave", "invade"};
    for (int i = 0; i < 4; ++i) {
        const bool pressed = ctrl && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (pressed && !down[i]) {
            const bool focus = game_has_focus();
            LOG_INFO("[coop] Ctrl+F%d (%s)%s", i + 1, kNames[i], focus ? "" : " ignored: the game window is not in front");
            if (focus) {
                switch (i) {
                    case 0: coop::session::host(); break;
                    case 1: coop::session::join(); break;
                    case 2: coop::session::leave(); break;
                    case 3: coop::session::invade(); break;
                }
            }
        }
        down[i] = pressed;
    }
}

// Right after the game's SteamAPI_RunCallbacks, on the game thread.
void tick_body() {
    coop::session::tick();
    coop::world::tick();
    coop::spectate::tick();
    if (g_items) coop::items::tick();
    if (g_cfg.hotkeys) hotkeys();
    static ULONGLONG last_beat = 0;
    if (GetTickCount64() - last_beat > 60000) {
        last_beat = GetTickCount64();
        LOG_INFO("[coop] Tick running (session %s)", coop::session::active() ? "active" : "idle");
    }
}

void tick() {
    if (!g_installed) return;
    __try {
        tick_body();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int reported = 0;
        if (reported++ < 5) LOG_ERROR("[coop] Exception %08lX in the co-op tick", GetExceptionCode());
    }
}

}  // namespace

void coop_install_early(const CoopConfig& config) {
    g_cfg = config;
    g_enabled = g_cfg.enabled;
    if (!g_enabled) return;
    patch_save_extension(g_cfg.save_extension);
    g_steam_hooked = coop::steam::install_early(static_cast<uint32_t>(g_cfg.app_id), &tick,
                                                coop::session::game_tag(g_cfg.password));
}

void coop_install(const CoopConfig& config) {
    if (!config.enabled) {
        LOG_INFO("[coop] Prepare to Co-op: off");
        return;
    }
    LOG_INFO("[coop] Prepare to Co-op: on (Steam app %d, password %s, invaders %s, max 4 players)", g_cfg.app_id,
             g_cfg.password[0] ? "set" : "EMPTY", g_cfg.allow_invaders ? "allowed" : "off");
    LOG_INFO("[coop] %s", g_save_report.c_str());
    if (!g_steam_hooked) {
        LOG_ERROR("[coop] Could not hook the game's Steam calls; Prepare to Co-op is unavailable.");
        return;
    }
    const int failed = coop::rules::install();
    coop::rules::set_guest_world_events(g_cfg.guest_world_events);
    LOG_INFO("[coop] Guests run the world's events in their own game: %s", g_cfg.guest_world_events ? "yes" : "no (host only)");
    const bool texts = coop::text::install();
    const bool world = coop::world::install();
    // Before the items and the rest handler that hands them out: with those in, a session could still start.
    if (failed || !world) {
        LOG_ERROR("[coop] %d co-op hook(s) could not be installed; Prepare to Co-op is off to keep the game safe.",
                  failed + (world ? 0 : 1));
        return;
    }
    if (!coop::session::install_nameplate()) LOG_ERROR("[coop] Companion nameplates were not changed");
    if (!coop::session::install_invader_bars()) LOG_ERROR("[coop] Invaders still see no health bars");
    coop::spectate::install();
    if (!coop::motion::install()) LOG_ERROR("[coop] Other players' gait and bonfire animations are not synced");
    coop::motion::set_bonfire_animations(g_cfg.bonfire_animations);
    LOG_INFO("[coop] Bonfire animations are shown to the other players: %s", g_cfg.bonfire_animations ? "yes" : "no");
    g_items = coop::items::install();
    coop::items::set_use_handler(&use_item);
    coop::items::set_can_use(&can_use);
    coop::items::set_dialog_text(&parchment_dialog);
    coop::world::set_before_rest_handler(&before_rest);
    coop::world::set_blossom_counter(&blossom_count);
    coop::world::Config wc;
    wc.death_debuffs = g_cfg.death_debuffs;
    wc.enemy_health = g_cfg.enemy_health;
    wc.enemy_damage = g_cfg.enemy_damage;
    wc.enemy_posture = g_cfg.enemy_posture;
    wc.boss_health = g_cfg.boss_health;
    wc.boss_damage = g_cfg.boss_damage;
    wc.boss_posture = g_cfg.boss_posture;
    coop::world::configure(wc);
    coop::session::Config cfg;
    cfg.password = g_cfg.password;
    cfg.allow_invaders = g_cfg.allow_invaders;
    cfg.death_debuffs = g_cfg.death_debuffs;
    cfg.overhead_display = g_cfg.overhead_display;
    cfg.test_protocol = g_cfg.test_protocol;
    cfg.test_version = g_cfg.test_version;
    coop::session::configure(cfg);
    static const char* const kOverhead[] = {"name only", "name and ping", "name and soul level", "name and deaths",
                                            "name, soul level and ping"};
    const int overhead = g_cfg.overhead_display >= 0 && g_cfg.overhead_display <= 4 ? g_cfg.overhead_display : 4;
    LOG_INFO("[coop] Companion health bars show the %s", kOverhead[overhead]);
    g_installed = true;
    LOG_INFO("[coop] Ready. Co-op items %s. %sKeys: Ctrl+F1 host, Ctrl+F2 join, Ctrl+F3 leave, Ctrl+F4 invade%s.",
             g_items ? "given at every bonfire rest" : "UNAVAILABLE", texts ? "" : "(On-screen texts unavailable.) ",
             g_cfg.hotkeys ? "" : " (off)");
}

bool coop_enabled() { return g_enabled; }
