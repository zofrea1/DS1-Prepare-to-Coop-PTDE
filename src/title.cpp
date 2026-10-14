#include "title.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

#include "title_art.inc"

// The logo is the 512x512 DXT5 texture "Title" in menutexture:/english/menu_local.tpf (the English "PREPARE TO DIE
// EDITION" lines) and in menutexture:/menu_0.tpf (the console "with ARTORIAS OF THE ABYSS EDITION" lines). The
// title dialog of menu.drb cuts "DARK SOULS" out of it in three strips, (0,0)-(512,170), (0,170)-(512,340) and
// (0,340)-(155,510); the edition lines sit to the right of the last strip, at about (175,340)-(505,435). The game
// creates its textures from the DDS data in memory with d3dx9_43!D3DXCreateTextureFromFileInMemory(Ex) (imports
// at 0x10CC3FC and 0x10CC40C; DSfix's texture override works on the same call). Both originals are recognized by
// size and FNV-1a hash in that call and replaced by the Prepare to Co-op logo: PrepareToCoop-Title.png (or .dds)
// next to the game if there is one, otherwise the logo built into the DLL (assets/ptde/PrepareToCoop-Title.png).
// D3DX reads PNG as well as DDS and converts it to whatever the game asks for.
constexpr UINT kTitleSize = 349680;
constexpr uint32_t kTitleHashes[] = {0x4646BCA4, 0xA77EED21};  // English menu_local, base menu_0

using FromMemoryEx = HRESULT(WINAPI*)(IDirect3DDevice9*, const void*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                      DWORD, DWORD, D3DCOLOR, void*, PALETTEENTRY*, IDirect3DTexture9**);
using FromMemory = HRESULT(WINAPI*)(IDirect3DDevice9*, const void*, UINT, IDirect3DTexture9**);
FromMemoryEx g_from_memory_ex = nullptr;
FromMemory g_from_memory = nullptr;
std::vector<uint8_t> g_art;
std::string g_art_from;
LONG g_replaced = 0;

uint32_t fnv1a(const uint8_t* p, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

bool is_title(const void* data, UINT size) {
    if (!data || size != kTitleSize) return false;
    uint32_t h = 0;
    __try {
        h = fnv1a(static_cast<const uint8_t*>(data), size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    for (uint32_t k : kTitleHashes) {
        if (h == k) return true;
    }
    return false;
}

void note_replaced() {
    if (InterlockedIncrement(&g_replaced) <= 4) {
        LOG_INFO("Title: \"PREPARE TO CO-OP EDITION\" replaces the game's logo (%s)", g_art_from.c_str());
    }
}

HRESULT WINAPI hk_from_memory_ex(IDirect3DDevice9* device, const void* data, UINT size, UINT width, UINT height,
                                 UINT mips, DWORD usage, D3DFORMAT format, D3DPOOL pool, DWORD filter, DWORD mip_filter,
                                 D3DCOLOR color_key, void* info, PALETTEENTRY* palette, IDirect3DTexture9** texture) {
    if (!g_art.empty() && is_title(data, size)) {
        const HRESULT hr = g_from_memory_ex(device, g_art.data(), static_cast<UINT>(g_art.size()), width, height, mips,
                                            usage, format, pool, filter, mip_filter, color_key, info, palette, texture);
        if (SUCCEEDED(hr)) {
            note_replaced();
            return hr;
        }
        LOG_ERROR("Title: the Prepare to Co-op logo could not be loaded (0x%08lX); the game's own is used", hr);
    }
    return g_from_memory_ex(device, data, size, width, height, mips, usage, format, pool, filter, mip_filter, color_key,
                            info, palette, texture);
}

HRESULT WINAPI hk_from_memory(IDirect3DDevice9* device, const void* data, UINT size, IDirect3DTexture9** texture) {
    if (!g_art.empty() && is_title(data, size)) {
        const HRESULT hr = g_from_memory(device, g_art.data(), static_cast<UINT>(g_art.size()), texture);
        if (SUCCEEDED(hr)) {
            note_replaced();
            return hr;
        }
        LOG_ERROR("Title: the Prepare to Co-op logo could not be loaded (0x%08lX); the game's own is used", hr);
    }
    return g_from_memory(device, data, size, texture);
}

// Points the game's import of `name` from `dll` at `hook`; the previous target goes to *orig.
bool patch_import(const char* dll, const char* name, void* hook, void** orig) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), name) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) return false;
            *orig = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
            return true;
        }
    }
    return false;
}

bool read_file(const std::wstring& path, std::vector<uint8_t>* out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    if (data.size() < 16) return false;
    out->swap(data);
    return true;
}

// PrepareToCoop-Title.png or .dds next to the game, for trying new art without a rebuild.
bool load_art_file() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? L"" : dir.substr(0, slash + 1);
    for (const wchar_t* name : {L"PrepareToCoop-Title.png", L"PrepareToCoop-Title.dds"}) {
        std::vector<uint8_t> data;
        if (!read_file(dir + name, &data)) continue;
        const bool png = std::memcmp(data.data(), "\x89PNG", 4) == 0, dds = std::memcmp(data.data(), "DDS ", 4) == 0;
        char narrow[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
        if (!png && !dds) {
            LOG_ERROR("Title: %s is not a PNG or DDS file; it is not used", narrow);
            continue;
        }
        g_art.swap(data);
        g_art_from = std::string(narrow) + " from the game folder";
        return true;
    }
    return false;
}

}  // namespace

bool title_install(bool coop) {
    if (!coop) {
        LOG_INFO("Title: the game's own logo (Prepare to Co-op is off)");
        return true;
    }
    if (!load_art_file()) {
        g_art.assign(kTitleArt, kTitleArt + sizeof(kTitleArt));
        g_art_from = "built in";
    }
    const bool ex = patch_import("d3dx9_43.dll", "D3DXCreateTextureFromFileInMemoryEx",
                                 reinterpret_cast<void*>(&hk_from_memory_ex), reinterpret_cast<void**>(&g_from_memory_ex));
    const bool plain = patch_import("d3dx9_43.dll", "D3DXCreateTextureFromFileInMemory",
                                    reinterpret_cast<void*>(&hk_from_memory), reinterpret_cast<void**>(&g_from_memory));
    if (!ex && !plain) {
        LOG_ERROR("Title: the game's texture loader import was not found; the game's own logo is shown");
        return false;
    }
    LOG_INFO("Title: \"PREPARE TO CO-OP EDITION\" logo ready (%s, %zu bytes)", g_art_from.c_str(), g_art.size());
    return true;
}
