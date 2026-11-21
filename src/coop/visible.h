// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The other players' characters are drawn wherever their map is loaded here, across map boundaries too. A character's
// model is drawn only where its display groups (chr+0x264, its floor's room) meet the draw mask of its own map's block
// here, and that mask comes from where we stand: a player who crossed from the Undead Burg onto the aqueduct to
// Firelink stood in a room of Firelink's map that our mask of that map did not list, so their character vanished while
// their updates (and health bar) went on (user report 2026-11-21, "long standing"). Their model is now given every group
// (the game's 0xE6AA30 hands the model chr+0x264 through 0x468DF0 at 0xE6AAF3), and after the game's character asks
// their map entity is kept asked for (an entity the asks let go is not updated and its model not placed). Enemies and
// NPCs keep the game's rooms.

#include <cstdint>

namespace coop::visible {

bool install();
// Game thread, every frame: notes the other players' characters (WorldChrMan +0x24/+0x28, the local player first).
void tick();
// From the character activation hook (spectate.cpp's hk_activation), after the game's asks: the other players'
// entities asked for again where the asks let them go. Returns how many.
int keep_players_asked();
// The enemies shown for the other players (farenemies.cpp): their models given every group too, as the players'.
void set_extra(const uint32_t* chrs, int n);

}  // namespace coop::visible
