// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The [PrepareToCoop] settings. They live in PTDE-Coop-FPS-Unlock.ini (or the older PTDE-FPS-Unlock.ini) beside the
// FPS Unlock's own, which ignores
// them; this reads them through the FPS Unlock's INI reader (settings_read_keys). Section headers carry no
// meaning, as for the FPS Unlock's keys.
struct CoopConfig {
    bool enabled = false;
    // Players with the same password find each other.
    char password[64] = {};
    // Steam app id the game runs as. PTDE's own (211420) has had its lobbies switched off since 2022,
    // so co-op runs under Spacewar (480), like the community's other PTDE multiplayer tools.
    int app_id = 480;
    // Others may join as evil spirits (invaders) to hunt the party.
    bool allow_invaders = true;
    // Humanity for the side that wins an invasion (the invader: one per co-op player defeated; else each co-op player).
    bool invasion_rewards = true;
    // Host: after an invasion of the world ends, no other invader comes in for this long (InvasionCooldown, in minutes
    // in the INI: 0 to 10).
    int invasion_cooldown_s = 180;
    // Host: while an invader is in the world, the boss fogs stay shut for the host and the guests (off by default:
    // vanilla shuts them only for covenant invaders, see world.cpp).
    bool seal_boss_fogs = false;
    // Each death in a session adds a Crimson Blossom (up to five), lowering maximum HP until anyone in the session
    // rests or the whole team falls (on by default since 1.1.0).
    bool death_debuffs = true;
    // Guests run the world's events in their own game as the host does (Remastered Seamless Co-op's
    // rule): bosses, lifts, cutscenes, scripted enemies. false = the game's own co-op (host only).
    bool guest_world_events = true;
    // The other players see your bonfire animations (sitting, kindling, Reverse Hollowing).
    bool bonfire_animations = true;
    // An item a co-op player drops lies on every co-op player's floor; the first to pick it up has it (itemshare.cpp).
    // The host's decides for its session.
    bool share_items = true;
    // Text over other players: 0 name only, 1 ping, 2 soul level, 3 deaths, 4 soul level and ping, 5 soul level,
    // ping and the smoothing their character is drawn with.
    int overhead_display = 5;
    // Host: how many times a second every game of the session sends its player's character and the enemies it moves
    // (6, the game's own, to 30). Guests and invaders use their host's.
    int net_rate = 30;
    // Each player's own: above the game's rate, the ms other players' characters (and the enemies other games move)
    // stay behind their updates as a margin for late ones (0 to 200). 33 ms is about a frame at 30 FPS.
    int net_smoothing = 33;
    // Each player's own: the smoothing follows each other player's (and each enemy copy's) connection, from 25 ms on
    // an even one up to 200 ms on a poor one, starting at NetworkSmoothing. false = NetworkSmoothing for all.
    bool dynamic_smoothing = true;
    // Players in a session, the host and every guest and invader together (2 to 8, 8 by default since 1.4.0). The
    // game holds 4; above that the co-op makes room for more (players.cpp). NPC summons do not count.
    int max_players = 8;
    // Enemy and boss scaling per extra player, in percent.
    int enemy_health = 45;
    int enemy_damage = 0;
    int enemy_posture = 30;
    int boss_health = 125;
    int boss_damage = 0;
    int boss_posture = 40;
    // Co-op characters live in their own save file (DRAKS0005.<extension>) so vanilla saves stay apart.
    char save_extension[16] = "co2";
    // Keyboard shortcuts as well as the co-op items (Ctrl+F1 host, Ctrl+F2 join, Ctrl+F3 leave).
    bool hotkeys = true;
    // At each start, ask GitHub for the latest release and say on the title screen when a newer one is out.
    bool check_updates = true;
    // Hidden test keys (not in the shipped INI): TestProtocol / TestVersion make this game speak another co-op
    // protocol and report another version. TestProtocol shows the version mismatch notices between two copies of
    // one build; versions alone are not compared for now (session.cpp, kRequireSameVersion). The update check
    // compares the latest release with TestVersion too, so an older one shows its notice.
    int test_protocol = 0;
    char test_version[16] = {};
};

// Reads the keys from PTDE-Coop-FPS-Unlock.ini next to the DLL, or PTDE-FPS-Unlock.ini when there is none (any
// letter case; missing keys keep their defaults).
CoopConfig coop_config_load(const wchar_t* dll_path);
