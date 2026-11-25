// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "netlog.h"

#include "bells.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "steam.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace coop::netlog {

namespace {

// Each network database (NS_FRPG::FrpgNet*Db) takes a packet from another player's game in its method table's slot 1
// (thiscall(db, from, packet), ret 8; the base class's FrpgNetDb::0xBEB270 only gives the packet back). A packet
// (FrpgNetDbPacket, method table 0x11D6FE0): +4 its kind (byte), +8 its data. The event meddle database's kind 0 is a
// multiplayer event: its data's first int is the event's id, which the database keeps for the maps' "IF Multiplayer
// Event" (0xFA10A0 -> 0xFA0E60).
struct DbType {
    const char* name;   // the class, without FrpgNet / Db
    uint32_t vtable;    // its method table
    uint32_t receive;   // slot 1 as the game has it
    bool each;          // a line for each packet (else counted for the summary)
    const char* what;
};
constexpr DbType kTypes[] = {
    {"EventMeddle", 0x011D9658, 0x00FA10A0, true, "shared event"},
    {"ItemVagrant", 0x011D9A90, 0x00BD3720, true, "vagrant (lost items or humanity passed on)"},
    {"ResonanceMagic", 0x011D9C54, 0x00BD1EA0, true, "miracle resonance ring"},
    {"SpiritualStatue", 0x011D94D0, 0x00BD7020, true, "basilisk curse statue"},
    {"NitoInvitation", 0x011DC538, 0x00FA0210, true, "Gravelord Servant's invitation (black phantoms)"},
    {"Bonfire", 0x011D9178, 0x00BD8F90, true, "bonfire kindling"},
    {"ColiseumEntrant", 0x011DC710, 0x00BC7C90, true, "Oolacile arena entrant"},
    {"Ranking", 0x011D9ED8, 0x00BCF040, true, "covenant ranking"},
    {"BloodMessage", 0x011D8DE8, 0x00BDDF30, false, "messages"},
    {"BloodStain", 0x011D8F98, 0x00BDB540, false, "bloodstains"},
    {"WanderingGhost", 0x011D8910, 0x00BE1080, false, "ghosts"},
    {"Sos", 0x011D6FE8, 0x00BEEA60, false, "signs"},
    {"Node", 0x011D8714, 0x00BE25B0, false, "node lists"},
    {"Connect", 0x011D78EC, 0x00BE71E0, false, "connections"},
};
constexpr int kTypeCount = sizeof(kTypes) / sizeof(kTypes[0]);

// The multiplayer events the maps use (every 2003[16] Trigger Multiplayer Event of PTDE's map scripts), and ours.
const char* event_name(int id) {
    switch (id) {
        case 10010:
            return "the Undead Parish's Bell of Awakening (heard in the Undead Burg and Parish: m10_01 event 11015170)";
        case 10020:
            return "the Bell of Awakening in Quelaag's Domain (Prepare to Co-op's: heard in Blighttown, bells.cpp)";
        default:
            return "an event no map listens for";
    }
}

bool g_on = false;
volatile LONG g_map = -1;  // the local player's map (tick), for the lines
uint32_t g_receive[kTypeCount] = {};
volatile LONG g_since[kTypeCount] = {};  // since the last summary
volatile LONG g_total[kTypeCount] = {};
volatile LONG g_lines = 0;
constexpr LONG kMaxLines = 600;  // a session's lines of each-packet kinds, all together
ULONGLONG g_next_summary = 0;

bool line_ok() { return InterlockedIncrement(&g_lines) <= kMaxLines; }

void map_text(char* out, size_t n) {
    const LONG m = g_map;
    if (m < 0) {
        std::snprintf(out, n, "no map");
    } else {
        std::snprintf(out, n, "in m%02u_%02u", static_cast<unsigned>((m >> 24) & 0xFF),
                      static_cast<unsigned>((m >> 16) & 0xFF));
    }
}

void note(int n, uint32_t from, uint32_t packet) {
    uint8_t kind = 0xFF;
    uint32_t data = 0, w[4] = {};
    if (packet) {
        read8(packet + 4, &kind);
        read32(packet + 8, &data);
        if (data) {
            for (int i = 0; i < 4; ++i) read32(data + 4 * i, &w[i]);
        }
    }
    if (n == 0 && kind == 0) bells::received(static_cast<int>(w[0]));  // the Blighttown bell (bells.cpp)
    if (!g_on) return;
    InterlockedIncrement(&g_since[n]);
    const LONG total = InterlockedIncrement(&g_total[n]);
    const DbType& t = kTypes[n];
    if (!t.each && total > 3) return;
    if (!line_ok()) return;
    char where[24];
    map_text(where, sizeof(where));
    if (n == 0 && kind == 0) {
        LOG_INFO("[net] Shared event %d arrived from a linked player: %s; we are %s", static_cast<int>(w[0]),
                 event_name(static_cast<int>(w[0])), where);
        return;
    }
    LOG_INFO("[net] %s %s: a %s packet (kind %u, from %08X, data %08X %08X %08X %08X); we are %s",
             t.each ? "Arrived:" : "First packets of", t.what, t.name, kind, from, w[0], w[1], w[2], w[3], where);
}

using ReceiveFn = void*(__fastcall*)(void* db, void* edx, void* from, void* packet);

template <int N>
void* __fastcall hk_receive(void* db, void* edx, void* from, void* packet) {
    __try {
        note(N, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(from)),
             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(packet)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return reinterpret_cast<ReceiveFn>(g_receive[N])(db, edx, from, packet);
}

template <int N>
void* receive_hook() {
    return reinterpret_cast<void*>(&hk_receive<N>);
}
void* const kHooks[kTypeCount] = {receive_hook<0>(),  receive_hook<1>(),  receive_hook<2>(),  receive_hook<3>(),
                                   receive_hook<4>(),  receive_hook<5>(),  receive_hook<6>(),  receive_hook<7>(),
                                   receive_hook<8>(),  receive_hook<9>(),  receive_hook<10>(), receive_hook<11>(),
                                   receive_hook<12>(), receive_hook<13>()};

// EMEVD 2003[16] Trigger Multiplayer Event (0xBC1F6E): `mov eax, [id]; call 0xFA0D50`, which hands a packet with the
// id to every linked player's game (0xFA0C30, [[0x137E204]+0xB50]).
constexpr uint32_t kTriggerCall = 0x00BC1F77;
constexpr uint32_t kTrigger = 0x00FA0D50;
uint32_t g_trigger = kTrigger;

void __cdecl note_trigger(int id) {
    if (!g_on || !line_ok()) return;
    char where[24];
    map_text(where, sizeof(where));
    LOG_INFO("[net] This game sent shared event %d to its linked players: %s; we are %s", id, event_name(id), where);
}

__declspec(naked) void hk_trigger() {
    __asm {
        pushad
        push eax
        call note_trigger
        add esp, 4
        popad
        jmp dword ptr [g_trigger]
    }
}

// The game's lobby (its node): the players it picked to link with, three Steam ids in the keys 20000015 + 20000017,
// 20000018 + 20000019 and 2000001A + 2000001B, each written as two "I<int64>" halves of the id's hex text.
std::mutex g_lobby_lock;
std::map<std::string, std::string> g_neighbour_keys;
std::string g_neighbours_logged;
int g_neighbour_lines = 0;

std::string id_half(const std::string& v) {
    if (v.size() < 2 || v[0] != 'I') return std::string();
    const long long x = _strtoi64(v.c_str() + 1, nullptr, 10);
    std::string s;
    for (int i = 0; i < 8; ++i) {
        const char c = static_cast<char>((x >> (8 * i)) & 0xFF);
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

std::string neighbours_text() {
    static const char* const kPairs[3][2] = {{"20000015", "20000017"}, {"20000018", "20000019"}, {"2000001a", "2000001b"}};
    std::string out;
    int n = 0;
    for (const auto& p : kPairs) {
        const auto a = g_neighbour_keys.find(p[0]), b = g_neighbour_keys.find(p[1]);
        const std::string id = (a == g_neighbour_keys.end() ? std::string() : id_half(a->second)) +
                               (b == g_neighbour_keys.end() ? std::string() : id_half(b->second));
        if (id.empty()) continue;
        const unsigned long long sid = std::strtoull(id.c_str(), nullptr, 16);
        const std::string name = sid ? steam::persona(sid) : std::string();
        out += (n++ ? ", " : "") + (name.empty() ? id : name + " " + id);
    }
    return n ? std::to_string(n) + ": " + out : std::string("none");
}

}  // namespace

void install(bool on) {
    g_on = on;
    // The hooks go in either way: the event meddle database's brings the Blighttown bell (bells.cpp).
    int hooked = 0;
    for (int i = 0; i < kTypeCount; ++i) {
        uint32_t old = 0;
        if (swap_pointer(kTypes[i].vtable + 4, kTypes[i].receive, kHooks[i], &old)) {
            g_receive[i] = old;
            ++hooked;
        } else {
            LOG_ERROR("[net] NetworkEventLog: the %s database's packet handler does not match; not logged",
                      kTypes[i].name);
        }
    }
    const bool trigger = redirect_call(kTriggerCall, kTrigger, reinterpret_cast<void*>(&hk_trigger));
    if (!on) {
        LOG_INFO("[net] NetworkEventLog: off (%d of %d network databases hooked for the Blighttown bell)", hooked,
                 kTypeCount);
        return;
    }
    LOG_INFO("[net] NetworkEventLog: the online extras other Prepare to Co-op players' games send are logged as they "
             "arrive (%d of %d kinds; bells, vagrants, miracle resonance, curse statues, Gravelord, bonfire kindling, "
             "arena, rankings each, the rest summed up every 5 minutes)%s",
             hooked, kTypeCount, trigger ? "" : "; the bell's sending is not logged (its site does not match)");
}

void tick() {
    if (!g_on) return;
    InterlockedExchange(&g_map, game::player() ? game::map_id() : -1);
    const ULONGLONG now = GetTickCount64();
    if (!g_next_summary) g_next_summary = now + 300000;
    if (now < g_next_summary) return;
    g_next_summary = now + 300000;
    std::string s;
    for (int i = 0; i < kTypeCount; ++i) {
        const LONG n = InterlockedExchange(&g_since[i], 0);
        if (!n) continue;
        char b[96];
        std::snprintf(b, sizeof(b), "%s%s %ld", s.empty() ? "" : ", ", kTypes[i].what, n);
        s += b;
    }
    std::string links;
    {
        std::lock_guard<std::mutex> lock(g_lobby_lock);
        links = neighbours_text();
    }
    if (s.empty()) {
        LOG_INFO("[net] Last 5 minutes: nothing arrived from other players' games; linked with %s", links.c_str());
    } else {
        LOG_INFO("[net] Last 5 minutes from other players' games: %s; linked with %s", s.c_str(), links.c_str());
    }
}

void lobby_data(uint64_t, const char* key, const char* value) {
    if (!g_on || !key || std::strncmp(key, "200000", 6) != 0) return;
    const std::string k(key);
    if (k < "20000015" || k > "2000001b") return;
    std::lock_guard<std::mutex> lock(g_lobby_lock);
    std::string& slot = g_neighbour_keys[k];
    const std::string v = value ? value : "";
    if (slot == v) return;
    slot = v;
    const std::string now = neighbours_text();
    if (now == g_neighbours_logged || g_neighbour_lines >= 200) return;
    g_neighbours_logged = now;
    ++g_neighbour_lines;
    LOG_INFO("[net] The game's links to other players (its node's neighbours): %s", now.c_str());
}

void search_distance(int value) {
    static int last = -100;
    if (!g_on || value == last) return;
    last = value;
    static const char* const kNames[] = {"close", "default (same region)", "far", "worldwide"};
    LOG_INFO("[net] The game searches for other players at Steam's distance \"%s\" (%d)",
             value >= 0 && value <= 3 ? kNames[value] : "?", value);
}

}  // namespace coop::netlog
