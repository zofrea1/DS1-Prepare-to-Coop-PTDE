#include "text.h"

#include "hook.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

namespace coop::text {

namespace {

constexpr uint32_t kFmgLookup = 0x00D175E0;   // edx = id, [esp+4] = FMG; returns wchar_t* or 0 (cdecl)
constexpr uint32_t kMsgRepoVar = 0x013785DC;  // MsgRepository: FMG of category N at +4 + 4*N, patch FMGs above
constexpr uint32_t kInfoMenu = 0x00D4C090;    // cdecl(type, -1, 0, -1, 1): the information list
constexpr uint32_t kInfoAdd = 0x00D4C190;     // ecx = message id, cdecl(push 30)

// FMG pointers in MsgRepository (base, then the 1.0.2 patch FMG the game checks first).
constexpr uint32_t kGoodsName[2] = {0x2C, 0x1C0};
constexpr uint32_t kGoodsInfo[2] = {0x54, 0x1BC};
constexpr uint32_t kGoodsCaption[2] = {0x64, 0x194};

const wchar_t* (*g_item_text)(uint32_t, Fmg) = nullptr;
const wchar_t* (*g_dialog_text)(uint32_t) = nullptr;
wchar_t g_ring[kMessageSlots][256];
int g_next = 0;
void* g_fmg_orig = nullptr;

Fmg classify(uint32_t fmg) {
    uint32_t repo = 0;
    if (!read32(kMsgRepoVar, &repo) || !repo) return Fmg::Other;
    auto is = [&](const uint32_t (&offs)[2]) {
        for (uint32_t o : offs) {
            uint32_t p = 0;
            if (read32(repo + o, &p) && p == fmg) return true;
        }
        return false;
    };
    if (is(kGoodsName)) return Fmg::GoodsName;
    if (is(kGoodsInfo)) return Fmg::GoodsInfo;
    if (is(kGoodsCaption)) return Fmg::GoodsCaption;
    return Fmg::Other;
}

const wchar_t* __stdcall custom_text(uint32_t id, uint32_t fmg) {
    if (id >= kMessageBase && id < kMessageBase + kMessageSlots) return g_ring[id - kMessageBase];
    const Fmg kind = classify(fmg);
    if (kind != Fmg::Other) return g_item_text ? g_item_text(id, kind) : nullptr;
    return g_dialog_text ? g_dialog_text(id) : nullptr;
}

// Only ids from 300000 up are ours to answer (items 389000+, messages 99900000+).
__declspec(naked) void hk_fmg() {
    __asm {
        cmp edx, 300000
        jb orig
        push edx
        push dword ptr [esp + 8]
        push edx
        call custom_text
        pop edx
        test eax, eax
        jz orig
        ret
    orig:
        jmp g_fmg_orig
    }
}

}  // namespace

bool install() {
    // push ebx ; mov ebx,[esp+8] ; push ebp ; push esi
    static const uint8_t kExpect[7] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55, 0x56};
    g_fmg_orig = detour(kFmgLookup, kExpect, sizeof(kExpect), reinterpret_cast<void*>(&hk_fmg));
    if (!g_fmg_orig) {
        LOG_ERROR("[coop] FMG lookup at %08X does not match this build; co-op texts are unavailable", kFmgLookup);
        return false;
    }
    return true;
}

void set_item_text(const wchar_t* (*fn)(uint32_t, Fmg)) { g_item_text = fn; }
void set_dialog_text(const wchar_t* (*fn)(uint32_t)) { g_dialog_text = fn; }

void show(const std::wstring& line) {
    if (!g_fmg_orig) return;
    const int slot = g_next;
    g_next = (g_next + 1) % kMessageSlots;
    wcsncpy_s(g_ring[slot], line.c_str(), _TRUNCATE);
    const uint32_t id = kMessageBase + slot;
    __try {
        __asm {
            push 1
            push -1
            push 0
            push -1
            push 0
            mov eax, kInfoMenu
            call eax
            add esp, 20
            push 30
            mov ecx, id
            mov eax, kInfoAdd
            call eax
            add esp, 4
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[coop] Showing a message failed");
    }
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &w[0], n);
    return w;
}

}  // namespace coop::text
