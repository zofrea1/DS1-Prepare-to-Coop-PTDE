// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The drops a world gives once. Some enemies never come back once they are dead: the Black Knights, the crystal
// lizards, the Titanite Demons, the mimics, Havel, the bosses. Their map scripts mark them gone with a flag, and on
// every later load the enemy is removed without a drop. In a session each game rolls its own drops, from its own
// copy of the enemy: a script's award goes straight into the inventory, the enemy's own lot drops on the floor
// where it dies. A player whose game did not see the death (loading, or in a map their game has not loaded) or
// whose character was down at that moment (the floor drop needs a living player; a fallen boss fighter spectates)
// never got their roll, and the enemy is gone for good.
//
// Every game of a session already learns of the death: its flag is shared (the host passes a guest's on). When
// such a flag turns on, this game checks a few seconds later whether it got the drop itself; if not, and the enemy
// is not standing in our game, it rolls the lot as the game does (our own luck, our own one-time item flags) and
// puts what it draws in the inventory. In a session the floor drops of these enemies go into the inventory too,
// so no reload (a death, a team wipe, a trip home) can take one off the floor before it is picked up.
//
// An NPC's gift works the same way: the talk script turns a shared flag on and a common event in every game awards
// the gift, wherever its player is; a game that was loading at that moment gets its roll from here.

namespace coop::drops {

// The floor drop hook (0xE806BD). False when this build does not match.
bool install();
// A flag that marks a one-time drop's enemy gone (or a boss's rewards due).
bool watched(int flag);
// Such a flag changed in our game: set by our own scripts or by another player's (the session's shared flags).
void flag_changed(int flag, bool on);
// Many flags were written at once (the host's world state or map flags taken in): what the poll reads next is a new
// baseline, not changes.
void flags_rewritten();
// A map script is about to award an item lot in our game (Award Items, Award Item Lot). `event`/`slot`: the map
// event running it (0 outside one). False: this drop was given already (by us); the award is skipped.
bool allow_award(int lot, int event, int slot);
// Per frame, game thread: the drops our game missed, rolled and given once the player can take them.
void tick();

}  // namespace coop::drops
