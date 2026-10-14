// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// Prepare to Co-op Edition: co-op for Dark Souls: Prepare to Die Edition ([PrepareToCoop] in
// PTDE-FPS-Unlock.ini).
//
// A reimplementation for PTDE of the ideas of Yui's Dark Souls Remastered Seamless Co-op (ds1sc),
// written from reverse engineering notes; no code of that mod is used. Play the whole game together:
//
//  * Players with the same password find each other through an invisible Steam lobby (under Spacewar,
//    app 480, because PTDE's own lobbies are switched off) and a private peer-to-peer channel.
//  * Joining uses the game's own session and summon path (the host's session invitation), but the guest
//    keeps their own body (human or hollow), can pick up items, rest, level up and talk to NPCs, and
//    stays in the host's world through deaths, boss kills, area changes and the host's warps.
//  * Progress made in the host's world (event flags) is also written to the guest's own world.
//  * Optional invasions, per-player enemy scaling, overhead player info, a separate save file.
//
// See docs/PREPARE-TO-COOP.md for the design and the game functions involved.

#include "config.h"

// DllMain: Steam app id and the per-frame hook (must run before the game initialises Steam).
void coop_install_early(const CoopConfig& config);
// Worker thread, after the game's code is unpacked: game hooks.
void coop_install(const CoopConfig& config);
// True while seamless co-op is switched on (settings), for other modules.
bool coop_enabled();
