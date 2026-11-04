// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The Death Knell (goods 389009, 1.5.0): a small bell a co-op player rings to go to another co-op player. The first
// use (the Blessed Eye Orb's animation and effect) opens the spectating view on a teammate: D-pad left / right or the
// arrow keys choose among every co-op player of the session; B or Esc, or a hit, closes it. A second use takes the
// player there: at once when that teammate's character stands in our world, else through a load into their map,
// placed where they stand. The player arrives as a summoned phantom does (one of the game's arrival animations, at
// random), and pays one soul level, never below their class's starting level ("! SOUL LEVEL DRAINED !" on the HUD
// either way).

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
// A texture the game made from data in memory (title.cpp's D3DX hook): the icon atlas gets the bell's art.
void note_texture(IDirect3DTexture9* texture, const void* data, unsigned size);
// Between two frames on the render thread: writes the icon into the atlas once it is known.
void render();
// The HUD line of the drain (an FMG id of ours, answered through the items' dialog text).
constexpr uint32_t kDrainedText = 93129710;
const wchar_t* drained_text();

}  // namespace coop::knell
