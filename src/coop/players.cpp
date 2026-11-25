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
#include <vector>

// More than four players in a session (MaxPlayers 5 to 64). The game holds four: itself and three others.
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
//    With more players the heap is made bigger (net_heap_size: 16 MB up to eight players, as in 1.4.0, and 1 MB more
//    for each player above eight: 72 MB at 64); the lowest free space of each minute in a session is logged.
//  * GameDataMan keeps the other players' PlayerGameData in one block of three (+0xC, 0x4C0 each, made by the
//    constructor 0xC06710 as 0x10 + 0xE40 bytes with the count at block+0xC), a state byte for each (+0x10, three
//    bytes; 0x1F once the player is ready to be made) and another byte each inline at +0x84..+0x86. A member's slot
//    is the first free state byte (0xC02CA0 from the member-joined handler 0xD2DF80); every user checks the slot
//    against 2 or 3 or walks the block to 0xE40. Two resets (session start 0xD29940, join 0xD2D200) and three
//    readers (0xC5AF40, 0xD59B7E, 0xD6A850) are written out for three slots.
//  * WorldChrMan's player characters live in a set of four (+0x24, made by 0xE63C90: 4 x 0x20); the others are
//    put in a free entry (0xE63F10) when they are made (0xEE2B10 -> 0xE40970).
// All of it is widened here before the game makes these objects (from DllMain), each patch checked against the
// bytes it replaces; nothing changes at four. Every bound above is an 8-bit immediate or wider and every size a
// 32-bit one, so the same patches hold up to kMaxPlayers (64 since 2.7.0; eight in 1.4.0 .. 2.6.0). The party list
// (PartyMemberInfo [GameMan+0xBEC]) is widened apart, at any player count, to partylist::kWide entries
// (partylist.cpp); only when that does not fit this build does it keep the game's four, and then a fifth player
// simply has no entry and none of the game's own party bookkeeping (its add 0xEA2510 refuses when no entry is free).
// The floating health bars do not need it: the HUD has seven of them (records MenuMan+0x8F4 + 0x20 * slot, slot < 7
// in the widget 0xFA89C0 and in the filler 0x1012C30), which the filler fed from party entry `slot` while `slot` <
// the party's count (+0x10), entry 0, our own character, drawing nothing. The filler takes its characters from
// bar_list instead (at four players too: an NPC phantom in one of the party's entries left the fourth player without
// a bar): every other player's character first (WorldChrMan's player set), then the party's entries but ours (its NPC
// phantoms). An NPC never keeps a bar from a player; it gets one back as soon as a player leaves. With more players
// the bars' records move into an array of ours and more bars are made in the HUD (below: kMaxBars).

namespace coop::players {

namespace {

int g_max = 4;      // players in a session
int g_remote = 3;   // the others (g_max - 1)
// The party list's scripted-summon slot (the game's +0x7C; -0x10 once partylist.cpp moved the list's fields).
int32_t g_party_slot = 0x7C;
bool g_ok = false;  // every patch in place
bool g_zeroed = false;
// The inline bytes GameDataMan+0x84 + slot, moved here for every slot.
uint8_t g_slot_bytes[kMaxPlayers] = {};

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

// The network heap's block (see the top): 4 MB, more with more players (net_heap_size).
constexpr uint32_t kNetHeapBase = 0x01000000;  // 16 MB: eight players (1.4.0 .. 2.6.0)
constexpr uint32_t kNetHeapPerPlayer = 0x00100000;
uint32_t net_heap_size(int max_players) {
    return kNetHeapBase + static_cast<uint32_t>(max_players > 8 ? max_players - 8 : 0) * kNetHeapPerPlayer;
}
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
// The HUD's floating bars over other players (and NPC phantoms) are dialogs of class FrpgMenuDlgFloatingPCGauge
// (vtable 0x11CAFAC, 0x11C bytes), each drawing the record of its slot (property 1004, set through vt+0x58 = 0xFA9330
// into +0xF4): MenuMan+0x8F4 + 0x20 * slot, seven records made by MenuMan's constructor (0xC2DA6F), filled every frame
// by 0x1012C30 and read by the gauge's update 0xFA89C0 and its placing 0xFA86F0 (both: slot < 7, else no record at
// all, a null read). The seven gauges are one definition of the HUD's menu file, "F21-00", seven times in a plain
// dialog (EzMenuDialog, vtable 0x110614C) inside the HUD (FrpgMenuDlgPCGauge, "F20-00"). A dialog holds its pieces as
// EzMenuDlgObj (vtable 0x1106264, 0x20 bytes: +0x10 the dialog, +0x14 the piece's shape, +0x18 flags, +0x1C the
// piece's record in the menu file) in a TVector at +0x8 (+0xC the array, +0x10 the count, +0x14 the room, +0x18 its
// memory {buffer, room, owned}); a gauge's shape is a CMenuNestedDialog (vtable 0x1107174, +0x10 the gauge, which has
// its piece at +0x34). The dialog's update (0x784380) and draw walk that vector (each piece's vt+0x40, its shape's,
// the gauge's), and its destructor frees every piece, its shape and its gauge.
//
// More bars (2.7.0): the records move into an array of ours (kMaxBars of them). The reads of MenuMan that feed a
// record's address (`mov reg, [MenuMan]` before `lea reg, [slot * 0x20 + reg + 0x8F4]`: the update, the placing, the
// filler's four, a getter and a setter) take our array's address less 0x8F4 instead, the slot bounds 6 become
// kMaxBars - 1, the filler's end 0xE0 kMaxBars * 0x20, and the filler's line-of-sight test (one bar a frame: the slot
// equal to [0x1378648], which went round modulo 7) goes round the bars in use. Then tick makes more gauges in the
// HUD's dialog, each the way the menu loader makes a nested dialog of a class (0x79BE30): the gauge from its class's
// constructor, its init from the same definition ("F21-00", which makes its pieces), a nested-dialog shape around it
// from the seventh's record of the menu file, and a piece holding the shape, put at the end of the dialog's vector
// (its memory grown by the game's own 0x783D30), slot property 1004 set to its bar. A piece's own attach (0x788D10)
// would make the gauge as a plain EzMenuDialog: the loader takes the class from the menu system's +0xB4, set only
// while the HUD loads (first try, 2026-11-24: 531 plain dialogs made). The loader needs the menu file's resource
// (+0x14 its heap, +0x54 its definitions), which the dialog keeps nowhere: the game's attach of every piece is watched
// (the pieces' vtable slot) for the resource of each dialog. The HUD's teardown frees our pieces with its own (each
// piece its shape, each shape its gauge), and a new HUD (every load) is grown again. A bar not in use is not drawn
// (bit 1 of its piece's flags): each drawn bar records its transforms into the menus' command buffer every frame.
constexpr int kGameBars = 7;
constexpr uint32_t kGameManVar = 0x013784A0;
constexpr uint32_t kWorldChrManVar = 0x0137D644;
uint32_t g_bar_handles[kMaxBars] = {};
uint32_t g_bar_chrs[kMaxBars] = {};
int g_bar_count = 0;
int32_t g_bar_limit = kGameBars;  // the records there are (bars_handle's bound)
int32_t g_bar_mod = 1;            // the bars the line-of-sight test goes round (at least 1)
bool g_bars = false;
bool g_records_moved = false;
struct alignas(16) Record {
    uint8_t b[0x20];
};
Record g_records[kMaxBars];
int g_gauges = kGameBars;
int g_want_extra = 0;

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
        if (g_bar_count >= g_bar_limit || !chr || chr == self) return;
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
    if (read32(wcm + 0x24, &n) && read32(wcm + 0x28, &entries) && entries && n <= kMaxPlayers &&
        read32(wcm + 0x10, &field) && field && read32(field + 0xC, &block)) {
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
    g_bar_mod = g_bar_count > 0 ? g_bar_count : 1;
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
        cmp esi, dword ptr [g_bar_limit]
        jae done
        mov edx, dword ptr [g_bar_handles + esi * 4]
    done:
        ret
    }
}

// The filler draws a bar only for a character of the player's kind (vt+0x9C: PlayerIns, which human NPCs are too;
// 0x1012D03: mov eax, [edx+9Ch] ; mov ecx, esi ; call eax, then test al, al). The stress test's stand-ins are
// enemies (stress.cpp), so while it runs (g_all_bars) any character of a bar is taken. Off, the game's own answer.
int32_t g_all_bars = 0;
constexpr uint32_t kBarKindSite = 0x01012D03;
const uint8_t kBarKindOld[10] = {0x8B, 0x82, 0x9C, 0x00, 0x00, 0x00, 0x8B, 0xCE, 0xFF, 0xD0};

__declspec(naked) void bars_kind() {
    __asm {
        mov eax, dword ptr [edx + 0x9C]
        mov ecx, esi
        call eax
        test al, al
        jnz done
        cmp dword ptr [g_all_bars], 0
        je done
        mov al, 1
    done:
        ret
    }
}

// 0x1013474 (`mov ecx, 7` before the idiv that turns the line-of-sight test's bar): ecx = the bars in use.
__declspec(naked) void bars_mod() {
    __asm {
        mov ecx, dword ptr [g_bar_mod]
        ret
    }
}

const uint8_t kBarsBegin[6] = {0x8B, 0x51, 0x10, 0x0F, 0x57, 0xC0};
const uint8_t kBarsHandle[14] = {0x83, 0xCA, 0xFF, 0x3B, 0x71, 0x10, 0x7D, 0x06, 0x8B, 0x44, 0x24, 0x28, 0x8B, 0x10};

// The records' sites (see above). A MenuMan load becomes `mov reg, imm32` (+ nop), a bound its new value.
struct RecordSite {
    uint32_t at;
    uint8_t n;
    uint8_t old[6];
    uint8_t op;  // 0: a bound (an 8-bit 6 -> kMaxBars - 1, the 32-bit 0xE0 -> kMaxBars * 0x20); else mov reg, imm32
};
const RecordSite kRecordSites[] = {
    {0x00FA89D3, 3, {0x83, 0xF8, 0x06}, 0},                          // update: cmp eax, 6
    {0x00FA89D8, 6, {0x8B, 0x0D, 0xD0, 0x86, 0x37, 0x01}, 0xB9},     // mov ecx, [MenuMan]
    {0x00FA86FD, 3, {0x83, 0xF8, 0x06}, 0},                          // placing: cmp eax, 6
    {0x00FA8702, 6, {0x8B, 0x0D, 0xD0, 0x86, 0x37, 0x01}, 0xB9},     // mov ecx, [MenuMan]
    {0x01012C5B, 6, {0x8B, 0x3D, 0xD0, 0x86, 0x37, 0x01}, 0xBF},     // filler: mov edi, [MenuMan]
    {0x010132E2, 6, {0x8B, 0x3D, 0xD0, 0x86, 0x37, 0x01}, 0xBF},
    {0x0101333C, 6, {0x8B, 0x3D, 0xD0, 0x86, 0x37, 0x01}, 0xBF},
    {0x01013373, 6, {0x8B, 0x3D, 0xD0, 0x86, 0x37, 0x01}, 0xBF},
    {0x01012C71, 3, {0x83, 0xFE, 0x06}, 0},                          // cmp esi, 6
    {0x0101345A, 5, {0x3D, 0xE0, 0x00, 0x00, 0x00}, 0},              // cmp eax, 0E0h
    {0x00C29D62, 3, {0x83, 0xF9, 0x06}, 0},                          // getter: cmp ecx, 6
    {0x00C29D67, 5, {0xA1, 0xD0, 0x86, 0x37, 0x01}, 0xB8},           // mov eax, [MenuMan]
    {0x00C29D84, 6, {0x8B, 0x15, 0xD0, 0x86, 0x37, 0x01}, 0xBA},     // setter: mov edx, [MenuMan]
};
constexpr uint32_t kBarsModSite = 0x01013474;
const uint8_t kBarsModOld[5] = {0xB9, 0x07, 0x00, 0x00, 0x00};

// The menus' drawing goes into one of three command buffers a frame (FrpgMenuDc, made once by 0xC246A0 from the heap
// [0x13787C8]: 0x40000 bytes each, the size kept at +0x10), played back by the render thread. Its writers do not check
// the room: a full buffer hands out a null place and the next write faults (0x437F73, the first try with 66 gauges,
// 2026-11-24: each nested dialog records its transform every frame). With more gauges the buffers are 1 MB.
constexpr uint32_t kMenuBufSize = 0x00100000;
const uint32_t kMenuBufSites[4] = {0x00C2471C + 3, 0x00C2472B + 1, 0x00C24740 + 1, 0x00C24755 + 1};
uint32_t g_menu_buf = 0x40000;

bool grow_menu_buffers() {
    const uint8_t mov[7] = {0xC7, 0x46, 0x10, 0x00, 0x00, 0x04, 0x00};
    const uint8_t push[5] = {0x68, 0x00, 0x00, 0x04, 0x00};
    if (!bytes_match(0x00C2471C, mov, sizeof(mov)) || !bytes_match(0x00C2472B, push, sizeof(push)) ||
        !bytes_match(0x00C24740, push, sizeof(push)) || !bytes_match(0x00C24755, push, sizeof(push))) {
        return false;
    }
    const uint32_t old = 0x40000;
    for (uint32_t at : kMenuBufSites) patch(at, &old, &kMenuBufSize, 4);
    g_menu_buf = kMenuBufSize;
    return true;
}

// All or nothing, before the HUD exists (from DllMain): every record read and write goes to g_records.
bool move_records() {
    if (!grow_menu_buffers()) {
        LOG_ERROR("[coop] Floating health bars: the menus' draw buffers do not match this build (seven bars)");
        return false;
    }
    for (const RecordSite& r : kRecordSites) {
        if (!bytes_match(r.at, r.old, r.n)) {
            LOG_ERROR("[coop] Floating health bars: %08X does not match this build (seven bars)", r.at);
            return false;
        }
    }
    if (!bytes_match(kBarsModSite, kBarsModOld, sizeof(kBarsModOld))) return false;
    for (Record& r : g_records) {
        std::memset(r.b, 0, sizeof(r.b));
        r.b[0xD] = 1;  // as the game's record constructor (0x5159B0)
    }
    const uint32_t base = reinterpret_cast<uint32_t>(&g_records[0]) - 0x8F4;
    for (const RecordSite& r : kRecordSites) {
        uint8_t code[6];
        std::memcpy(code, r.old, r.n);
        if (r.op) {
            code[0] = r.op;
            std::memcpy(code + 1, &base, 4);
            if (r.n == 6) code[5] = 0x90;
        } else if (r.n == 3) {
            code[2] = static_cast<uint8_t>(kMaxBars - 1);
        } else {
            const uint32_t end = kMaxBars * 0x20;
            std::memcpy(code + 1, &end, 4);
        }
        if (!patch(r.at, r.old, code, r.n)) return false;  // (none can fail after the checks above)
    }
    uint8_t code[5] = {0xE8};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&bars_mod) - (kBarsModSite + 5));
    std::memcpy(code + 1, &rel, 4);
    patch(kBarsModSite, kBarsModOld, code, sizeof(code));
    if (bytes_match(kBarKindSite, kBarKindOld, sizeof(kBarKindOld))) {
        uint8_t kind[10];
        std::memset(kind, 0x90, sizeof(kind));
        kind[0] = 0xE8;
        const int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&bars_kind) - (kBarKindSite + 5));
        std::memcpy(kind + 1, &to, 4);
        patch(kBarKindSite, kBarKindOld, kind, sizeof(kind));
    }
    g_records_moved = true;
    g_bar_limit = kMaxBars;
    return true;
}

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

// ---------------------------------------------------------------- more gauges in the HUD
constexpr uint32_t kGaugeVtable = 0x011CAFAC;      // FrpgMenuDlgFloatingPCGauge
constexpr uint32_t kGaugeUpdateSlot = 0x011CAFEC;  // its vt+0x40 (0xFA8C90): the update, every frame
constexpr uint32_t kGaugeUpdate = 0x00FA8C90;
constexpr uint32_t kDlgObjVtable = 0x01106264;     // EzMenu::EzMenuDlgObj
constexpr uint32_t kDialogVtable = 0x0110614C;     // EzMenu::EzMenuDialog
constexpr uint32_t kNestedVtable = 0x01107174;     // EzMenu::DynamicUI::CMenuNestedDialog
constexpr uint32_t kDlgObjNew = 0x00789460;        // stdcall(context 0 = the menu heap, flag) -> EzMenuDlgObj
constexpr uint32_t kTymeAlloc = 0x00783D30;        // thiscall(memory {buffer, room, owned}, n, name, 0): frees, makes
constexpr uint32_t kHeapOf = 0x00562C30;           // cdecl(block) -> its heap (free: vt+0x34)
uint32_t g_gauge_update = kGaugeUpdate;
volatile uint32_t g_seen_gauge = 0;  // a gauge the game updated (the hook below)
double g_gauges_checked = 0;
int g_grow_lines = 0;
constexpr uint32_t kAttachSlot = 0x011062A8;  // EzMenuDlgObj vt+0x44 (0x788D10)
constexpr uint32_t kAttach = 0x00788D10;
struct DialogResource {
    uint32_t dialog = 0, resource = 0;
};
constexpr uint32_t kDialogResources = 4096;
DialogResource g_dialog_resources[kDialogResources];  // by the dialog's address (a later dialog in its place wins)
uint32_t g_failed_dialog = 0;   // a dialog we could not grow (not tried again until the next load)
// The pieces we made (bar kGameBars + i), drawn only while their bar is in use: each one drawn records its transforms
// into the menus' command buffer every frame, shown or not.
uint32_t g_made_dialog = 0;
uint32_t g_made_pieces[kMaxBars] = {};
int g_made_count = 0;

__declspec(naked) void hk_gauge_update() {
    __asm {
        mov dword ptr [g_seen_gauge], ecx
        jmp dword ptr [g_gauge_update]
    }
}

uint32_t vtable_of(uint32_t p) {
    uint32_t vt = 0;
    return p >= 0x10000 && read32(p, &vt) ? vt : 0;
}

// The gauge of a piece of the dialog, or 0.
uint32_t gauge_of_piece(uint32_t obj) {
    uint32_t shape = 0, gauge = 0;
    if (vtable_of(obj) != kDlgObjVtable || !read32(obj + 0x14, &shape) || vtable_of(shape) != kNestedVtable ||
        !read32(shape + 0x10, &gauge) || vtable_of(gauge) != kGaugeVtable) {
        return 0;
    }
    return gauge;
}

using DlgObjNewFn = uint32_t(__stdcall*)(uint32_t context, uint32_t flag);
using AttachFn = char(__thiscall*)(uint32_t obj, uint32_t record, uint32_t resource, uint32_t dialog);

// In place of every piece's attach (vt+0x44): the game's, then the resource of a gauge's.
char __fastcall hk_attach(uint32_t obj, void*, uint32_t record, uint32_t resource, uint32_t dialog) {
    const char made = reinterpret_cast<AttachFn>(kAttach)(obj, record, resource, dialog);
    // The resource each dialog's pieces were attached with (the HUD's bars come with the rest of its dialog).
    if (made && resource && dialog) {
        DialogResource& d = g_dialog_resources[(dialog >> 4) % kDialogResources];
        d.dialog = dialog;
        d.resource = resource;
    }
    return made;
}
using SetPropFn = void(__thiscall*)(uint32_t dlg, int32_t id, int32_t value, int32_t type);
using TymeAllocFn = char(__thiscall*)(uint32_t memory, int32_t n, const wchar_t* name, int32_t);
using HeapOfFn = uint32_t(__cdecl*)(uint32_t block);
using DtorFn = void(__thiscall*)(uint32_t obj, int32_t flags);
using FreeFn = void(__thiscall*)(uint32_t heap, uint32_t block);

using GaugeNewFn = uint32_t(__thiscall*)(uint32_t context, uint32_t flag);  // 0x50D350: a floating bar, 0x11C
using SetDefIndexFn = void(__thiscall*)(uint32_t dlg, uint32_t index);        // 0x516320: +0x28
using InitFromDefFn = char(__thiscall*)(uint32_t dlg, uint32_t def, uint32_t resource, uint32_t sys);  // 0x788970
using AllocFn = uint32_t(__cdecl*)(uint32_t size, uint32_t align, uint32_t heap);  // 0x5629C0
using ShapeCtorFn = uint32_t(__thiscall*)(uint32_t shape, uint32_t data, uint32_t resource, uint32_t sys,
                                          uint32_t child);  // 0x79F890: CMenuNestedDialog
using SetPieceFn = void(__thiscall*)(uint32_t dlg, uint32_t piece);  // 0x783300: +0x34
constexpr uint32_t kGaugeNew = 0x0050D350, kSetDefIndex = 0x00516320, kInitFromDef = 0x00788970;
constexpr uint32_t kAlloc = 0x005629C0, kShapeCtor = 0x0079F890, kSetPiece = 0x00783300;

// An object of the game's heaps, destroyed and freed as the game's loader does on a failure (0x79BF5B).
void destroy(uint32_t obj) {
    const uint32_t heap = reinterpret_cast<HeapOfFn>(kHeapOf)(obj);
    reinterpret_cast<DtorFn>(*ptr<uint32_t>(vtable_of(obj) + 4))(obj, 0);
    if (heap) reinterpret_cast<FreeFn>(*ptr<uint32_t>(vtable_of(heap) + 0x34))(heap, obj);
}

// One more bar in `dialog`, as the menu loader makes a nested dialog of a class (0x79BE30): the gauge from its
// class's constructor (0x50D350), its definition's index (+0x28) and its init from the definition (0x788970, which
// makes its pieces), the nested dialog's shape around it (0x79F890, from the record's shape data) and the piece that
// holds the shape (0x789460, its fields as its attach 0x788D10 sets them), the gauge told its piece (+0x34).
uint32_t make_bar(uint32_t dialog, uint32_t source, uint32_t source_gauge, uint32_t resource, uint32_t sys, int slot) {
    // The record's shape: +4 {its type's name, its data} (0x79D070 picks the loader by the name, which gets the data).
    const uint32_t record = *ptr<uint32_t>(source + 0x1C);
    const uint32_t shape_def = record ? *ptr<uint32_t>(record + 4) : 0;
    const uint32_t data = shape_def ? *ptr<uint32_t>(shape_def + 4) : 0;
    const uint32_t def = *ptr<uint32_t>(source_gauge + 0x2C);
    const uint32_t heap = *ptr<uint32_t>(resource + 0x14);
    if (!record || !data || !def || !heap) return 0;
    const uint32_t gauge = reinterpret_cast<GaugeNewFn>(kGaugeNew)(0, 0);
    if (!gauge || vtable_of(gauge) != kGaugeVtable) return 0;
    reinterpret_cast<SetDefIndexFn>(kSetDefIndex)(gauge, *ptr<uint32_t>(source_gauge + 0x28));
    if (!reinterpret_cast<InitFromDefFn>(kInitFromDef)(gauge, def, resource, sys)) {
        destroy(gauge);
        return 0;
    }
    const uint32_t shape = reinterpret_cast<AllocFn>(kAlloc)(0x1C, 4, heap);
    if (!shape) {
        destroy(gauge);
        return 0;
    }
    reinterpret_cast<ShapeCtorFn>(kShapeCtor)(shape, data, resource, sys, gauge);
    const uint32_t obj = reinterpret_cast<DlgObjNewFn>(kDlgObjNew)(0, 0);
    if (!obj) {
        destroy(shape);  // its destructor frees the gauge
        return 0;
    }
    *ptr<uint32_t>(obj + 0x1C) = record;
    *ptr<uint32_t>(obj + 0x10) = dialog;
    *ptr<uint32_t>(obj + 0x14) = shape;
    *ptr<uint32_t>(obj + 0x18) = *ptr<uint32_t>(source + 0x18);
    reinterpret_cast<SetPieceFn>(kSetPiece)(gauge, obj);
    reinterpret_cast<SetPropFn>(*ptr<uint32_t>(kGaugeVtable + 0x58))(gauge, 0x3EC, slot, 2);
    return obj;
}

bool make_gauges(uint32_t dialog, int want, int* made_out) {
    *made_out = 0;
    uint32_t arr = 0, count = 0, room = 0, buffer = 0, sys = 0;
    if (!read32(dialog + 0xC, &arr) || !read32(dialog + 0x10, &count) || !read32(dialog + 0x14, &room) ||
        !read32(dialog + 0x18, &buffer) || !read32(dialog + 0xD4, &sys) || !sys || !arr || arr != buffer ||
        count > 4096 || room < count) {
        return false;
    }
    std::vector<uint32_t> pieces(count);
    uint32_t source = 0, source_gauge = 0;
    int gauges = 0, top = -1;
    for (uint32_t i = 0; i < count; ++i) {
        read32(arr + i * 4, &pieces[i]);
        const uint32_t g = gauge_of_piece(pieces[i]);
        if (!g) continue;
        ++gauges;
        uint32_t slot = 0;
        if (read32(g + 0xF4, &slot) && static_cast<int>(slot) > top) {
            top = static_cast<int>(slot);
            source = pieces[i];
            source_gauge = g;
        }
    }
    if (!source || gauges >= want) {
        g_gauges = gauges;
        return source != 0;
    }
    const DialogResource& known = g_dialog_resources[(dialog >> 4) % kDialogResources];
    const uint32_t resource = known.dialog == dialog ? known.resource : 0;
    if (!resource) {
        LOG_INFO("[coop] Floating health bars: the HUD's dialog %08X has no menu resource known", dialog);
        return false;
    }
    // The vector's memory, grown by the game's own allocation (which frees the old one).
    const int32_t grown = static_cast<int32_t>(count) + (want - gauges);
    if (static_cast<int32_t>(room) < grown) {
        static const wchar_t kName[] = L"TTymeMemory[%d]";
        if (!reinterpret_cast<TymeAllocFn>(kTymeAlloc)(dialog + 0x18, grown, kName, grown)) {
            LOG_ERROR("[coop] Floating health bars: the HUD's dialog could not grow (%d bars)", gauges);
            *ptr<uint32_t>(dialog + 0xC) = 0;
            *ptr<uint32_t>(dialog + 0x10) = 0;
            *ptr<uint32_t>(dialog + 0x14) = 0;
            return false;
        }
        const uint32_t fresh = *ptr<uint32_t>(dialog + 0x18);
        for (uint32_t i = 0; i < count; ++i) *ptr<uint32_t>(fresh + i * 4) = pieces[i];
        *ptr<uint32_t>(dialog + 0xC) = fresh;
        *ptr<uint32_t>(dialog + 0x14) = static_cast<uint32_t>(grown);
        arr = fresh;
    }
    if (dialog != g_made_dialog || gauges <= kGameBars) {
        g_made_dialog = dialog;  // a new HUD (perhaps where the last one was)
        g_made_count = 0;
    }
    for (int slot = gauges; slot < want; ++slot) {
        const uint32_t obj = make_bar(dialog, source, source_gauge, resource, sys, slot);
        if (!obj) {
            LOG_ERROR("[coop] Floating health bars: the menu did not make bar %d", slot);
            g_gauges = gauges + *made_out;
            return false;
        }
        *ptr<uint32_t>(arr + count * 4) = obj;
        ++count;
        *ptr<uint32_t>(dialog + 0x10) = count;
        ++*made_out;
        if (slot >= kGameBars && slot - kGameBars < kMaxBars) {
            g_made_pieces[slot - kGameBars] = obj;
            if (slot - kGameBars + 1 > g_made_count) g_made_count = slot - kGameBars + 1;
        }
    }
    g_gauges = gauges + *made_out;
    return true;
}

// The gauges the HUD should have: one for every other member of a full session and its NPC phantoms (three, the
// game's limit), and the test's extra ones.
int gauges_wanted() {
    const int want = g_max - 1 + 3 + g_want_extra;
    return want < kGameBars ? kGameBars : want > kMaxBars ? kMaxBars : want;
}

// Every frame: our pieces draw only while their bar is in use (bit 1 of the piece's flags, tested by its draw 0x788D80).
void gauges_draw_tick() {
    if (!g_made_count || vtable_of(g_made_dialog) != kDialogVtable) return;
    uint32_t arr = 0, count = 0;
    if (!read32(g_made_dialog + 0xC, &arr) || !read32(g_made_dialog + 0x10, &count) || !arr) return;
    for (int i = 0; i < g_made_count; ++i) {
        const uint32_t obj = g_made_pieces[i];
        const uint32_t at = static_cast<uint32_t>(count) - static_cast<uint32_t>(g_made_count) + i;
        uint32_t listed = 0;
        if (!obj || at >= count || !read32(arr + at * 4, &listed) || listed != obj) continue;
        uint8_t* flags = ptr<uint8_t>(obj + 0x18);
        const bool use = kGameBars + i < g_bar_count;
        *flags = use ? (*flags | 2) : (*flags & ~2);
    }
}

void gauges_tick() {
    if (!g_records_moved) return;
    gauges_draw_tick();
    if (game::loading()) {
        g_failed_dialog = 0;
        return;
    }
    const uint32_t g = g_seen_gauge;
    if (!g) return;
    const double t = GetTickCount64() / 1000.0;
    if (t - g_gauges_checked < 0.5) return;
    g_gauges_checked = t;
    uint32_t obj = 0, dialog = 0;
    if (vtable_of(g) != kGaugeVtable || !read32(g + 0x34, &obj) || vtable_of(obj) != kDlgObjVtable ||
        !read32(obj + 0x10, &dialog) || vtable_of(dialog) != kDialogVtable) {
        return;
    }
    if (dialog == g_failed_dialog) return;
    const int want = gauges_wanted();
    int made = 0;
    bool ok = false;
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    __try {
        ok = make_gauges(dialog, want, &made);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    if (made > 0 || !ok) {
        if (++g_grow_lines <= 40) {
            LOG_INFO("[coop] Floating health bars: %d made in the HUD in %.1f ms (%d now, %d wanted)%s", made,
                     static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart),
                     g_gauges, want, ok ? "" : "; stopped: this HUD keeps the bars it has");
        }
    }
    // A HUD that failed is left as it is (a fault partway could leave half a piece behind each try).
    if (!ok) g_failed_dialog = dialog;
}

bool install_gauges() {
    const uint8_t old[4] = {0x90, 0x8C, 0xFA, 0x00};  // 0xFA8C90
    const uint8_t attach_old[4] = {0x10, 0x8D, 0x78, 0x00};  // 0x788D10
    if (!bytes_match(kGaugeUpdateSlot, old, 4) || !bytes_match(kAttachSlot, attach_old, 4)) return false;
    const uint32_t attach = reinterpret_cast<uint32_t>(&hk_attach);
    const uint32_t hook = reinterpret_cast<uint32_t>(&hk_gauge_update);
    return patch(kAttachSlot, attach_old, &attach, 4) && patch(kGaugeUpdateSlot, old, &hook, 4);
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
            const uint32_t heap = net_heap_size(g_max);
            uint8_t code[5] = {0x68};
            std::memcpy(code + 1, &heap, 4);
            int done = 0;
            for (uint32_t at : kNetHeapSites) done += patch(at, kNetHeapOld, code, sizeof(code)) ? 1 : 0;
            if (done == 3) {
                g_net_heap = heap;
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
    if (!g_bars) {
        LOG_ERROR("[coop] More players: the floating health bars do not match this build (four at most)");
    } else if (move_records() && !install_gauges()) {
        LOG_ERROR("[coop] More players: the floating health bar's update does not match this build (seven bars)");
        g_bar_limit = kGameBars;
    }
    return true;
}

int max_players() { return g_max; }

bool bars_fed() { return g_bars; }

uint32_t bar_chr(int slot) { return g_bars && slot >= 0 && slot < g_bar_count ? g_bar_chrs[slot] : 0; }

uint32_t bar_record(int slot) {
    if (slot < 0 || slot >= (g_records_moved ? kMaxBars : kGameBars)) return 0;
    if (g_records_moved) return reinterpret_cast<uint32_t>(&g_records[slot]);
    uint32_t mm = 0;
    return read32(0x013786D0, &mm) && mm ? mm + 0x8F4 + slot * 0x20 : 0;  // MenuMan's own
}

int bar_slot(uint32_t rec) {
    const uint32_t first = bar_record(0);
    const int n = g_records_moved ? kMaxBars : kGameBars;
    if (!first || rec < first || rec >= first + n * 0x20 || (rec - first) % 0x20 != 0) return -1;
    return static_cast<int>((rec - first) / 0x20);
}

int gauges() { return g_records_moved ? g_gauges : kGameBars; }

void show_all_bars(bool on) { g_all_bars = on ? 1 : 0; }

void want_bars(int extra) {
    g_want_extra = extra < 0 ? 0 : extra;
    g_all_bars = extra > 0 ? 1 : 0;
}

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
    gauges_tick();
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
             !g_bars            ? "stay with the party's four"
             : g_records_moved  ? "show every other player and NPC phantom, more made in the HUD as needed"
                                : "show every other player, seven at most",
             partylist::widened() ? "the party list holds 72" : "players past the party's four have no party entry");
}

}  // namespace coop::players
