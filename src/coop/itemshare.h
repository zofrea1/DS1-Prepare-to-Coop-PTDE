// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// Items dropped for the other co-op players. An item a co-op player drops from the inventory (a weapon, a shield, a
// soul, anything the game lets them drop and marks for the other players: the game's own "multiplayer" bit on the
// floor item, the one vanilla co-op hands to the host's phantoms) lies on the floor in every co-op player's game, and
// whoever picks it up first takes it: it is gone from everyone's floor. The session host decides who was first.

#include <cstdint>
#include <vector>

namespace coop::itemshare {

constexpr int kEntryBytes = 0x90;  // the item manager's entry for a floor item (the object's +0x10)

struct Drop {
    uint64_t origin = 0;  // the player who dropped it (Steam id)
    uint32_t seq = 0;     // their count: the drop is (origin, seq) in every game
    int32_t map = -1;     // the map it lies in (map_id(): area << 24 | block << 16)
    uint8_t entry[kEntryBytes] = {};
};

// The hooks on the game's item manager. False when this build does not match.
bool install();
// ShareDroppedItems.
void set_enabled(bool on);
// Every frame: whether we share drops now (a co-op host with a session, or a co-op guest in a sharing host's world),
// whether we are the host, and our Steam id.
void set_session(bool coop, bool host, uint64_t self);
// Where our game's news goes (session.cpp sends it): a drop of ours or one the host passes on, a guest's request to
// pick one up (to the host), and the host's word on who took one (to everyone).
void set_out(void (*drop)(const Drop& d), void (*claim)(uint64_t origin, uint32_t seq),
             void (*taken)(uint64_t origin, uint32_t seq, uint64_t taker));
// From the others: a drop (from the host, or a guest's to the host), a guest's request (the host), who took it.
void remote_drop(const Drop& d);
void remote_claim(uint64_t from, uint64_t origin, uint32_t seq);
void remote_taken(uint64_t origin, uint32_t seq, uint64_t taker);
// The host: the drops still on the floor, for a guest who arrives.
std::vector<Drop> live();
void tick();

}  // namespace coop::itemshare
