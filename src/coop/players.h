// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

#include <cstdint>

// Sessions of more than the game's four players (MaxPlayers, players.cpp).

namespace coop::players {

constexpr int kMaxPlayers = 8;

// From DllMain, before the game makes its managers: room for `max_players` (4 .. kMaxPlayers) in a session. False
// when this build of the game differs (sessions keep four).
bool install_early(int max_players);
// The players a session can hold (4 unless install_early widened the game).
int max_players();
// The floating health bars are fed by bar_list (more players); bar_chr: the character of bar `slot` (0 .. 6), 0 none.
bool bars_fed();
uint32_t bar_chr(int slot);
// Every frame on the game thread.
void tick();
// The start-up line.
void log_status();

}  // namespace coop::players
