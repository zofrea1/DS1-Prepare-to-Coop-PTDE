// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// Jolly cooperation (1.9.0): two co-op players who Praise the Sun together, side by side, are each given the warmth
// of Replenishment (the miracle's own effect, SpEffect 570: 10 HP a second for a minute) and the line "Jolly
// cooperation!" in the action box. Each game sees its own player begin the gesture and tells the others (the
// session's kPraise message, through the host); a game whose player began it within kWindow of a teammate's,
// with that teammate's character within kReach here, gives the effect to its own player only (each game does the
// same for its own). Once every kCooldown for each player. Invaders neither send nor count.

#include <cstdint>
#include <string>

namespace coop::jolly {

// [PrepareToCoopDetails] JollyCooperation (default on).
void set_enabled(bool on);
// Hidden INI key JollyTest (default off): Beckon counts as Praise the Sun, and our own gesture counts as a
// teammate's too, so the detection, the effect and the line can be seen in one game without a session.
void set_test(bool on);
// Every frame on the game thread. True once each time our player begins Praise the Sun (the session tells the others).
bool tick();
// Another co-op player began Praise the Sun; `chr` is their character here (0 none), `name` what to call them.
void remote_praise(uint64_t who, uint32_t chr, const std::wstring& name);

}  // namespace coop::jolly
