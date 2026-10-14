// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
//
// Prepare to Co-op Edition inside the FPS Unlock's xinput1_3.dll: the FPS Unlock calls these at fixed points of
// its own start-up (fps-unlock/src/ptde/extension.h). This file takes the place of its extension_none.cpp.
#include "extension.h"

#include "config.h"
#include "coop/coop.h"
#include "coop/overlay.h"
#include "title.h"
#include "version.h"

namespace {

CoopConfig g_config;
bool g_loaded = false;

// Read once, in DllMain: Steam's app id has to be set before the game starts Steam.
void load(HMODULE dll) {
    if (g_loaded) return;
    g_loaded = true;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(dll, path, MAX_PATH);
    g_config = coop_config_load(path);
}

void attach(HMODULE dll) {
    load(dll);
    coop_install_early(g_config);
}

void start(HMODULE dll) {
    load(dll);
    coop_install(g_config);
    title_install(g_config.enabled);
}

bool wants_device() { return g_config.enabled; }

// The player panel draws on the game's device.
void device_created(IDirect3DDevice9* device) {
    static bool attached = false;
    if (!g_config.enabled || attached) return;
    attached = true;
    coop::overlay::attach(device);
}

const PtdeExtension kExtension = {"Prepare to Co-op Edition v" PREPARE_TO_COOP_VERSION, &attach, &start,
                                  &wants_device, &device_created};

}  // namespace

const PtdeExtension* ptde_extension() { return &kExtension; }
