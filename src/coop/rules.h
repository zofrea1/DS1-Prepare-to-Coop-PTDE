#pragma once

// Game rule changes for seamless co-op: the hooks that keep a session alive through boss kills, deaths,
// map changes and warps, keep the guest in their own body, carry the guest's progress home, and apply
// Remastered Seamless Co-op's world rules (teams, world tendency, bonfire warps and lighting).

#include <cstdint>
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
    int own_chr_type = -1;    // guest: our chr type at home (0 human, 8 hollow)
    // The host's world rules (the host decides, guests mirror them).
    int world_rules = 0;      // Ominous Tome: 0 none, 1 allies are hostile, 2 allies fight in teams
    bool taunt = false;       // Dried Fingers: invaders find this world first
    bool infection = false;   // Cursed Pendant: the world counts as pure black World Tendency
};

State& state();
// Installs every hook; returns the number that failed (0 = all in place).
int install();
// Guests run the world's events in their own game (Remastered Seamless Co-op's rule; default on).
void set_guest_world_events(bool on);
// Per-frame upkeep (the room timer, the lock), game thread.
void tick();
// Flags that change in a session's world while seamless rules apply (game thread, value really changed).
void set_flag_listener(void (*fn)(int id, bool on));
// Sets a flag another player changed, without reporting it back.
void apply_remote_flag(int id, bool on);

// The host travels by bonfire (WarpNextStage_Bonfire): `fn(bonfire)` tells the guests, who follow.
void set_bonfire_warp_listener(void (*fn)(int bonfire));
// A bonfire was lit for the first time (Util_RequestLevelUpFirst): `fn(bonfire)` tells the others.
void set_bonfire_lit_listener(void (*fn)(int bonfire));
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

// Per-frame record of where the local player stood, for the bloodstain of a guest who dies in the host's world.
void track_safe_position();

// The other players' characters in our world, for the teams (world rules, invaders).
struct RemotePlayer {
    uint32_t chr = 0;
    bool known = false;    // matched to a session player
    bool invader = false;  // that player invades
    bool host = false;     // that player hosts (we are a guest)
};
void set_remote_players(const std::vector<RemotePlayer>& players);

}  // namespace coop::rules
