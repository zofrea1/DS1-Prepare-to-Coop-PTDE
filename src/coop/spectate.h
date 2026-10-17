// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// Spectating, as in Remastered Seamless Co-op: bosses and invaders are fought as one team. A co-op player (the host
// or a guest) who falls during a boss fight or an invasion while another co-op player still stands does not die
// yet: the game's death event (the global Lua OnEvent_4000: "YOU DIED", the lost souls, the trip to the bonfire) is
// held back, and the camera follows a living teammate. The death goes ahead once the fight is decided: the boss is
// gone and no invader is left, every co-op player has fallen (a team wipe), someone rests at a bonfire, the session
// ends, or after ten minutes. After a team wipe the respawns wait for each other, so that every game reloads its
// world at once (session::team_respawn_wait, world.cpp). Invaders keep the game's own rules.

#include <cstdint>
#include <string>
#include <vector>

namespace coop::spectate {

struct Teammate {
    uint64_t id = 0;
    std::wstring name;
    uint32_t chr = 0;    // their character in our world (0 none: another map, or not built yet)
    bool alive = false;  // as they report it
};

bool install();
// Every frame on the game thread, before tick(): the other co-op players of the session (invaders not included).
void set_teammates(const std::vector<Teammate>& teammates);
void tick();
// Another player rested at a bonfire: a death that waits goes ahead.
void someone_rested();
// Our death waits (we spectate).
bool holding();
// The death event can be held. Without it, world.cpp holds the respawn warp instead (the old limbo).
bool installed();

}  // namespace coop::spectate
