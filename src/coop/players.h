// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

#include <cstdint>

// Sessions of more than the game's four players (MaxPlayers, players.cpp).

namespace coop::players {

constexpr int kMaxPlayers = 64;
// The floating health bars at most: every other member of a full party list (partylist::kWide - 1: 63 players and
// eight NPC phantoms).
constexpr int kMaxBars = 71;

// From DllMain, before the game makes its managers: room for `max_players` (4 .. kMaxPlayers) in a session. False
// when this build of the game differs (sessions keep four).
bool install_early(int max_players);
// The players a session can hold (4 unless install_early widened the game).
int max_players();
// The floating health bars are fed by bar_list (more players); bar_chr: the character of bar `slot`
// (0 .. kMaxBars - 1), 0 none.
bool bars_fed();
uint32_t bar_chr(int slot);
// The bars' records (0x20 bytes each: the game's seven at MenuMan+0x8F4, or ours once moved for more bars): the
// record of bar `slot`, and the bar of a record's address (-1 none).
uint32_t bar_record(int slot);
int bar_slot(uint32_t rec);
// The bars the game can draw now: the HUD's floating bar dialogs (seven of its own, more made in it by tick).
int gauges();
// Test (stress.cpp): bars wanted for this many members beyond the session's own (NPC phantoms in for players); while
// above 0 a bar is drawn for any character, enemies standing in as phantoms too.
void want_bars(int extra);
// Test: the bars of the stand-ins on (true) or off, the phantoms kept (to measure the bars alone).
void show_all_bars(bool on);
// Every frame on the game thread.
void tick();
// The start-up line.
void log_status();

}  // namespace coop::players
