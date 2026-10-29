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
// What a game tells the others of an NPC's line.
enum Kind : int {
    kPage = 0,   // it turned to page `row` of `line`
    kEnd = 1,    // it was cut short (not one that ran out: every copy runs out by itself)
    kStart = 2,  // it began `line`, from `row` (a paused line goes on from its page)
};
// Where our lines go: `talk` is the NPC's talk id (its MSB part's, the same in every game), `line` the TalkParam row
// the line began with, `row` the one it says now.
void set_listener(void (*fn)(int talk, int line, int row, int a2, int a3, Kind kind));
// Another player's game: the NPC with talk id `talk` says `row` of `line` (or stops).
void remote(int talk, int line, int row, int a2, int a3, Kind kind);
// Every frame: the log's watch on our own lines' pages.
void tick();

}  // namespace coop::talk
