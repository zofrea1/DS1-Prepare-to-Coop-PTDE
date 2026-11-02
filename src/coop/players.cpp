// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "players.h"

#include "game.h"
#include "hook.h"
#include "partylist.h"
#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>

// More than four players in a session (MaxPlayers 5 to 8). The game holds four: itself and three others.
//  * The game session's member limit, SessMan+0x8C, is set to 4 when the session manager starts (0xD2CB1B) and is
//    what the session's room is made with (0xD2D4F0 -> attribute 0x80000001).
//  * The network library (DLNRD) makes each session object (SteamSessionLight) with a pool of member objects and
//    their streams, made up front for a number of people, its own player among them: the session factory,
//    SteamMatchingSystem's slot 9 (0x722940, thiscall(out, people), ret 8), passes people - 1 and people to the
//    constructor (0x72B040 -> 0x713050, +0x144 and +0x148), and 4 and 5 when asked for none: the game's four and one
//    to spare. A member the pool has no object for is not added ("member joined but could not be added", 0x72BEEC)
//    and its join falls through. Until 1.4.0's last test build this was left at five: on 2026-11-02 a sixth player
//    was counted for a moment and gone again three times, and one knocking on a session of five was turned away by
//    its own game within 0.2 s (notice 20001004), with nothing of it in our log.
//  * The network library allocates from the game's network heap ([0x13787F8], 0x137F768 + 0x104), made once with a
//    block of 4 MB (0xF8B520: push 400000h at 0xF8B595 for the CRT's _aligned_malloc, and the same size at 0x40A50F
//    for the heap's set-up 0x40D390 and at 0x40A530 for the memory ranges' list 0x562C70). Every frame of a session
//    the game asks it for its free space (vt+0x18, 0xD2EF0C) and leaves the session when under 64 KB
//    (OnIrregularLeaveSession). A seventh player of 2026-11-02 got to the world, all six others made, and left it
//    0.3 s later so, back home; on the try before, the game died at a C runtime error box a moment after arriving.
//    With more players the heap is made 16 MB (kNetHeap); the lowest free space of each minute in a session is logged.
//  * GameDataMan keeps the other players' PlayerGameData in one block of three (+0xC, 0x4C0 each, made by the
//    constructor 0xC06710 as 0x10 + 0xE40 bytes with the count at block+0xC), a state byte for each (+0x10, three
//    bytes; 0x1F once the player is ready to be made) and another byte each inline at +0x84..+0x86. A member's slot
//    is the first free state byte (0xC02CA0 from the member-joined handler 0xD2DF80); every user checks the slot
//    against 2 or 3 or walks the block to 0xE40. Two resets (session start 0xD29940, join 0xD2D200) and three
//    readers (0xC5AF40, 0xD59B7E, 0xD6A850) are written out for three slots.
//  * WorldChrMan's player characters live in a set of four (+0x24, made by 0xE63C90: 4 x 0x20); the others are
//    put in a free entry (0xE63F10) when they are made (0xEE2B10 -> 0xE40970).
// All of it is widened here before the game makes these objects (from DllMain), each patch checked against the
// bytes it replaces; nothing changes at four. The party list (PartyMemberInfo [GameMan+0xBEC]) is widened apart,
// at any player count, to eight entries (partylist.cpp); only when that does not fit this build does it keep the
// game's four, and then a fifth player simply has no entry and none of the game's own party bookkeeping (its add
// 0xEA2510 refuses when no entry is free). The floating health bars do not need it: the HUD has seven of them
// (records MenuMan+0x8F4 + 0x20 * slot, slot < 7 in the widget 0xFA89C0 and in the filler 0x1012C30), which the
// filler fed from party entry `slot`
// while `slot` < the party's count (+0x10), entry 0, our own character, drawing nothing. The filler takes its
// characters from bar_list instead (at four players too: an NPC phantom in one of the party's entries left the
// fourth player without a bar): every other player's character first (WorldChrMan's player set), then the party's
// entries but ours (its NPC phantoms), seven at most. An NPC never keeps a bar from a player; it gets one back as
// soon as a player leaves.

namespace coop::players {

namespace {

int g_max = 4;      // players in a session
int g_remote = 3;   // the others (g_max - 1)
// The party list's scripted-summon slot (the game's +0x7C; -0x10 once partylist.cpp moved the list's fields).
int32_t g_party_slot = 0x7C;
bool g_ok = false;  // every patch in place
bool g_zeroed = false;
// The inline bytes GameDataMan+0x84 + slot, moved here for every slot.
uint8_t g_slot_bytes[16] = {};

constexpr uint32_t kGameDataManVar = 0x01378700;
constexpr uint32_t kPgdSize = 0x4C0;
constexpr uint32_t kResetTail = 0x00D29590;  // what both unrolled resets call last (esi = the session manager)

struct Bound {
    uint32_t at;
    uint8_t code[6];
    int len;
};

// `cmp reg, 2` before `ja`: the slot is past the last.
const Bound kLastSlot[] = {
    {0x00C02C80, {0x83, 0xF9, 0x02}, 3}, {0x00C02CC0, {0x83, 0xF8, 0x02}, 3}, {0x00C02E23, {0x83, 0xFE, 0x02}, 3},
    {0x00C02E77, {0x83, 0xFE, 0x02}, 3}, {0x00C037A7, {0x83, 0xFA, 0x02}, 3}, {0x00D27F56, {0x83, 0xF8, 0x02}, 3},
    {0x00D2B19A, {0x83, 0xFF, 0x02}, 3}, {0x00D2B214, {0x83, 0xFF, 0x02}, 3}, {0x00D2C380, {0x83, 0xF8, 0x02}, 3},
    {0x00D2E6AF, {0x83, 0xFB, 0x02}, 3}, {0x00D2E7A9, {0x83, 0xFB, 0x02}, 3}, {0x00D2FB73, {0x83, 0xFE, 0x02}, 3},
    {0x00D2FDF1, {0x83, 0xFF, 0x02}, 3}, {0x00D30431, {0x83, 0xF8, 0x02}, 3}, {0x00D31C66, {0x83, 0xFE, 0x02}, 3},
    {0x00EE2B8A, {0x83, 0xF8, 0x02}, 3},
};
// `cmp reg, 3`: the slot count.
const Bound kSlotCount[] = {
    {0x00C02CB7, {0x83, 0xF8, 0x03}, 3},  // the free-slot search (state byte 0)
    {0x00C03760, {0x83, 0xF9, 0x03}, 3},
    {0x00C068A4, {0x83, 0xFE, 0x03}, 3},  // the constructor's loop
};
// `cmp reg, 0E40h`: the end of the block.
const Bound kBlockEnd[] = {
    {0x00C02E3B, {0x81, 0xFA, 0x40, 0x0E, 0x00, 0x00}, 6}, {0x00C02E90, {0x81, 0xFA, 0x40, 0x0E, 0x00, 0x00}, 6},
    {0x00C037BF, {0x81, 0xF9, 0x40, 0x0E, 0x00, 0x00}, 6}, {0x00C039F9, {0x81, 0xFF, 0x40, 0x0E, 0x00, 0x00}, 6},
    {0x00C06ADE, {0x81, 0xFB, 0x40, 0x0E, 0x00, 0x00}, 6}, {0x00C2B7A1, {0x81, 0xF9, 0x40, 0x0E, 0x00, 0x00}, 6},
    {0x00C5AF2B, {0x81, 0xFF, 0x40, 0x0E, 0x00, 0x00}, 6}, {0x00D6B360, {0x81, 0xFF, 0x40, 0x0E, 0x00, 0x00}, 6},
    {0x00D75E54, {0x81, 0xFB, 0x40, 0x0E, 0x00, 0x00}, 6},
};

// The inline slot bytes: [GameDataMan + slot + 84h] -> [slot + g_slot_bytes]. `mod` is the new ModRM byte for
// [slot register + disp32]; the old instruction's tail after the new one is filled with nops.
struct SlotByte {
    uint32_t at;
    uint8_t code[8];
    int len;
    uint8_t op;   // the opcode (C6 store imm8, 8A load al, 88 store al)
    uint8_t mod;  // ModRM for [reg + disp32]
    bool imm;     // a C6 store carries its imm8 (0)
};
const SlotByte kSlotBytes[] = {
    {0x00C02C93, {0xC6, 0x84, 0x0A, 0x84, 0x00, 0x00, 0x00, 0x00}, 8, 0xC6, 0x81, true},   // [edx+ecx+84h] = 0
    {0x00C02E4E, {0x8A, 0x84, 0x3E, 0x84, 0x00, 0x00, 0x00}, 7, 0x8A, 0x86, false},        // al = [esi+edi+84h]
    {0x00C02EA6, {0x88, 0x84, 0x3E, 0x84, 0x00, 0x00, 0x00}, 7, 0x88, 0x86, false},        // [esi+edi+84h] = al
    {0x00C03786, {0xC6, 0x84, 0x0A, 0x84, 0x00, 0x00, 0x00, 0x00}, 8, 0xC6, 0x81, true},   // [edx+ecx+84h] = 0
    {0x00D2B234, {0xC6, 0x84, 0x3B, 0x84, 0x00, 0x00, 0x00, 0x00}, 8, 0xC6, 0x87, true},   // [ebx+edi+84h] = 0
    {0x00D2E7CF, {0xC6, 0x84, 0x19, 0x84, 0x00, 0x00, 0x00, 0x00}, 8, 0xC6, 0x83, true},   // [ecx+ebx+84h] = 0
};
// The unrolled resets' `mov byte ptr [eax+84h+n], bl` -> `mov byte ptr [g_slot_bytes+n], bl`.
const uint32_t kResetBytes[] = {0x00D29969, 0x00D29983, 0x00D29996, 0x00D2D224, 0x00D2D23E, 0x00D2D24F};
const uint32_t kResetCalls[] = {0x00D2999C, 0x00D2D255};  // call 0xD29590, right after each reset

// The slots past the game's three, as the two resets leave the first three: unused (+8 = -1), state 0, byte 0.
void __stdcall reset_extra_slots() {
    uint32_t gdm = 0, block = 0, states = 0;
    for (int s = 0; s < g_remote; ++s) g_slot_bytes[s] = 0;
    if (!read32(kGameDataManVar, &gdm) || !gdm || !read32(gdm + 0xC, &block) || !read32(gdm + 0x10, &states)) return;
    for (int s = 3; s < g_remote; ++s) {
        if (block) *ptr<int32_t>(block + s * kPgdSize + 8) = -1;
        if (states) *ptr<uint8_t>(states + s) = 0;
    }
}

uint32_t g_reset_tail = kResetTail;

// The network heap's block (see the top): 4 MB, 16 MB with more players.
constexpr uint32_t kNetHeap = 0x01000000;
const uint32_t kNetHeapSites[3] = {0x00F8B595, 0x0040A50F, 0x0040A530};
const uint8_t kNetHeapOld[5] = {0x68, 0x00, 0x00, 0x40, 0x00};  // push 400000h
constexpr uint32_t kNetHeapVar = 0x013787F8;      // the heap (its allocator interface), 0 until made
constexpr uint32_t kNetHeapMade = 0x0137CDD8;     // the heap object, 0 until made (0x40C030)
uint32_t g_net_heap = 0x00400000;
uint32_t g_net_free_min = 0xFFFFFFFF;
ULONGLONG g_net_free_at = 0;

// The network heap's free space (the game's own test, 0xD2EF0C), 0 when there is no heap yet.
uint32_t net_heap_free() {
    uint32_t heap = 0, vt = 0, fn = 0;
    if (!read32(kNetHeapVar, &heap) || !heap || !read32(heap, &vt) || !vt || !read32(vt + 0x18, &fn) || !fn) return 0;
    uint32_t free_bytes = 0;
    __asm {
        mov ecx, heap
        mov eax, fn
        call eax
        mov free_bytes, eax
    }
    return free_bytes;
}

// The network sessions' size: MaxPlayers and one to spare, as the game's own five for four.
constexpr uint32_t kFactorySlot = 0x010FDE60;  // SteamMatchingSystem vtable 0x10FDE3C, slot 9
const uint8_t kFactoryOld[4] = {0x40, 0x29, 0x72, 0x00};  // 0x722940
uint32_t g_factory = 0x00722940;
uint32_t g_people = 5;
int g_sessions_made = 0;

void __stdcall note_session(uint32_t asked) {
    if (g_sessions_made++ < 10) {
        LOG_INFO("[coop] The game's network library made a session object for %u people (asked for %u)",
                 asked < g_people ? g_people : asked, asked);
    }
}

// In place of the factory: `people` ([esp+8]) raised to g_people. ecx (the matching system) and the arguments are
// kept; eax is the factory's to change (it is set from fs:[0] first).
__declspec(naked) void hk_session_factory() {
    __asm {
        mov eax, dword ptr [esp + 8]
        pushad
        push eax
        call note_session
        popad
        cmp eax, dword ptr [g_people]
        jae go
        mov eax, dword ptr [g_people]
        mov dword ptr [esp + 8], eax
    go:
        jmp dword ptr [g_factory]
    }
}

// The network library's error lines: 0x7075F0 (cdecl(format, ...), level 0x53724E01) hands them to the library's
// log (0x707470), which formats a line only for an output function at 0x136304C that retail never sets, so they went
// nowhere (a player who could not get into a session of five left no word of why; 2026-11-02). They are formatted and
// logged here instead (the library's own text, in Japanese), each message 5 times at most and 300 in all. The
// formatting is guarded: the game never formatted these, so a bad argument in one would never have shown.
constexpr uint32_t kNetError = 0x007075F0;
const uint8_t kNetErrorOld[8] = {0x8B, 0x54, 0x24, 0x04, 0x8D, 0x4C, 0x24, 0x08};
std::mutex g_net_mu;
const wchar_t* g_net_formats[64] = {};
int g_net_counts[64] = {};
int g_net_lines = 0;

bool format_net_line(wchar_t* out, size_t n, const wchar_t* fmt, va_list args) {
    __try {
        _vsnwprintf(out, n - 1, fmt, args);
        out[n - 1] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __cdecl net_error(const wchar_t* fmt, ...) {
    if (!fmt) return;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        if (g_net_lines >= 300) return;
        int i = 0;
        while (i < 64 && g_net_formats[i] && g_net_formats[i] != fmt) ++i;
        if (i < 64) {
            g_net_formats[i] = fmt;
            if (++g_net_counts[i] > 5) return;
        }
        ++g_net_lines;
    }
    wchar_t line[512] = {};
    va_list args;
    va_start(args, fmt);
    const bool ok = format_net_line(line, 512, fmt, args);
    va_end(args);
    char utf8[1024] = {};
    WideCharToMultiByte(CP_UTF8, 0, ok ? line : fmt, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    LOG_INFO("[coop] The game's network library: %s%s", utf8, ok ? "" : " (could not be formatted)");
}

__declspec(naked) void hk_reset_tail() {
    __asm {
        pushad
        call reset_extra_slots
        popad
        jmp dword ptr [g_reset_tail]
    }
}

// 0xC5AF40: how many other players' records are in use by players who are not dark spirits (chr type 2). The game's
// writes out three; eax, ecx and edx are its own to change.
__declspec(naked) void count_coop_remotes() {
    __asm {
        mov ecx, dword ptr ds:[01378700h]
        mov ecx, dword ptr [ecx + 0Ch]
        xor eax, eax
        test ecx, ecx
        jz done
        mov edx, dword ptr [g_remote]
    next:
        cmp dword ptr [ecx + 8], -1
        je skip
        cmp dword ptr [ecx + 9Ch], 2
        je skip
        inc eax
    skip:
        add ecx, 4C0h
        dec edx
        jnz next
    done:
        ret
    }
}

// 0xD59B7E..0xD59BD1 (inside its function): dl = one of the other players' records is chr type 0xC. The caller tests
// dl at 0xD59BD1; eax, ecx and edx are free there.
__declspec(naked) void any_type_c() {
    __asm {
        mov eax, dword ptr ds:[01378700h]
        mov eax, dword ptr [eax + 0Ch]
        xor dl, dl
        test eax, eax
        jz done
        mov ecx, dword ptr [g_remote]
    next:
        cmp dword ptr [eax + 8], -1
        je skip
        cmp dword ptr [eax + 9Ch], 0Ch
        jne skip
        mov dl, 1
    skip:
        add eax, 4C0h
        dec ecx
        jnz next
    done:
        ret
    }
}

// 0xD6A850: the players whose summon type (PlayerGameData+0x110) is esi: ourselves (when a guest, or with a scripted
// summon in the party) and the others. The game's keeps edi; ebx is kept here too.
__declspec(naked) void count_summon_type() {
    __asm {
        push edi
        push ebx
        mov ecx, dword ptr ds:[0137D82Ch]
        mov edx, dword ptr ds:[01378700h]
        xor eax, eax
        or edi, -1
        cmp dword ptr [ecx + 90h], 6
        je self
        mov ecx, dword ptr ds:[013784A0h]
        mov ecx, dword ptr [ecx + 0BECh]
        mov ebx, dword ptr [g_party_slot]
        cmp dword ptr [ecx + ebx], edi
        je others
    self:
        mov ecx, dword ptr [edx + 8]
        test ecx, ecx
        jz others
        movzx ecx, byte ptr [ecx + 110h]
        cmp ecx, esi
        jne others
        mov eax, 1
    others:
        mov ecx, dword ptr [edx + 0Ch]
        test ecx, ecx
        jz done
        mov ebx, dword ptr [g_remote]
    next:
        cmp dword ptr [ecx + 8], edi
        je skip
        movzx edx, byte ptr [ecx + 110h]
        cmp edx, esi
        jne skip
        inc eax
    skip:
        add ecx, 4C0h
        dec ebx
        jnz next
    done:
        pop ebx
        pop edi
        ret
    }
}

// ---------------------------------------------------------------- the floating health bars
constexpr int kBars = 7;
constexpr uint32_t kGameManVar = 0x013784A0;
constexpr uint32_t kWorldChrManVar = 0x0137D644;
uint32_t g_bar_handles[kBars] = {};
uint32_t g_bar_chrs[kBars] = {};
int g_bar_count = 0;
bool g_bars = false;

// A character handle's character (0x10000000 | block << 14 | place): WorldChrMan+0x240 + block * 4, as 0xE3E340.
uint32_t chr_of_handle(uint32_t wcm, uint32_t handle) {
    uint32_t set = 0, n = 0, entries = 0, chr = 0;
    const uint32_t block = (handle >> 14) & 0x3F, at = handle & 0x3FFF;
    if (handle == 0xFFFFFFFF || !read32(wcm + 0x240 + block * 4, &set) || !set || !read32(set, &n) || at >= n ||
        !read32(set + 4, &entries) || !entries || !read32(entries + at * 0x20, &chr)) {
        return 0;
    }
    return chr;
}

void __stdcall bar_list() {
    g_bar_count = 0;
    uint32_t gm = 0, pmi = 0, wcm = 0, self = 0;
    if (!read32(kGameManVar, &gm) || !gm || !read32(kWorldChrManVar, &wcm) || !wcm) return;
    read32(wcm + 0x3C, &self);
    auto add = [&](uint32_t handle, uint32_t chr) {
        if (g_bar_count >= kBars || !chr || chr == self) return;
        for (int i = 0; i < g_bar_count; ++i) {
            if (g_bar_chrs[i] == chr) return;
        }
        g_bar_handles[g_bar_count] = handle;
        g_bar_chrs[g_bar_count] = chr;
        ++g_bar_count;
    };
    // The other players first, every one of them (WorldChrMan+0x24: {count, entries of 0x20}; 0 is ours), under the
    // handle the game made them with (0xE63F10: the players' block from [WorldChrMan+0x10]+0xC): an NPC phantom
    // never keeps a bar from a player, however early it was summoned.
    uint32_t n = 0, entries = 0, field = 0, block = 0;
    if (read32(wcm + 0x24, &n) && read32(wcm + 0x28, &entries) && entries && n <= 64 && read32(wcm + 0x10, &field) &&
        field && read32(field + 0xC, &block)) {
        for (uint32_t s = 1; s < n; ++s) {
            uint32_t chr = 0;
            if (!read32(entries + s * 0x20, &chr) || !chr) continue;
            const uint32_t handle = 0x10000000 | (block & 0x3F) << 14 | s;
            if (chr_of_handle(wcm, handle) == chr) add(handle, chr);
        }
    }
    // Then the party's members in the party's order: its NPC phantoms take the bars left (and a player the set
    // above missed gets one here).
    if (read32(gm + 0xBEC, &pmi) && pmi) {
        for (int i = 0; i < partylist::entries(); ++i) {
            uint32_t handle = 0xFFFFFFFF;
            if (read32(pmi + 0x18 + i * 0x14, &handle)) add(handle, chr_of_handle(wcm, handle));
        }
    }
}

// 0x1012C47: `mov edx, [ecx+10h]` (the party's count) ; `xorps xmm0, xmm0`. Everything else is kept.
__declspec(naked) void bars_begin() {
    __asm {
        pushfd
        pushad
        sub esp, 128
        movdqu xmmword ptr [esp + 16], xmm1
        movdqu xmmword ptr [esp + 32], xmm2
        movdqu xmmword ptr [esp + 48], xmm3
        movdqu xmmword ptr [esp + 64], xmm4
        movdqu xmmword ptr [esp + 80], xmm5
        movdqu xmmword ptr [esp + 96], xmm6
        movdqu xmmword ptr [esp + 112], xmm7
        call bar_list
        movdqu xmm1, xmmword ptr [esp + 16]
        movdqu xmm2, xmmword ptr [esp + 32]
        movdqu xmm3, xmmword ptr [esp + 48]
        movdqu xmm4, xmmword ptr [esp + 64]
        movdqu xmm5, xmmword ptr [esp + 80]
        movdqu xmm6, xmmword ptr [esp + 96]
        movdqu xmm7, xmmword ptr [esp + 112]
        add esp, 128
        popad
        popfd
        xorps xmm0, xmm0
        mov edx, dword ptr [g_bar_count]
        ret
    }
}

// 0x1012CDA..0x1012CE7: edx = the handle of bar esi (the game's: or edx, -1 ; cmp esi, [ecx+10h] ; jge ;
// mov eax, [esp+28h] ; mov edx, [eax]). The flags and eax are not read after it.
__declspec(naked) void bars_handle() {
    __asm {
        or edx, -1
        cmp esi, dword ptr [g_bar_count]
        jae done
        cmp esi, 7
        jae done
        mov edx, dword ptr [g_bar_handles + esi * 4]
    done:
        ret
    }
}

const uint8_t kBarsBegin[6] = {0x8B, 0x51, 0x10, 0x0F, 0x57, 0xC0};
const uint8_t kBarsHandle[14] = {0x83, 0xCA, 0xFF, 0x3B, 0x71, 0x10, 0x7D, 0x06, 0x8B, 0x44, 0x24, 0x28, 0x8B, 0x10};

bool install_bars() {
    if (!bytes_match(0x01012C47, kBarsBegin, sizeof(kBarsBegin)) ||
        !bytes_match(0x01012CDA, kBarsHandle, sizeof(kBarsHandle))) {
        return false;
    }
    uint8_t code[14];
    std::memset(code, 0x90, sizeof(code));
    code[0] = 0xE8;
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&bars_begin) - (0x01012C47 + 5));
    std::memcpy(code + 1, &rel, 4);
    if (!patch(0x01012C47, kBarsBegin, code, sizeof(kBarsBegin))) return false;
    std::memset(code, 0x90, sizeof(code));
    code[0] = 0xE8;
    rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&bars_handle) - (0x01012CDA + 5));
    std::memcpy(code + 1, &rel, 4);
    return patch(0x01012CDA, kBarsHandle, code, sizeof(kBarsHandle));
}

const uint8_t kC5AF40[6] = {0x8B, 0x0D, 0x00, 0x87, 0x37, 0x01};  // mov ecx, [01378700h]
const uint8_t kD59B7E[5] = {0xA1, 0x00, 0x87, 0x37, 0x01};        // mov eax, [01378700h]
const uint8_t kD59BD1[2] = {0x84, 0xD2};                          // test dl, dl
const uint8_t kD6A850[6] = {0x8B, 0x0D, 0x2C, 0xD8, 0x37, 0x01};  // mov ecx, [0137D82Ch]

// The member limit, the constructor's block and state bytes, the player character set.
const uint8_t kMemberLimit[10] = {0xC7, 0x85, 0x8C, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00};  // 0xD2CB1B
const uint8_t kBlockAlloc[5] = {0x68, 0x50, 0x0E, 0x00, 0x00};                                // 0xC06848 push 0E50h
const uint8_t kBlockCount[7] = {0xC7, 0x41, 0xFC, 0x03, 0x00, 0x00, 0x00};                    // 0xC068A9
const uint8_t kStateAlloc[2] = {0x6A, 0x03};                                                  // 0xC068C3 push 3
const uint8_t kSetAlloc[5] = {0x68, 0x80, 0x00, 0x00, 0x00};                                  // 0xE63CA6 / 0xE63CBA
const uint8_t kSetCount[6] = {0xC7, 0x07, 0x04, 0x00, 0x00, 0x00};                            // 0xE63CC1

bool jump_to(uint32_t at, const uint8_t* expect, size_t n, const void* to, bool call) {
    uint8_t code[8];
    std::memset(code, 0x90, sizeof(code));
    code[0] = call ? 0xE8 : 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (at + 5));
    std::memcpy(code + 1, &rel, 4);
    return n >= 5 && n <= sizeof(code) && patch(at, expect, code, n);
}

template <typename T>
bool patch_imm(uint32_t at, const uint8_t* expect, size_t n, size_t off, T value) {
    uint8_t code[16];
    std::memcpy(code, expect, n);
    std::memcpy(code + off, &value, sizeof(T));
    return patch(at, expect, code, n);
}

int install_all() {
    int failed = 0;
    auto one = [&](bool ok, const char* what) {
        if (!ok) {
            ++failed;
            LOG_ERROR("[coop] More players: %s does not match this build", what);
        }
    };
    // Everything is checked first, so that a build that differs anywhere is left as it is.
    bool match = bytes_match(0x00D2CB1B, kMemberLimit, sizeof(kMemberLimit)) &&
                 bytes_match(0x00C06848, kBlockAlloc, sizeof(kBlockAlloc)) &&
                 bytes_match(0x00C068A9, kBlockCount, sizeof(kBlockCount)) &&
                 bytes_match(0x00C068C3, kStateAlloc, sizeof(kStateAlloc)) &&
                 bytes_match(0x00E63CA6, kSetAlloc, sizeof(kSetAlloc)) &&
                 bytes_match(0x00E63CBA, kSetAlloc, sizeof(kSetAlloc)) &&
                 bytes_match(0x00E63CC1, kSetCount, sizeof(kSetCount)) &&
                 bytes_match(0x00C5AF40, kC5AF40, sizeof(kC5AF40)) && bytes_match(0x00D59B7E, kD59B7E, sizeof(kD59B7E)) &&
                 bytes_match(0x00D59BD1, kD59BD1, sizeof(kD59BD1)) && bytes_match(0x00D6A850, kD6A850, sizeof(kD6A850));
    for (const Bound& b : kLastSlot) match = match && bytes_match(b.at, b.code, b.len);
    for (const Bound& b : kSlotCount) match = match && bytes_match(b.at, b.code, b.len);
    for (const Bound& b : kBlockEnd) match = match && bytes_match(b.at, b.code, b.len);
    for (const SlotByte& b : kSlotBytes) match = match && bytes_match(b.at, b.code, b.len);
    for (int i = 0; i < 6; ++i) {
        const uint8_t old[6] = {0x88, 0x98, static_cast<uint8_t>(0x84 + i % 3), 0x00, 0x00, 0x00};
        match = match && bytes_match(kResetBytes[i], old, sizeof(old));
    }
    for (uint32_t at : kResetCalls) {
        uint8_t code[5] = {0xE8};
        const int32_t rel = static_cast<int32_t>(kResetTail - (at + 5));
        std::memcpy(code + 1, &rel, 4);
        match = match && bytes_match(at, code, sizeof(code));
    }
    match = match && bytes_match(kFactorySlot, kFactoryOld, sizeof(kFactoryOld));
    for (uint32_t at : kNetHeapSites) match = match && bytes_match(at, kNetHeapOld, sizeof(kNetHeapOld));
    if (!match) {
        LOG_ERROR("[coop] More players: this build of the game differs; sessions keep the game's four players");
        return -1;
    }
    // The constructor's block, its loop and the state bytes go together (the loop makes each record of the block),
    // and before anything that walks further than three: should one fail, nothing else is changed (more room than
    // used is harmless). The member limit comes last.
    one(patch_imm<int32_t>(0x00C06848, kBlockAlloc, sizeof(kBlockAlloc), 1,
                           static_cast<int32_t>(0x10 + g_remote * kPgdSize)), "the records' block");
    if (failed) return failed;
    one(patch_imm<int32_t>(0x00C068A9, kBlockCount, sizeof(kBlockCount), 3, g_remote), "the records' count");
    one(patch_imm<int8_t>(0x00C068A4, kSlotCount[2].code, 3, 2, static_cast<int8_t>(g_remote)), "the records' loop");
    one(patch_imm<int8_t>(0x00C068C3, kStateAlloc, sizeof(kStateAlloc), 1, static_cast<int8_t>(g_remote)),
        "the state bytes");
    one(patch_imm<int32_t>(0x00E63CA6, kSetAlloc, sizeof(kSetAlloc), 1, g_max * 0x20), "the character set");
    one(patch_imm<int32_t>(0x00E63CBA, kSetAlloc, sizeof(kSetAlloc), 1, g_max * 0x20), "the character set's clear");
    one(patch_imm<int32_t>(0x00E63CC1, kSetCount, sizeof(kSetCount), 2, g_max), "the character set's count");
    if (failed) return failed;
    for (const Bound& b : kLastSlot) {
        one(patch_imm<int8_t>(b.at, b.code, b.len, 2, static_cast<int8_t>(g_remote - 1)), "a last-slot check");
    }
    for (const Bound& b : kSlotCount) {
        if (b.at == 0x00C068A4) continue;  // with the constructor above
        one(patch_imm<int8_t>(b.at, b.code, b.len, 2, static_cast<int8_t>(g_remote)), "a slot count");
    }
    for (const Bound& b : kBlockEnd) {
        one(patch_imm<int32_t>(b.at, b.code, b.len, 2, static_cast<int32_t>(g_remote * kPgdSize)), "a block end");
    }
    const uint32_t bytes = reinterpret_cast<uint32_t>(g_slot_bytes);
    for (const SlotByte& b : kSlotBytes) {
        uint8_t code[8];
        std::memset(code, 0x90, sizeof(code));
        code[0] = b.op;
        code[1] = b.mod;
        std::memcpy(code + 2, &bytes, 4);
        if (b.imm) code[6] = 0;
        one(patch(b.at, b.code, code, b.len), "a slot byte");
    }
    for (int i = 0; i < 6; ++i) {
        const uint8_t old[6] = {0x88, 0x98, static_cast<uint8_t>(0x84 + i % 3), 0x00, 0x00, 0x00};
        uint8_t code[6] = {0x88, 0x1D};
        const uint32_t to = bytes + i % 3;
        std::memcpy(code + 2, &to, 4);
        one(patch(kResetBytes[i], old, code, sizeof(code)), "a reset's slot byte");
    }
    for (uint32_t at : kResetCalls) {
        one(redirect_call(at, kResetTail, reinterpret_cast<void*>(&hk_reset_tail)), "a reset");
    }
    one(jump_to(0x00C5AF40, kC5AF40, sizeof(kC5AF40), reinterpret_cast<void*>(&count_coop_remotes), false),
        "the count of co-op records");
    {
        // call any_type_c ; jmp short 0xD59BD1
        uint8_t code[7];
        code[0] = 0xE8;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&any_type_c) - (0x00D59B7E + 5));
        std::memcpy(code + 1, &rel, 4);
        code[5] = 0xEB;
        code[6] = static_cast<uint8_t>(0x00D59BD1 - (0x00D59B7E + 7));
        uint8_t old[7];
        std::memcpy(old, reinterpret_cast<const void*>(0x00D59B7E), sizeof(old));
        one(bytes_match(0x00D59B7E, kD59B7E, sizeof(kD59B7E)) && patch(0x00D59B7E, old, code, sizeof(code)),
            "the dark spirit test");
    }
    one(jump_to(0x00D6A850, kD6A850, sizeof(kD6A850), reinterpret_cast<void*>(&count_summon_type), false),
        "the summon type count");
    if (failed) return failed;
    g_people = static_cast<uint32_t>(g_max + 1);
    const uint32_t factory = reinterpret_cast<uint32_t>(&hk_session_factory);
    one(patch(kFactorySlot, kFactoryOld, &factory, sizeof(factory)), "the network sessions' size");
    if (failed) return failed;
    {
        // The three sizes go together, before the heap is made (the game makes it once, at start-up).
        uint32_t made = 0;
        if (read32(kNetHeapMade, &made) && made) {
            LOG_ERROR("[coop] More players: the network heap was made before us (it stays 4 MB)");
        } else {
            uint8_t code[5] = {0x68};
            std::memcpy(code + 1, &kNetHeap, 4);
            int done = 0;
            for (uint32_t at : kNetHeapSites) done += patch(at, kNetHeapOld, code, sizeof(code)) ? 1 : 0;
            if (done == 3) {
                g_net_heap = kNetHeap;
            } else {
                for (uint32_t at : kNetHeapSites) patch(at, code, kNetHeapOld, sizeof(kNetHeapOld));
                one(false, "the network heap's size");
            }
        }
    }
    if (failed) return failed;
    one(patch_imm<int32_t>(0x00D2CB1B, kMemberLimit, sizeof(kMemberLimit), 6, g_max), "the member limit");
    return failed;
}

}  // namespace

bool install_early(int max_players) {
    // The network library's error lines, at any player count.
    if (!jump_to(kNetError, kNetErrorOld, sizeof(kNetErrorOld), reinterpret_cast<void*>(&net_error), false)) {
        LOG_ERROR("[coop] The network library's error lines do not match this build (not logged)");
    }
    // The party list first, at any player count: with four players and an NPC phantom the fourth had no entry.
    partylist::install();
    g_party_slot = partylist::field(0x7C);
    g_max = max_players < 4 ? 4 : max_players > kMaxPlayers ? kMaxPlayers : max_players;
    g_remote = g_max - 1;
    if (g_max == 4) {
        // The bars alone: with an NPC phantom in one of the party's four entries, the game's own filler left the
        // fourth player without one.
        g_bars = install_bars();
        if (!g_bars) LOG_ERROR("[coop] The floating health bars do not match this build (the party's alone)");
        return true;
    }
    const int failed = install_all();
    if (failed != 0) {
        // The member limit goes last, so a session still holds four (the parts already widened have room to spare).
        g_ok = false;
        if (failed > 0) LOG_ERROR("[coop] More players: %d patch(es) failed", failed);
        g_max = 4;
        g_remote = 3;
        return false;
    }
    g_ok = true;
    g_bars = install_bars();
    if (!g_bars) LOG_ERROR("[coop] More players: the floating health bars do not match this build (four at most)");
    return true;
}

int max_players() { return g_max; }

bool bars_fed() { return g_bars; }

uint32_t bar_chr(int slot) { return g_bars && slot >= 0 && slot < g_bar_count ? g_bar_chrs[slot] : 0; }

void tick() {
    if (g_ok && game::session_state() != 0) {
        const uint32_t free_bytes = net_heap_free();
        if (free_bytes && free_bytes < g_net_free_min) g_net_free_min = free_bytes;
        const ULONGLONG t = GetTickCount64();
        if (g_net_free_at == 0) g_net_free_at = t;
        if (t - g_net_free_at >= 60000 && g_net_free_min != 0xFFFFFFFF) {
            LOG_INFO("[coop] Network memory: %u KB free at the least in the last minute (of %u KB; the game leaves "
                     "the session under 64 KB)", g_net_free_min / 1024, g_net_heap / 1024);
            g_net_free_min = 0xFFFFFFFF;
            g_net_free_at = t;
        }
    } else {
        g_net_free_at = 0;
    }
    if (!g_ok || g_zeroed) return;
    // The state bytes past the game's three are not cleared by its constructor (it clears three).
    uint32_t gdm = 0, states = 0;
    if (!read32(kGameDataManVar, &gdm) || !gdm || !read32(gdm + 0x10, &states) || !states) return;
    for (int s = 3; s < g_remote; ++s) *ptr<uint8_t>(states + s) = 0;
    g_zeroed = true;
}

void log_status() {
    if (g_max == 4) return;
    LOG_INFO("[coop] More players: up to %d in a session (the game's records, slots and character set widened, its "
             "network sessions made for %u, its network heap %u MB; the floating health bars %s; %s)", g_max,
             g_people, g_net_heap >> 20,
             g_bars ? "show every other player, seven at most" : "stay with the party's four",
             partylist::widened() ? "the party list holds eight" : "players past the party's four have no party entry");
}

}  // namespace coop::players
