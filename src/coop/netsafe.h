// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// Guards for the game's own network code: data from another player that the unpatched game would let write past
// its buffers (netsafe.cpp).

namespace coop::netsafe {

// Installs the guards. Done at startup whether co-op is on or off, before any network traffic (and before the log
// is open). False when a site does not match this build.
bool install();

// Logs what install() did (once the log is open).
void report();

}  // namespace coop::netsafe
