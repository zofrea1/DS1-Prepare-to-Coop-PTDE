// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "netlog.h"

#include "bells.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "session.h"
#include "steam.h"
#include "vagrants.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <string>
#include <vector>

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
    // Each item holds a player's Steam id (16 hex digits) and every linked game re-sends its list every 20 s (the
    // 2026-11-25 logs): summed up, not a line each.
    {"SpiritualStatue", 0x011D94D0, 0x00BD7020, false, "curse statue lists"},
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

// The games re-send what they hold every few seconds (each node's curse statues came every 20 s, 2026-11-25): an item
// has its line once (its kind and first data words).
std::mutex g_seen_lock;
std::set<std::tuple<int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>> g_seen;

bool first_time(int n, uint8_t kind, const uint32_t* w) {
    std::lock_guard<std::mutex> lock(g_seen_lock);
    if (g_seen.size() > 4096) return false;
    return g_seen.insert(std::make_tuple(n, static_cast<uint32_t>(kind), w[0], w[1], w[2], w[3])).second;
}

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
    uint32_t data = 0, w[8] = {};
    if (packet) {
        read8(packet + 4, &kind);
        read32(packet + 8, &data);
        if (data) {
            for (int i = 0; i < 8; ++i) read32(data + 4 * i, &w[i]);
        }
    }
    if (n == 0 && kind == 0) bells::received(static_cast<int>(w[0]));  // the Blighttown bell (bells.cpp)
    if (!g_on) return;
    InterlockedIncrement(&g_since[n]);
    const LONG total = InterlockedIncrement(&g_total[n]);
    const DbType& t = kTypes[n];
    const bool fresh = first_time(n, kind, w);
    if (!t.each && total > 3) return;
    if (t.each && !fresh) return;
    if (!line_ok()) return;
    char where[24];
    map_text(where, sizeof(where));
    if (n == 0 && kind == 0) {
        LOG_INFO("[net] Shared event %d arrived from a linked player: %s; we are %s", static_cast<int>(w[0]),
                 event_name(static_cast<int>(w[0])), where);
        return;
    }
    if (n == 1 && kind == 2) {
        // A vagrant (FrpgNetItemVagrantDb, kind 2: 0xBD3720 -> 0xBD36A0 -> 0xBD2CC0 adds it to our list; vagrants.cpp):
        // where it was lost (x, y, z, facing), the map's index in bits 20-27 of the next word, the item twice (category
        // in the top 4 bits; goods 350 is the game's humanity vagrant), then its count (bits 0-9) and the number of
        // games it has passed through (bits 20-29).
        float f[4];
        std::memcpy(f, w, sizeof(f));
        const bool humanity = w[6] == 0x4000015E;
        char item[48];
        if (humanity) {
            std::snprintf(item, sizeof(item), "lost humanity");
        } else {
            static const char* const kCat[] = {"weapon", "armor", "ring", "?", "goods"};
            const unsigned cat = w[6] >> 28;
            std::snprintf(item, sizeof(item), "%u x %s %u", w[7] & 0x3FF, cat < 5 ? kCat[cat] : "?", w[6] & 0x0FFFFFFF);
        }
        LOG_INFO("[net] Arrived: a vagrant (%s, lost in map index %u at (%.1f, %.1f, %.1f), through %u game(s) so far) "
                 "passed on by a linked game; we are %s (%d held now)", item, (w[4] >> 20) & 0xFF, f[0], f[1], f[2],
                 (w[7] >> 20) & 0x3FF, where, vagrants::held() + 1);
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

// The game's lobbies hold Steam ids as two "I<int64>" halves of the id's hex text: its owner in 2000000E + 2000000F,
// three of its links in 20000015 + 20000017, 20000018 + 20000019 and 2000001A + 2000001B. Those three are only the
// ones it advertises (its node has up to 20 links, read below); up to 2026-11-25 they were logged as "the game's
// links", which looked like a limit of three.
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

// ---------------------------------------------------------------- the game's links (its node's table)
// Read where DSCM reads them (metal-crow's DaS-PC-MPChan, the same addresses in this exe): the count with our own node
// at 0x1362DD0, the first node at [0x1362DCC] (each: +0 the next, +0xC its player: +0x30 the name, +0xC -> +0x30 the
// Steam id as UTF-16 hex), the limit at [[[0x137F838]+0x38]+0x70] (20). On 2026-11-25 a test game had 6 to 8 other
// games there while the lobby advertised 3.
constexpr uint32_t kNodeFirst = 0x01362DCC;
constexpr uint32_t kNodeCount = 0x01362DD0;
constexpr uint32_t kNodeLimit = 0x0137F838;

struct Node {
    uint64_t id = 0;
    std::string name;
};

bool read_wide(uint32_t addr, wchar_t* out, int n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), n * sizeof(wchar_t));
        out[n - 1] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

int node_limit() {
    uint32_t a = 0, b = 0, v = 0;
    return read32(kNodeLimit, &a) && a && read32(a + 0x38, &b) && b && read32(b + 0x70, &v) && v <= 64
               ? static_cast<int>(v)
               : -1;
}

std::vector<Node> read_nodes() {
    std::vector<Node> out;
    uint32_t count = 0, list = 0, node = 0;
    if (!read32(kNodeCount, &count) || count > 64 || !read32(kNodeFirst, &list) || !list || !read32(list, &node)) {
        return out;
    }
    const uint64_t self = steam::self();
    for (uint32_t i = 0; i < count && node; ++i) {
        uint32_t player = 0, online = 0;
        wchar_t id[24] = {}, name[33] = {};
        if (read32(node + 0xC, &player) && player) {
            read_wide(player + 0x30, name, 33);
            if (read32(player + 0xC, &online) && online) read_wide(online + 0x30, id, 24);
        }
        Node n;
        n.id = std::wcstoull(id, nullptr, 16);
        char utf8[100] = "";
        WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr);
        n.name = utf8;
        if (n.id && n.id != self) out.push_back(n);
        if (!read32(node, &node)) break;
    }
    return out;
}

// Which of them are Prepare to Co-op players: the owners of the game lobbies with our tag (a search of our own every
// minute; each lobby's owner is in its keys 2000000E + 2000000F). A linked game that is not among them found us from
// a search without the tag: an unmodded game on Spacewar (the community's steam_appid.txt way, DSCM's players), or a
// Prepare to Co-op of another tag.
std::mutex g_ptc_lock;
std::map<uint64_t, ULONGLONG> g_ptc;  // Steam id -> when last found
ULONGLONG g_ptc_searched = 0;
ULONGLONG g_next_ptc_search = 0;

uint64_t lobby_owner_key(uint64_t lobby) {
    const std::string id = id_half(steam::lobby_data(lobby, "2000000e")) + id_half(steam::lobby_data(lobby, "2000000f"));
    return id.size() == 16 ? std::strtoull(id.c_str(), nullptr, 16) : 0;
}

void search_ptc() {
    steam::find_lobbies({{"ptdesc_game", session::game_tag()}}, 50, [](std::vector<uint64_t> lobbies) {
        const ULONGLONG t = GetTickCount64();
        std::lock_guard<std::mutex> lock(g_ptc_lock);
        for (uint64_t lobby : lobbies) {
            const uint64_t owner = lobby_owner_key(lobby);
            if (owner) g_ptc[owner] = t;
        }
        g_ptc_searched = t;
    });
}

const char* kind_of(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_ptc_lock);
    const auto it = g_ptc.find(id);
    if (it != g_ptc.end() && GetTickCount64() - it->second < 15 * 60000) return "Prepare to Co-op";
    return g_ptc_searched ? "not a Prepare to Co-op game: unmodded on Spacewar?" : "not searched yet";
}

std::string node_text(const Node& n) {
    char b[200];
    std::snprintf(b, sizeof(b), "%s %016llx (%s)", n.name.empty() ? "?" : n.name.c_str(),
                  static_cast<unsigned long long>(n.id), kind_of(n.id));
    return b;
}

std::string links_text() {
    const std::vector<Node> nodes = read_nodes();
    std::string out = std::to_string(nodes.size()) + " other game(s) (limit " + std::to_string(node_limit()) + ")";
    for (size_t i = 0; i < nodes.size(); ++i) out += (i ? ", " : ": ") + node_text(nodes[i]);
    return out;
}

// Every 2 s: who joined and who left the game's links.
std::map<uint64_t, std::string> g_linked;
int g_link_lines = 0;

void watch_links() {
    static ULONGLONG next = 0;
    const ULONGLONG t = GetTickCount64();
    if (t < next) return;
    next = t + 2000;
    // A link's name comes a moment after the link: one counts once it has its name.
    std::vector<Node> nodes = read_nodes();
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [](const Node& n) { return n.name.empty(); }), nodes.end());
    std::map<uint64_t, std::string> now;
    for (const auto& n : nodes) now[n.id] = n.name;
    if (now == g_linked) return;
    std::string change;
    for (const auto& n : nodes) {
        if (!g_linked.count(n.id)) change += (change.empty() ? "" : ", ") + std::string("+") + node_text(n);
    }
    for (const auto& [id, name] : g_linked) {
        if (!now.count(id)) {
            char b[120];
            std::snprintf(b, sizeof(b), "-%s %016llx", name.empty() ? "?" : name.c_str(),
                          static_cast<unsigned long long>(id));
            change += (change.empty() ? "" : ", ") + std::string(b);
        }
    }
    g_linked = now;
    if (g_link_lines >= 300) return;
    ++g_link_lines;
    LOG_INFO("[net] The game's links: %zu other game(s) (limit %d): %s", nodes.size(), node_limit(), change.c_str());
}

void watch_links_safe() {
    __try {
        watch_links();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
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
    watch_links_safe();
    if (game::player() && steam::ready() && now >= g_next_ptc_search) {
        g_next_ptc_search = now + 60000;
        search_ptc();
    }
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
    const std::string links = links_text();
    const int held = vagrants::held();
    if (s.empty()) {
        LOG_INFO("[net] Last 5 minutes: nothing arrived from other players' games; linked now with %s; vagrants held: %d",
                 links.c_str(), held);
    } else {
        LOG_INFO("[net] Last 5 minutes from other players' games: %s; linked now with %s; vagrants held: %d", s.c_str(),
                 links.c_str(), held);
    }
}

void lobby_data(uint64_t, const char*, const char*) {
    // The three neighbours the game advertises are no longer logged (watch_links reads all of its links).
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
