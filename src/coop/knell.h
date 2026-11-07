// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The Death Knell (goods 389009, 1.5.0): a small bell a co-op player rings to go to another co-op player. The ring
// (the Blessed Eye Orb's animation and effect) opens the spectating view on a teammate: D-pad left / right or the
// arrow keys choose among every co-op player of the session and the summon signs they left; B or Esc, or a hit,
// closes it. The view's prompt at the bottom of the screen ("Summon yourself to <name>", where that is) is ours, and A
// or Enter confirms: the ring's animation plays on with the camera still on them, inside the ring's rings, and the
// player goes there: at once when that teammate's character stands in our world, else through a load into their map,
// placed where they stand. The player arrives as a summoned phantom does (one of the game's arrival animations, at
// random); once it lets go, the poise breaks with a riposte's stab and the price is taken (knell.cpp). A guest's join
// plays the same arrival.

#include <cstdint>

struct IDirect3DTexture9;

namespace coop::knell {

bool install();
// The item check: may the Death Knell be rung now (it closes nothing; use() says why it does nothing).
bool can_use();
// The Death Knell was rung (PlayerIns::UseGoods, game thread).
void use();
// Every frame on the game thread.
void tick();
// The view is open (the camera may be on a teammate).
bool viewing();
// The ring's rings on `chr` for this frame (game thread, once a frame while they last): another player's character
// here while that player goes (session.cpp).
void rings(uint32_t chr);
// A texture the game made from data in memory (title.cpp's D3DX hook): the icon atlas gets the Death Knell's and the
// Vigil Soapstone's icons.
void note_texture(IDirect3DTexture9* texture, const void* data, unsigned size);
// Between two frames on the render thread: writes the icons into the atlas once it is known.
void render();
// The HUD line of the drain and the prompt's lines (FMG ids of ours, answered through the items' dialog text).
constexpr uint32_t kDrainedText = 93129710;
constexpr uint32_t kDebuffedText = 93129711;
const wchar_t* text(uint32_t id);
// DeathKnellSoulLevel: going to a teammate takes a soul level instead of their souls and humanity (off by default).
void set_soul_level_price(bool on);
// We arrived in the host's world as a co-op guest (a join, not a reload): the summon arrival plays once we stand.
void joined();
// The session ended or our trip home started (session.cpp): a join's arrival not played yet is dropped.
void left_session(const char* why);
// The ring's animation plays again for the confirmation: an item use it sets off is not the player's (it would use
// whatever item is selected), so it is swallowed. True when `goods` is.
bool swallow_use(int32_t goods);

}  // namespace coop::knell
