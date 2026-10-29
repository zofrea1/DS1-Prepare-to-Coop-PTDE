// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// NPC voice lines heard by every player near the NPC. A line is played by the game of the player it concerns: the
// one who talks to the NPC, or whose game's talk script reacts to the NPC being hit or killed. The other games
// never played it, so the voice and its subtitle were the speaker's alone. Each line our game starts (and each page
// it moves on to, and a line it cuts short) is told to the others, and a game whose player stands near its own copy
// of that NPC plays the same line on it.

namespace coop::talk {

// The hooks on the game's talk player. False when this build does not match.
bool install();
// Where our lines go: `talk` is the NPC's talk id (its MSB part's, the same in every game), `row` the TalkParam row
// it says now; `end` a line cut short.
void set_listener(void (*fn)(int talk, int row, int a2, int a3, bool end));
// Another player's game: the NPC with talk id `talk` says `row` (or stops, `end`).
void remote(int talk, int row, int a2, int a3, bool end);

}  // namespace coop::talk
