// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

#include <cstddef>

// A stress test for sessions of many players (stress.cpp): NPC phantoms stand in for players. Driven by the remote
// control (diagnostics builds; a release build's remote control is a stub, so its commands never come).

namespace coop::stress {

// A remote command of ours (npcs, perf): true when it was one. On the remote control's poll thread.
bool remote(const char* line, char* reply, size_t size);
// Every frame on the game thread.
void tick();

}  // namespace coop::stress
