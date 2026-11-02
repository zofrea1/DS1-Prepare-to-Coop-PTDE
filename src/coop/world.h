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
    int enemy_health = 45, enemy_damage = 0, enemy_posture = 30;
    int boss_health = 125, boss_damage = 0, boss_posture = 40;
};

bool install();
void configure(const Config& cfg);
// A guest in a host's world scales the enemies with the host's settings (null: our own INI's again). The six are
// enemy health, damage, posture, then boss health, damage, posture, in percent per extra player.
void set_host_scaling(const int* scaling);
void tick();
// A boss's defeat flag (every common event 970's first parameter).
bool boss_defeat_flag(int id);

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
// Takes every Crimson Blossom out of the inventory: besides our own rest, another player's rest and a death whose
// reload starts the world over (a team wipe) wither them, and the health the debuff took comes back.
void set_blossom_wither(void (*fn)());
// Adds one Crimson Blossom (up to five; coop.cpp): true when one was added. A death that keeps the world costs one,
// when its respawn warp goes (keep_dead_on_reload).
void set_blossom_give(bool (*fn)());
// A bonfire rest: take off every death-debuff effect immediately. The inventory clear is separate.
void clear_death_debuff();
// Another player rested: reset this world (enemies respawn), wither the Crimson Blossoms, end limbo.
void remote_rest();
// The session is over for us (a guest back home, a host who closed the world): the Crimson Blossoms wither at once,
// or as soon as we have a character again.
void session_over_blossoms(const char* why);

// Bonfire kindling shared from the host's world (see world.cpp, "our own world's bonfires").
struct BonfirePoints {
    int32_t object;
    int16_t a, b;   // the entry's points (+8) and the network's (+0xA)
    uint8_t level;  // its level (+0xE)
    uint8_t pad[3];
};
constexpr uint32_t kBonfireBytes = sizeof(BonfirePoints);
constexpr uint32_t kMaxBonfires = 100;
// The host: our table, BonfirePoints each.
std::vector<uint8_t> bonfire_points();
// The host: our table changed since the last call (a light, a Kindle, a rest's reset).
bool bonfires_changed();
// A co-op guest: the host's table (its kindling is ours while we are in its world; ours comes back at home).
void set_host_bonfires(const uint8_t* data, uint32_t size);
// This bonfire is lit in our game (any points or level).
bool bonfire_lit(int object);
// A bonfire lit during this visit (by us, or by another player for everyone): lit at home too, unkindled.
void note_lit_here(int object);
// A guest: we kindled this bonfire in the host's world (our game added its points; the host's answer follows).
void note_own_kindle(int object);
// The host: a co-op guest kindled this bonfire of our world (their humanity): 10 points here, in our game's limits.
bool kindle_for_guest(int object);

bool boss_fight();
// The co-op scaling of this enemy's health is in place in this game (or none is wanted): its max HP is the scaled
// one, so health given as a share of it comes out right.
bool scale_ready(uint32_t chr);
bool local_dead();
// The special effect `id` is on the character.
bool has_effect(uint32_t chr, int id);
// The death we are in is part of a team wipe: every co-op player fell and the world starts over (session::wipe_reload).
bool team_wipe_death();
// A menu or dialog is open (the bonfire menu and its shops included). The game never starts a load under one: our
// own loads wait for it to close.
bool menu_open();
// This game reloads after a lost boss fight (a rest while its boss was awake) and is waiting to do it.
bool reset_pending();
bool in_limbo();
// The host's trip home reached us while we are down: a respawn waiting in limbo goes now, and limbo does not hold
// again until we live (spectate::trip_release for a held death).
void trip_release();
// Shared light: the local player's own light (0x08 Cast Light or the Sunlight Maggot, 0x10 the Skull Lantern;
// 1.0.0 sent bits 0 to 2, which are ignored), and showing the other players' lights on their characters here.
struct RemoteLight {
    uint32_t chr;  // the player's character in our world (the session's match)
    int mask;
};
int local_lights();
void share_lights(const std::vector<RemoteLight>& peers);

// Our character's return-to-bonfire state (stateInfo 16: the Homeward Bone's, the Darksign's and the Homeward
// miracle's effects) is about to send us to our last bonfire. `fn` returns true to stop the game's own warp. False
// when that call could not be hooked (`fn` is never asked).
bool set_homeward_state_handler(bool (*fn)());

// Warps that must not wait in limbo (going home, following the host).
void warp_now(int map, int point);
void bypass_limbo(bool on);

}  // namespace coop::world
