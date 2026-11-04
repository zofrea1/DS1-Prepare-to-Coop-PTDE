// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// Spectating, as in Remastered Seamless Co-op: bosses and invaders are fought as one team. A co-op player (the host
// or a guest) who falls during a boss fight or an invasion while another co-op player still stands does not die
// yet: the game's death event (the global Lua OnEvent_4000: "YOU DIED", the lost souls, the trip to the bonfire) is
// held back, and the camera follows a living teammate. The death goes ahead once the fight is decided: the boss is
// gone and no invader is left, every co-op player has fallen (a team wipe), someone rests at a bonfire, the session
// ends, or after ten minutes. After a team wipe the respawns wait for each other, so that every game reloads its
// world at once (session::team_respawn_wait, world.cpp). Invaders are a team of their own (1.3.0, a host of 1.3.0 or
// later): an invader who falls while another invader stands spectates them until the invasion is decided.

#include <cstdint>
#include <string>
#include <vector>

namespace coop::spectate {

struct Teammate {
    uint64_t id = 0;
    std::wstring name;
    uint32_t chr = 0;    // their character in our world (0 none: another map, or not built yet)
    bool alive = false;  // as they report it
    // Where they stand as they report it every second (Info), for the Death Knell (knell.cpp).
    int map = -1;          // area << 24 | block << 16 ..., -1 unknown
    float pos[3] = {};
    double pos_at = 0;     // when map and pos came (GetTickCount64 seconds), 0 never
    bool boss_fight = false;  // a boss gauge is up in their game
};

bool install();
// Every frame on the game thread, before tick(): the other co-op players of the session (invaders not included).
void set_teammates(const std::vector<Teammate>& teammates);
void tick();
// Another player rested at a bonfire: a death that waits goes ahead.
void someone_rested();
// The host's trip home (Homeward Bone, Darksign, Homeward miracle) reached us while we are down: our death goes
// ahead now and is not held again until we live (we follow once back on our feet).
void trip_release();
// Our death waits (we spectate).
bool holding();
// The death event can be held. Without it, world.cpp holds the respawn warp instead (the old limbo).
bool installed();
// The other co-op players as set_teammates last gave them.
const std::vector<Teammate>& teammates();
// The Death Knell's view (knell.cpp): the camera follows `chr` (a teammate's character in our world) while we stand;
// 0 gives the camera back. Ignored while our death waits (that view comes first).
void knell_view(uint32_t chr);
// D-pad left or right (any controller) or the arrow keys pressed since the last call, while the game has the focus:
// -1, 1, or 0. The spectating view and the Death Knell's take turns (never both at once).
int switch_step();
// The controllers' buttons (XINPUT_GAMEPAD bits of every connected pad, polled every 30 ms).
uint16_t pad_buttons();

}  // namespace coop::spectate
