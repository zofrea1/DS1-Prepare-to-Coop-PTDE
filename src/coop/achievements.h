// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
// Dark Souls' Steam achievements while the game runs as Spacewar (app 480).
#pragma once

#include <cstdint>

namespace coop::achievements {

// Every co-op tick: once Steam is ready (and `enabled`, and the game runs as another app than Dark Souls), the
// helper starts and the game's achievement calls go to it.
void tick(bool enabled);

// Test (stress "ach get|set N", "ach witness FLAG"): calls the game's stats object as the game does. "set" only for an
// achievement Dark Souls already has, so a test never unlocks one; "witness" runs witnessed() for that flag.
void test(const char* line, char* reply, size_t size);

// A world event whose map script awards an achievement to the player whose game runs it (a bell rung, a boss
// defeated): whether `flag` (the award event's own flag) is one.
bool witnessed_award(int flag);
// That event happened in another player's game of our co-op session (its flag came on here from a session message):
// the game's own award call gives us its achievement too (the user's rule, 2026-11-26). `from`: whose message.
void witnessed(int flag, const char* from);

}  // namespace coop::achievements
