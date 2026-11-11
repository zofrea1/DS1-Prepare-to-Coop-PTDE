// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "vanillamp.h"

#include "hook.h"
#include "log.h"

#include <windows.h>

#include <cstring>

namespace coop::vanillamp {

namespace {

// Every vanilla summon and invasion is a sign: a white or red soapstone's, a covenant's invisible one (the Forest
// Hunters' automatic summons), or an invader's (the Red Eye, Cracked Red Eye, Blue Eye Orbs and the Dragon Eye), which
// the world it goes to summons as soon as it arrives. Two steps make it a session:
//  * The summoner's game touches the sign (0xDB3590). For a player's sign (+0x88 == -1) it first opens its session
//    (CreateSession, 0xD2D4F0, at 0xDB3613) and gives up when that fails; an NPC's sign (and an NPC invader's black
//    sign) has its entity at +0x88 and makes no session. Here the call fails for every player's sign.
//  * The sign owner's game gets the session's invitation in the sign manager's update (0xDB4250, every frame), reads
//    it (0xDB4815) and joins (JoinSession at 0xDB4849, with the join type and the arrival point around it). Here the
//    invitation is passed over as if it could not be read (0xDB481C: the jump the game takes for an unreadable one),
//    and the game drops it as it does that one.
// The mod's own sessions open and join through game.cpp (create_session, join), never through these two sites. The
// game's own lobbies are shared only among Prepare to Co-op players (steam.cpp), so both ends of a vanilla summon run
// these patches.
constexpr uint32_t kTouchCreate = 0x00DB3613;  // call 0xD2D4F0 (thiscall, two arguments, ret 8)
constexpr uint32_t kCreateSession = 0x00D2D4F0;
constexpr uint32_t kInviteRead = 0x00DB481C;   // je 0xDB4918 (the invitation could not be read)
constexpr uint32_t kInviteDrop = 0x00DB4918;
const uint8_t kInviteReadOld[6] = {0x0F, 0x84, 0xF6, 0x00, 0x00, 0x00};

LONG g_summons = 0, g_invitations = 0;

void __cdecl note_summon() {
    const LONG n = InterlockedIncrement(&g_summons);
    if (n <= 5 || n % 100 == 0) {
        LOG_INFO("[coop] A player's summon sign was touched: the game's own summoning is off (%ld so far)", n);
    }
}

void __cdecl note_invitation() {
    const LONG n = InterlockedIncrement(&g_invitations);
    if (n <= 5 || n % 100 == 0) {
        LOG_INFO("[coop] The game's own invitation to a session came for our sign: not joined, its summoning is off "
                 "(%ld so far)", n);
    }
}

// In place of CreateSession at the touch: ecx = SessMan, two arguments; al = 0 (failed).
__declspec(naked) void hk_touch_create() {
    __asm {
        pushad
        call note_summon
        popad
        xor eax, eax
        ret 8
    }
}

// In place of the jump at 0xDB481C (reached with the invitation read): the unreadable invitation's way out.
__declspec(naked) void hk_invite_read() {
    __asm {
        pushad
        call note_invitation
        popad
        push kInviteDrop
        ret
    }
}

}  // namespace

bool install() {
    bool ok = redirect_call(kTouchCreate, kCreateSession, reinterpret_cast<void*>(&hk_touch_create));
    uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_invite_read) - (kInviteRead + 5));
    std::memcpy(jump + 1, &rel, 4);
    ok = patch(kInviteRead, kInviteReadOld, jump, sizeof(jump)) && ok;
    if (ok) {
        LOG_INFO("[coop] The game's own summon signs and invasions are off (the co-op items take their place)");
    } else {
        LOG_ERROR("[coop] The game's own summoning steps do not match this build; vanilla summons are not blocked");
    }
    return ok;
}

const wchar_t* item_note(int32_t goods) {
    switch (goods) {
        case 100:  // White Sign Soapstone
        case 101:  // Red Sign Soapstone
            return L"Vanilla summons are off: use the Blessed or Crystal Eye Orb";
        case 102:  // Red Eye Orb
        case 111:  // Cracked Red Eye Orb
        case 113:  // Blue Eye Orb
        case 114:  // Dragon Eye
            return L"Vanilla invasions are off: use the Chaos Eye Orb";
        default:
            return nullptr;
    }
}

}  // namespace coop::vanillamp
