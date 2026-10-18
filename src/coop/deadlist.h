// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// A death in a session keeps the world as it is: the characters that died since the last rest stay dead in the
// reload of the player who fell, host or guest, as they do in every other game of the session. The game has the
// means: the list of a world's dead characters that a host sends a summoned phantom ("CHR " list, WorldChrMan
// 0xE3E540), which a load applies (0xE40EC0: the own world's list, or the host's in someone else's world). The
// host's list now comes with every death of a guest, the guests' with the host's, and the reload takes our own
// world's list merged with theirs. A team wipe still starts the world over, and so does a rest.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::deadlist {

// The dead characters of the world in this game, as the game lists them for a phantom; empty while loading or
// unreadable. Game thread.
std::vector<uint8_t> build();
// Another player's list arrived: the host's (we are a co-op guest), or a co-op guest's (we host). Kept while we
// are dead; a reload that is still to take its list takes the new one.
void note_list(uint64_t from, bool from_host, const uint8_t* data, size_t size);
// A rest, ours or another player's: the world starts over in every game, and the lists from before it are void.
void note_rest();
// Our death's respawn warp goes (an individual death in a session, not a team wipe): the reload keeps the world's
// dead, ours merged with the host's (a guest) or the guests' (the host). False when it does not (a rest during our
// death, or no list): the reload starts the world over.
bool arm(bool own_world);
// Per frame, game thread: drops the kept lists once we live again, and a list the load did not take.
void tick();

}  // namespace coop::deadlist
