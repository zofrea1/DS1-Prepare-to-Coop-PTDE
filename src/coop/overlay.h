// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// Text drawn over the game with Direct3D 9 right before the game presents a frame; the text is rendered with GDI
// into a texture when it changes. Three boxes: a small panel (top right) for seamless co-op, the session and the other
// players with the details OverheadPlayerDisplay asks for (ping, soul level, deaths), a notice (top centre, larger,
// its first line gold) such as the update check's on the title screen, and a prompt (bottom centre, its first line
// gold) such as the Death Knell's choice.

#include <string>
#include <vector>

struct IDirect3DDevice9;

namespace coop::overlay {

// Hooks the device's Present and Reset (from IDirect3D9::CreateDevice).
void attach(IDirect3DDevice9* device);
// Replaces the panel's lines (any thread). An empty list hides the panel.
void set_lines(const std::vector<std::wstring>& lines);
// Replaces the notice's lines (any thread). An empty list hides it.
void set_notice(const std::vector<std::wstring>& lines);
// Replaces the prompt's lines (any thread). An empty list hides it.
void set_prompt(const std::vector<std::wstring>& lines);

}  // namespace coop::overlay
