#pragma once

// Game rule changes for seamless co-op: the hooks that keep a session alive through boss kills, deaths,
// map changes and warps, keep the guest in their own body, and carry the guest's progress home.

#include <cstdint>

namespace coop::rules {

struct State {
    bool active = false;      // a seamless session is running (we host or we are a guest)
    bool host = false;        // we host
    bool guest = false;       // we are a co-op guest in someone's world
    bool invader = false;     // we invade (vanilla invader rules apply to us)
    bool leaving = false;     // we are leaving on purpose: the game's own leave code may run
    bool invader_present = false;  // an invader is in the session (vanilla multiplayer limits apply)
    int own_chr_type = -1;    // guest: our chr type at home (0 human, 8 hollow)
};

State& state();
// Installs every hook; returns the number that failed (0 = all in place).
int install();
// Per-frame upkeep (the room timer, the lock), game thread.
void tick();
// Flags that change in a session's world while seamless rules apply (game thread, value really changed).
void set_flag_listener(void (*fn)(int id, bool on));
// Sets a flag another player changed, without reporting it back.
void apply_remote_flag(int id, bool on);

}  // namespace coop::rules
