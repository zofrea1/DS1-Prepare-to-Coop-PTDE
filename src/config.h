// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The [PrepareToCoop] settings. They live in PTDE-FPS-Unlock.ini beside the FPS Unlock's own, which ignores
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
    // Dying lowers maximum HP until the next bonfire rest.
    bool death_debuffs = false;
    // Guests run the world's events in their own game as the host does (Remastered Seamless Co-op's
    // rule): bosses, lifts, cutscenes, scripted enemies. false = the game's own co-op (host only).
    bool guest_world_events = true;
    // The other players see your bonfire animations (sitting, kindling, Reverse Hollowing).
    bool bonfire_animations = true;
    // Text over other players: 0 name only, 1 ping, 2 soul level, 3 deaths, 4 soul level and ping.
    int overhead_display = 4;
    // Enemy and boss scaling per extra player, in percent.
    int enemy_health = 35;
    int enemy_damage = 0;
    int enemy_posture = 15;
    int boss_health = 100;
    int boss_damage = 0;
    int boss_posture = 20;
    // Co-op characters live in their own save file (DRAKS0005.<extension>) so vanilla saves stay apart.
    char save_extension[16] = "co2";
    // Keyboard shortcuts as well as the co-op items (Ctrl+F1 host, Ctrl+F2 join, Ctrl+F3 leave).
    bool hotkeys = true;
    // Hidden test keys (not in the shipped INI): TestProtocol / TestVersion make this game speak another co-op
    // protocol and report another version, to see the version mismatch notices between two copies of one build.
    int test_protocol = 0;
    char test_version[16] = {};
};

// Reads the keys from PTDE-FPS-Unlock.ini next to the DLL (missing keys keep their defaults).
CoopConfig coop_config_load(const wchar_t* dll_path);
