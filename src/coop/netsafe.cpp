// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "netsafe.h"

#include "hook.h"
#include "log.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace coop::netsafe {

namespace {

// The game's reader of a property list sent over the network (0x708AC0, which logs "reading properties" in Japanese;
// called at 0x6F9837 and 0x70C319): each entry is an int, an int64, a double or a text. A text is copied
// one UTF-16 character at a time (the stream reader's 0x608D40, which gives 0 at the end of the data) into a stack
// buffer of 1024 characters, esp+0xC8..esp+0x8C8 in the loop at 0x708D10, until a 0 arrives, with no limit. Right
// after the buffer lie the stack cookie, the exception record and the return address, so a longer text from another
// player overwrites them: the remote code execution reported for Dark Souls in 2022, which Prepare to Die Edition never
// received a patch for. DSCM (metal-crow's DaS-PC-MPChan, commit cbd76b7f, 2022-03-27) published a guard at this
// place; its limit (esp+0xC8+2440) lies past the return address, so this one is written anew with the buffer's own
// end. The copy stops at the buffer's last character, which then always holds the 0 that ends the text: a text is
// cut to 1023 characters and nothing after the buffer is written.
constexpr uint32_t kCopySite = 0x00708D1B;  // call eax (read a character); mov word ptr [edi], ax
constexpr uint32_t kCopyBack = 0x00708D20;  // add edi, 2; test ax, ax; jne 0x708D10
const uint8_t kCopyOld[5] = {0xFF, 0xD0, 0x66, 0x89, 0x07};

LONG g_cut = 0;
int g_state = 0;  // install(): 1 guarded, -1 the site did not match, 0 not tried

void __cdecl note_cut() {
    const LONG n = InterlockedIncrement(&g_cut);
    if (n <= 5 || n % 100 == 0) {
        LOG_ERROR("[coop] A text in network data was longer than the game's buffer (1023 characters) and was cut; "
                  "the unpatched game would have written past its stack (%ld so far)", n);
    }
}

__declspec(naked) void hk_copy_char() {
    __asm {
        call eax                    // the next character (the reader object in ecx, as the game set it)
        lea ecx, [esp + 0x8C6]      // the buffer's last character (the stack is the loop's: reached by a jump)
        cmp edi, ecx
        jb store
        test ax, ax
        jz store                    // the text ends there anyway
        call note_cut               // keeps edi; eax, ecx and edx are set again below or by the loop
        xor eax, eax                // end the text here
    store:
        mov word ptr [edi], ax
        mov ecx, kCopyBack
        jmp ecx
    }
}

}  // namespace

bool install() {
    uint8_t code[5];
    code[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_copy_char) - (kCopySite + 5));
    std::memcpy(code + 1, &rel, 4);
    if (!patch(kCopySite, kCopyOld, code, sizeof(code))) {
        g_state = -1;
        return false;
    }
    g_state = 1;
    return true;
}

void report() {
    if (g_state > 0) {
        LOG_INFO("[coop] Network safety: texts in network property lists are kept within the game's buffer (the 2022 "
                 "remote code execution, 0x%08X)", kCopySite);
    } else if (g_state < 0) {
        LOG_ERROR("[coop] Network safety: the game's property text copy at %08X does not match this build; it is NOT "
                  "guarded", kCopySite);
    }
}

}  // namespace coop::netsafe
