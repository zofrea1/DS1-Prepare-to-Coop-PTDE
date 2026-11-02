// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// Game rule changes for seamless co-op: the hooks that keep a session alive through boss kills, deaths,
// map changes and warps, keep the guest in their own body, carry the guest's progress home, and apply
// Remastered Seamless Co-op's world rules (teams, world tendency, bonfire warps and lighting).

#include <cstdint>
#include <string>
#include <vector>

namespace coop::rules {

struct State {
    bool active = false;      // a seamless session is running (we host or we are a guest)
    bool host = false;        // we host
    bool guest = false;       // we are a co-op guest in someone's world
    bool invader = false;     // we invade (vanilla invader rules apply to us)
    bool leaving = false;     // we are leaving on purpose: the game's own leave code may run
    bool closing = false;     // host: the session is being disbanded; the guests are on their way out
    bool invader_present = false;  // an invader is in the session (vanilla multiplayer limits apply)
    bool teammate_boss_fight = false;  // a living co-op teammate has a boss gauge up: no rest, no travel
    bool team_invasion = false;  // we invade, undecided, another invader stands: our fall waits (spectate.cpp)
    int own_chr_type = -1;    // guest: our chr type at home (0 human, 8 hollow)
    // The host's world rules (the host decides, guests mirror them).
    int world_rules = 0;      // Ominous Tome: 0 none, 1 allies are hostile, 2 allies fight in teams, 3 free for all
                              // (1.4.0: allies hostile and locked on to)
    bool taunt = false;       // Dried Fingers: invaders find this world first
    bool infection = false;   // Cursed Pendant: the world we are in counts as pure black World Tendency
    int host_map = -1;        // guest: the host's map as last reported (kept while the host's game loads)
};

State& state();
// The Cursed Pendant's curse is lifted, or a curse of a previous game left disasters in the saved flags: the maps'
// disasters end. Every map's disaster flag is cleared and the current map's black phantoms die, as the game's own
// events do when a disaster ends. cursed_here: the current map was cursed while we were in it, whatever its flag
// says now. Returns how many maps had a disaster. Game thread.
int end_disasters(bool cursed_here);
// Whether any map's disaster flag is on (the current world's store).
bool disasters_active();
// Installs every hook; returns the number that failed (0 = all in place).
int install();
// Guests run the world's events in their own game (Remastered Seamless Co-op's rule; default on).
void set_guest_world_events(bool on);
// An event's wait for a character's animation ended in this game, which moves that character: `fn` tells the others,
// whose copies never end it themselves (rules.cpp, follow_wait).
void set_anim_done_listener(void (*fn)(int entity, int anim));
// Another player's game said so: a wait of ours for that animation on our copy of the character ends.
void remote_anim_done(int entity, int anim);
// Per-frame upkeep (the room timer, the lock), game thread.
void tick();
// Flags that change in a session's world while seamless rules apply (game thread, value really changed).
void set_flag_listener(void (*fn)(int id, bool on));
// Our game's runs of map events are not sent to the other players while on (a co-op guest whose copy of the host's
// world is not the host's yet; session.cpp).
void hold_event_runs(bool on);
// The runs held back since the last call.
uint32_t take_held_runs();
// Our game paid its player a boss's souls (the clear bonus), for the other players' games.
void set_clear_bonus_listener(void (*fn)(int souls));
// A rest (ours or another player's) is about to reset the world's characters.
void note_world_reset();
// A cutscene is about to move our player into another map (map id, the cutscene's point entity).
void set_cutscene_listener(void (*fn)(int map, int point));
// Our copy of such a cutscene could not take us there, not even after loading its map here: the map and point,
// once. The session then takes us there itself.
bool take_failed_cutscene(int* map, int* point);
// Our copy of such a cutscene is loading its map to play once the map is there, or is waiting to move us.
bool cutscene_loading();
// The game's cutscene manager has a cutscene queued or playing.
bool cutscene_playing();
// The map the world has us in (FieldArea's WorldRes +0x7E8), -1 unknown. A cutscene's in-place move into another
// map sets it at once; the player's own map id (game::map_id) follows a few seconds later.
int world_map();
// Sets a flag another player changed, without reporting it back.
void apply_remote_flag(int id, bool on);
// Host: the world's map flags (groups 1 and 6 of every map) for a co-op guest who arrived; empty if unreadable.
std::vector<uint8_t> world_flag_snapshot();
// Guest: the host's snapshot replaces those groups in the visited world's store. Returns the blocks written,
// or -1 for a snapshot that does not fit this store (nothing is written then).
int apply_world_flag_snapshot(const uint8_t* data, size_t size);
// Host: the world's state below the map flags (flags 0-1999: bosses, bells, NPCs) for a co-op guest; empty if
// unreadable.
std::vector<uint8_t> world_global_snapshot();
// Guest: the host's world state replaces ours in the visited world's store, but for the flags that belong to the
// player (kept from our own world). Returns how many flags changed (the first few in `changes`), -1 if unreadable.
int apply_world_globals(const uint8_t* data, size_t size, std::string* changes);
// A boss gauge's character is here and dead (a gauge left up after its fight).
bool gauge_boss_dead(uint32_t handle);
// The characters of the bosses with a health bar that are in this world now.
void gauge_boss_chrs(std::vector<uint32_t>* out);
// The character of a handle (0x10000000 | block << 14 | index), 0 if it is not in this world.
uint32_t chr_of_handle(uint32_t handle);
// A guest: the host's last report of this character's HP (the game's HP sync, 0xE40FE0), -1 none.
int host_hp_report(uint32_t chr);

// Bonfire travel (WarpNextStage_Bonfire; `bonfire` is the warp's point, the bonfire object - 980): the host's own
// warp goes ahead and `fn(bonfire)` tells the guests, who follow; a guest's own warp does not go ahead, `fn(bonfire)`
// asks the host to take everyone there. Neither goes while a living teammate fights a boss.
void set_bonfire_warp_listener(void (*fn)(int bonfire));
// Our own world's travel (the host's checks of a guest's request): we own the Lordvessel; the warp list's bonfires
// we have rested at (bit i = entry i); the bonfire is one of them.
bool own_lordvessel();
uint32_t own_warps();
bool warp_destination(int bonfire);
// A guest: what the host's world has (known: the host reported it). The bonfire menu then offers the warp only
// when the host owns the Lordvessel, and the warp list only the bonfires of the host's world.
void set_host_world(bool known, bool lordvessel, uint32_t warps);
// A bonfire was lit for the first time (Util_RequestLevelUpFirst): `fn(bonfire)` tells the others.
void set_bonfire_lit_listener(void (*fn)(int bonfire));
// A co-op guest kindled a bonfire in the host's world (Util_RequestLevelUp): `fn(bonfire)` tells the host.
void set_bonfire_kindle_listener(void (*fn)(int bonfire));
// The same, on behalf of another player (does not report back).
void remote_bonfire_warp(int bonfire);
void remote_bonfire_lit(int bonfire);
// The local player used a Black Separation Crystal in a seamless session (game thread, inside the player's
// update): `fn(true)` the host closes their world, `fn(false)` a guest goes home. The game's own separation
// (the host's phantom menu, the guest's Lua leave) is skipped.
void set_separation_listener(void (*fn)(bool host));

// WarpNextStageKick is about to run. A dead guest is aimed at their respawn bonfire (the host's, until the
// guest rests). WarpSelfBloodMark's map 0x01000000 has no collision; that kick is not started. Returns true
// when the kick itself must not run.
bool prepare_warp_kick(bool dead);

// A boss with a health bar whose fight began in this game since its last load and that still lives (its entity),
// else 0. After a rest it means the fight was lost: only a load puts it back to sleep (world.cpp).
int awake_boss();
// The boss of our boss gauge (its entity), 0 none: the player report passes it on.
int own_gauge_boss();
// A teammate reports this boss on their gauge: if our world holds it, its fight has begun here too.
void note_teammate_boss(int entity);
// The next load is a living guest's reload at its bonfire in the host's world after a lost boss fight: it is placed
// and its copy of the host's world starts over as after a death.
void arm_reset_reload();
// The next load follows the host's Homeward Bone or Darksign: the host's world's temporary flags start over in it.
void arm_trip_reload();
// Spectating: this frame's world is drawn as from the floor `chr` stands on (the draw groups, the area's light, the
// connected blocks), instead of ours. Null when done, else why not (no floor, its block not loaded here).
const char* draw_from(uint32_t chr);
// An event animation starts on a character other than ours (motion.cpp's hook): bosses and watched enemies are
// followed for a few seconds in the log (diagnostic), and the game whose own event run began it moves the character
// until it ends (the others are told through set_event_lead_listener).
void note_event_anim(uint32_t chr, int anim, bool loop);
// Our game began (start) or ended a scripted move of a character (its net handle, the same in every game): `fn`
// tells the others, who follow ours meanwhile.
void set_event_lead_listener(void (*fn)(uint32_t handle, int entity, int anim, bool start));
// Another player's game (Steam id `leader`) said so.
void remote_event_lead(uint32_t handle, int entity, int anim, bool start, uint64_t leader);

// Per-frame record of where the local player stood, for the bloodstain of a guest who dies in the host's world.
void track_safe_position();

// The other players' characters in our world, for the teams (world rules, invaders).
struct RemotePlayer {
    uint32_t chr = 0;
    bool known = false;    // matched to a session player
    bool invader = false;  // that player invades
    bool host = false;     // that player hosts (we are a guest)
    int map = -1;          // the map that player last reported standing in (-1 unknown)
};
void set_remote_players(const std::vector<RemotePlayer>& players);

// Where the other session players stand, as their own games last reported it (alive, heard from lately).
struct PeerSpot {
    float pos[3] = {};
    int map = -1;
    bool dead = false;  // fallen (spectating, or on the way to their bonfire): their game still runs
};
void set_peer_spots(const std::vector<PeerSpot>& spots);
// What this game bids for an enemy now (the game's bid with the session's rules on it), for the log.
uint32_t bid_for(uint32_t chr);
// The enemy's AI is off in this game (Set Character AI State, ChrCtrl+0xC0 bit 0).
bool enemy_ai_off(uint32_t chr);

}  // namespace coop::rules
