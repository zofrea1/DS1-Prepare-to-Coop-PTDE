// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// Vagrants (the game's FrpgNetItemVagrantDb): items a player lost and humanity they never recovered, passed from one
// linked game to the next until they appear in a world in the area they were lost in. Few players and short
// sessions meant they never did (2026-11-25): an item had to wander through ten games first, and the game holding
// it when it closed took it along. The user's choice: VagrantWanders (how far an item wanders before it can come as a
// Good Vagrant), VagrantStay (how long each game keeps one) and VagrantPassOnAtQuit (a game passes on what it holds
// before it leaves). With NetworkEventLog the map loads' vagrant checks have their lines too.

#include <cstddef>

namespace coop::vagrants {

struct Settings {
    int wanders = 2;               // VagrantWanders, 2..10 (the game's own 10)
    int stay_seconds = 120;        // VagrantStay, 10..300 (the game's own 300)
    bool pass_on_at_quit = true;   // VagrantPassOnAtQuit
    bool log = true;               // NetworkEventLog
    bool firelink_spots = true;    // VagrantSpotsFirelink: Firelink Shrine's map gets vagrant spots
};

void install(const Settings& s);
// Every frame on the game thread: the hand-offs before a return to the title or a close, the test requests.
void tick();
// How many vagrants this game holds (-1 when its list is not there), and how many linked games it can pass them to.
int held();
int targets();
// Test (RemoteControl "vagrant"): on the next frame, a vagrant at our feet as if another game had passed it on, for
// map index `map_index` (the number the map load's check is given; -1: the last one checked), with wander count
// `wanders`; `humanity`: the game's humanity vagrant (goods 350) instead of an item (Prism Stone).
void add_test(int map_index, int wanders, bool humanity);
// Test (RemoteControl "vagrantcheck N"): on the next frame, the map check for map index N, as a load runs it.
void check_test(int map_index);

}  // namespace coop::vagrants
