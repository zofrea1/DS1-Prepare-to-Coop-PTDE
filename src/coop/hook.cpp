// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "hook.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

namespace coop {

namespace {

bool writable_copy(uint32_t addr, const void* src, size_t n) {
    auto* dst = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(addr));
    DWORD old = 0;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(dst, src, n);
    DWORD ignored = 0;
    VirtualProtect(dst, n, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

uint8_t* alloc_code(size_t n) {
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
}

}  // namespace

bool bytes_match(uint32_t addr, const void* expect, size_t n) {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), expect, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool patch(uint32_t addr, const void* expect, const void* repl, size_t n) {
    if (expect && !bytes_match(addr, expect, n)) return false;
    return writable_copy(addr, repl, n);
}

void* detour(uint32_t addr, const uint8_t* expect, size_t stolen, void* hook, int rel_at0, int rel_at1) {
    if (stolen < 5 || stolen > 32 || !bytes_match(addr, expect, stolen)) return nullptr;
    uint8_t* tramp = alloc_code(64);
    if (!tramp) return nullptr;
    std::memcpy(tramp, expect, stolen);
    // A rel32 call/jmp (E8/E9 xx xx xx xx) inside the stolen bytes keeps its absolute target.
    const int rels[2] = {rel_at0, rel_at1};
    for (int at : rels) {
        if (at < 0) continue;
        int32_t rel = 0;
        std::memcpy(&rel, expect + at + 1, 4);
        const uint32_t target = addr + at + 5 + rel;
        const int32_t moved = static_cast<int32_t>(target - (reinterpret_cast<uintptr_t>(tramp) + at + 5));
        std::memcpy(tramp + at + 1, &moved, 4);
    }
    tramp[stolen] = 0xE9;
    const int32_t back = static_cast<int32_t>((addr + stolen) - (reinterpret_cast<uintptr_t>(tramp) + stolen + 5));
    std::memcpy(tramp + stolen + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);

    uint8_t jmp[32];
    std::memset(jmp, 0x90, sizeof(jmp));
    jmp[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (addr + 5));
    std::memcpy(jmp + 1, &rel, 4);
    if (!writable_copy(addr, jmp, stolen)) return nullptr;
    return tramp;
}

bool redirect_call(uint32_t site, uint32_t expect_target, const void* new_target) {
    uint8_t cur[5];
    __try {
        std::memcpy(cur, reinterpret_cast<const void*>(static_cast<uintptr_t>(site)), 5);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    int32_t rel = 0;
    std::memcpy(&rel, cur + 1, 4);
    if (cur[0] != 0xE8 || site + 5 + rel != expect_target) return false;
    const int32_t now = static_cast<int32_t>(reinterpret_cast<uintptr_t>(new_target) - (site + 5));
    return writable_copy(site + 1, &now, 4);
}

bool swap_pointer(uint32_t slot, uint32_t expect, const void* value, uint32_t* old) {
    uint32_t cur = 0;
    if (!read32(slot, &cur) || cur != expect) return false;
    if (old) *old = cur;
    const uint32_t v = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value));
    return writable_copy(slot, &v, 4);
}

bool read32(uint32_t addr, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint32_t*>(static_cast<uintptr_t>(addr));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read8(uint32_t addr, uint8_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint8_t*>(static_cast<uintptr_t>(addr));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace coop
