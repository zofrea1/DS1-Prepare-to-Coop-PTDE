// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "bells.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#include <windows.h>

#include <cstdlib>

namespace coop::bells {

namespace {

constexpr int kBlighttownEvent = 10020;  // ours: no map script of the game triggers or waits for it
constexpr int kQuelaagLever = 11400201;  // the ObjAct flag of the bell's lever (m14_00 event 11400200 waits for it)
constexpr int kQuelaagBell = 11400200;   // the event's end (after its cutscene), in case the lever's flag never came
constexpr int kBellEntity = 1401110;     // o4560 in m14_00: the bell with its lever
// The bell's own toll in Blighttown's sound bank (frpg_m14.fev: o456000001, a ring of about two seconds; o456000002 is
// a short knock). The Parish's toll (o130300002, frpg_m10.fev) is not loaded in Blighttown: played there, it is silent.
constexpr int kBellSound = 456000001;
constexpr int kBlighttown = 0x0E000000;  // m14_00 (area << 24 | block << 16)

// Event command 2010[02] Play SE (0xBBFD6F): stdcall 0xD6EA70([0x137D790], entity, sound type, sound id), ret 0x10;
// type 3 is an object's sound.
constexpr uint32_t kPlaySe = 0x00D6EA70;
constexpr uint32_t kSoundMan = 0x0137D790;
// Event command 2003[16] Trigger Multiplayer Event (0xBC1F6E): 0xFA0D50 with the id in eax.
constexpr uint32_t kTriggerEvent = 0x00FA0D50;

volatile LONG g_send = 0;          // an event id to send from the tick
ULONGLONG g_last_sent = 0;         // one toll a ring: the lever's flag and the event's end both come
ULONGLONG g_tolls[3] = {};         // when to play the next tolls (0 = none)
volatile LONG g_test_entity = 0, g_test_sound = 0;  // diagnostics: a sound to play from the tick

bool call_play(int entity, int sound) {
    uint32_t man = 0;
    if (!read32(kSoundMan, &man) || !man) return false;
    __try {
        __asm {
            push sound
            push 3
            push entity
            push man
            mov eax, kPlaySe
            call eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void call_trigger(int id) {
    __try {
        __asm {
            mov eax, id
            mov ecx, kTriggerEvent
            call ecx
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}  // namespace

void flag_set_here(int id, bool on) {
    if (!on || (id != kQuelaagLever && id != kQuelaagBell)) return;
    const ULONGLONG now = GetTickCount64();
    if (g_last_sent && now - g_last_sent < 120000) return;
    g_last_sent = now;
    InterlockedExchange(&g_send, kBlighttownEvent);
    LOG_INFO("[coop] The bell in Quelaag's Domain was rung here (flag %d): its toll goes to the linked players", id);
}

void received(int id) {
    if (id != kBlighttownEvent) return;
    const int map = game::player() ? game::map_id() : -1;
    if ((map & 0xFFFF0000) != kBlighttown) {
        LOG_INFO("[coop] The bell in Quelaag's Domain was rung in another player's world; not heard here (we are not "
                 "in Blighttown)");
        return;
    }
    // As the Parish's listener: a toll now, then two more 0.5 to 2 s apart.
    const ULONGLONG now = GetTickCount64();
    g_tolls[0] = now;
    g_tolls[1] = now + 500 + static_cast<ULONGLONG>(std::rand() % 1501);
    g_tolls[2] = g_tolls[1] + 500 + static_cast<ULONGLONG>(std::rand() % 1501);
    LOG_INFO("[coop] The bell in Quelaag's Domain was rung in another player's world: it tolls here (we are in "
             "Blighttown)");
}

void tick() {
    const LONG id = InterlockedExchange(&g_send, 0);
    if (id) call_trigger(id);
    const LONG test = InterlockedExchange(&g_test_sound, 0);
    if (test) {
        const bool ok = call_play(static_cast<int>(g_test_entity), static_cast<int>(test));
        LOG_INFO("[coop] bellsound: sound %ld on entity %ld %s", test, g_test_entity, ok ? "played" : "could not be played");
    }
    const ULONGLONG now = GetTickCount64();
    for (ULONGLONG& t : g_tolls) {
        if (!t || now < t) continue;
        t = 0;
        if (game::loading() || !game::player()) continue;
        if (!call_play(kBellEntity, kBellSound)) LOG_INFO("[coop] The bell's toll could not be played");
    }
}

bool play_sound(int entity, int sound) {
    InterlockedExchange(&g_test_entity, entity);
    InterlockedExchange(&g_test_sound, sound);
    return true;
}

void send_event(int id) { InterlockedExchange(&g_send, id); }

}  // namespace coop::bells
