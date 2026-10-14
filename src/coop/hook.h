// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// Small code-patching toolkit for the seamless co-op module. Every patch first checks the bytes it
// replaces, so a different game build is left alone instead of being corrupted.

#include <cstddef>
#include <cstdint>

namespace coop {

// True when the `n` bytes at `addr` equal `expect`.
bool bytes_match(uint32_t addr, const void* expect, size_t n);

// Writes `n` bytes at `addr` after checking they currently equal `expect` (null skips the check).
bool patch(uint32_t addr, const void* expect, const void* repl, size_t n);

// Detours the function at `addr`: its first `stolen` bytes must equal `expect` and contain only whole
// instructions; a rel32 call/jmp among them must start at an offset listed in `rel_at` (up to two) so it
// can be moved. Returns the trampoline that runs the original (stolen bytes + jump back), or null.
void* detour(uint32_t addr, const uint8_t* expect, size_t stolen, void* hook, int rel_at0 = -1, int rel_at1 = -1);

// Retargets the call instruction (E8 rel32) at `site`, which must currently call `expect_target`.
bool redirect_call(uint32_t site, uint32_t expect_target, const void* new_target);

// Replaces a pointer in a vtable (or any pointer slot) after checking its current value. `old` receives it.
bool swap_pointer(uint32_t slot, uint32_t expect, const void* value, uint32_t* old = nullptr);

// Reads guarded against access violations.
bool read32(uint32_t addr, uint32_t* out);
bool read8(uint32_t addr, uint8_t* out);
template <typename T>
inline T* ptr(uint32_t addr) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(addr));
}

}  // namespace coop
