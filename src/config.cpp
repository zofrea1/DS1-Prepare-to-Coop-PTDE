// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "config.h"

#include "settings.h"  // the FPS Unlock's INI reader

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

CoopConfig coop_config_load(const wchar_t* dll_path) {
    CoopConfig c;
    settings_read_keys(dll_path, [&c](const std::string& key, const std::string& val) {
        if (key == "preparetocoop" || key == "seamlesscoop") {  // old name kept
            c.enabled = settings_parse_bool(val);
        } else if (key == "password" || key == "cooppassword") {
            std::strncpy(c.password, val.c_str(), sizeof(c.password) - 1);
        } else if (key == "steamappid") {
            c.app_id = std::atoi(val.c_str());
        } else if (key == "allowinvaders") {
            c.allow_invaders = settings_parse_bool(val);
        } else if (key == "invasionrewards") {
            c.invasion_rewards = settings_parse_bool(val);
        } else if (key == "deathdebuffs") {
            c.death_debuffs = settings_parse_bool(val);
        } else if (key == "guestworldevents") {
            c.guest_world_events = settings_parse_bool(val);
        } else if (key == "bonfireanimations") {
            c.bonfire_animations = settings_parse_bool(val);
        } else if (key == "overheadplayerdisplay") {
            c.overhead_display = std::atoi(val.c_str());
        } else if (key == "enemyhealthscaling") {
            c.enemy_health = std::atoi(val.c_str());
        } else if (key == "enemydamagescaling") {
            c.enemy_damage = std::atoi(val.c_str());
        } else if (key == "enemyposturescaling") {
            c.enemy_posture = std::atoi(val.c_str());
        } else if (key == "bosshealthscaling") {
            c.boss_health = std::atoi(val.c_str());
        } else if (key == "bossdamagescaling") {
            c.boss_damage = std::atoi(val.c_str());
        } else if (key == "bossposturescaling") {
            c.boss_posture = std::atoi(val.c_str());
        } else if (key == "savefileextension") {
            std::string ext = val;
            if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
            std::string clean;
            for (char ch : ext) {
                if (std::isalnum(static_cast<unsigned char>(ch))) clean += ch;
            }
            if (!clean.empty()) {
                std::memset(c.save_extension, 0, sizeof(c.save_extension));
                std::strncpy(c.save_extension, clean.c_str(), sizeof(c.save_extension) - 1);
            }
        } else if (key == "coophotkeys") {
            c.hotkeys = settings_parse_bool(val);
        } else if (key == "testprotocol") {
            c.test_protocol = std::atoi(val.c_str());
        } else if (key == "testversion") {
            std::strncpy(c.test_version, val.c_str(), sizeof(c.test_version) - 1);
        }
    });
    return c;
}
