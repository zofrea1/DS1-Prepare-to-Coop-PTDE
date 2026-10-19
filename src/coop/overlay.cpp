// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "overlay.h"

#include "deathtext.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#pragma comment(lib, "gdi32.lib")

#include <algorithm>
#include <cstring>
#include <mutex>

namespace coop::overlay {

namespace {

using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
constexpr int kResetSlot = 16;
constexpr int kPresentSlot = 17;

PresentFn g_present = nullptr;
ResetFn g_reset = nullptr;

std::mutex g_mutex;
std::vector<std::wstring> g_lines;
bool g_dirty = false;

IDirect3DTexture9* g_tex = nullptr;
IDirect3DStateBlock9* g_state = nullptr;
int g_tex_w = 0, g_tex_h = 0;
int g_text_w = 0, g_text_h = 0;
bool g_visible = false;

void* patch_slot(void* object, int slot, void* replacement) {
    void** vtable = *reinterpret_cast<void***>(object);
    void* original = vtable[slot];
    if (original == replacement) return nullptr;
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    vtable[slot] = replacement;
    DWORD ignored = 0;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &ignored);
    return original;
}

// Renders the lines with GDI and uploads them as white text on a translucent dark box.
void rebuild(IDirect3DDevice9* dev, const std::vector<std::wstring>& lines, int screen_h) {
    g_visible = !lines.empty();
    if (!g_visible) return;
    const int px = std::max(14, screen_h / 54);
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    int w = 0, line_h = px + px / 4;
    for (const auto& l : lines) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, l.c_str(), static_cast<int>(l.size()), &sz);
        w = std::max<int>(w, sz.cx);
    }
    const int pad = px / 2;
    g_text_w = w + 2 * pad;
    g_text_h = static_cast<int>(lines.size()) * line_h + 2 * pad;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = g_text_w;
    bi.bmiHeader.biHeight = -g_text_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        // out of GDI memory: no panel until the next change
        if (bmp) DeleteObject(bmp);
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        g_visible = false;
        return;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    std::memset(bits, 0, static_cast<size_t>(g_text_w) * g_text_h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (size_t i = 0; i < lines.size(); ++i) {
        TextOutW(dc, pad, pad + static_cast<int>(i) * line_h, lines[i].c_str(), static_cast<int>(lines[i].size()));
    }
    GdiFlush();
    if (!g_tex || g_tex_w < g_text_w || g_tex_h < g_text_h) {
        if (g_tex) g_tex->Release();
        g_tex = nullptr;
        g_tex_w = std::max(256, g_text_w);
        g_tex_h = std::max(64, g_text_h);
        if (FAILED(dev->CreateTexture(g_tex_w, g_tex_h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr))) {
            g_tex = nullptr;
        }
    }
    if (g_tex) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(g_tex->LockRect(0, &lr, nullptr, 0))) {
            for (int y = 0; y < g_tex_h; ++y) {
                auto* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
                for (int x = 0; x < g_tex_w; ++x) {
                    if (x >= g_text_w || y >= g_text_h) {
                        dst[x] = 0;
                        continue;
                    }
                    const uint32_t s = static_cast<const uint32_t*>(bits)[y * g_text_w + x];
                    const uint32_t c = std::max({s & 0xFF, (s >> 8) & 0xFF, (s >> 16) & 0xFF});
                    const uint32_t a = 110 + c * 145 / 255;
                    dst[x] = (a << 24) | (c << 16) | (c << 8) | c;
                }
            }
            g_tex->UnlockRect(0);
        }
    }
    SelectObject(dc, old_bmp);
    SelectObject(dc, old_font);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);
}

struct Vertex {
    float x, y, z, rhw, u, v;
};

void draw(IDirect3DDevice9* dev) {
    D3DVIEWPORT9 vp{};
    dev->GetViewport(&vp);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_dirty) {
            g_dirty = false;
            rebuild(dev, g_lines, static_cast<int>(vp.Height));
        }
    }
    if (!g_visible || !g_tex) return;
    if (!g_state && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_state))) {
        g_state = nullptr;
        return;
    }
    g_state->Capture();
    const float margin = vp.Height / 40.0f;
    const float x1 = vp.X + vp.Width - margin, x0 = x1 - g_text_w;
    const float y0 = vp.Y + margin * 3.0f, y1 = y0 + g_text_h;
    const float u1 = static_cast<float>(g_text_w) / g_tex_w, v1 = static_cast<float>(g_text_h) / g_tex_h;
    const Vertex quad[4] = {
        {x0 - 0.5f, y0 - 0.5f, 0, 1, 0, 0},
        {x1 - 0.5f, y0 - 0.5f, 0, 1, u1, 0},
        {x0 - 0.5f, y1 - 0.5f, 0, 1, 0, v1},
        {x1 - 0.5f, y1 - 0.5f, 0, 1, u1, v1},
    };
    if (SUCCEEDED(dev->BeginScene())) {
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetTexture(0, g_tex);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
        dev->EndScene();
    }
    g_state->Apply();
}

HRESULT WINAPI hk_present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
    deathtext::render();  // the death text's band, between two frames (deathtext.cpp)
    __try {
        draw(dev);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int reported = 0;
        if (reported++ < 3) LOG_ERROR("[coop] Overlay drawing failed");
    }
    return g_present(dev, src, dst, wnd, dirty);
}

HRESULT WINAPI hk_reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_state) {
        g_state->Release();
        g_state = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_dirty = true;  // re-render at the new size
    }
    return g_reset(dev, pp);
}

}  // namespace

void attach(IDirect3DDevice9* device) {
    if (!device) return;
    void* p = patch_slot(device, kPresentSlot, reinterpret_cast<void*>(&hk_present));
    if (p && !g_present) g_present = reinterpret_cast<PresentFn>(p);
    void* r = patch_slot(device, kResetSlot, reinterpret_cast<void*>(&hk_reset));
    if (r && !g_reset) g_reset = reinterpret_cast<ResetFn>(r);
    LOG_INFO("[coop] Player panel %s", g_present && g_reset ? "ready" : "could not hook the device");
}

void set_lines(const std::vector<std::wstring>& lines) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (lines == g_lines) return;
    g_lines = lines;
    g_dirty = true;
}

}  // namespace coop::overlay
