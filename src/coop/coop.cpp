#include "coop.h"

#include "game.h"
#include "hook.h"
#include "items.h"
#include "log.h"
#include "overlay.h"
#include "rules.h"
#include "session.h"
#include "steam.h"
#include "text.h"
#include "world.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <cwchar>

namespace {

Settings::Coop g_cfg;
bool g_enabled = false;
bool g_installed = false;
bool g_items = false;
bool g_steam_hooked = false;
std::string g_save_report;
bool g_had_player = false;

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
// The NPC repairs of the Writ of Clemency (the fix seamless co-op applies when someone joins).
void clemency() {
    static const int kOn[] = {755};
    static const int kOff[] = {1322, 1402, 11010520, 11010521, 11010523, 11010524, 11010526, 11010527};
    const bool home = coop::game::own_world();
    for (int id : kOn) home ? coop::game::set_flag(id, true) : coop::game::set_own_world_flag(id, true);
    for (int id : kOff) home ? coop::game::set_flag(id, false) : coop::game::set_own_world_flag(id, false);
    coop::text::show(L"The fallen are pardoned. Some lost allies may return.");
}

void use_item(int32_t id) {
    using namespace coop;
    switch (id) {
        case items::kHost: session::host(); break;
        case items::kJoin: session::join(); break;
        case items::kInvade: session::invade(); break;
        case items::kLeave: session::leave(); break;
        case items::kRules: session::toggle_invaders(); break;
        case items::kTaunt: session::taunt(); break;
        case items::kMisfortune: session::misfortune(); break;
        case items::kBlossom:
            world::set_blossom(true);
            text::show(L"The blossom takes root: less vigour, more souls until the next rest.");
            break;
        case items::kClemency: clemency(); break;
        default: break;
    }
}

// Homeward Bone (330) and Darksign (117) do not work while an invader is in the world.
bool block_item(int32_t id) {
    const auto& st = coop::rules::state();
    if ((id == 330 || id == 117) && st.active && !st.invader && st.invader_present) {
        coop::text::show(L"You cannot flee while an invader is in this world.");
        return true;
    }
    return false;
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
    for (int i = 0; i < 4; ++i) {
        const bool now = ctrl && (GetAsyncKeyState(keys[i]) & 0x8000) != 0 && game_has_focus();
        if (now && !down[i]) {
            switch (i) {
                case 0: coop::session::host(); break;
                case 1: coop::session::join(); break;
                case 2: coop::session::leave(); break;
                case 3: coop::session::invade(); break;
            }
        }
        down[i] = now;
    }
}

// Right after the game's SteamAPI_RunCallbacks, on the game thread.
void tick_body() {
    coop::session::tick();
    coop::world::tick();
    if (g_cfg.hotkeys) hotkeys();
    static ULONGLONG last_panel = 0;
    if (GetTickCount64() - last_panel > 250) {
        last_panel = GetTickCount64();
        coop::overlay::set_lines(coop::session::panel_lines(g_cfg.overhead_display));
    }
    const bool has_player = coop::game::in_game() && !coop::game::loading();
    if (g_items && has_player && !g_had_player && coop::game::own_world()) coop::items::grant_missing();
    g_had_player = has_player;
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

void coop_install_early(const Settings& settings) {
    g_cfg = settings.coop;
    g_enabled = g_cfg.enabled;
    if (!g_enabled) return;
    patch_save_extension(g_cfg.save_extension);
    g_steam_hooked = coop::steam::install_early(static_cast<uint32_t>(g_cfg.app_id), &tick,
                                                coop::session::game_tag(g_cfg.password));
}

void coop_install(const Settings& settings) {
    if (!settings.coop.enabled) {
        LOG_INFO("[coop] Seamless co-op: off");
        return;
    }
    LOG_INFO("[coop] Seamless co-op: on (Steam app %d, password %s, invaders %s, max 4 players)", g_cfg.app_id,
             g_cfg.password[0] ? "set" : "EMPTY", g_cfg.allow_invaders ? "allowed" : "off");
    LOG_INFO("[coop] %s", g_save_report.c_str());
    if (!g_steam_hooked) {
        LOG_ERROR("[coop] Could not hook the game's Steam calls; seamless co-op is unavailable.");
        return;
    }
    const int failed = coop::rules::install();
    const bool texts = coop::text::install();
    const bool world = coop::world::install();
    g_items = coop::items::install();
    coop::items::set_use_handler(&use_item);
    coop::items::set_block_filter(&block_item);
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
    coop::session::configure(cfg);
    if (failed || !world) {
        LOG_ERROR("[coop] %d co-op hook(s) could not be installed; seamless co-op is off to keep the game safe.",
                  failed + (world ? 0 : 1));
        return;
    }
    g_installed = true;
    LOG_INFO("[coop] Ready. Co-op items %s. %sKeys: Ctrl+F1 host, Ctrl+F2 join, Ctrl+F3 leave, Ctrl+F4 invade.",
             g_items ? "in the inventory (given in your own world)" : "UNAVAILABLE",
             texts ? "" : "(On-screen texts unavailable.) ");
}

bool coop_enabled() { return g_enabled; }
