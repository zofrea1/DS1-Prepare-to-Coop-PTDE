// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The log upload (UploadLogs, off by default): when the game closes, this game's log (PTDE-FPS-Unlock.log, next to
// the DLL) is sent to the developer's upload folder UploadLogsTo names (its user and password are the developer's
// upload account), as <UploadLogsName or the PC's name>/<the log's start>.log, so the developer gets the logs of the
// players who help with the mod without asking for files. A log a crash or a kill left behind goes at the next
// start instead (the FPS Unlock keeps it as PTDE-FPS-Unlock.log.prev). Nothing is sent unless both keys are set.
// The log holds the Steam names and ids of the players met, their characters' names and the PC's name.

namespace coop::logupload {

// `dll_path`: our DLL's path (the log and the record of what was sent lie beside it). Hooks the game's exit (its
// KERNEL32 imports ExitProcess and TerminateProcess), and sends a previous log not sent yet, on a thread of its own.
void install(const wchar_t* dll_path, bool on, const char* to, const char* name);

}  // namespace coop::logupload
