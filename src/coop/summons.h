// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// NPC phantoms (Solaire and the other summon signs' NPCs) in a session. The host summons one in its game; the game
// tells the session's other players with packet 48 (the NPC's character handle and its party data), and each of them
// turns its own copy of that NPC into the same phantom (0xE407F0) and puts it in its party (0xEA2510). The handle
// names the NPC's map block and its place in it: in vanilla every player stands in the host's map, so the block is
// loaded. A co-op guest or an invader who arrives elsewhere, or comes in later, does not have that block: the lookup
// came back empty and the game read through it (the guests' crash on joining a host with Solaire, 2026-10-23). Here
// the host also sends which NPC each of its phantoms is (kNpcPhantoms, by entity id): a player's game turns its copy
// into the phantom when the NPC is in its world, also after its own loads (a reload makes a fresh, unsummoned copy).
// Every copy goes in as a live summon does, with the summon-in animation: only that draws it in a guest's game, where
// the NPC's own load event disabled it (the game's resend to a joining player skipped it: never seen).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::summons {

struct Phantom {
    int32_t entity = -1;       // the NPC's entity id (the same in every game)
    uint32_t handle = 0;       // its character handle in the host's game
    int32_t kind = 0;          // its character type as a phantom (chr+0x70: 1 white, 2 black)
    int32_t f1 = -1, f2 = -1;  // its party entry's data (+0xC, +0x10), as packet 48 carries them
};

// Host: the NPC phantoms in our party (empty while loading).
std::vector<Phantom> ours();
// The NPC phantoms in our party, for the session's player limit (the game's party has four places).
int count();
// A guest or an invader: the host's NPC phantoms, as it last sent them.
void note_host_list(const std::vector<Phantom>& list);
// Per frame, game thread. `visitor`: we are a co-op guest or an invader in the host's world.
void tick(bool visitor);
// The session is over (settle): every party entry goes but our NPC phantoms (and our own entry), as when the
// game is alone with a summoned NPC. Only in our own world.
bool settle_party(uint32_t party);
// The session is over with no NPC phantom of ours: every party entry goes but our own (type 0, which the game makes
// with our character at 0xEFCB64 and takes out only at the world's teardown, 0xEFC942).
void solo_party(uint32_t party);
// Packet 48 and 49 handlers: the host's handle is put into ours, and a packet for an NPC this game cannot find
// waits for tick.
bool install();

}  // namespace coop::summons
