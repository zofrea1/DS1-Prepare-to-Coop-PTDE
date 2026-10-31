// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

#include <cstdint>

// The game's party list (PartyMemberInfo, [GameMan+0xBEC]) with eight members instead of four (partylist.cpp).

namespace coop::partylist {

constexpr int kWide = 8;

// From DllMain, before the game makes GameMan (and with it the party list). All or nothing: false when this build
// of the game differs, and the list keeps the game's four.
bool install();
bool widened();
// The member entries (from +0x18, 0x14 bytes each): 8 widened, else 4.
int entries();
// The offset of one of the list's own fields after the entries (the game's 0x68 .. 0x88: the NPC summons' handles
// and count, the scripted summon's slot +0x7C) in the list as it is now: moved before the list when widened.
int field(int vanilla_offset);

}  // namespace coop::partylist
