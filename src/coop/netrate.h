// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// How often the games of a session tell each other where things are (NetworkTickRate). The game sends its player's
// character (position, facing, animations, HP) six times a second, and the positions of the enemies it moves in a
// round every quarter second; the others are drawn about 1.2 of that gap behind, plus the ping. In a session every
// game sends at the host's rate (session::net_rate): a faster one draws the others closer to where they are, for a
// little more upload (a packet of about 60 bytes to each player per update).

namespace coop::netrate {

constexpr int kVanilla = 6;  // the game's own rate: every 5 frames at 30 FPS
constexpr int kMax = 30;     // once per 30 FPS frame

// The receiving side: another player's character turns toward each update in the time the updates come at, instead
// of a fixed fifth of the turn per 30 FPS frame (made for six a second). False when this build of the game differs.
bool install();
// Every frame on the game thread: send `rate` updates a second (kVanilla .. kMax).
void tick(int rate);

}  // namespace coop::netrate
