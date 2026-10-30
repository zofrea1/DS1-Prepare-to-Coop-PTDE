// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "players.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#include <cstdint>
#include <cstring>

// More than four players in a session (MaxPlayers 5 to 8). The game holds four: itself and three others.
//  * The game session's member limit, SessMan+0x8C, is set to 4 when the session manager starts (0xD2CB1B) and is
//    what the session's room is made with (0xD2D4F0 -> attribute 0x80000001); nothing else in the session layer has
//    room for a fixed number (members, packets and queues are lists).
//  * GameDataMan keeps the other players' PlayerGameData in one block of three (+0xC, 0x4C0 each, made by the
//    constructor 0xC06710 as 0x10 + 0xE40 bytes with the count at block+0xC), a state byte for each (+0x10, three
//    bytes; 0x1F once the player is ready to be made) and another byte each inline at +0x84..+0x86. A member's slot
//    is the first free state byte (0xC02CA0 from the member-joined handler 0xD2DF80); every user checks the slot
//    against 2 or 3 or walks the block to 0xE40. Two resets (session start 0xD29940, join 0xD2D200) and three
//    readers (0xC5AF40, 0xD59B7E, 0xD6A850) are written out for three slots.
//  * WorldChrMan's player characters live in a set of four (+0x24, made by 0xE63C90: 4 x 0x20); the others are
//    put in a free entry (0xE63F10) when they are made (0xEE2B10 -> 0xE40970).
// All of it is widened here before the game makes these objects (from DllMain), each patch checked against the
// bytes it replaces; nothing changes at four. Not widened: the party list (PartyMemberInfo, four entries inline;
// its add 0xEA2510 looks for a free entry and refuses when there is none, so a fifth player simply has no entry:
// no floating health bar, no party state of the game's own) and the floating health bars (MenuMan+0x8F4, four).

namespace coop::players {

namespace {

int g_max = 4;      // players in a session
int g_remote = 3;   // the others (g_max - 1)
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
        cmp dword ptr [ecx + 7Ch], edi
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
    one(patch_imm<int32_t>(0x00D2CB1B, kMemberLimit, sizeof(kMemberLimit), 6, g_max), "the member limit");
    return failed;
}

}  // namespace

bool install_early(int max_players) {
    g_max = max_players < 4 ? 4 : max_players > kMaxPlayers ? kMaxPlayers : max_players;
    g_remote = g_max - 1;
    if (g_max == 4) return true;
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
    return true;
}

int max_players() { return g_max; }

void tick() {
    if (!g_ok || g_zeroed) return;
    // The state bytes past the game's three are not cleared by its constructor (it clears three).
    uint32_t gdm = 0, states = 0;
    if (!read32(kGameDataManVar, &gdm) || !gdm || !read32(gdm + 0x10, &states) || !states) return;
    for (int s = 3; s < g_remote; ++s) *ptr<uint8_t>(states + s) = 0;
    g_zeroed = true;
}

void log_status() {
    if (g_max == 4) return;
    LOG_INFO("[coop] More players: up to %d in a session (the game's records, slots and character set widened; "
             "players past the fourth have no party entry or floating health bar)", g_max);
}

}  // namespace coop::players
