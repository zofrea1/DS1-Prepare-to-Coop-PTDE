// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The death text of a team wipe: "YOUR TEAM DIED" instead of "YOU DIED" when every co-op player fell and the world
// starts over. "YOU DIED" is no string but a band of the game's English menu atlas (see deathtext.cpp); for a wipe
// that band is lettered anew (assets/TeamDied.png, made from the game's own letters by tools/build_team_died.py)
// while the text shows, and the game's own band is put back afterwards. Every other death keeps "YOU DIED".

struct IDirect3DTexture9;

namespace coop::deathtext {

// Hooks the game's SetTextEffect (the death script asks for the text there).
bool install();
// A texture the game just created from file data (title.cpp's hooks on the game's texture loader; any thread).
void note_texture(IDirect3DTexture9* texture, const void* data, unsigned size);
// The render thread, right before the game presents a frame: the band takes the text that is asked for.
void render();
// Game thread, every frame: once the player stands in the world again, the band goes back to the game's own text.
void tick();

}  // namespace coop::deathtext
