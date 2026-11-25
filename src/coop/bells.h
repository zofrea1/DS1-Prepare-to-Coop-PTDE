// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The Bells of Awakening heard by other players. Vanilla sends only the Undead Parish's bell to the players a game is
// linked with (its map event 11010700 triggers multiplayer event 10010, and m10_01's event 11015170 tolls the bell for
// whoever has that map loaded). The bell in Quelaag's Domain sent nothing; here its ringing (its lever's flag 11400201,
// set by our own game) goes out as multiplayer event 10020 over the same links, and a game that gets it tolls the bell
// three times as the Parish's does, on the bell itself (o4560, entity 1401110), with its own toll from Blighttown's
// sound bank, when its player is in Blighttown (m14_00): the sound is the bell's, where the bell is, so it carries as
// far as the game's sound for it does.

#include <cstdint>

namespace coop::bells {

// Our game set a flag (not one another player's game sent us): Quelaag's bell's lever sends the toll.
void flag_set_here(int id, bool on);
// A multiplayer event arrived from a linked player's game (netlog.cpp's event meddle hook).
void received(int id);
// Every frame (the game thread): the toll to send, and the tolls to play.
void tick();
// Diagnostics (RemoteControl `bellsound ENTITY SOUND`): plays a sound on an object (from the tick), as the event
// command Play SE.
bool play_sound(int entity, int sound);
// Diagnostics (RemoteControl `bellsend ID`): sends a multiplayer event, as the event command Trigger Multiplayer Event.
void send_event(int id);

}  // namespace coop::bells
