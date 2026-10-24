// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// How often the games of a session tell each other where things are (NetworkTickRate). The game sends its player's
// character (position, facing, animations, HP) six times a second, and the positions of the enemies it moves in a
// round every quarter second; the others are drawn about 1.2 of that gap behind, plus the ping. In a session every
// game sends at the host's rate (session::net_rate): a faster one draws the others closer to where they are, for a
// little more upload (a packet of about 60 bytes to each player per update).
//
// Above the game's own rate the receiving side changes too (NetworkSmoothing, each player's own): of the updates that
// came for a player in one frame only the newest is taken (the game took each, and the second jumped), and a
// character covers each update in at least the gap plus the smoothing (the game's 20% of slack is a few ms at thirty
// a second, so an update a little late made it stop and go). A character is then drawn about one update interval
// plus the smoothing behind, plus the ping: about 58 ms at thirty a second with the default 25 ms.

namespace coop::netrate {

constexpr int kVanilla = 6;         // the game's own rate: every 5 frames at 30 FPS
constexpr int kMax = 30;            // once per 30 FPS frame
constexpr int kMaxSmoothing = 200;  // ms

// The receiving side's patches (newest update, movement and turn in the update's time). False when this build of the
// game differs somewhere (what did match is still used).
bool install();
// NetworkSmoothing in ms (0 .. kMaxSmoothing): the margin for late updates above the game's own rate.
void set_smoothing(int ms);
// Every frame on the game thread: send `rate` updates a second (kVanilla .. kMax), and receive as that rate wants.
void tick(int rate);

}  // namespace coop::netrate
