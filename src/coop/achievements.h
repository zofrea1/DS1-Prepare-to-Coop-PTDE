// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
// Dark Souls' Steam achievements while the game runs as Spacewar (app 480).
#pragma once

#include <cstdint>

namespace coop::achievements {

// Every co-op tick: once Steam is ready (and `enabled`, and the game runs as another app than Dark Souls), the
// helper starts and the game's achievement calls go to it.
void tick(bool enabled);

// Test (stress "ach get|set N"): calls the game's stats object as the game does. "set" only for an achievement Dark
// Souls already has, so a test never unlocks one.
void test(const char* line, char* reply, size_t size);

}  // namespace coop::achievements
