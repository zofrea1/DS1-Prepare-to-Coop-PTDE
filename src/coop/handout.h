// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The session host's hand-out of enemies settled in time linear in the bids, with the game's results (handout.cpp,
// handout_pair.h).

#include <cstddef>

namespace coop::handout {

// Replaces the pair loop of the hand-out (0xE559DD in 0xE558F0). False when the game's code there, or the pair step
// it calls, is not this build's (the game's loop is then left as it is).
bool install();

// The passes run so far and the last one's numbers, for the remote control's `handout` (diagnostics builds).
void describe(char* out, size_t size);

}  // namespace coop::handout
