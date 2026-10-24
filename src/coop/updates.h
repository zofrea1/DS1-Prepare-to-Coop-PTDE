// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The update check (CheckForUpdates): once per start, the latest release on Prepare to Co-op's GitHub Releases page
// is compared with this build's version, and a newer one is announced in a box on the title screen (overlay.h's
// notice) until a character is loaded. Releases keep the same co-op protocol as long as they can, so an older build
// still plays with a newer one: nothing in a session tells anyone to update.

namespace coop::updates {

// Asks GitHub on a thread of its own (a few seconds at most; nothing happens when there is no answer), against
// `version` (this build's, or the hidden TestVersion key's, which shows the notice on a build that is current).
void start(const char* version);
// Every frame on the game thread: shows the notice while no character is loaded, and closes it for good once one is.
void tick();

}  // namespace coop::updates
