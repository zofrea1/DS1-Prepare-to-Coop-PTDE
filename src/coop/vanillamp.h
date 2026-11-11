// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The game's own multiplayer (summon signs, invasions, covenant summons) is off while Prepare to Co-op is on: the
// players meet through the mod's items only. Ghosts, messages and bloodstains stay (steam.cpp shares the game's
// lobbies with every Prepare to Co-op player for them).

#include <cstdint>

namespace coop::vanillamp {

// Patches the game's two steps every vanilla summon and invasion goes through. False when a site does not match.
bool install();

// The note for a vanilla multiplayer item (the soapstones and orbs that would summon or invade), or null for any
// other item. The item's use is then answered with the note instead of its effect (coop.cpp).
const wchar_t* item_note(int32_t goods);

}  // namespace coop::vanillamp
