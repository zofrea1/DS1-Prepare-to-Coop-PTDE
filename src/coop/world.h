// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// World rules of seamless co-op that are not about the session itself: bonfire rests (the world resets for
// every player), limbo (when spectate.cpp cannot hold a death during a boss fight or an invasion, the respawn
// waits for the fight to end), the respawn after a team wipe (it waits until every game can reload at once:
// session::team_respawn_wait), enemy scaling per player and the optional death debuff (Crimson Blossoms). The
// SpEffects these use are Remastered Seamless Co-op's extra SpEffectParam rows, served by the SpEffect param getter.

#include <cstdint>
#include <vector>

namespace coop::world {

struct Config {
    bool death_debuffs = false;
    int enemy_health = 35, enemy_damage = 0, enemy_posture = 15;
    int boss_health = 100, boss_damage = 0, boss_posture = 20;
};

bool install();
void configure(const Config& cfg);
void tick();

// Session state the rules depend on.
void set_session(bool active, bool invader, bool invader_present);
void set_players(int coop_players);       // players sharing the host's world, invaders not counted
void set_everyone_dead(bool all_dead);    // every other player is dead too
// A local rest at a bonfire: `before` runs before the game's own rest (items), `fn` after it (tell the others).
void set_rest_handler(void (*fn)());
void set_before_rest_handler(void (*fn)());
// The local player died (in a session).
void set_death_handler(void (*fn)());
// How many Crimson Blossoms the player carries (the death debuff's strength).
void set_blossom_counter(int (*fn)());
// A bonfire rest: take off every death-debuff effect immediately. The inventory clear is separate.
void clear_death_debuff();
// Another player rested: reset this world (enemies respawn), clear rest-cured effects, end limbo.
void remote_rest();

bool boss_fight();
bool local_dead();
// The death we are in is part of a team wipe: every co-op player fell and the world starts over (session::wipe_reload).
bool team_wipe_death();
// A menu or dialog is open (the bonfire menu and its shops included). The game never starts a load under one: our
// own loads wait for it to close.
bool menu_open();
// This game reloads after a lost boss fight (a rest while its boss was awake) and is waiting to do it.
bool reset_pending();
bool in_limbo();
// Shared light: the local player's own light (0x08 Cast Light or the Sunlight Maggot, 0x10 the Skull Lantern;
// 1.0.0 sent bits 0 to 2, which are ignored), and showing the other players' lights on their characters here.
struct RemoteLight {
    uint32_t chr;  // the player's character in our world (the session's match)
    int mask;
};
int local_lights();
void share_lights(const std::vector<RemoteLight>& peers);

// Warps that must not wait in limbo (going home, following the host).
void warp_now(int map, int point);
void bypass_limbo(bool on);

}  // namespace coop::world
