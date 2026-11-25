// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// NetworkEventLog: the game's own online extras in the log, as they arrive from the other Prepare to Co-op players
// the game is linked with (its "nodes": steam.cpp shares the game's lobbies among every Prepare to Co-op player).
// Prepare to Die Edition keeps them in network databases (FrpgNet*Db) that travel between the players' games over
// those links: the shared events (the Undead Parish's bell, multiplayer event 10010), vagrants, miracle resonance
// rings, basilisk curse statues, Gravelord Servants' invitations, bonfire kindling, the Oolacile arena's entrants and
// the covenant rankings each have a line as they come; messages, bloodstains, ghosts, signs and the node lists are
// counted and summed up every five minutes. The bell being rung here (the event that sends 10010) has a line too, and
// so do the players the game picks to link with (its lobby's neighbour keys) and the distance it searches.

#include <cstdint>

namespace coop::netlog {

// Hooks each database's packet handler and the multiplayer event's trigger. `on`: NetworkEventLog.
void install(bool on);
// Every frame (the game thread): the map for the lines, and the five-minute summary.
void tick();
// The game's own lobby data as it sets it (steam.cpp): the neighbours it picked, logged when they change.
void lobby_data(uint64_t lobby, const char* key, const char* value);
// The distance the game's node search asks Steam for (0 close, 1 default, 2 far, 3 worldwide), logged on change.
void search_distance(int value);

}  // namespace coop::netlog
