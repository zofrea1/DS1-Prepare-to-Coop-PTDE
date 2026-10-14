// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The title screen logo while Prepare to Co-op is on: "PREPARE TO CO-OP EDITION" in place of "PREPARE TO DIE
// EDITION". The game's logo texture is swapped as the game creates it, and the title movie that leads into it
// as the game opens it (see title.cpp). `coop` is the PrepareToCoop setting; with it off the game keeps its own
// logo and movie.
bool title_install(bool coop);
