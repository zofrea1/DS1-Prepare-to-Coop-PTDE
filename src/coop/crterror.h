// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The game's own C runtime errors (its "Microsoft Visual C++ Runtime Library" box), logged with where they came from.

namespace coop::crterror {

// From DllMain.
void install();

}  // namespace coop::crterror
