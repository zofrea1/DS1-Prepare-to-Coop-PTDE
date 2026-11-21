// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// EnemiesNearOtherPlayers (default on, 2026-11-21, the user's idea): the enemies around another player in our map are
// moved and drawn here however far from us, so a player on the Undead Burg's bridge watches a teammate fight the
// hollows in the streets below. Our game only shows them: another player's game runs each of them (it is in that
// player's rooms there), our copy follows its updates as it does for any enemy another game runs near us, our game
// never bids for one (so its AI never runs here, far from us) and is paid nothing for its death. Regular enemies only
// (team 6): no NPCs, bosses or the Hellkite, nor an enemy our game runs or one that runs its own AI in every game.

#include <cstdint>

namespace coop::farenemies {

void set_enabled(bool on);
bool enabled();
// From the character activation hook (spectate.cpp's hk_activation), after the game's asks: the enemies around the
// other players in our map asked for. Game thread.
void ask(uint32_t world_chr_man);
// An enemy asked for here only because another player is near it (not around us).
bool shown_only(uint32_t chr);
// Game thread, every frame: the log, and everything let go when there is no session.
void tick();

}  // namespace coop::farenemies
