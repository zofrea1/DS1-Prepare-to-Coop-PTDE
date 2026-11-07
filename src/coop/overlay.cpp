// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "overlay.h"

#include "deathtext.h"
#include "knell.h"

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

// One box of text: its lines (under g_mutex), and the texture they were last rendered to (Present only).
struct Box {
    std::vector<std::wstring> lines;
    bool dirty = false;
    IDirect3DTexture9* tex = nullptr;
    int tex_w = 0, tex_h = 0;
    int text_w = 0, text_h = 0;
    bool visible = false;
    bool notice = false;  // larger, gold first line, more opaque, drawn at the top centre
    bool prompt = false;  // as a notice, drawn at the bottom centre
};

std::mutex g_mutex;
Box g_panel;
Box g_notice_box;
Box g_prompt_box;
IDirect3DStateBlock9* g_state = nullptr;
bool g_notice_told = false;  // the notice's first frame since it was rendered was logged

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

HFONT make_font(int px) {
    return CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

// Renders the lines with GDI and uploads them as text on a translucent dark box (white; a notice's first line gold).
// The text is sized by the height a 16:9 picture as wide as the screen would have (no more than the screen's), and
// made smaller until the box fits the screen's width: sized by the height alone, on a portrait screen (2160 x 3840,
// FixAspect) the Death Knell's prompt was some 2900 pixels wide and was not seen at all (2026-11-06).
void rebuild(IDirect3DDevice9* dev, Box& box, const std::vector<std::wstring>& lines, int screen_w, int screen_h) {
    box.visible = !lines.empty();
    if (!box.visible) return;
    const int ref = std::min(screen_h, screen_w * 9 / 16);
    const int min_px = 12;
    int px = std::max(box.notice ? 16 : 14, ref / (box.notice ? 38 : 54));
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = nullptr;
    HGDIOBJ old_font = nullptr;
    int w = 0;
    for (int pass = 0; pass < 4; ++pass) {
        HFONT f = make_font(px);
        HGDIOBJ was = SelectObject(dc, f);
        if (!old_font) old_font = was;
        if (font) DeleteObject(font);
        font = f;
        w = 0;
        for (const auto& l : lines) {
            SIZE sz{};
            GetTextExtentPoint32W(dc, l.c_str(), static_cast<int>(l.size()), &sz);
            w = std::max<int>(w, sz.cx);
        }
        const int pad_now = box.notice ? px : px / 2;
        const int room = screen_w * 94 / 100 - 2 * pad_now;
        if (w <= room || px <= min_px || w <= 0) break;
        px = std::max(min_px, px * room / w);
    }
    const int line_h = px + px / 4;
    const int pad = box.notice ? px : px / 2;
    box.text_w = w + 2 * pad;
    box.text_h = static_cast<int>(lines.size()) * line_h + 2 * pad;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = box.text_w;
    bi.bmiHeader.biHeight = -box.text_h;
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
        box.visible = false;
        return;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    std::memset(bits, 0, static_cast<size_t>(box.text_w) * box.text_h * 4);
    SetBkMode(dc, TRANSPARENT);
    for (size_t i = 0; i < lines.size(); ++i) {
        SetTextColor(dc, box.notice && i == 0 ? RGB(232, 196, 104) : RGB(255, 255, 255));
        TextOutW(dc, pad, pad + static_cast<int>(i) * line_h, lines[i].c_str(), static_cast<int>(lines[i].size()));
    }
    GdiFlush();
    if (!box.tex || box.tex_w < box.text_w || box.tex_h < box.text_h) {
        if (box.tex) box.tex->Release();
        box.tex = nullptr;
        box.tex_w = std::max(256, box.text_w);
        box.tex_h = std::max(64, box.text_h);
        if (FAILED(dev->CreateTexture(box.tex_w, box.tex_h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &box.tex, nullptr))) {
            box.tex = nullptr;
        }
    }
    if (box.tex) {
        const uint32_t base = box.notice ? 175 : 110;  // the box's own opacity
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(box.tex->LockRect(0, &lr, nullptr, 0))) {
            for (int y = 0; y < box.tex_h; ++y) {
                auto* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
                for (int x = 0; x < box.tex_w; ++x) {
                    if (x >= box.text_w || y >= box.text_h) {
                        dst[x] = 0;
                        continue;
                    }
                    // Text drawn on black: its brightest channel is how much of it covers the pixel.
                    const uint32_t s = static_cast<const uint32_t*>(bits)[y * box.text_w + x];
                    const uint32_t c = std::max({s & 0xFF, (s >> 8) & 0xFF, (s >> 16) & 0xFF});
                    const uint32_t a = base + c * (255 - base) / 255;
                    dst[x] = (a << 24) | (s & 0xFFFFFF);
                }
            }
            box.tex->UnlockRect(0);
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

// Where a box goes on screen: the panel at the top right, a notice at the top centre, a prompt at the bottom centre
// (above the game's own action box, where its prompt would be).
void place(const Box& box, const D3DVIEWPORT9& vp, float* x0, float* y0) {
    const float margin = vp.Height / 40.0f;
    if (box.prompt) {
        *x0 = vp.X + (vp.Width - box.text_w) / 2.0f;
        *y0 = vp.Y + vp.Height * 0.78f - box.text_h;
    } else if (box.notice) {
        *x0 = vp.X + (vp.Width - box.text_w) / 2.0f;
        *y0 = vp.Y + margin * 3.0f;
    } else {
        *x0 = vp.X + vp.Width - margin - box.text_w;
        *y0 = vp.Y + margin * 3.0f;
    }
}

// The box's quad is cut to the screen (with its texture coordinates): a screen-space quad reaching past the screen's
// edge may not be drawn at all.
void draw_box(IDirect3DDevice9* dev, const Box& box, const D3DVIEWPORT9& vp) {
    if (!box.visible || !box.tex) return;
    float x0 = 0, y0 = 0;
    place(box, vp, &x0, &y0);
    float x1 = x0 + box.text_w, y1 = y0 + box.text_h;
    const float tw = static_cast<float>(box.tex_w), th = static_cast<float>(box.tex_h);
    float u0 = 0, v0 = 0, u1 = box.text_w / tw, v1 = box.text_h / th;
    const float left = static_cast<float>(vp.X), top = static_cast<float>(vp.Y);
    const float right = left + vp.Width, bottom = top + vp.Height;
    if (x0 < left) {
        u0 += (left - x0) / tw;
        x0 = left;
    }
    if (y0 < top) {
        v0 += (top - y0) / th;
        y0 = top;
    }
    if (x1 > right) {
        u1 -= (x1 - right) / tw;
        x1 = right;
    }
    if (y1 > bottom) {
        v1 -= (y1 - bottom) / th;
        y1 = bottom;
    }
    if (x1 <= x0 || y1 <= y0) return;
    const Vertex quad[4] = {
        {x0 - 0.5f, y0 - 0.5f, 0, 1, u0, v0},
        {x1 - 0.5f, y0 - 0.5f, 0, 1, u1, v0},
        {x0 - 0.5f, y1 - 0.5f, 0, 1, u0, v1},
        {x1 - 0.5f, y1 - 0.5f, 0, 1, u1, v1},
    };
    dev->SetTexture(0, box.tex);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
}

// What the game had bound when it presented, when that was not the whole back buffer (logged on each change, a few
// times): on the title screen the box drawn into the game's own target and viewport was never seen.
void note_target(IDirect3DSurface9* rt, IDirect3DSurface9* back, const D3DVIEWPORT9& vp, const D3DSURFACE_DESC& bd) {
    static int logged = 0;
    static bool last_other = false;
    static D3DVIEWPORT9 last_vp{};
    const bool other = rt != back;
    const bool full = vp.X == 0 && vp.Y == 0 && vp.Width == bd.Width && vp.Height == bd.Height;
    if (!other && full) return;
    if (other == last_other && std::memcmp(&vp, &last_vp, sizeof(vp)) == 0) return;
    last_other = other;
    last_vp = vp;
    if (logged >= 8) return;
    ++logged;
    D3DSURFACE_DESC rd{};
    if (rt) rt->GetDesc(&rd);
    LOG_INFO("[coop] Overlay: the game presented with %s %ux%u bound, viewport %lu,%lu %lux%lu (screen %ux%u); "
             "drawing on the screen",
             other ? "another target" : "the screen", rd.Width, rd.Height, vp.X, vp.Y, vp.Width, vp.Height,
             bd.Width, bd.Height);
}

void draw(IDirect3DDevice9* dev) {
    // Drawn into the back buffer over all of it, whatever target, depth surface and viewport the game left bound.
    IDirect3DSurface9* back = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
    D3DSURFACE_DESC bd{};
    back->GetDesc(&bd);
    const D3DVIEWPORT9 vp{0, 0, bd.Width, bd.Height, 0.0f, 1.0f};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Box* box : {&g_panel, &g_notice_box, &g_prompt_box}) {
            if (box->dirty) {
                box->dirty = false;
                rebuild(dev, *box, box->lines, static_cast<int>(vp.Width), static_cast<int>(vp.Height));
                if (box == &g_notice_box) g_notice_told = false;
            }
        }
    }
    if ((!g_panel.visible || !g_panel.tex) && (!g_notice_box.visible || !g_notice_box.tex) &&
        (!g_prompt_box.visible || !g_prompt_box.tex)) {
        back->Release();
        return;
    }
    if (!g_state && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_state))) {
        g_state = nullptr;
        back->Release();
        return;
    }
    static DWORD targets = 0;
    if (!targets) {
        D3DCAPS9 caps{};
        targets = SUCCEEDED(dev->GetDeviceCaps(&caps)) ? std::min<DWORD>(std::max<DWORD>(caps.NumSimultaneousRTs, 1), 4) : 1;
    }
    g_state->Capture();  // the viewport and every state below; targets and the depth surface are kept by hand
    IDirect3DSurface9* old_rt[4] = {};
    IDirect3DSurface9* old_ds = nullptr;
    for (DWORD i = 0; i < targets; ++i) {
        if (FAILED(dev->GetRenderTarget(i, &old_rt[i]))) old_rt[i] = nullptr;
    }
    if (FAILED(dev->GetDepthStencilSurface(&old_ds))) old_ds = nullptr;
    D3DVIEWPORT9 game_vp{};
    dev->GetViewport(&game_vp);
    note_target(old_rt[0], back, game_vp, bd);
    dev->SetRenderTarget(0, back);
    for (DWORD i = 1; i < targets; ++i) {
        if (old_rt[i]) dev->SetRenderTarget(i, nullptr);
    }
    dev->SetDepthStencilSurface(nullptr);
    dev->SetViewport(&vp);
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
        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        draw_box(dev, g_panel, vp);
        draw_box(dev, g_notice_box, vp);
        draw_box(dev, g_prompt_box, vp);
        dev->EndScene();
        if (!g_notice_told && g_notice_box.visible && g_notice_box.tex) {
            g_notice_told = true;
            LOG_INFO("[coop] Overlay: notice drawn (%dx%d on a %ux%u screen)", g_notice_box.text_w,
                     g_notice_box.text_h, bd.Width, bd.Height);
        }
    }
    // The targets first (each SetRenderTarget resets the viewport), then the state block puts the viewport back.
    dev->SetRenderTarget(0, old_rt[0] ? old_rt[0] : back);
    for (DWORD i = 1; i < targets; ++i) {
        if (old_rt[i]) dev->SetRenderTarget(i, old_rt[i]);
    }
    dev->SetDepthStencilSurface(old_ds);
    for (IDirect3DSurface9* s : old_rt) {
        if (s) s->Release();
    }
    if (old_ds) old_ds->Release();
    g_state->Apply();
    back->Release();
}

HRESULT WINAPI hk_present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
    deathtext::render();  // the death text's band, between two frames (deathtext.cpp)
    knell::render();      // the Death Knell's icon, once its atlas is loaded (knell.cpp)
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
        g_panel.dirty = true;  // re-render at the new size
        g_notice_box.dirty = true;
        g_prompt_box.dirty = true;
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
    if (lines == g_panel.lines) return;
    g_panel.lines = lines;
    g_panel.dirty = true;
}

void set_notice(const std::vector<std::wstring>& lines) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_notice_box.notice = true;
    if (lines == g_notice_box.lines) return;
    g_notice_box.lines = lines;
    g_notice_box.dirty = true;
}

void set_prompt(const std::vector<std::wstring>& lines) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_prompt_box.notice = true;
    g_prompt_box.prompt = true;
    if (lines == g_prompt_box.lines) return;
    g_prompt_box.lines = lines;
    g_prompt_box.dirty = true;
}

}  // namespace coop::overlay
