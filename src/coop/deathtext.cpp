// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "deathtext.h"

#include "game.h"
#include "hook.h"
#include "log.h"
#include "world.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace coop::deathtext {

namespace {

#include "team_died_art.inc"

// Lua SetTextEffect(type) (0xD5E430, stdcall, ret 4) queues a text effect for the menu (0xC2A7B0, MenuMan+0xE60).
// The death script (OnEvent_4000 -> SetTextEffect(DeadtextEffectId)) asks for TEXT_TYPE_Dead (2), or for
// TEXT_TYPE_GhostDead (6) when the death costs nothing (isDeathPenaltySkip). Their menu.drb dialogs, F23_YouDied
// and F23_YouDide_phantom, both draw the sprite main_text_youdied: rows 496-596 of the 1024 x 1024 DXT5 atlas
// Menu14 (menu.drb texture 16; menu/english/menu_local.tpf, 11 levels) across its whole width, onto 1024 x 100 of
// the 1280 x 720 menu screen. The band is opaque: dark red letters on black. "YOU DIED" takes 475 of its 1024
// pixels, so "YOUR TEAM DIED" (856) fits at the same size.
constexpr uint32_t kSetTextEffect = 0x00D5E430;
constexpr int kTextDead = 2;
constexpr int kTextGhostDead = 6;
constexpr unsigned kAtlasSize = 1398256;     // the English Menu14 as the game hands it to D3DX (a DDS file)
constexpr uint32_t kAtlasHash = 0x38B3EDFD;  // FNV-1a of it
constexpr int kBandTop = 496;
constexpr int kBandBottom = 596;
constexpr int kBandWidth = 1024;
// The levels written. The menu shows the band at its size or larger (1024 of 1280 at 720p and up), so the smaller
// levels are a margin only. Each level takes the band's whole 4 x 4 blocks (DXT5).
constexpr int kLevels = 4;
constexpr DWORD kFilterNone = 1;      // D3DX_FILTER_NONE
constexpr DWORD kFilterTriangle = 4;  // D3DX_FILTER_TRIANGLE

using TextFn = void(__stdcall*)(int);
using LoadSurfaceFn = HRESULT(WINAPI*)(IDirect3DSurface9*, const PALETTEENTRY*, const RECT*, const void*, UINT,
                                       const RECT*, DWORD, D3DCOLOR, void*);

TextFn g_text_orig = nullptr;
LoadSurfaceFn g_load_surface = nullptr;  // d3dx9_43!D3DXLoadSurfaceFromFileInMemory (reads the PNG, writes DXT5)

std::mutex g_mutex;                     // the atlas and its saved band (loader and render threads)
IDirect3DTexture9* g_atlas = nullptr;   // one reference held
std::vector<uint8_t> g_saved[kLevels];  // the band's blocks as the game made them
bool g_have_saved = false;
std::atomic<int> g_shown{0};            // what the band holds: 0 the game's text, 1 ours (render thread)
std::atomic<bool> g_broken{false};      // the atlas could not be changed: the game's text stays
std::atomic<int> g_want{0};             // 1 from a team wipe's death text until we stand again (game thread)
ULONGLONG g_up_since = 0;

uint32_t fnv1a(const uint8_t* p, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

bool is_atlas(const void* data, unsigned size) {
    if (!data || size != kAtlasSize) return false;
    __try {
        return fnv1a(static_cast<const uint8_t*>(data), size) == kAtlasHash;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The band's whole blocks at a level.
RECT band_rect(int level) {
    RECT r;
    r.left = 0;
    r.right = kBandWidth >> level;
    r.top = ((kBandTop >> level) + 3) & ~3;
    r.bottom = (kBandBottom >> level) & ~3;
    return r;
}

size_t band_bytes(const RECT& r) { return static_cast<size_t>((r.bottom - r.top) / 4) * ((r.right - r.left) / 4) * 16; }

bool atlas_fits(IDirect3DTexture9* t) {
    D3DSURFACE_DESC d{};
    __try {
        if (t->GetLevelCount() < static_cast<DWORD>(kLevels) || FAILED(t->GetLevelDesc(0, &d))) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return d.Format == D3DFMT_DXT5 && d.Width == 1024 && d.Height == 1024 && d.Pool == D3DPOOL_MANAGED;
}

// Copies a level's band blocks out of the atlas (save) or back into it.
bool copy_blocks(IDirect3DTexture9* t, int level, const RECT* r, uint8_t* buf, bool save) {
    const int rows = (r->bottom - r->top) / 4;
    const int row_bytes = (r->right - r->left) / 4 * 16;
    D3DLOCKED_RECT lr{};
    __try {
        if (FAILED(t->LockRect(level, &lr, r, save ? D3DLOCK_READONLY : 0)) || !lr.pBits) return false;
        for (int y = 0; y < rows; ++y) {
            uint8_t* line = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch;
            if (save) {
                std::memcpy(buf + y * row_bytes, line, row_bytes);
            } else {
                std::memcpy(line, buf + y * row_bytes, row_bytes);
            }
        }
        t->UnlockRect(level);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Letters a level's band from the PNG (D3DX scales it down for the smaller levels and compresses it).
bool write_level(IDirect3DTexture9* t, int level, const RECT* r) {
    IDirect3DSurface9* s = nullptr;
    HRESULT hr = E_FAIL;
    __try {
        if (FAILED(t->GetSurfaceLevel(level, &s)) || !s) return false;
        RECT src;
        src.left = 0;
        src.right = kBandWidth;
        src.top = (r->top << level) - kBandTop;
        src.bottom = (r->bottom << level) - kBandTop;
        hr = g_load_surface(s, nullptr, r, kTeamDiedArt, sizeof(kTeamDiedArt), &src,
                            level == 0 ? kFilterNone : kFilterTriangle, 0, nullptr);
        s->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return SUCCEEDED(hr);
}

void give_up(const char* why) {
    g_broken = true;
    LOG_ERROR("[coop] Death text: %s; a team wipe shows YOU DIED", why);
}

bool restore_all() {
    bool ok = true;
    for (int level = 0; level < kLevels; ++level) {
        const RECT r = band_rect(level);
        ok &= copy_blocks(g_atlas, level, &r, g_saved[level].data(), false);
    }
    return ok;
}

void __stdcall hk_set_text_effect(int type) {
    if (type == kTextDead || type == kTextGhostDead) {
        const bool wipe = world::team_wipe_death();
        g_want = wipe ? 1 : 0;
        g_up_since = 0;
        if (wipe) {
            LOG_INFO("[coop] Death text: YOUR TEAM DIED (a team wipe)%s",
                     g_broken ? ", but the atlas could not be changed" : "");
        }
    }
    g_text_orig(type);
}

}  // namespace

bool install() {
    // mov ecx,[esp+4] ; test ecx,ecx
    static const uint8_t k[6] = {0x8B, 0x4C, 0x24, 0x04, 0x85, 0xC9};
    g_text_orig = reinterpret_cast<TextFn>(detour(kSetTextEffect, k, sizeof(k), reinterpret_cast<void*>(&hk_set_text_effect)));
    if (!g_text_orig) {
        LOG_ERROR("[coop] Death text: SetTextEffect does not match this build; a team wipe shows YOU DIED");
        return false;
    }
    return true;
}

void note_texture(IDirect3DTexture9* texture, const void* data, unsigned size) {
    if (!texture || !is_atlas(data, size)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (texture == g_atlas) return;
    texture->AddRef();
    if (g_atlas) g_atlas->Release();
    g_atlas = texture;
    g_have_saved = false;
    g_shown = 0;
    g_broken = false;
    LOG_INFO("[coop] Death text: the menu atlas with YOU DIED is loaded (a team wipe shows YOUR TEAM DIED)");
}

void render() {
    const int want = g_want.load();
    if (want == g_shown.load() || g_broken.load()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_atlas) return;
    if (!g_load_surface) {
        const HMODULE d3dx = GetModuleHandleW(L"d3dx9_43.dll");
        g_load_surface = d3dx ? reinterpret_cast<LoadSurfaceFn>(GetProcAddress(d3dx, "D3DXLoadSurfaceFromFileInMemory"))
                              : nullptr;
        if (!g_load_surface) {
            give_up("d3dx9_43.dll's D3DXLoadSurfaceFromFileInMemory was not found");
            return;
        }
    }
    if (!g_have_saved) {
        if (!atlas_fits(g_atlas)) {
            give_up("the menu atlas is not the 1024 x 1024 DXT5 managed texture it should be");
            return;
        }
        for (int level = 0; level < kLevels; ++level) {
            const RECT r = band_rect(level);
            g_saved[level].resize(band_bytes(r));
            if (!copy_blocks(g_atlas, level, &r, g_saved[level].data(), true)) {
                give_up("the menu atlas could not be read");
                return;
            }
        }
        g_have_saved = true;
    }
    if (want == 1) {
        bool ok = true;
        for (int level = 0; level < kLevels && ok; ++level) {
            const RECT r = band_rect(level);
            ok = write_level(g_atlas, level, &r);
        }
        if (!ok) {
            restore_all();
            give_up("YOUR TEAM DIED could not be written into the menu atlas");
            return;
        }
    } else if (!restore_all()) {
        give_up("YOU DIED could not be put back into the menu atlas");
        return;
    }
    g_shown = want;
    LOG_INFO("[coop] Death text: the menu atlas shows %s", want ? "YOUR TEAM DIED" : "YOU DIED again");
}

void tick() {
    if (g_want.load() == 0) return;
    if (!game::player() || game::loading() || world::local_dead()) {
        g_up_since = 0;
        return;
    }
    const ULONGLONG t = GetTickCount64();
    if (g_up_since == 0) {
        g_up_since = t;
    } else if (t - g_up_since > 2000) {
        g_want = 0;  // standing again since the reload: the next death says YOU DIED unless it is a wipe too
    }
}

}  // namespace coop::deathtext
