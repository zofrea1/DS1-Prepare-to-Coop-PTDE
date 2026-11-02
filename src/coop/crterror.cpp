// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "crterror.h"

#include "hook.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "dbghelp.lib")

// The game's C runtime (linked into DARKSOULS.exe) reports its fatal errors in a box of its own, through _NMSG_WRITE
// (0xACAB28, cdecl(int code)): R6025 pure virtual function call, R6010 abort() and the rest. None of them is an
// exception, so the crash handler sees nothing and the box waits on the game's thread. On 2026-11-02 a seventh
// player's game stopped at one a moment after arriving and left no trace. Before the box now: the error, the return
// addresses on the stack (the game's, and ours as xinput1_3+offset), and a minidump next to the game
// (PTDE-Coop-runtime-error.dmp). The box then shows as before.

namespace coop::crterror {

namespace {

constexpr uint32_t kMsgWrite = 0x00ACAB28;
const uint8_t kMsgWriteOld[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};  // mov edi, edi ; push ebp ; mov ebp, esp
using MsgWriteFn = void(__cdecl*)(int);
MsgWriteFn g_orig = nullptr;
volatile LONG g_reported = 0;

uintptr_t g_exe_lo = 0, g_exe_hi = 0;  // the game's code
uintptr_t g_dll_lo = 0, g_dll_hi = 0, g_dll_base = 0;  // ours

void code_range(HMODULE m, uintptr_t* lo, uintptr_t* hi) {
    const auto base = reinterpret_cast<uintptr_t>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            *lo = base + s->VirtualAddress;
            *hi = *lo + s->Misc.VirtualSize;
            return;
        }
    }
}

// `ret` follows a call instruction (E8 rel32, FF /2 through a register or memory).
bool after_call(uintptr_t ret) {
    const auto* p = reinterpret_cast<const uint8_t*>(ret);
    if (p[-5] == 0xE8) return true;
    if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) return true;                     // call reg
    if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0x10 && (p[-1] & 7) != 4 && (p[-1] & 7) != 5) return true;  // call [reg]
    if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50) return true;                     // call [reg+d8]
    if (p[-6] == 0xFF && ((p[-5] & 0xF8) == 0x90 || p[-5] == 0x15)) return true;  // call [reg+d32] / [mem]
    if (p[-4] == 0xFF && p[-3] == 0x54) return true;                              // call [reg+reg*s+d8]
    return false;
}

void report(int code) {
    LOG_ERROR("CRASH: the game's C runtime error R60%02d (%s)", code,
              code == 25 ? "pure virtual function call" : code == 10 ? "abort() has been called" : "see the code");
    // The stack, from here to its base.
    const auto* tib = reinterpret_cast<const NT_TIB*>(__readfsdword(0x18));
    uintptr_t at = reinterpret_cast<uintptr_t>(&code);
    const uintptr_t top = reinterpret_cast<uintptr_t>(tib->StackBase);
    char line[1024];
    int len = 0, shown = 0;
    for (; at + 4 <= top && shown < 48; at += 4) {
        const uintptr_t v = *reinterpret_cast<const uintptr_t*>(at);
        const bool exe = v >= g_exe_lo + 6 && v < g_exe_hi;
        const bool dll = v >= g_dll_lo + 6 && v < g_dll_hi;
        if ((!exe && !dll) || !after_call(v)) continue;
        if (exe) {
            len += std::snprintf(line + len, sizeof(line) - len, " %08X", static_cast<unsigned>(v));
        } else {
            len += std::snprintf(line + len, sizeof(line) - len, " xinput1_3+%X", static_cast<unsigned>(v - g_dll_base));
        }
        ++shown;
        if (len > 900 || shown % 12 == 0) {
            LOG_ERROR("CRASH: return addresses on the stack:%s", line);
            len = 0;
        }
    }
    if (len) LOG_ERROR("CRASH: return addresses on the stack:%s", line);
    // The minidump.
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = n ? std::wcsrchr(path, L'\\') : nullptr;
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"PTDE-Coop-runtime-error.dmp");
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        LOG_ERROR("CRASH: the runtime error's minidump could not be written");
        return;
    }
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xE0436F00u | static_cast<DWORD>(code & 0xFF);  // ours: "Co" + the runtime error number
    rec.ExceptionAddress = reinterpret_cast<void*>(ctx.Eip);
    EXCEPTION_POINTERS ep{&rec, &ctx};
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), &ep, FALSE};
    const BOOL ok = MiniDumpWriteDump(
        GetCurrentProcess(), GetCurrentProcessId(), file,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &mei, nullptr,
        nullptr);
    CloseHandle(file);
    LOG_ERROR("CRASH: the runtime error's minidump %s (PTDE-Coop-runtime-error.dmp next to the game)",
              ok ? "was written" : "could not be written");
}

void __cdecl on_msg_write(int code) {
    // 252 (_RT_BANNER) and 255 (_RT_CRNL) are the box's own lines around the real one.
    if (code != 252 && code != 255 && InterlockedExchange(&g_reported, 1) == 0) {
        __try {
            report(code);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR("CRASH: the runtime error R60%02d could not be described further", code);
        }
    }
    g_orig(code);
}

}  // namespace

void install() {
    code_range(GetModuleHandleW(nullptr), &g_exe_lo, &g_exe_hi);
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&install), &self)) {
        code_range(self, &g_dll_lo, &g_dll_hi);
        g_dll_base = reinterpret_cast<uintptr_t>(self);
    }
    g_orig = reinterpret_cast<MsgWriteFn>(
        detour(kMsgWrite, kMsgWriteOld, sizeof(kMsgWriteOld), reinterpret_cast<void*>(&on_msg_write)));
    if (!g_orig) LOG_ERROR("[coop] The game's C runtime error box does not match this build (its errors not logged)");
}

}  // namespace coop::crterror
