// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "session.h"

#include "game.h"
#include "hook.h"
#include "items.h"
#include "log.h"
#include "motion.h"
#include "party.h"
#include "rules.h"
#include "steam.h"
#include "text.h"
#include "world.h"
#include "version.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>

namespace coop::session {

namespace {

// ---------------------------------------------------------------- wire format
constexpr uint32_t kMagic = 0x31435350;  // "PSC1"
// Players of the same protocol and the same Prepare to Co-op version play together (see same_version). 1.0.0 spoke 8
// and compares only the protocol, so 1.1.0 speaks 9: a 1.0.0 host then refuses a 1.1.0 player too.
constexpr uint32_t kProtocol = 9;
// What this game speaks and reports: kProtocol and PREPARE_TO_COOP_VERSION, unless the hidden INI keys TestProtocol /
// TestVersion pretend otherwise (to see the version notices between two copies of the same build).
uint32_t g_protocol = kProtocol;
std::string g_version = PREPARE_TO_COOP_VERSION;
// Lobby data. "ptdesc_ver" with 8 is what Prepare to Co-op 1.0.0 searches for, so every host keeps publishing it:
// a 1.0.0 player then still finds a newer host, is refused, and the host is told who tried. The protocol a host
// really speaks is "ptdesc_proto" and its Prepare to Co-op version "ptdesc_mod" (both from 1.1.0 on; a lobby without
// them is 1.0.0's, protocol 8). Searches do not filter by version, so a session of another version is reported
// to the player instead of being hidden.
constexpr const char* kLegacyProtocol = "8";

enum MsgType : uint8_t {
    kHello = 1,        // guest/invader -> host: please let me in
    kWelcome = 2,      // host -> joiner: the session invitation, where to appear, the world rules
    kState = 3,        // host -> all, every second (players, world rules)
    kInfo = 4,         // everyone -> everyone, every second (player panel, light sharing)
    kBonfireWarp = 5,  // host -> guests: I warped to this bonfire, follow
    kRest = 6,         // anyone -> all: rested at a bonfire, the world resets
    kBye = 7,          // leaving the session
    kPing = 8,
    kPong = 9,
    kDead = 10,        // anyone -> all: died
    kFlags = 11,       // event flags changed in the session's world (guest -> host -> guests)
    kHomeward = 12,    // host -> guests: I used a Homeward Bone, come to my last bonfire
    kBonfireLit = 13,  // a bonfire was lit for the first time (guest -> host -> guests)
    kArrived = 14,     // guest/invader -> host -> all: arrived in the host's world
    kLeft = 15,        // host -> guests: another player left
    kGait = 16,        // everyone -> everyone: our walk/run/sprint (on change and twice a second)
    kAnim = 17,        // everyone -> everyone: a bonfire event animation of ours started or stopped
    kWorldFlags = 18,  // host -> a guest who arrived: the map flags of the host's world (rules::world_flag_snapshot)
    kCutFollow = 19,   // anyone -> all (through the host): a cutscene took me into this map, follow me there
    kDropped = 20,     // host -> a player it dropped for silence who is talking again: go home
};

enum WelcomeStatus : uint32_t { kOk = 0, kFull = 1, kBusy = 2, kRefused = 3, kVersion = 4 };

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint8_t type;
};
struct Hello {
    Header h;
    uint32_t protocol;
    uint32_t invade;
    int32_t soul_level;
    char name[64];
    char version[16];  // the sender's Prepare to Co-op version (from 1.1.0 on; 1.0.0's hello ends before it)
};
struct Welcome {
    Header h;
    uint32_t status;
    int32_t map;
    float pos[3];
    float angle;
    int32_t bonfire;
    uint8_t world_rules;
    uint8_t infection;
    uint8_t taunt;
    uint8_t coop_players;
    uint32_t invitation_size;
    uint8_t invitation[512];
};
struct StateMsg {
    Header h;
    int32_t map;
    int32_t bonfire;
    uint8_t invader_present;
    uint8_t players;       // session members (the game's count)
    uint8_t coop_players;  // host + co-op guests (enemy scaling)
    uint8_t allow_invaders;
    uint8_t world_rules;
    uint8_t infection;
    uint8_t taunt;
};
struct Info {
    Header h;
    int32_t soul_level;
    int32_t deaths;
    int32_t map;
    float pos[3];
    int32_t ping_ms;
    uint8_t invader;
    uint8_t dead;
    int32_t light;  // the player's own light (world::local_lights: 0x08 Cast Light / Sunlight Maggot, 0x10 lantern)
    char name[64];
    wchar_t char_name[17];  // character name: matches the player to their character in our world
    int32_t ceremony;       // the bonfire animation the player is looping (the sit), 0 none
};
struct BonfireMsg {
    Header h;
    int32_t bonfire;
};
struct Simple {
    Header h;
    uint32_t value;
};
struct PlayerEvent {
    Header h;
    uint64_t who;
    uint8_t invader;
    char name[64];
    uint8_t lost;  // kLeft: the host dropped them after their connection went quiet
};
struct GaitMsg {
    Header h;
    int8_t gait;
};
struct AnimMsg {
    Header h;
    uint8_t kind;  // 0 start, 1 stop
    int32_t anim;
    uint8_t loop;
};
struct CutFollowMsg {
    Header h;
    int32_t map;      // where the cutscene took the sender
    int32_t point;    // the cutscene's point entity there
    int32_t bonfire;  // the sender's respawn point on arrival (the event usually sets one there)
    int32_t from;     // the map the sender was in when the cutscene began: only players there follow
};
constexpr int kFlagsPerMsg = 96;
struct FlagsMsg {
    Header h;
    uint16_t count;
    struct {
        int32_t id;
        uint8_t on;
    } f[kFlagsPerMsg];
};
#pragma pack(pop)
// How much of a hello 1.0.0 sends: a host reads at least this much, and the version only when it is there.
constexpr size_t kHelloV1Size = offsetof(Hello, version);

// ---------------------------------------------------------------- messages (Remastered Seamless Co-op's)
std::wstring fmt(const wchar_t* pattern, const std::wstring& arg) {
    std::wstring s(pattern);
    const size_t at = s.find(L"{}");
    if (at != std::wstring::npos) s.replace(at, 2, arg);
    return s;
}
constexpr const wchar_t* kMsgJoined = L"{} has joined!";
constexpr const wchar_t* kMsgInvaded = L"Evil spirit {} has invaded!";
constexpr const wchar_t* kMsgLeft = L"{} has left";
constexpr const wchar_t* kMsgInvaderLeft = L"Evil spirit {} has left";
constexpr const wchar_t* kMsgLost = L"{} lost the connection";
constexpr const wchar_t* kMsgInvaderLost = L"Evil spirit {} lost the connection";
constexpr const wchar_t* kMsgHostLost = L"Lost the connection to the host. Returning to your world...";
constexpr const wchar_t* kMsgRulesNone = L"World rules: allies cannot harm each other";
constexpr const wchar_t* kMsgRulesHostile = L"World rules: allies can harm each other";
constexpr const wchar_t* kMsgRulesTeams = L"World rules: the host against the guests";
constexpr const wchar_t* kMsgTaunt = L"Invaders are taunted into this world";
constexpr const wchar_t* kMsgCursed = L"This world is cursed";
constexpr const wchar_t* kMsgCurseLifted = L"The curse on this world is lifted";
constexpr const wchar_t* kMsgCurseFaded = L"The curse on this world has faded";  // a curse of a previous game
constexpr const wchar_t* kMsgDied = L"{} has died";
constexpr const wchar_t* kMsgInvaderDied = L"Evil spirit {} has died";
constexpr const wchar_t* kMsgGuestStart = L"Joining another world as a guest...";
constexpr const wchar_t* kMsgGuestMission = L"Joined another world!";
constexpr const wchar_t* kMsgHostWarpOut = L"The power of the homeward bone consumes you...";
constexpr const wchar_t* kMsgBreakInStart = L"Invading another world as an evil spirit";
constexpr const wchar_t* kMsgBreakInMission = L"Invaded another world! Defeat {} players";
constexpr const wchar_t* kMsgBreakInSuccess = L"Invasion success! Returning to your world";
constexpr const wchar_t* kMsgBreakInFailed = L"Invasion failed... Returning to your world";
constexpr const wchar_t* kMsgHostTimeout = L"Failed to create session";
constexpr const wchar_t* kMsgJoinTimeout = L"Failed to find session";
constexpr const wchar_t* kMsgBreakInTimeout = L"Failed to find session to invade";
constexpr const wchar_t* kMsgConnectTimeout = L"Failed to connect";
constexpr const wchar_t* kMsgNoOne = L"No sessions found";
constexpr const wchar_t* kMsgReturnHome = L"Returning to your world...";
constexpr const wchar_t* kMsgHostClosed = L"The host closed their world. Returning to your world...";
constexpr const wchar_t* kMsgWorldClosed = L"Your world is closed. Guests return to their worlds";
constexpr const wchar_t* kMsgWorldOpen = L"Your world is open";

// ---------------------------------------------------------------- state
enum class Role { None, Host, Guest };
enum class Step {
    Idle,
    HostLobby,     // creating our lobby
    Hosting,
    Closing,       // host: disbanding; the guests leave, then the game session ends
    Searching,     // guest: looking for a lobby
    LobbyJoin,     // guest: entering the host's lobby
    Hello,         // guest: waiting for the host's invitation
    Joining,       // guest: game session join in progress
    Loading,       // guest: reloading into the host's world
    InWorld,       // guest: in the host's world
    GoingHome,     // guest: warping home after leaving
};

struct Pending {
    uint64_t from = 0;
    bool invade = false;
    double since = 0;
};

struct RestPoint {
    bool valid = false;
    int bonfire = -1;
    int map = -1;
    game::Vec3 pos{};
    float angle = 0;
};

Config g_cfg;
Role g_role = Role::None;
Step g_step = Step::Idle;
bool g_invade = false;
uint64_t g_lobby = 0;
uint64_t g_host = 0;
std::string g_key;
double g_step_since = 0;
double g_last_create = -100;
double g_last_state = 0;
double g_last_info = 0;
double g_last_ping = 0;
int g_hello_tries = 0;
int g_search_waves = 0;
int g_search_gen = 0;
std::vector<Pending> g_pending;
std::vector<PeerInfo> g_peers;
// Host: the players it let in (a Welcome with kOk). Steam passes on packets from anyone in the lobby, so the host
// takes world changes only from these. Kept apart from g_peers, which also holds players not let in.
std::vector<uint64_t> g_admitted;
// A player whose connection went quiet is dropped (nothing heard for kLostQuiet s after Steam reported the link
// lost, or for kLostSilent s in any case): the host tells the others and stops sending to them; a guest goes
// home. Nothing is counted while this game itself was not running (a machine asleep, a long stall).
constexpr double kLostQuiet = 10.0;
constexpr double kLostSilent = 60.0;
std::map<uint64_t, double> g_p2p_failed;  // when Steam last reported the link to a player lost
std::vector<uint64_t> g_lost;             // host: dropped players, until they ask to join again (kDropped)
double g_host_heard = 0;                  // guest: the last message from the host (not counting kPong)
double g_quiet_grace_until = 0;           // no drops before this (we were not running)
int g_ping_ms = -1;
int64_t g_ping_qpc = 0;  // guest: counter when the last ping was sent (0 none)
std::vector<std::pair<int, bool>> g_flag_queue;
int g_host_coop_players = 1;
RestPoint g_rest;
// Host: Homeward Bone used (seconds), see tick.
double g_homeward_at = 0;
// Guest: the host's Homeward Bone, warp to this bonfire at g_follow_at.
int g_follow_bonfire = -1;
double g_follow_at = 0;
// A cutscene is taking us into another map (rules, Play Cutscene and Warp Player): announced once we are there.
int g_cut_map = -1;
int g_cut_point = -1;
int g_cut_from = -1;
double g_cut_at = 0;
double g_cut_arrived_at = 0;  // when we were first seen there (the event sets its respawn point a moment later)
// Our own copy of such a cutscene could not take us (rules::take_failed_cutscene): we go when it is over.
int g_cut_own_map = -1;
int g_cut_own_point = -1;
double g_cut_own_at = 0;
// The respawn bonfire the other player reported on arriving there (kCutFollow), for our own way there.
int g_cut_hint_map = -1;
int g_cut_hint_bonfire = -1;
// Another player's cutscene took them into this map: we follow them there.
int g_cut_follow_map = -1;
int g_cut_follow_point = -1;
int g_cut_follow_bonfire = -1;
double g_cut_follow_at = 0;
// Guest bookkeeping.
int g_home_bonfire = -1;
int g_guest_bonfire = -1;  // respawn id while we are a guest (-1 none)
bool g_home_warp_sent = true;  // GoingHome: the warp home was requested (or the game makes its own)
int g_home_map = -1;
int g_host_map = -1;
int g_host_bonfire = -1;
// Guest: the host's world rules as last seen in the world (-1 not yet), to tell the player when they change.
int g_seen_rules = -1;
int g_seen_infection = -1;
int g_seen_taunt = -1;
// The Cursed Pendant. Our own world's curse is kept in memory only, so nothing new is written to the save: it lasts
// until the pendant lifts it, the game is closed or another character is loaded, through any number of sessions,
// offline or hosting. g_host_curse is the curse of the world we visit as a guest or invader (the host's rules).
bool g_own_curse = false;
std::wstring g_curse_owner;  // the character whose world we last stood in at home
bool g_host_curse = false;
bool g_callbacks_set = false;
// A Black Separation Crystal was used (1 guest, 2 host); handled on the next tick, outside the player's update.
int g_separation = 0;
bool g_anim_log = false;  // animation sync diagnostic lines in the log (session only)
// Which player each other character in our world belongs to (kept while both exist, see update_remote_players).
std::map<uint32_t, uint64_t> g_chr_peer;
// Our gait as last sent (motion sync).
int g_sent_gait = -1;
double g_last_gait = 0;
// Host, while disbanding: when it began and whether the game's own leave was asked for.
double g_close_at = 0;
bool g_close_left = false;
bool g_close_forced = false;  // a guest was still in the game session when it was ended

double now() { return GetTickCount64() / 1000.0; }
// After a session ends, the party is put back to solo for a while (see settle_after_session).
double g_settle_until = 0;
// Guest: waiting for the host's map flags since this time (0 not waiting), and the flags we changed meanwhile,
// which the snapshot (taken before the host heard of them) would otherwise undo here.
double g_world_wait = 0;
std::vector<std::pair<int, bool>> g_world_ours;

void set_step(Step s) {
    g_step = s;
    g_step_since = now();
}

std::string hex64(uint64_t v) {
    char b[17];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
    return b;
}

// FNV-1a over a domain-separated password; only equal passwords produce equal lobby keys.
std::string password_key(const std::string& password) {
    uint64_t h = 1469598103934665603ULL;
    const std::string s = "ptde-seamless-coop/" + password;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return hex64(h);
}

void say(const std::wstring& w) {
    text::show(w);
    char narrow[512];
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
    LOG_INFO("[coop] %s", narrow);
}

// ---------------------------------------------------------------- version mismatch notices
// A message that matters (a connection refused for another Prepare to Co-op version) is shown three times, four
// seconds apart, so it is not missed among the game's own messages. The middle showing can say who has to update.
struct Flash {
    std::wstring lines[3];
    int next = 3;
    double at = 0;
};
Flash g_flash;

void flash(const std::wstring& line, const std::wstring& middle = L"") {
    g_flash.lines[0] = line;
    g_flash.lines[1] = middle.empty() ? line : middle;
    g_flash.lines[2] = line;
    g_flash.next = 0;
    g_flash.at = 0;
}

void flash_tick() {
    if (g_flash.next >= 3 || now() < g_flash.at) return;
    const std::wstring& line = g_flash.lines[g_flash.next];
    if (g_flash.next == 0 || line != g_flash.lines[0]) {
        say(line);  // logged once per distinct line
    } else {
        text::show(line);
    }
    ++g_flash.next;
    g_flash.at = now() + 4.0;
}

std::wstring our_version() { return text::widen(g_version); }

// The action box shows one line, and the longest of our own notices (kMsgHostLost) is 59 characters. A notice with a
// player's name that would be longer is shown in its short form.
constexpr size_t kNoticeMax = 59;

std::wstring fit_or(const std::wstring& line, const std::wstring& short_form) {
    return line.size() <= kNoticeMax ? line : short_form;
}

// The protocol and Prepare to Co-op version a lobby's host or a hello's sender speaks. 1.0.0 publishes only
// "ptdesc_ver" and its hello ends before the version: protocol 8 without a version is 1.0.0.
struct PeerVersion {
    uint32_t protocol = 0;
    std::string mod;
};

PeerVersion lobby_version(uint64_t lobby) {
    PeerVersion v;
    std::string p = steam::lobby_data(lobby, "ptdesc_proto");
    if (p.empty()) p = steam::lobby_data(lobby, "ptdesc_ver");
    v.protocol = static_cast<uint32_t>(std::strtoul(p.c_str(), nullptr, 10));
    v.mod = steam::lobby_data(lobby, "ptdesc_mod");
    return v;
}

// Players can play together only with the same protocol and the same Prepare to Co-op version.
bool same_version(const PeerVersion& v) { return v.protocol == g_protocol && v.mod == g_version; }

// Compares two versions part by part as numbers ("1.10.0" is newer than "1.9.0"); a missing version is the oldest.
int compare_versions(const std::string& a, const std::string& b) {
    const char* pa = a.c_str();
    const char* pb = b.c_str();
    while (*pa || *pb) {
        char* end_a = nullptr;
        char* end_b = nullptr;
        const unsigned long na = std::strtoul(pa, &end_a, 10);
        const unsigned long nb = std::strtoul(pb, &end_b, 10);
        if (na != nb) return na < nb ? -1 : 1;
        if (end_a == pa && end_b == pb) break;  // neither has a number left
        pa = *end_a == '.' ? end_a + 1 : end_a;
        pb = *end_b == '.' ? end_b + 1 : end_b;
    }
    return 0;
}

// Whether the other side has the newer Prepare to Co-op: the higher protocol, else the higher version.
bool is_newer(const PeerVersion& v) {
    if (v.protocol != g_protocol) return v.protocol > g_protocol;
    return compare_versions(v.mod, g_version) > 0;
}

// "Prepare to Co-op 1.1.0", or "an older / a newer Prepare to Co-op" when the other side did not say which.
std::wstring version_words(const PeerVersion& v) {
    if (!v.mod.empty()) return L"Prepare to Co-op " + text::widen(v.mod);
    if (v.protocol == 8) return L"Prepare to Co-op 1.0.0";
    return is_newer(v) ? L"a newer Prepare to Co-op" : L"an older Prepare to Co-op";
}

std::string version_text(const PeerVersion& v) {
    if (!v.mod.empty()) return v.mod;
    return v.protocol == 8 ? "1.0.0" : "unknown";
}

// A search found only sessions of another version (a joiner still knocks on that host's door, so the host learns
// why we cannot come in); what we know of that session.
struct Mismatch {
    bool active = false;
    PeerVersion host;
};
Mismatch g_mismatch;

void show_join_mismatch(const PeerVersion& host) {
    LOG_INFO("[coop] The host speaks protocol %u (Prepare to Co-op %s), we speak %u (%s)", host.protocol,
             version_text(host).c_str(), g_protocol, g_version.c_str());
    const std::wstring you = L"You have " + our_version();
    flash(L"Can't join: the host has " + version_words(host),
          is_newer(host) ? you + L". Please update to join this host" : you + L". The host needs to update");
}

void show_invade_mismatch(const std::vector<uint64_t>& lobbies) {
    PeerVersion newest = lobby_version(lobbies.front());
    for (uint64_t l : lobbies) {
        const PeerVersion v = lobby_version(l);
        if (v.protocol > newest.protocol ||
            (v.protocol == newest.protocol && compare_versions(v.mod, newest.mod) > 0)) {
            newest = v;
        }
    }
    LOG_INFO("[coop] %zu world(s) to invade, none with our protocol %u and version %s (newest found: %u, "
             "Prepare to Co-op %s)",
             lobbies.size(), g_protocol, g_version.c_str(), newest.protocol, version_text(newest).c_str());
    const std::wstring you = L"You have " + our_version();
    flash(L"Can't invade: these worlds have " + version_words(newest),
          is_newer(newest) ? you + L". Please update to invade them" : you + L". Their hosts need to update");
}

// Host: someone of another version knocked. Told once a minute per player (a 1.0.0 player retries).
std::map<uint64_t, double> g_version_notice;

PeerVersion hello_version(const Hello& m, bool has_version) {
    PeerVersion them;
    them.protocol = m.protocol;
    if (has_version) {
        char v[sizeof(m.version) + 1] = {};
        std::memcpy(v, m.version, sizeof(m.version));
        them.mod = v;
    }
    return them;
}

void show_hello_mismatch(uint64_t from, const Hello& m, const PeerVersion& them) {
    char name[sizeof(m.name) + 1] = {};
    std::memcpy(name, m.name, sizeof(m.name));
    LOG_INFO("[coop] %s (%s) knocked with protocol %u (Prepare to Co-op %s); we speak %u (%s): refused", name,
             m.invade ? "invader" : "guest", them.protocol, version_text(them).c_str(), g_protocol, g_version.c_str());
    auto it = g_version_notice.find(from);
    if (it != g_version_notice.end() && now() - it->second < 60.0) return;
    g_version_notice[from] = now();
    const std::wstring line = m.invade ? L"An evil spirit with " + version_words(them) + L" can't invade"
                                       : L"A player with " + version_words(them) + L" can't join";
    // The middle showing names a guest when the name fits; an invader stays an evil spirit.
    const std::wstring you = L"You have " + our_version();
    const std::wstring who = m.invade ? std::wstring() : text::widen(name);
    std::wstring middle;
    if (is_newer(them)) {
        middle = you + L". Please update to play with them";
        if (!who.empty()) middle = fit_or(L"Please update to play with " + who, middle);
    } else {
        middle = you + L". They need to update";
        if (!who.empty()) middle = fit_or(who + L" needs to update, you have " + our_version(), middle);
    }
    flash(line, middle);
}

template <typename T>
void fill_header(T* m, MsgType t) {
    m->h.magic = kMagic;
    m->h.type = t;
}

template <typename T>
bool send_to(uint64_t to, T& msg, bool reliable = true) {
    return steam::send(to, &msg, sizeof(msg), reliable);
}

bool is_lost(uint64_t id) { return std::find(g_lost.begin(), g_lost.end(), id) != g_lost.end(); }

void unmark_lost(uint64_t id) { g_lost.erase(std::remove(g_lost.begin(), g_lost.end(), id), g_lost.end()); }

void broadcast_raw(const void* data, uint32_t size, bool reliable = true, uint64_t except = 0) {
    if (!g_lobby) return;
    const uint64_t me = steam::self();
    for (uint64_t m : steam::lobby_members(g_lobby)) {
        if (m != me && m != except && !is_lost(m)) steam::send(m, data, size, reliable);
    }
}

template <typename T>
void broadcast(T& msg, bool reliable = true, uint64_t except = 0) {
    broadcast_raw(&msg, sizeof(msg), reliable, except);
}

PeerInfo* peer(uint64_t id) {
    for (auto& p : g_peers) {
        if (p.id == id) return &p;
    }
    PeerInfo p;
    p.id = id;
    p.name = steam::persona(id);
    g_peers.push_back(p);
    return &g_peers.back();
}

bool admitted(uint64_t id) { return std::find(g_admitted.begin(), g_admitted.end(), id) != g_admitted.end(); }

void set_admitted(uint64_t id, bool on) {
    g_admitted.erase(std::remove(g_admitted.begin(), g_admitted.end(), id), g_admitted.end());
    if (on) g_admitted.push_back(id);
}

bool peer_is_invader(uint64_t id) {
    for (const auto& p : g_peers) {
        if (p.id == id) return p.invader;
    }
    return false;
}

// The world we stand in decides which curse the tendency tests see: ours at home, the host's in another player's
// world. Deciding by the world and not by the role keeps the host's curse out of a joiner's own world in the
// seconds between the host's answer and the warp, and ours out of the host's world on the way home.
void apply_curse() { rules::state().infection = game::own_world() ? g_own_curse : g_host_curse; }

void reset_rules() {
    // Our own curse outlives the session; it is lifted only with the pendant. A guest has nothing to clean up: its
    // copy of the host's world is dropped when it goes home, and the disaster flags set there are never copied into
    // its own world (rules.cpp, kDisasterFlags).
    rules::state() = rules::State{};
    g_host_curse = false;
    apply_curse();
}

void finish(const std::wstring& why) {
    if (!why.empty()) say(why);
    if (g_lobby) steam::leave_lobby(g_lobby);
    for (const auto& p : g_peers) steam::close(p.id);
    g_lobby = 0;
    g_host = 0;
    g_role = Role::None;
    g_invade = false;
    g_pending.clear();
    g_peers.clear();
    g_admitted.clear();
    g_lost.clear();
    g_p2p_failed.clear();
    g_host_heard = 0;
    g_seen_rules = g_seen_infection = g_seen_taunt = -1;
    g_homeward_at = 0;
    g_follow_bonfire = -1;
    g_guest_bonfire = -1;
    g_cut_map = -1;
    g_cut_own_map = -1;
    g_cut_follow_map = -1;
    g_close_at = 0;
    g_close_left = false;
    g_close_forced = false;
    g_world_wait = 0;
    g_world_ours.clear();
    reset_rules();
    set_step(Step::Idle);
    g_settle_until = now() + 20.0;
}

PlayerEvent player_event(MsgType type, uint64_t who, bool invader, const std::string& name) {
    PlayerEvent e{};
    fill_header(&e, type);
    e.who = who;
    e.invader = invader ? 1 : 0;
    strncpy_s(e.name, name.c_str(), _TRUNCATE);
    return e;
}

// ---------------------------------------------------------------- host
void host_open_lobby() {
    set_step(Step::HostLobby);
    steam::create_lobby(g_cfg.max_players, [](bool ok, uint64_t lobby) {
        if (!ok) {
            finish(kMsgHostTimeout);
            return;
        }
        g_lobby = lobby;
        steam::set_lobby_data(lobby, "ptdesc", "1");
        steam::set_lobby_data(lobby, "ptdesc_ver", kLegacyProtocol);
        steam::set_lobby_data(lobby, "ptdesc_proto", std::to_string(g_protocol).c_str());
        steam::set_lobby_data(lobby, "ptdesc_mod", g_version.c_str());
        steam::set_lobby_data(lobby, "ptdesc_key", g_key.c_str());
        steam::set_lobby_data(lobby, "ptdesc_name", steam::persona(steam::self()).c_str());
        steam::set_lobby_data(lobby, "ptdesc_invade", g_cfg.allow_invaders ? "1" : "0");
        steam::set_lobby_data(lobby, "ptdesc_taunt", "0");
        steam::set_lobby_joinable(lobby, true);
        auto& st = rules::state();
        st.active = true;
        st.host = true;
        set_step(Step::Hosting);
        LOG_INFO("[coop] Hosting a co-op session (lobby %016llx)", static_cast<unsigned long long>(lobby));
        say(kMsgWorldOpen);
    });
}

void host_ensure_game_session() {
    const int state = game::session_state();
    if (state == 3) return;
    if (!game::in_game() || !game::own_world()) return;
    if (state == 2 || state == 5) {
        // a failed attempt: clear it so a new one can start
        rules::state().leaving = true;
        game::leave_session();
        rules::state().leaving = false;
        return;
    }
    if (state != 0) return;
    if (now() - g_last_create < 5.0) return;
    g_last_create = now();
    const bool ok = game::create_session(game::map_id(), 55.0f);
    LOG_INFO("[coop] Game session %s (map %08X)", ok ? "starting" : "could not start", game::map_id());
}

int coop_guest_count() {
    int n = 0;
    for (const auto& p : g_peers) {
        if (!p.invader && now() - p.last_seen <= 10.0) ++n;
    }
    return n;
}

// Host: players in the session we have not heard from (see kLostQuiet). The game's own session lets them go by
// itself when Steam gives up on the link; this tells everyone and stops our messages to them.
void drop_quiet_players() {
    if (now() < g_quiet_grace_until) return;
    const std::vector<uint64_t> ids = g_admitted;
    for (uint64_t id : ids) {
        auto it = std::find_if(g_peers.begin(), g_peers.end(), [id](const PeerInfo& q) { return q.id == id; });
        if (it == g_peers.end()) continue;
        const double quiet = now() - it->last_seen;
        const auto f = g_p2p_failed.find(id);
        const bool failed = f != g_p2p_failed.end() && f->second >= it->last_seen;
        if (!(failed && quiet >= kLostQuiet) && quiet < kLostSilent) continue;
        const PeerInfo p = *it;
        LOG_INFO("[coop] %s: nothing heard for %.0f s%s; dropped from the session", p.name.c_str(), quiet,
                 failed ? " and Steam reports the link lost" : "");
        say(fmt(p.invader ? kMsgInvaderLost : kMsgLost, text::widen(p.name)));
        PlayerEvent e = player_event(kLeft, id, p.invader, p.name);
        e.lost = 1;
        broadcast(e, true, id);
        steam::close(id);
        steam::accept(id);  // still take their packets, to tell them (kDropped) if they come back
        g_peers.erase(it);
        set_admitted(id, false);
        g_p2p_failed.erase(id);
        if (!is_lost(id)) g_lost.push_back(id);
    }
}

void host_answer(const Pending& p) {
    Welcome w{};
    fill_header(&w, kWelcome);
    w.status = kOk;
    int players = 1;
    for (const auto& peer_info : g_peers) players += peer_info.invader || peer_info.id == p.from ? 0 : 1;
    if (!p.invade && players >= g_cfg.max_players) w.status = kFull;
    if (p.invade && !g_cfg.allow_invaders) w.status = kRefused;
    // No invasions in the Undead Asylum or Firelink Shrine (as in Remastered Seamless Co-op).
    const int host_map = game::map_id();
    if (p.invade && (host_map == 0x12010000 || host_map == 0x0A020000)) w.status = kRefused;
    // Between maps or at the title screen there is no world to arrive in yet: the joiner asks again.
    if (w.status == kOk && (!game::in_game() || game::loading() || !game::own_world())) w.status = kBusy;
    std::vector<uint8_t> inv;
    if (w.status == kOk) {
        // join type: -1 white (co-op, own body kept by the guest's rules), -2 black (invader)
        if (!game::build_invitation(p.invade ? -2 : -1, 0, &inv) || inv.size() > sizeof(w.invitation)) {
            w.status = kBusy;
        }
    }
    if (w.status == kOk) {
        const auto& st = rules::state();
        // Guests arrive at the host's last bonfire (where the host last rested); invaders near the host.
        const int bonfire = game::last_bonfire();
        if (!p.invade && g_rest.valid && g_rest.bonfire == bonfire) {
            w.map = g_rest.map;
            w.pos[0] = g_rest.pos.x;
            w.pos[1] = g_rest.pos.y;
            w.pos[2] = g_rest.pos.z;
            w.angle = g_rest.angle;
        } else {
            game::Vec3 pos{};
            float angle = 0;
            game::position(&pos, &angle);
            w.map = game::map_id();
            w.pos[0] = pos.x;
            w.pos[1] = pos.y;
            w.pos[2] = pos.z;
            w.angle = angle;
        }
        w.bonfire = bonfire;
        w.world_rules = static_cast<uint8_t>(st.world_rules);
        w.infection = st.infection ? 1 : 0;
        w.taunt = st.taunt ? 1 : 0;
        w.coop_players = static_cast<uint8_t>(1 + coop_guest_count());
        w.invitation_size = static_cast<uint32_t>(inv.size());
        std::memcpy(w.invitation, inv.data(), inv.size());
    }
    set_admitted(p.from, w.status == kOk);
    send_to(p.from, w);
    LOG_INFO("[coop] Answered %s (%s): status %u", steam::persona(p.from).c_str(), p.invade ? "invader" : "guest",
             w.status);
}

// The party counts the game keeps beside the session (PartyMemberInfo +0x10/+0x14, slot +0x7C). The game
// brings them down itself when a guest's character is taken out of the party. They are only put right here
// when a guest never left and the session had to be ended over them: HasParty would keep Quit grey.
void settle_party_counts() {
    const uint32_t gm = game::game_man();
    uint32_t party = 0;
    if (!gm || !read32(gm + game::kGmParty, &party) || party < 0x10000) return;
    uint32_t n10 = 0, n14 = 0, slot = 0;
    read32(party + 0x10, &n10);
    read32(party + 0x14, &n14);
    read32(party + 0x7C, &slot);
    if (static_cast<int>(n10) <= 1 && static_cast<int>(n14) <= 1 && static_cast<int>(slot) == -1) return;
    if (static_cast<int>(n10) > 1) *ptr<int>(party + 0x10) = 1;
    if (static_cast<int>(n14) > 1) *ptr<int>(party + 0x14) = 1;
    if (static_cast<int>(slot) != -1) *ptr<int>(party + 0x7C) = -1;
    LOG_INFO("[coop] Party counts settled after the session (were %d/%d, slot %d)", static_cast<int>(n10),
             static_cast<int>(n14), static_cast<int>(slot));
}

// 0xEA1900 writes -1 over the four party member handles and does not touch the characters. Quit destroys
// the characters and then counts whoever those handles still name.
void blank_party_handles(uint32_t party) {
    if (party < 0x10000) return;
    __try {
        __asm {
            mov eax, party
            mov edx, 0x00EA1900
            call edx
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Once a session is over (the game session is solo and no other player's character is left in our world) the
// party is put back to a solo game's: counts 1, no slot, no member handles. The game lowers the counts as each
// member's character goes, but not in every way a session ends (a guest's own leave, a host left alone), and
// a party that still counts two keeps HasPartyMembers true: the bonfire is unusable, talk refuses service and
// the menu treats the game as multiplayer. Run for a while after every session (see tick).
void settle_after_session() {
    if (game::session_state() != 0 || game::loading() || !game::remote_player_chrs().empty()) return;
    settle_party_counts();
    const uint32_t gm = game::game_man();
    uint32_t party = 0;
    if (gm && read32(gm + game::kGmParty, &party) && party > 0x10000) blank_party_handles(party);
}

// ---------------------------------------------------------------- state probe (diagnostics)
// One line with what decides whether the local player can act (interact, rest, answer a dialog) and whether the
// game still counts a party, written whenever it changes (Ctrl+F12 writes it at once). For the reports where
// world objects stop answering and dialogs take no input.
uint32_t peek32(uint32_t addr);
uint32_t peek8(uint32_t addr);

bool __cdecl raw_has_party() {
    using Fn = bool(__cdecl*)();
    // It reads SessMan ([0x137D82C]) and GameMan's party ([0x13784A0] + 0xBEC) without checking them.
    const uint32_t gm = game::game_man();
    if (!game::sess_man() || !gm || peek32(gm + game::kGmParty) < 0x10000) return false;
    __try {
        return reinterpret_cast<Fn>(0x00D2A4B0)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t peek32(uint32_t addr) {
    uint32_t v = 0xFFFFFFFF;
    read32(addr, &v);
    return v;
}

uint32_t peek8(uint32_t addr) {
    uint8_t v = 0xFF;
    if (!read8(addr, &v)) return 0xFF;
    return v;
}

void probe_state(bool force) {
    static double last = 0;
    static std::string previous;
    static int lines = 0;
    if (!force && now() - last < 0.5) return;
    last = now();
    char text[640];
    int n = 0;
    auto put = [&](const char* fmt, auto... args) {
        if (n < static_cast<int>(sizeof(text))) n += _snprintf_s(text + n, sizeof(text) - n, _TRUNCATE, fmt, args...);
    };
    const auto& st = rules::state();
    put("sess %d step %d mem %d", game::session_state(), game::join_step(), game::member_count());
    const uint32_t gm = game::game_man();
    const uint32_t party = gm ? peek32(gm + game::kGmParty) : 0;
    if (party > 0x10000) {
        put(" party %d/%d slot %d", static_cast<int>(peek32(party + 0x10)), static_cast<int>(peek32(party + 0x14)),
            static_cast<int>(peek32(party + 0x7C)));
    }
    put(" hasparty %d remote %d", raw_has_party() ? 1 : 0, static_cast<int>(game::remote_player_chrs().size()));
    put(" rules %s%s%s%s%s", st.active ? "A" : "-", st.host ? "H" : "-", st.guest ? "G" : "-", st.leaving ? "L" : "-",
        st.closing ? "C" : "-");
    const uint32_t mm = peek32(0x013786D0);
    if (mm > 0x10000 && mm != 0xFFFFFFFF) {
        static const uint32_t kOpen[] = {0x40, 0x4C, 0x78, 0x84, 0x80, 0x50, 0xAC};
        uint32_t open = 0;
        for (size_t i = 0; i < sizeof(kOpen) / sizeof(kOpen[0]); ++i) {
            if (peek8(mm + kOpen[i]) != 0) open |= 1u << i;
        }
        put(" menu req %d open %02X", static_cast<int>(peek32(mm + 0x60)), open);
        const uint32_t dlg = peek32(mm + 0x9E8);
        if (dlg > 0x10000 && dlg != 0xFFFFFFFF) {
            put(" dialog %d res %d/%d", static_cast<int>(peek32(dlg)), static_cast<int>(peek32(dlg + 0x18)),
                static_cast<int>(peek32(dlg + 0x1C)));
        }
        // The dialog slots (MenuMan+0x9EC: index at +4, five slots of 0x34 from +8, +0x24 = requested), where
        // the "items obtained" box waits to be shown.
        char slots[6] = {};
        for (int i = 0; i < 5; ++i) slots[i] = peek8(mm + 0x9F4 + i * 0x34 + 0x24) ? '1' : '0';
        put(" slots %d:%s", static_cast<int>(peek32(mm + 0x9F0)), slots);
    }
    put(" box %s/%d", peek32(0x01378548) ? "yes" : "no", static_cast<int>(peek8(0x01378543)));
    char filter[64];
    text::debug_state(filter, sizeof(filter));
    put(" %s", filter);
    const uint32_t me = game::player();
    if (me) {
        const uint32_t pgd = game::player_game_data();
        put(" map %08X %s chr %d/%d hp %s", static_cast<uint32_t>(game::map_id()), game::own_world() ? "own" : "visit",
            game::chr_type(), pgd ? static_cast<int>(peek32(pgd + 0x9C)) : -1,
            static_cast<int>(peek32(me + 0x2D4)) > 0 ? "up" : "down");
        const bool slot_open = !(peek32(me + 0xFC) == peek32(0x012E0C64) && peek32(me + 0x100) == peek32(0x012E0C68));
        const uint32_t request = peek32(me + 0x10);
        int anim = -1, loop = 0;
        if (request > 0x10000 && request != 0xFFFFFFFF) {
            anim = static_cast<int>(peek32(request + 4));
            loop = static_cast<int>(peek8(request + 8));
        }
        put(" event %s %d%s 1FE %04X", slot_open ? "open" : "none", anim, loop ? " loop" : "",
            peek32(me + 0x1FE) & 0xFFFF);
    } else {
        put(" no character");
    }
    put(" left-joined %d", static_cast<int>(peek8(0x01376EE4)));
    const std::string now_text(text);
    if (!force && now_text == previous) return;
    previous = now_text;
    if (++lines > 800 && !force) return;
    LOG_INFO("[coop] state: %s", text);
}

// The host disbands the way the game ends a session after the Black Separation Crystal's menu: the guests
// leave first (they were told), their characters go out of the party through the game's own member-left code,
// and when the host is the last member the game's own leave closes the session (0xD2B460: leaving, then solo).
// Ending the session over the guests' heads (the earlier builds) left the party and the member list behind.
void host_close_tick() {
    const int state = game::session_state();
    const double waited = now() - g_close_at;
    if (state == 0) {
        if (g_close_forced) settle_party_counts();
        settle_after_session();
        LOG_INFO("[coop] Session disbanded (%.1f s); the host stays in the world", waited);
        finish(L"");
        return;
    }
    const int members = game::member_count();
    // Hosting (3): once alone, or when a guest has not left after six seconds. Still being created or failed
    // (1, 2, 5): after a moment, the same leave clears it. Leaving (7) is the game's own leave in progress.
    const bool hosting_done = state == 3 && (members <= 1 || waited > 6.0);
    const bool never_hosted = state != 3 && state != 7 && waited > 3.0;
    if (!g_close_left && (hosting_done || never_hosted)) {
        g_close_left = true;
        g_close_forced = state == 3 && members > 1;
        rules::state().leaving = true;
        LOG_INFO("[coop] Ending the game session (state %d, %d member(s) in it, %.1f s after the request)", state,
                 members, waited);
        game::leave_session();
        return;
    }
    if (waited > 25.0) {
        // The game's leave never finished (no answer from Steam): return the session to solo by hand.
        LOG_ERROR("[coop] The game session did not close (state %d); returned to solo", state);
        rules::state().leaving = true;
        game::abort_session();
        if (game::session_state() != 0) {
            if (const uint32_t s = game::sess_man()) *ptr<int>(s + game::kSessState) = 0;
        }
        settle_party_counts();
        finish(L"");
    }
}

void host_tick() {
    if (g_step == Step::Closing) {
        host_close_tick();
        return;
    }
    if (g_step != Step::Hosting) return;
    host_ensure_game_session();
    drop_quiet_players();
    const bool ready = game::session_state() == 3;
    for (size_t i = 0; i < g_pending.size();) {
        if (ready) {
            host_answer(g_pending[i]);
            g_pending.erase(g_pending.begin() + i);
        } else if (now() - g_pending[i].since > 20.0) {
            Welcome w{};
            fill_header(&w, kWelcome);
            w.status = kBusy;
            send_to(g_pending[i].from, w);
            g_pending.erase(g_pending.begin() + i);
        } else {
            ++i;
        }
    }
    // A Homeward Bone in a session (Remastered Seamless Co-op): the host goes to their last bonfire and the
    // guests follow there.
    if (g_homeward_at > 0) {
        if (now() - g_homeward_at < 2.0 && game::in_game() && !game::loading()) {
            const int bonfire = game::last_bonfire();
            BonfireMsg m{};
            fill_header(&m, kHomeward);
            m.bonfire = bonfire;
            broadcast(m);
            LOG_INFO("[coop] Host Homeward Bone: everyone goes to bonfire %d", bonfire);
            if (bonfire >= 0) world::warp_now(game::bonfire_map(bonfire), -1);
        }
        g_homeward_at = 0;
    }
    if (now() - g_last_state > 1.0) {
        g_last_state = now();
        const auto& st = rules::state();
        StateMsg s{};
        fill_header(&s, kState);
        s.map = game::map_id();
        s.bonfire = game::last_bonfire();
        s.players = static_cast<uint8_t>(game::member_count());
        bool invader = false;
        for (const auto& p : g_peers) invader |= p.invader && now() - p.last_seen <= 10.0;
        const int coop = 1 + coop_guest_count();
        s.invader_present = invader ? 1 : 0;
        s.coop_players = static_cast<uint8_t>(coop);
        s.allow_invaders = g_cfg.allow_invaders ? 1 : 0;
        s.world_rules = static_cast<uint8_t>(st.world_rules);
        s.infection = st.infection ? 1 : 0;
        s.taunt = st.taunt ? 1 : 0;
        rules::state().invader_present = invader;
        g_host_coop_players = coop;
        broadcast(s);
    }
}

// ---------------------------------------------------------------- guest
void guest_send_hello() {
    Hello h{};
    fill_header(&h, kHello);
    h.protocol = g_protocol;
    h.invade = g_invade ? 1 : 0;
    h.soul_level = game::soul_level();
    strncpy_s(h.name, steam::persona(steam::self()).c_str(), _TRUNCATE);
    strncpy_s(h.version, g_version.c_str(), _TRUNCATE);
    steam::accept(g_host);
    send_to(g_host, h);
    ++g_hello_tries;
    set_step(Step::Hello);
}

void issue_search() {
    const int gen = ++g_search_gen;
    ++g_search_waves;
    std::vector<std::pair<std::string, std::string>> filters;
    if (g_invade) {
        filters.push_back({"ptdesc_invade", "1"});
    } else {
        filters.push_back({"ptdesc_key", g_key});
    }
    g_mismatch = Mismatch{};
    steam::find_lobbies(filters, 50, [gen](std::vector<uint64_t> found) {
        LOG_INFO("[coop] Lobby list returned %zu", found.size());
        // A newer request replaces this one. An older empty answer must not cancel the retry.
        if (gen != g_search_gen || g_step != Step::Searching) return;
        const uint64_t me = steam::self();
        found.erase(std::remove_if(found.begin(), found.end(),
                                   [me](uint64_t l) { return steam::lobby_owner(l) == me; }),
                    found.end());
        if (found.empty()) {
            finish(kMsgNoOne);
            return;
        }
        // Sessions of another Prepare to Co-op version cannot be joined or invaded: they are set apart and,
        // when nothing else was found, reported.
        std::vector<uint64_t> other;
        for (uint64_t l : found) {
            if (!same_version(lobby_version(l))) other.push_back(l);
        }
        found.erase(std::remove_if(found.begin(), found.end(),
                                   [](uint64_t l) { return !same_version(lobby_version(l)); }),
                    found.end());
        if (found.empty()) {
            if (g_invade) {
                finish(L"");
                show_invade_mismatch(other);
                return;
            }
            // Knock anyway: the host refuses us for the version and learns why we cannot come in.
            g_mismatch.active = true;
            g_mismatch.host = lobby_version(other.front());
            found.push_back(other.front());
        }
        uint64_t pick = g_invade ? found[GetTickCount() % found.size()] : found.front();
        if (g_invade) {
            for (uint64_t l : found) {
                if (steam::lobby_data(l, "ptdesc_taunt") == "1") {  // Dried Fingers draw invaders first
                    pick = l;
                    break;
                }
            }
        }
        set_step(Step::LobbyJoin);
        steam::join_lobby(pick, [](bool ok, uint64_t lobby) {
            if (g_step != Step::LobbyJoin) return;
            if (!ok) {
                if (g_mismatch.active) {
                    finish(L"");
                    show_join_mismatch(g_mismatch.host);
                    return;
                }
                finish(kMsgConnectTimeout);
                return;
            }
            g_lobby = lobby;
            g_host = steam::lobby_owner(lobby);
            g_host_heard = now();
            LOG_INFO("[coop] Entered %s's lobby", steam::persona(g_host).c_str());
            g_hello_tries = 0;
            guest_send_hello();
        });
    });
}

void guest_search() {
    g_search_waves = 0;
    set_step(Step::Searching);
    issue_search();
}

const wchar_t* rules_text(int world_rules) {
    return world_rules == 1 ? kMsgRulesHostile : world_rules == 2 ? kMsgRulesTeams : kMsgRulesNone;
}

void apply_host_rules(int world_rules, bool infection, bool taunt) {
    auto& st = rules::state();
    st.world_rules = world_rules;
    g_host_curse = infection;
    apply_curse();
    st.taunt = taunt;
    // Changes the host makes while we are there (Ominous Tome, Dried Fingers, Cursed Pendant) are shown to guests,
    // and a curse also when the guest arrives in a world that is already cursed.
    if (g_step != Step::InWorld || g_invade) return;
    if (g_seen_rules >= 0 && world_rules != g_seen_rules) say(rules_text(world_rules));
    if (g_seen_taunt == 0 && taunt) say(kMsgTaunt);
    if (g_seen_infection != 1 && infection) say(kMsgCursed);
    if (g_seen_infection == 1 && !infection) {
        say(kMsgCurseLifted);
        rules::end_disasters(true);  // this game runs the host's world's events too, phantoms included
    }
    g_seen_rules = world_rules;
    g_seen_taunt = taunt ? 1 : 0;
    g_seen_infection = infection ? 1 : 0;
}

void guest_on_welcome(const Welcome& w) {
    if (g_step != Step::Hello) return;
    if (w.status == kBusy && g_hello_tries < 8) {
        set_step(Step::Hello);
        g_step_since = now() - 17.0;  // retry in 3 s
        return;
    }
    if (w.status == kVersion || (w.status != kOk && g_mismatch.active)) {
        LOG_INFO("[coop] The host refused us (status %u: another Prepare to Co-op version)", w.status);
        const PeerVersion host = g_mismatch.active ? g_mismatch.host : lobby_version(g_lobby);
        finish(L"");
        show_join_mismatch(host);
        return;
    }
    if (w.status != kOk) {
        LOG_INFO("[coop] The host refused us (status %u)", w.status);
        finish(w.status == kFull || w.status == kRefused ? kMsgNoOne : kMsgConnectTimeout);
        return;
    }
    // The invitation's length comes from the host's packet: the game's reader would take it as the buffer's size.
    if (w.invitation_size < 8 || w.invitation_size > sizeof(w.invitation)) {
        LOG_ERROR("[coop] The host's invitation has a bad length (%u bytes); not joining", w.invitation_size);
        finish(kMsgConnectTimeout);
        return;
    }
    if (!game::in_game() || !game::own_world() || game::loading()) {
        finish(kMsgConnectTimeout);
        return;
    }
    const int type = game::chr_type();
    if (type != 0 && type != 8) {
        LOG_INFO("[coop] Joining needs our own body (chr type %d)", type);
        finish(kMsgConnectTimeout);
        return;
    }
    auto& st = rules::state();
    st.active = true;
    st.guest = true;
    st.invader = g_invade;
    st.own_chr_type = type;
    apply_host_rules(w.world_rules, w.infection != 0, w.taunt != 0);
    g_host_coop_players = w.coop_players ? w.coop_players : 1;
    g_home_bonfire = game::last_bonfire();
    g_home_map = game::map_id();
    g_host_map = w.map;
    g_host_bonfire = w.bonfire;
    // Until the guest rests, a death uses the host's bonfire. Set before the join: the reload can run inside it.
    if (w.bonfire >= 0 && !g_invade) g_guest_bonfire = w.bonfire;
    const game::Vec3 pos{w.pos[0], w.pos[1], w.pos[2]};
    if (!game::join_from_invitation(w.invitation, w.invitation_size, w.map, pos, w.angle)) {
        finish(kMsgConnectTimeout);
        return;
    }
    // Deaths in the host's world respawn at the host's last bonfire (until the guest rests at one there).
    if (w.bonfire >= 0 && !g_invade) *ptr<int>(game::game_man() + game::kGmLastBonfire) = w.bonfire;
    set_step(Step::Joining);
    say(g_invade ? kMsgBreakInStart : kMsgGuestStart);
    LOG_INFO("[coop] Joining %s: map %08X bonfire %d, own type %d, home bonfire %d",
             steam::persona(g_host).c_str(), w.map, w.bonfire, type, g_home_bonfire);
}

void warp_home() {
    g_home_warp_sent = true;
    const uint32_t gm = game::game_man();
    LOG_INFO("[coop] Warping home (bonfire %d, map %08X)", g_home_bonfire, g_home_map);
    if (g_home_bonfire >= 0) {
        *ptr<int>(gm + game::kGmLastBonfire) = g_home_bonfire;
        world::warp_now(game::bonfire_map(g_home_bonfire), -1);
    } else {
        world::warp_now(g_home_map, -1);
    }
    const int own = rules::state().own_chr_type;
    if (own >= 0) *ptr<int>(game::game_data_man() + 0x60) = own;
}

void guest_go_home(const std::wstring& why, const char* reason) {
    LOG_INFO("[coop] Going home (%s; session state %d, step %d)", reason, game::session_state(), game::join_step());
    if (!why.empty()) say(why);
    auto& st = rules::state();
    st.leaving = true;
    if (game::session_state() != 0) game::leave_session();
    game::set_join_step(0);
    if (g_lobby) steam::leave_lobby(g_lobby);
    g_lobby = 0;
    if (game::loading() || !game::own_world()) {
        // Still in the host's world (or in a load, which may be the game's own way out): the warp home is made
        // once a character is in a map that is not ours (see GoingHome), never on top of a load.
        g_home_warp_sent = false;
        set_step(Step::GoingHome);
        if (game::in_game() && !game::loading() && !game::own_world()) warp_home();
    } else {
        finish(L"");
    }
}

void guest_arrived() {
    set_step(Step::InWorld);
    if (g_invade) {
        int defeat = g_host_coop_players;
        say(fmt(kMsgBreakInMission, std::to_wstring(defeat)));
    } else {
        say(kMsgGuestMission);
    }
    PlayerEvent e = player_event(kArrived, steam::self(), g_invade, steam::persona(steam::self()));
    if (g_host) send_to(g_host, e);
    if (!g_invade) {
        g_world_wait = now();
        g_world_ours.clear();
    }
}

// Host: the world's map flags for a co-op guest who has just arrived (see rules::world_flag_snapshot).
void send_world_flags(uint64_t to) {
    const std::vector<uint8_t> snap = rules::world_flag_snapshot();
    if (snap.empty()) {
        LOG_ERROR("[coop] Could not read our world's map flags for %s", steam::persona(to).c_str());
        return;
    }
    std::vector<uint8_t> msg(sizeof(Header) + snap.size());
    Header h{kMagic, kWorldFlags};
    std::memcpy(msg.data(), &h, sizeof(h));
    std::memcpy(msg.data() + sizeof(h), snap.data(), snap.size());
    const bool sent = steam::send(to, msg.data(), static_cast<uint32_t>(msg.size()), true);
    uint16_t blocks = 0;
    std::memcpy(&blocks, snap.data(), 2);
    LOG_INFO("[coop] Sent our world's map flags to %s (%u blocks, %u bytes)%s", steam::persona(to).c_str(), blocks,
             static_cast<unsigned>(msg.size()), sent ? "" : ": send failed");
}

// Guest: the host's map flags replace our copy of the host's world (every map, not only the one we joined in).
void on_world_flags(const uint8_t* data, uint32_t size) {
    const int written = rules::apply_world_flag_snapshot(data, size);
    if (written < 0) {
        LOG_ERROR("[coop] The host's map flags did not fit our copy of their world (%u bytes); kept ours", size);
    } else {
        for (const auto& [id, on] : g_world_ours) rules::apply_remote_flag(id, on);
        LOG_INFO("[coop] Took the host's map flags (%d blocks; %zu flags of ours set again)", written,
                 g_world_ours.size());
    }
    g_world_wait = 0;
    g_world_ours.clear();
}

void guest_tick() {
    const double in_step = now() - g_step_since;
    switch (g_step) {
        case Step::Searching:
            // One lobby-list request hung after the previous session's P2P failure. Ask once more
            // without restarting the wait; an empty answer still finishes as "No sessions found".
            if (g_search_waves < 2 && in_step > 8.0) {
                LOG_INFO("[coop] Still searching; asking Steam again");
                issue_search();
            }
            if (in_step > 20.0) finish(g_invade ? kMsgBreakInTimeout : kMsgJoinTimeout);
            break;
        case Step::LobbyJoin:
            if (in_step > 20.0) finish(g_invade ? kMsgBreakInTimeout : kMsgJoinTimeout);
            break;
        case Step::Hello:
            if (in_step > 20.0 && g_mismatch.active) {
                finish(L"");
                show_join_mismatch(g_mismatch.host);
            } else if (in_step > 20.0) {
                if (g_hello_tries < 8) {
                    guest_send_hello();
                } else {
                    finish(kMsgConnectTimeout);
                }
            }
            break;
        case Step::Joining: {
            const int s = game::session_state();
            if (s == 6) {
                set_step(Step::Loading);
            } else if (s == 5 || in_step > 40.0) {
                rules::state().leaving = true;
                if (s != 0) game::leave_session();
                finish(kMsgConnectTimeout);
            }
            break;
        }
        case Step::Loading:
            if (game::session_state() != 6 && in_step > 5.0) {
                guest_go_home(kMsgConnectTimeout, "the game session ended while loading");
            } else if (game::join_step() == 4 && game::in_game() && !game::own_world()) {
                guest_arrived();
            } else if (in_step > 180.0) {
                guest_go_home(kMsgConnectTimeout, "loading took over 3 minutes");
            }
            break;
        case Step::InWorld:
            if (g_host_heard > 0 && now() >= g_quiet_grace_until) {
                const double quiet = now() - g_host_heard;
                const auto f = g_p2p_failed.find(g_host);
                const bool failed = f != g_p2p_failed.end() && f->second >= g_host_heard;
                if ((failed && quiet >= kLostQuiet) || quiet >= kLostSilent) {
                    char why[96];
                    std::snprintf(why, sizeof(why), "nothing heard from the host for %.0f s%s", quiet,
                                  failed ? ", link lost" : "");
                    guest_go_home(kMsgHostLost, why);
                    break;
                }
            }
            if (g_world_wait > 0 && now() - g_world_wait > 30.0) {
                LOG_INFO("[coop] The host's map flags did not come; our copy of their world stays as it is");
                g_world_wait = 0;
                g_world_ours.clear();
            }
            if (game::session_state() != 6 && !game::loading()) {
                if (g_invade) {
                    // The game sends a finished invader home itself (its death or the host's); give it time.
                    rules::state().leaving = true;
                    if (g_lobby) steam::leave_lobby(g_lobby);
                    g_lobby = 0;
                    g_home_warp_sent = true;  // the game's own way home
                    set_step(Step::GoingHome);
                } else {
                    guest_go_home(kMsgReturnHome, "the game session ended");
                }
            }
            // The host's Homeward Bone: follow after a moment.
            if (g_follow_bonfire >= 0 && now() >= g_follow_at && game::in_game() && !game::loading()) {
                const int bonfire = g_follow_bonfire;
                g_follow_bonfire = -1;
                g_guest_bonfire = bonfire;
                *ptr<int>(game::game_man() + game::kGmLastBonfire) = bonfire;
                world::warp_now(game::bonfire_map(bonfire), -1);
            }
            break;
        case Step::GoingHome:
            if (g_invade && in_step > 10.0 && game::in_game() && !game::own_world() && !game::loading()) {
                g_invade = false;  // the game did not send us home: do it ourselves
                guest_go_home(L"", "the invasion ended");
                break;
            }
            if (!g_home_warp_sent && game::in_game() && !game::loading() && !game::own_world()) warp_home();
            if (game::in_game() && game::own_world() && !game::loading()) {
                if (rules::state().own_chr_type >= 0 && game::chr_type() != rules::state().own_chr_type) {
                    LOG_INFO("[coop] Home as chr type %d (was %d before joining)", game::chr_type(),
                             rules::state().own_chr_type);
                }
                finish(L"");
            } else if (in_step > 120.0) {
                finish(L"");
            }
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------- shared
void send_info() {
    Info i{};
    fill_header(&i, kInfo);
    i.soul_level = game::soul_level();
    i.deaths = game::death_count();
    i.map = game::map_id();
    game::Vec3 pos{};
    game::position(&pos, nullptr);
    i.pos[0] = pos.x;
    i.pos[1] = pos.y;
    i.pos[2] = pos.z;
    i.ping_ms = g_ping_ms;
    i.invader = g_invade ? 1 : 0;
    // Dead until the respawned character exists (the load after a death has no character at all), so the
    // others rebuild our character only once we are back.
    static bool dead = false;
    if (world::local_dead()) {
        dead = true;
    } else if (game::player() && !game::loading()) {
        dead = false;
    }
    i.dead = dead ? 1 : 0;
    i.light = world::local_lights();
    strncpy_s(i.name, steam::persona(steam::self()).c_str(), _TRUNCATE);
    wcsncpy_s(i.char_name, party::local_name().c_str(), _TRUNCATE);
    i.ceremony = motion::local_loop_anim();
    broadcast(i, false);
}

std::wstring event_name(const PlayerEvent& e) {
    char name[65];
    std::memcpy(name, e.name, 64);
    name[64] = 0;
    return text::widen(name);
}

// The character in our world that belongs to a player (0 none), from the last match (update_remote_players).
// Only characters the game still lists are answered: a map load frees them all.
uint32_t chr_of_peer(uint64_t id) {
    for (const auto& [chr, who] : g_chr_peer) {
        if (who != id) continue;
        for (uint32_t live : game::remote_player_chrs()) {
            if (live == chr) return chr;
        }
    }
    return 0;
}

// Bonfire animations are passed on as they start and stop. A message can be missed (a character that was
// not built yet, a player who arrived while the other was already sitting), so each player also reports
// the loop they are in once a second, and the character here is brought in line with that: the sit is
// started for a character that is not sitting, and a sit nobody reports any more is stopped.
void reconcile_ceremonies() {
    static double last = 0;
    if (now() - last < 0.5) return;
    last = now();
    if (g_role == Role::None || game::loading()) return;
    static std::map<uint64_t, double> tried;  // one attempt per player every few seconds
    static int lines = 0;
    for (const auto& p : g_peers) {
        if (p.invader || now() - p.last_seen > 3.0 || now() - p.anim_at < 1.5) continue;
        const uint32_t chr = chr_of_peer(p.id);
        if (!chr) continue;
        const int have = motion::remote_loop_anim(chr);
        if (p.ceremony == have) {
            // Not sitting on either side. A one-shot the game never let go of is cleared once its player
            // has been quiet for longer than any of these animations lasts.
            const int open = have == 0 ? motion::remote_open_anim(chr) : 0;
            if (open == 0 || now() - p.anim_at < 15.0) continue;
            double& tried_at = tried[p.id];
            if (now() - tried_at < 3.0) continue;
            tried_at = now();
            if (motion::play_remote_anim(chr, motion::Anim{1, 0, 0}) && ++lines <= 60) {
                LOG_INFO("[coop] %s's bonfire animation %d was still held on their character: cleared",
                         p.name.c_str(), open);
            }
            continue;
        }
        double& at = tried[p.id];
        if (now() - at < 3.0) continue;
        at = now();
        if (p.ceremony != 0) {
            if (motion::play_remote_anim(chr, motion::Anim{0, p.ceremony, 1}) && ++lines <= 60) {
                LOG_INFO("[coop] %s is sitting at a bonfire (animation %d): shown on their character",
                         p.name.c_str(), p.ceremony);
            }
        } else if (motion::play_remote_anim(chr, motion::Anim{1, 0, 0}) && ++lines <= 60) {
            LOG_INFO("[coop] %s is no longer in bonfire animation %d: stopped on their character", p.name.c_str(),
                     have);
        }
    }
}

void on_message(uint64_t from, const uint8_t* data, uint32_t size) {
    if (size < sizeof(Header)) return;
    Header h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic) return;
    auto as = [&](auto* out) {
        if (size < sizeof(*out)) return false;
        std::memcpy(out, data, sizeof(*out));
        return true;
    };
    // A kPong only shows the host's game is running, not that it still has us in its session.
    if (g_role == Role::Guest && from == g_host && h.type != kPong) g_host_heard = now();
    // A player we dropped for silence is still playing in our world as far as they know (a machine woken from
    // sleep): tell them to go home.
    if (g_role == Role::Host && h.type != kHello && is_lost(from)) {
        static std::map<uint64_t, double> told;
        if (now() - told[from] >= 2.0) {
            told[from] = now();
            Simple d{};
            fill_header(&d, kDropped);
            send_to(from, d);
            LOG_INFO("[coop] %s is talking again after being dropped: told them to go home", steam::persona(from).c_str());
        }
        return;
    }
    // The host takes a Hello, Bye or Ping from anyone in the lobby and the rest only from the players it let in:
    // Steam passes on packets from every lobby member, also ones refused or not answered yet.
    if (g_role == Role::Host && h.type != kHello && h.type != kBye && h.type != kPing && !admitted(from)) {
        static int lines = 0;
        // a joiner's status, gait and animations are sent before the answer comes
        if (h.type != kInfo && h.type != kGait && h.type != kAnim && ++lines <= 50) {
            LOG_INFO("[coop] Message %u from %s ignored: they were not let into the session", h.type,
                     steam::persona(from).c_str());
        }
        return;
    }
    switch (h.type) {
        case kHello: {
            Hello m{};
            if (size < kHelloV1Size || g_role != Role::Host || g_step != Step::Hosting) return;
            std::memcpy(&m, data, size < sizeof(m) ? size : sizeof(m));
            const PeerVersion them = hello_version(m, size >= sizeof(m));
            if (!same_version(them)) {
                Welcome w{};
                fill_header(&w, kWelcome);
                w.status = kVersion;
                send_to(from, w);
                show_hello_mismatch(from, m, them);
                return;
            }
            steam::accept(from);
            unmark_lost(from);  // asking again
            PeerInfo* p = peer(from);
            m.name[sizeof(m.name) - 1] = 0;
            p->name = m.name;
            p->soul_level = m.soul_level;
            p->invader = m.invade != 0;
            p->last_seen = now();
            g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                           [from](const Pending& q) { return q.from == from; }),
                            g_pending.end());
            g_pending.push_back(Pending{from, m.invade != 0, now()});
            break;
        }
        case kWelcome: {
            Welcome m;
            if (from == g_host && as(&m)) guest_on_welcome(m);
            break;
        }
        case kState: {
            StateMsg m;
            if (from != g_host || !as(&m)) return;
            g_host_map = m.map;
            if (m.bonfire >= 0) g_host_bonfire = m.bonfire;
            rules::state().invader_present = m.invader_present != 0;
            g_host_coop_players = m.coop_players ? m.coop_players : 1;
            apply_host_rules(m.world_rules, m.infection != 0, m.taunt != 0);
            break;
        }
        case kInfo: {
            Info m;
            if (!as(&m)) return;
            PeerInfo* p = peer(from);
            m.name[sizeof(m.name) - 1] = 0;
            p->name = m.name;
            p->soul_level = m.soul_level;
            p->deaths = m.deaths;
            p->map = m.map;
            std::memcpy(p->pos, m.pos, sizeof(p->pos));
            p->ping_ms = m.ping_ms;
            p->invader = m.invader != 0;
            const bool dead = m.dead != 0;
            if (!dead && (p->dead || p->alive_since == 0)) p->alive_since = now();
            p->dead = dead;
            p->light = m.light;
            m.char_name[16] = 0;
            p->char_name = m.char_name;
            p->ceremony = motion::synced_anim(m.ceremony) ? m.ceremony : 0;
            p->last_seen = now();
            break;
        }
        case kArrived: {
            PlayerEvent m;
            if (!as(&m)) return;
            if (g_role == Role::Host) {
                if (m.who != from) return;
                broadcast(m, true, from);  // tell the other guests
                if (!m.invader) send_world_flags(from);
            } else if (from != g_host) {
                return;
            }
            if (m.who == steam::self()) return;
            unmark_lost(m.who);
            say(fmt(m.invader ? kMsgInvaded : kMsgJoined, event_name(m)));
            break;
        }
        case kWorldFlags: {
            if (from != g_host || g_role != Role::Guest || g_invade || g_step != Step::InWorld) return;
            on_world_flags(data + sizeof(Header), size - static_cast<uint32_t>(sizeof(Header)));
            break;
        }
        case kRest: {
            if (g_role == Role::None) return;
            if (g_role == Role::Host) {
                // a guest rested: reset here and tell the other guests
                Simple r{};
                fill_header(&r, kRest);
                r.value = 0;
                broadcast(r, true, from);
            } else if (from != g_host) {
                return;
            }
            world::remote_rest();
            break;
        }
        case kFlags: {
            if (size < sizeof(Header) + 2) return;
            FlagsMsg m{};
            std::memcpy(&m, data, std::min<size_t>(size, sizeof(m)));
            const int n = std::min<int>(m.count, kFlagsPerMsg);
            const bool from_guest = g_role == Role::Host;
            if (!from_guest && from != g_host) return;
            static int flag_lines = 0;
            for (int i = 0; i < n; ++i) {
                if (++flag_lines <= 300) {
                    LOG_INFO("[coop] flag %d %s: from %s", m.f[i].id, m.f[i].on ? "on" : "off",
                             steam::persona(from).c_str());
                }
                rules::apply_remote_flag(m.f[i].id, m.f[i].on != 0);
            }
            if (from_guest) broadcast_raw(data, size, true, from);  // pass a guest's progress on
            break;
        }
        case kDead: {
            PlayerEvent m;
            if (!as(&m)) return;
            if (g_role == Role::Host) {
                if (m.who != from) return;
                broadcast(m, true, from);
            } else if (from != g_host) {
                return;
            }
            if (PeerInfo* p = peer(m.who)) {
                p->dead = true;
                ++p->death_events;  // their character here gets rebuilt (party::tick)
            }
            say(fmt(m.invader ? kMsgInvaderDied : kMsgDied, event_name(m)));
            // The host fell while we invade: the game's own invader victory (HostDead) runs here.
            if (m.who == g_host && g_invade && g_step == Step::InWorld) {
                LOG_INFO("[coop] The host died: invasion victory");
                say(kMsgBreakInSuccess);
                game::call_lua("HostDead");
            }
            break;
        }
        case kBonfireWarp: {
            BonfireMsg m;
            if (from != g_host || !as(&m) || g_step != Step::InWorld || g_invade) return;
            LOG_INFO("[coop] Following the host to bonfire %d", m.bonfire);
            rules::remote_bonfire_warp(m.bonfire);
            break;
        }
        case kHomeward: {
            BonfireMsg m;
            if (from != g_host || !as(&m) || g_step != Step::InWorld || g_invade || m.bonfire < 0) return;
            say(kMsgHostWarpOut);
            g_follow_bonfire = m.bonfire;
            g_follow_at = now() + 3.0;
            break;
        }
        case kBonfireLit: {
            BonfireMsg m;
            if (!as(&m)) return;
            if (g_role == Role::Host) {
                broadcast(m, true, from);
            } else if (from != g_host) {
                return;
            }
            rules::remote_bonfire_lit(m.bonfire);
            break;
        }
        case kCutFollow: {
            CutFollowMsg m;
            if (!as(&m) || peer(from)->invader) return;
            if (g_role == Role::Host) {
                broadcast(m, true, from);
            } else if (from != g_host) {
                return;
            }
            if (g_invade || (g_role == Role::Guest && g_step != Step::InWorld)) return;
            {
                const uint32_t here = static_cast<uint32_t>(game::map_id()) & 0xFFFF0000u;
                if (m.from != -1 && here != static_cast<uint32_t>(m.from)) {
                    LOG_INFO("[coop] %s arrived in map %08X by cutscene from map %08X; we are in %08X and stay",
                             steam::persona(from).c_str(), m.map, m.from, here);
                    return;
                }
            }
            LOG_INFO("[coop] %s arrived in map %08X by cutscene (point %d, bonfire %d): we follow",
                     steam::persona(from).c_str(), m.map, m.point, m.bonfire);
            g_cut_follow_map = m.map;
            g_cut_follow_point = m.point;
            g_cut_follow_bonfire = m.bonfire;
            g_cut_hint_map = m.map;
            g_cut_hint_bonfire = m.bonfire;
            // Our own copy of the cutscene may still bring us (the host's game ran the crow's warp on itself once,
            // a second behind the guest): give it a few seconds. The next time it only played the guest's flight
            // and never moved the host (2026-10-13 13:42), so it does not get the last word.
            g_cut_follow_at = now() + (g_cut_map == m.map ? 5.0 : 1.0);
            break;
        }
        case kBye: {
            if (from == g_host && g_role == Role::Guest) {
                guest_go_home(kMsgHostClosed, "the host closed the session");
            } else if (g_role == Role::Host) {
                steam::close(from);
                const PeerInfo* p = peer(from);
                PlayerEvent e = player_event(kLeft, from, p->invader, p->name);
                broadcast(e, true, from);
                say(fmt(p->invader ? kMsgInvaderLeft : kMsgLeft, text::widen(p->name)));
                g_peers.erase(std::remove_if(g_peers.begin(), g_peers.end(),
                                             [from](const PeerInfo& q) { return q.id == from; }),
                              g_peers.end());
                set_admitted(from, false);
            }
            break;
        }
        case kLeft: {
            PlayerEvent m;
            if (from != g_host || g_role != Role::Guest || !as(&m) || m.who == steam::self()) return;
            say(fmt(m.lost ? (m.invader ? kMsgInvaderLost : kMsgLost) : (m.invader ? kMsgInvaderLeft : kMsgLeft),
                    event_name(m)));
            const uint64_t who = m.who;
            g_peers.erase(std::remove_if(g_peers.begin(), g_peers.end(),
                                         [who](const PeerInfo& q) { return q.id == who; }),
                          g_peers.end());
            // Steam keeps a lost player in the lobby for a while: stop sending to them as the host does.
            if (m.lost && !is_lost(who)) g_lost.push_back(who);
            break;
        }
        case kGait: {
            GaitMsg m;
            if (!as(&m) || g_role == Role::None) return;
            PeerInfo* p = peer(from);
            p->gait = m.gait;
            p->gait_at = now();
            break;
        }
        case kAnim: {
            AnimMsg m;
            if (!as(&m) || g_role == Role::None || game::loading()) return;
            PeerInfo* p = peer(from);
            p->anim_at = now();
            if (m.kind == 0) {
                if (!motion::synced_anim(m.anim)) return;
                p->ceremony = m.loop ? m.anim : 0;
            } else {
                p->ceremony = 0;
            }
            const uint32_t chr = chr_of_peer(from);
            static int lines = 0;
            if (++lines <= 200) {
                LOG_INFO("[coop] %s: bonfire animation %s %d%s%s", p->name.c_str(), m.kind == 0 ? "start" : "stop",
                         m.kind == 0 ? m.anim : 0, m.kind == 0 && m.loop ? " (loop)" : "",
                         chr ? "" : " (their character is not here)");
            }
            if (chr) motion::play_remote_anim(chr, motion::Anim{m.kind, m.anim, m.loop});
            break;
        }
        case kPing: {
            Simple m;
            // Only for players in the session: answering everyone kept a player woken from sleep in a world that had
            // dropped them (2026-10-14).
            if (!as(&m) || (g_role == Role::Host && !admitted(from))) return;
            m.h.type = kPong;
            send_to(from, m, false);
            break;
        }
        case kDropped: {
            if (from != g_host || g_role != Role::Guest || (g_step != Step::InWorld && g_step != Step::Loading)) return;
            guest_go_home(kMsgHostLost, "the host dropped us after our connection went quiet");
            break;
        }
        case kPong: {
            Simple m;
            if (!as(&m) || from != g_host || g_ping_qpc == 0) return;
            // GetTickCount jumps by about 15 ms, so a fast reply was stored as 0 and both nameplates
            // showed 0ms until the next sample. The counter here is the guest's own round trip.
            LARGE_INTEGER now_q{}, freq{};
            QueryPerformanceCounter(&now_q);
            QueryPerformanceFrequency(&freq);
            const int64_t delta = now_q.QuadPart - g_ping_qpc;
            g_ping_qpc = 0;
            if (delta <= 0 || freq.QuadPart <= 0) return;
            const int64_t ms = (delta * 1000) / freq.QuadPart;
            if (ms >= 10000) return;
            g_ping_ms = ms < 1 ? 1 : static_cast<int>(ms);
            break;
        }
        default:
            break;
    }
}

void send_rest() {
    if (g_role == Role::None || !g_lobby) return;
    Simple r{};
    fill_header(&r, kRest);
    r.value = static_cast<uint32_t>(game::last_bonfire());
    if (g_role == Role::Host) {
        broadcast(r);
    } else if (g_host && g_step == Step::InWorld && !g_invade) {
        send_to(g_host, r);
    }
}

void send_death() {
    if (g_role == Role::None || !g_lobby) return;
    if (g_invade && g_step == Step::InWorld) say(kMsgBreakInFailed);
    // The death debuff (Remastered Seamless Co-op): a Crimson Blossom per death in a session, up to five.
    if (g_cfg.death_debuffs && !g_invade && items::count(items::kBlossom) < 5) items::give(items::kBlossom, 1);
    PlayerEvent e = player_event(kDead, steam::self(), g_invade, steam::persona(steam::self()));
    if (g_role == Role::Host) {
        broadcast(e);
    } else if (g_host) {
        send_to(g_host, e);
    }
}

void send_bonfire_warp(int bonfire) {
    if (g_role != Role::Host) return;
    BonfireMsg m{};
    fill_header(&m, kBonfireWarp);
    m.bonfire = bonfire;
    broadcast(m);
}

void send_bonfire_lit(int bonfire) {
    BonfireMsg m{};
    fill_header(&m, kBonfireLit);
    m.bonfire = bonfire;
    if (g_role == Role::Host) {
        broadcast(m);
    } else if (g_host && g_step == Step::InWorld && !g_invade) {
        send_to(g_host, m);
    }
}

void queue_flag(int id, bool on) {
    if (g_role == Role::None || !g_lobby) return;
    if (g_role == Role::Guest && (g_step != Step::InWorld || g_invade)) return;
    if (g_flag_queue.size() < 4096) g_flag_queue.push_back({id, on});
    if (g_world_wait > 0 && g_world_ours.size() < 4096) g_world_ours.push_back({id, on});
    static int lines = 0;
    if (++lines <= 300) LOG_INFO("[coop] flag %d %s: sent", id, on ? "on" : "off");
}

void flush_flags() {
    while (!g_flag_queue.empty()) {
        FlagsMsg m{};
        fill_header(&m, kFlags);
        const size_t n = std::min<size_t>(g_flag_queue.size(), kFlagsPerMsg);
        m.count = static_cast<uint16_t>(n);
        for (size_t i = 0; i < n; ++i) {
            m.f[i].id = g_flag_queue[i].first;
            m.f[i].on = g_flag_queue[i].second ? 1 : 0;
        }
        g_flag_queue.erase(g_flag_queue.begin(), g_flag_queue.begin() + n);
        const uint32_t bytes = static_cast<uint32_t>(sizeof(Header) + 2 + n * 5);
        if (g_role == Role::Host) {
            const uint64_t me = steam::self();
            for (uint64_t member : steam::lobby_members(g_lobby)) {
                if (member != me && !peer_is_invader(member) && !is_lost(member)) steam::send(member, &m, bytes, true);
            }
        } else if (g_host) {
            steam::send(g_host, &m, bytes, true);
        }
    }
}

// ---------------------------------------------------------------- characters of the other players
const PeerInfo* peer_by_char_name(const std::wstring& name) {
    if (name.empty()) return nullptr;
    for (const auto& p : g_peers) {
        if (p.char_name == name && now() - p.last_seen <= 10.0) return &p;
    }
    return nullptr;
}

const PeerInfo* peer_by_chr(uint32_t chr) {
    auto it = g_chr_peer.find(chr);
    if (it == g_chr_peer.end()) return nullptr;
    for (const auto& p : g_peers) {
        if (p.id == it->second) return &p;
    }
    return nullptr;
}

party::PeerState peer_state(uint32_t chr, const std::wstring& name) {
    party::PeerState s;
    const PeerInfo* p = chr ? peer_by_chr(chr) : nullptr;
    if (!p) p = peer_by_char_name(name);
    if (!p) return s;
    s.known = true;
    s.id = p->id;
    s.invader = p->invader;
    s.alive = !p->dead && p->alive_since > 0 && now() - p->alive_since >= 2.0 && now() - p->last_seen <= 5.0;
    s.deaths = p->deaths;
    s.death_events = p->death_events;
    s.map = p->map;
    return s;
}

// Which character in our world belongs to which player, for the host's world rules (teams).
void update_remote_players() {
    static double last = 0;
    if (now() - last < 0.5) return;
    last = now();
    std::vector<rules::RemotePlayer> out;
    if (g_role == Role::None || game::loading()) {
        rules::set_remote_players(out);
        return;
    }
    // Every other player's character in our world (WorldChrMan's player slots), then who it belongs to: by
    // character name (session member data), else by position (each player reports theirs every second), else by
    // elimination (one character and one player left).
    struct Match {
        uint32_t chr;
        const PeerInfo* peer;
        const char* how;
    };
    std::vector<Match> chrs;
    for (uint32_t chr : game::remote_player_chrs()) chrs.push_back({chr, nullptr, "none"});
    const std::vector<party::Remote> members = party::remotes();
    // A match made earlier stays (the position match is only good to a few metres and flip-flopped).
    for (auto& m : chrs) {
        if (const PeerInfo* p = peer_by_chr(m.chr)) {
            m.peer = p;
            m.how = "kept";
        }
    }
    for (auto& m : chrs) {
        if (m.peer) continue;
        for (const auto& r : members) {
            if (r.chr != m.chr) continue;
            if (const PeerInfo* p = peer_by_char_name(r.name)) {
                m.peer = p;
                m.how = "name";
            }
        }
    }
    auto taken = [&](const PeerInfo* p) {
        for (const auto& m : chrs) {
            if (m.peer == p) return true;
        }
        return false;
    };
    for (auto& m : chrs) {
        if (m.peer) continue;
        game::Vec3 pos{};
        if (!game::chr_position(m.chr, &pos)) continue;
        const PeerInfo* best = nullptr;
        float best_d = 12.0f * 12.0f;  // a second's run, and some
        for (const auto& p : g_peers) {
            if (now() - p.last_seen > 5.0 || taken(&p)) continue;
            const float dx = pos.x - p.pos[0], dy = pos.y - p.pos[1], dz = pos.z - p.pos[2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < best_d) {
                best_d = d;
                best = &p;
            }
        }
        if (best) {
            m.peer = best;
            m.how = "position";
        }
    }
    {
        int open_chrs = 0, open_peers = 0;
        Match* last_open = nullptr;
        const PeerInfo* last_peer = nullptr;
        for (auto& m : chrs) {
            if (!m.peer) {
                ++open_chrs;
                last_open = &m;
            }
        }
        for (const auto& p : g_peers) {
            if (now() - p.last_seen <= 10.0 && !taken(&p)) {
                ++open_peers;
                last_peer = &p;
            }
        }
        if (open_chrs == 1 && open_peers == 1) {
            last_open->peer = last_peer;
            last_open->how = "elimination";
        }
    }
    std::string report, key;
    std::map<uint32_t, uint64_t> matched;
    for (const auto& m : chrs) {
        rules::RemotePlayer rp;
        rp.chr = m.chr;
        if (m.peer) {
            rp.known = true;
            rp.invader = m.peer->invader;
            rp.host = m.peer->id == g_host && g_role == Role::Guest;
            rp.map = m.peer->map;
            matched[m.chr] = m.peer->id;
        }
        out.push_back(rp);
        char line[160];
        std::snprintf(line, sizeof(line), "%s%08X=%s(%s%s)", report.empty() ? "" : ", ", m.chr,
                      m.peer ? m.peer->name.c_str() : "?", m.how, rp.host ? ", host" : rp.invader ? ", invader" : "");
        report += line;
        std::snprintf(line, sizeof(line), "%08X=%s;", m.chr, m.peer ? m.peer->name.c_str() : "?");
        key += line;
    }
    g_chr_peer = matched;
    static std::string last_key;
    static int reports = 0;
    if (key != last_key && reports < 100) {
        ++reports;
        last_key = key;
        LOG_INFO("[coop] Other players' characters here: %s (%zu session member record(s))",
                 report.empty() ? "none" : report.c_str(), members.size());
        // Why the name match fails (it never matched in the 2026-10-07 test): the names on both sides.
        std::string names;
        for (const auto& r : members) {
            char n8[64] = "";
            WideCharToMultiByte(CP_UTF8, 0, r.name.c_str(), -1, n8, sizeof(n8), nullptr, nullptr);
            names += std::string(" member[") + std::to_string(r.slot) + "]='" + n8 + "'";
        }
        for (const auto& p : g_peers) {
            char n8[64] = "";
            WideCharToMultiByte(CP_UTF8, 0, p.char_name.c_str(), -1, n8, sizeof(n8), nullptr, nullptr);
            names += " " + p.name + "='" + n8 + "'";
        }
        LOG_INFO("[coop] Character names:%s", names.c_str());
    }
    rules::set_remote_players(out);
}

// Diagnostic for animation sync (bonfire rests not seen by the others): our animations and whether an override
// manipulator drives us (the network recorder belongs to our own PadManipulator), and for each other player's
// character the animations its NetworkManipulator last received (+0x1EC/+0x1F0) and what it plays. Logged when
// something changes, a limited number of lines per game.
void anim_sync_log() {
    static double last = 0;
    static std::string last_line;
    static int lines = 0;
    if (lines >= 1500 || now() - last < 0.2 || game::loading()) return;
    last = now();
    const uint32_t me = game::player();
    if (!me) return;
    auto id = [](uint32_t packed) { return packed == 0xFFFFFFFF ? -1 : static_cast<int>(packed & 0xFFFF); };
    char buf[512];
    int n = std::snprintf(buf, sizeof(buf), "me %d/%d%s", id(game::anim_state(me, 0)), id(game::anim_state(me, 1)),
                          game::override_manipulator(me) ? " (override)" : "");
    for (uint32_t chr : game::remote_player_chrs()) {
        const uint32_t m = game::manipulator(chr);
        uint32_t r0 = 0xFFFFFFFF, r1 = 0xFFFFFFFF;
        if (m) {
            read32(m + 0x1EC, &r0);
            read32(m + 0x1F0, &r1);
        }
        n += std::snprintf(buf + n, sizeof(buf) - n, " | %08X got %d/%d plays %d/%d%s", chr, id(r0), id(r1),
                           id(game::anim_state(chr, 0)), id(game::anim_state(chr, 1)),
                           game::override_manipulator(chr) ? " (override)" : "");
        if (n >= static_cast<int>(sizeof(buf)) - 64) break;
    }
    if (last_line == buf) return;
    last_line = buf;
    ++lines;
    LOG_INFO("[coop] anim %s", buf);
}

// A cutscene that moves us into another map (the Undead Asylum's crow, the Painted World): once we are there the
// other players are told and warped after us. They go to our respawn bonfire when the event put one in that map
// (the crow sets Firelink Shrine's), else to the cutscene's own point.
void on_cutscene(int map, int point) {
    if (g_role == Role::None || g_invade) return;
    g_cut_map = map;
    g_cut_point = point;
    g_cut_from = static_cast<int>(static_cast<uint32_t>(game::map_id()) & 0xFFFF0000u);
    g_cut_at = now();
    g_cut_arrived_at = 0;
}

// Warps our player to `map` by its bonfire `bonfire` (a respawn point there). Never by the cutscene's point: a warp
// point is the map's player start part (MSB Player, 0xDE2690), and a region of that name is not one, so the game
// put the host at its first block's default start, outside the loaded map, and the host fell through the world
// (2026-10-13 15:45 test). Returns false when the bonfire is not in that map.
bool cutscene_warp(int map, int bonfire, const char* why) {
    if (bonfire < 0 || game::bonfire_map(bonfire) != map) return false;
    LOG_INFO("[coop] %s: to bonfire %d (map %08X)", why, bonfire, map);
    if (g_role == Role::Guest) note_guest_respawn(bonfire);
    *ptr<int>(game::game_man() + game::kGmLastBonfire) = bonfire;
    world::warp_now(map, -1);
    return true;
}

// A bonfire of `map` to arrive at: our own respawn when the event put it there (the crow sets Firelink's), else
// the one the other player reported on arriving (kCutFollow), else -1.
int cutscene_bonfire(int map) {
    const int own = game::last_bonfire();
    if (game::bonfire_map(own) == map) return own;
    if (g_cut_hint_map == map && game::bonfire_map(g_cut_hint_bonfire) == map) return g_cut_hint_bonfire;
    return -1;
}

void cutscene_tick() {
    if (g_role == Role::None || g_invade) {
        g_cut_map = -1;
        g_cut_own_map = -1;
        g_cut_follow_map = -1;
        int map = 0, point = 0;
        rules::take_failed_cutscene(&map, &point);
        return;
    }
    const bool settled = game::in_game() && !game::loading();
    const uint32_t here = static_cast<uint32_t>(game::map_id()) & 0xFFFF0000u;
    const int world_map = rules::world_map();
    // In `map` already: by our map id, or by the world's, which a cutscene's in-place move sets first.
    auto in_map = [&](int map) { return here == static_cast<uint32_t>(map) || (world_map != -1 && world_map == map); };
    {
        int map = -1, point = -1;
        if (rules::take_failed_cutscene(&map, &point)) {
            g_cut_own_map = map;
            g_cut_own_point = point;
            g_cut_own_at = now();
            if (g_cut_map == map) g_cut_map = -1;  // it will not bring us: nothing to announce
        }
    }
    const uint32_t me = game::player();
    uint32_t hp = 0;
    const bool alive = me && read32(me + 0x2D4, &hp) && static_cast<int>(hp) > 0;
    const bool in_cutscene = rules::cutscene_loading() || rules::cutscene_playing();
    if (g_cut_own_map != -1 && settled) {
        // Our copy of the cutscene could not take us. Once it is over the event goes on and usually sets our respawn
        // in the new map (the crow: Firelink's bonfire); give it 3 s, then go there, or to the bonfire the other
        // player reports on arriving. Without either we stay: a warp without a bonfire lands outside the map.
        if (in_map(g_cut_own_map)) {
            g_cut_own_map = -1;
        } else if (alive && !in_cutscene && now() - g_cut_own_at >= 3.0) {
            const int map = g_cut_own_map;
            const int bonfire = cutscene_bonfire(map);
            if (bonfire >= 0) {
                g_cut_own_map = -1;
                if (g_cut_follow_map == map) g_cut_follow_map = -1;
                cutscene_warp(map, bonfire, "Our cutscene could not take us, we go ourselves");
                return;
            }
            if (now() - g_cut_own_at > 120.0) {
                LOG_INFO("[coop] No bonfire of map %08X is known here: we stay where we are", map);
                g_cut_own_map = -1;
            }
        }
    }
    if (g_cut_map != -1) {
        if (now() - g_cut_at > 120.0) {
            LOG_INFO("[coop] The cutscene into map %08X never brought us there; nobody follows", g_cut_map);
            g_cut_map = -1;
        } else if (!settled || here != static_cast<uint32_t>(g_cut_map)) {
            g_cut_arrived_at = 0;
        } else if (g_cut_arrived_at == 0) {
            g_cut_arrived_at = now();
        } else if (now() - g_cut_arrived_at >= 1.5) {
            CutFollowMsg m{};
            fill_header(&m, kCutFollow);
            m.map = g_cut_map;
            m.point = g_cut_point;
            m.bonfire = game::last_bonfire();
            m.from = g_cut_from;
            broadcast(m);
            LOG_INFO("[coop] Arrived in map %08X by cutscene: the others follow (point %d, bonfire %d)", m.map,
                     m.point, m.bonfire);
            g_cut_map = -1;
        }
    }
    if (g_cut_follow_map == -1 || now() < g_cut_follow_at || !settled) return;
    if (!alive) {
        if (now() - g_cut_follow_at > 60.0) g_cut_follow_map = -1;
        return;
    }
    const int map = g_cut_follow_map;
    if (in_map(map)) {
        LOG_INFO("[coop] Already in map %08X: no need to follow", map);
        g_cut_follow_map = -1;
        return;
    }
    if (g_cut_own_map == map) {
        LOG_INFO("[coop] We go to map %08X ourselves once our cutscene is over", map);
        g_cut_follow_map = -1;
        return;
    }
    if (in_cutscene) {
        // Our own copy is loading the map or playing (the crow's flight, then its arrival): it takes us itself.
        if (now() - g_cut_follow_at < 120.0) return;
        LOG_INFO("[coop] Our cutscene into map %08X is still not over after 2 minutes: following anyway", map);
    }
    g_cut_follow_map = -1;
    if (g_cut_map == map) {
        LOG_INFO("[coop] Our own cutscene into map %08X did not bring us there: following instead", map);
        g_cut_map = -1;
    }
    const int bonfire = game::bonfire_map(g_cut_follow_bonfire) == map ? g_cut_follow_bonfire : cutscene_bonfire(map);
    if (!cutscene_warp(map, bonfire, "Following the cutscene")) {
        LOG_INFO("[coop] No bonfire of map %08X is known here (the other player's is %d): we stay where we are", map,
                 g_cut_follow_bonfire);
    }
}

void set_callbacks() {
    if (g_callbacks_set) return;
    g_callbacks_set = true;
    world::set_rest_handler(&send_rest);
    world::set_death_handler(&send_death);
    rules::set_flag_listener(&queue_flag);
    rules::set_bonfire_warp_listener(&send_bonfire_warp);
    rules::set_bonfire_lit_listener(&send_bonfire_lit);
    rules::set_cutscene_listener(&on_cutscene);
    rules::set_separation_listener([](bool host) { g_separation = host ? 2 : 1; });
    steam::on_lobby_member([](uint64_t lobby, uint64_t user, bool entered) {
        if (lobby != g_lobby) return;
        if (entered) {
            steam::accept(user);
            if (g_role == Role::Host) LOG_INFO("[coop] %s entered the lobby", steam::persona(user).c_str());
            return;
        }
        if (g_role == Role::Guest && user == g_host) {
            guest_go_home(kMsgReturnHome, "the host left the lobby");
        } else if (g_role == Role::Host) {
            const bool member = admitted(user);
            for (const auto& p : g_peers) {
                if (p.id != user) continue;
                say(fmt(p.invader ? kMsgInvaderLeft : kMsgLeft, text::widen(p.name)));
                if (member) {
                    // The guests see it too (an invader sent home by the game leaves without a Bye).
                    PlayerEvent e = player_event(kLeft, user, p.invader, p.name);
                    broadcast(e, true, user);
                }
            }
            g_peers.erase(std::remove_if(g_peers.begin(), g_peers.end(),
                                         [user](const PeerInfo& q) { return q.id == user; }),
                          g_peers.end());
            set_admitted(user, false);
            g_p2p_failed.erase(user);
            // A dropped player stays marked: woken from sleep, they still talk to us as if in the session.
            if (is_lost(user)) {
                steam::accept(user);
            } else {
                steam::close(user);
            }
        }
    });
    steam::on_p2p_failed([](uint64_t peer_id, int error) {
        LOG_INFO("[coop] P2P with %016llx failed (%d)", static_cast<unsigned long long>(peer_id), error);
        steam::close(peer_id);
        g_p2p_failed[peer_id] = now();
        // A player in the session stays until drop_quiet_players finds them quiet: the link often comes back.
        if (g_role != Role::Host || admitted(peer_id)) return;
        const auto keep = std::remove_if(g_peers.begin(), g_peers.end(),
                                         [peer_id](const PeerInfo& q) { return q.id == peer_id; });
        if (keep != g_peers.end()) g_peers.erase(keep, g_peers.end());
    });
}

}  // namespace

std::string game_tag(const std::string& password) { return "g" + password_key(password); }

void configure(const Config& cfg) {
    g_cfg = cfg;
    g_cfg.max_players = std::max(2, std::min(4, g_cfg.max_players));
    g_key = password_key(g_cfg.password);
    if (cfg.test_protocol > 0) g_protocol = static_cast<uint32_t>(cfg.test_protocol);
    if (!cfg.test_version.empty()) g_version = cfg.test_version;
    if (g_protocol != kProtocol || g_version != PREPARE_TO_COOP_VERSION) {
        LOG_INFO("[coop] TEST: speaking protocol %u as Prepare to Co-op %s (TestProtocol / TestVersion)", g_protocol,
                 g_version.c_str());
    }
}

void tick() {
    flash_tick();
    // The first time a character stands at home (after the game starts or another character is loaded), a curse
    // laid on another character is dropped, and disasters still on in the saved flags (the game was closed while the
    // world was cursed) end, as the phantoms would otherwise roam for up to 600 s more in each such map.
    if (game::in_game() && !game::loading() && game::own_world() && g_role != Role::Guest) {
        const std::wstring name = party::local_name();
        if (!name.empty() && name != g_curse_owner) {
            if (g_own_curse) LOG_INFO("[coop] Another character: the curse on the world is dropped");
            g_own_curse = false;
            g_curse_owner = name;
            if (rules::disasters_active() && rules::end_disasters(false) > 0) say(kMsgCurseFaded);
        }
    }
    apply_curse();
    {
        static bool f12_down = false;
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        probe_state(down && !f12_down);
        f12_down = down;
        if (g_role == Role::None && g_settle_until > 0 && now() < g_settle_until) {
            static double last_settle = 0;
            if (now() - last_settle >= 1.0) {
                last_settle = now();
                settle_after_session();
            }
        }
    }
    if (!steam::ready()) return;
    {
        static double last_tick = 0;
        static ULONGLONG last_wall = 0;
        const double t = now();
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        const ULONGLONG wall = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        double away = last_tick > 0 ? t - last_tick : 0;
        if (last_wall && wall > last_wall) away = std::max(away, static_cast<double>(wall - last_wall) / 1e7);
        if (away > 3.0 && g_role != Role::None) {
            g_quiet_grace_until = t + kLostQuiet + 5.0;
            static int lines = 0;
            if (++lines <= 20) {
                LOG_INFO("[coop] This game did not run for %.0f s; no one is dropped for silence for %.0f s", away,
                         kLostQuiet + 5.0);
            }
            // Longer than the host waits for a quiet player: it has dropped us already.
            if (g_role == Role::Guest && g_step == Step::InWorld && away >= kLostSilent + 5.0) {
                char why[96];
                std::snprintf(why, sizeof(why), "this game did not run for %.0f s (asleep)", away);
                guest_go_home(kMsgHostLost, why);
            }
        }
        last_tick = t;
        last_wall = wall;
    }
    set_callbacks();
    steam::poll(on_message);
    if (g_separation) {
        const int what = g_separation;
        g_separation = 0;
        if (what == 2 && g_role == Role::Host) {
            close_world();
        } else if (what == 1 && g_role == Role::Guest) {
            leave();
        }
    }
    if (g_role == Role::Host) host_tick();
    if (g_role == Role::Guest) guest_tick();
    text::set_seamless_active(g_role != Role::None);
    text::tick();
    // Characters of the other players: rebuilt after loads and after a death (co-op only; invaders keep the
    // game's own rules).
    party::tick(!g_invade && (g_step == Step::Hosting || g_step == Step::InWorld), &peer_state);
    if (g_step == Step::Hosting || g_step == Step::InWorld) cutscene_tick();
    update_remote_players();
    {
        std::vector<rules::PeerSpot> spots;
        if (g_step == Step::Hosting || g_step == Step::InWorld) {
            for (const auto& p : g_peers) {
                if (p.dead || p.map == -1 || now() - p.last_seen > 3.0) continue;
                rules::PeerSpot s;
                std::memcpy(s.pos, p.pos, sizeof(s.pos));
                s.map = p.map;
                spots.push_back(s);
            }
        }
        rules::set_peer_spots(spots);
    }
    if (g_role != Role::None && g_anim_log) anim_sync_log();
    // Motion sync: our gait and event animations out; the others' gaits onto their characters here.
    {
        const int gait = motion::local_gait();
        const std::vector<motion::Anim> anims = motion::take_local_anims();
        if (g_role != Role::None && g_lobby && !g_invade) {
            if (gait != g_sent_gait || now() - g_last_gait > 0.5) {
                g_sent_gait = gait;
                g_last_gait = now();
                GaitMsg m{};
                fill_header(&m, kGait);
                m.gait = static_cast<int8_t>(gait);
                broadcast(m, false);
            }
            for (const motion::Anim& a : anims) {
                AnimMsg m{};
                fill_header(&m, kAnim);
                m.kind = a.kind;
                m.anim = a.anim;
                m.loop = a.loop;
                broadcast(m);
                static int lines = 0;
                if (++lines <= 200) {
                    LOG_INFO("[coop] Our bonfire animation %s %d%s: told the others", a.kind == 0 ? "start" : "stop",
                             a.anim, a.loop ? " (loop)" : "");
                }
            }
        }
        motion::clear_remote_gaits();
        // A map load frees every character: the matches made before it point at freed memory.
        if (game::loading()) g_chr_peer.clear();
        if (g_role != Role::None && !game::loading()) {
            const std::vector<uint32_t> live = game::remote_player_chrs();
            for (const auto& [chr, id] : g_chr_peer) {
                if (std::find(live.begin(), live.end(), chr) == live.end()) continue;
                for (const auto& p : g_peers) {
                    if (p.id != id || p.invader) continue;
                    const bool fresh = p.gait >= 0 && now() - p.gait_at < 1.5;
                    motion::set_remote_gait(chr, fresh ? static_cast<motion::Gait>(p.gait) : motion::kUnknown);
                }
            }
        }
    }
    reconcile_ceremonies();
    rules::tick();
    {
        const auto& st = rules::state();
        world::set_session(st.active, st.invader, st.invader_present);
        int coop = 1;
        bool all_dead = true;
        int others = 0;
        if (g_role == Role::Host || (g_role == Role::Guest && g_step == Step::InWorld)) coop = g_host_coop_players;
        for (const auto& p : g_peers) {
            if (p.invader || now() - p.last_seen > 10.0) continue;
            ++others;
            all_dead &= p.dead;
        }
        world::set_players(coop);
        world::set_everyone_dead(others > 0 && all_dead);
        static double last_light = 0;
        if (g_role != Role::None && now() - last_light > 0.5 && !game::loading()) {
            last_light = now();
            std::vector<world::RemoteLight> lights;
            for (const auto& [chr, id] : g_chr_peer) {
                for (const auto& p : g_peers) {
                    if (p.id != id || now() - p.last_seen > 5.0 || p.light <= 0) continue;
                    lights.push_back({chr, p.light});
                }
            }
            world::share_lights(lights);
        }
    }
    if (!g_flag_queue.empty()) flush_flags();
    if (g_role != Role::None && g_lobby) {
        if (now() - g_last_info > 1.0) {
            g_last_info = now();
            send_info();
        }
        if (g_role == Role::Guest && g_host && now() - g_last_ping > 2.0) {
            g_last_ping = now();
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            g_ping_qpc = qpc.QuadPart;
            Simple p{};
            fill_header(&p, kPing);
            p.value = GetTickCount();
            send_to(g_host, p, false);
        }
    }
}

void host() {
    if (g_role != Role::None) {
        LOG_INFO("[coop] Host request ignored (already %s)", g_role == Role::Host ? "hosting" : "joining");
        return;
    }
    if (!game::in_game() || !game::own_world()) {
        LOG_INFO("[coop] Host request ignored (not in our own world)");
        return;
    }
    g_role = Role::Host;
    host_open_lobby();
}

void join() {
    if (g_role != Role::None) {
        LOG_INFO("[coop] Join request ignored (already in a session)");
        return;
    }
    if (!game::in_game() || !game::own_world()) {
        LOG_INFO("[coop] Join request ignored (not in our own world)");
        return;
    }
    g_role = Role::Guest;
    g_invade = false;
    LOG_INFO("[coop] Searching for a session");
    guest_search();
}

void invade() {
    if (g_role != Role::None) {
        LOG_INFO("[coop] Invade request ignored (already in a session)");
        return;
    }
    if (!game::in_game() || !game::own_world()) {
        LOG_INFO("[coop] Invade request ignored (not in our own world)");
        return;
    }
    g_role = Role::Guest;
    g_invade = true;
    LOG_INFO("[coop] Searching for a world to invade");
    guest_search();
}

void leave() {
    if (g_role == Role::None) {
        LOG_INFO("[coop] Leave request ignored (not in a session)");
        return;
    }
    if (g_role == Role::Host && g_step == Step::Closing) {
        LOG_INFO("[coop] Leave request ignored (the session is already closing)");
        return;
    }
    Simple bye{};
    fill_header(&bye, kBye);
    broadcast(bye);
    if (g_role == Role::Host) {
        if (g_step != Step::Hosting) {
            // The lobby was still being made: nothing to end in the game.
            finish(L"");
            return;
        }
        // See host_close_tick. The guests go first; the host's own game session ends when it is alone.
        // Earlier builds ended the game session right here, with the guests still in it.
        if (g_lobby) steam::set_lobby_joinable(g_lobby, false);
        g_pending.clear();
        g_close_at = now();
        g_close_left = false;
        g_close_forced = false;
        rules::state().closing = true;
        set_step(Step::Closing);
        LOG_INFO("[coop] Disbanding: %zu guest(s) told to leave (game session %d, %d member(s))", g_peers.size(),
                 game::session_state(), game::member_count());
        return;
    }
    if (g_step == Step::InWorld || g_step == Step::Loading) {
        guest_go_home(kMsgReturnHome, "we left");
    } else {
        rules::state().leaving = true;
        if (game::session_state() != 0) game::leave_session();
        finish(L"");
    }
}

void close_world() {
    if (g_role != Role::Host) return;
    LOG_INFO("[coop] The host closes the world (%zu other player(s))", g_peers.size());
    say(kMsgWorldClosed);
    leave();
}

void cycle_rules() {
    if (!hosting()) return;
    auto& st = rules::state();
    st.world_rules = (st.world_rules + 1) % 3;
    LOG_INFO("[coop] World rules now %d (0 none, 1 allies hostile, 2 teams)", st.world_rules);
    say(rules_text(st.world_rules));
    g_last_state = 0;  // tell the guests now
}

void taunt() {
    if (!hosting()) return;
    rules::state().taunt = true;
    steam::set_lobby_data(g_lobby, "ptdesc_taunt", "1");
    LOG_INFO("[coop] Invaders are taunted into this world");
    say(kMsgTaunt);
    g_last_state = 0;
}

// The Cursed Pendant curses our own world, offline or hosting, and lifts the curse when used again. It cannot be
// used in another player's world, nor while joining one (coop.cpp can_use).
void curse_world() {
    if (!game::own_world() || g_role == Role::Guest) return;
    g_own_curse = !g_own_curse;
    g_curse_owner = party::local_name();
    apply_curse();
    if (g_own_curse) {
        LOG_INFO("[coop] The world is cursed (pure black World Tendency, %s)", hosting() ? "hosting" : "no session");
        say(kMsgCursed);
    } else {
        LOG_INFO("[coop] The curse on the world is lifted");
        say(kMsgCurseLifted);
        rules::end_disasters(false);
    }
    g_last_state = 0;
}

void host_homeward() {
    if (!hosting()) return;
    g_homeward_at = now();
}

bool in_session() {
    if (g_role == Role::Host) return g_step == Step::Hosting || g_step == Step::Closing;
    if (g_role == Role::Guest) {
        return g_step == Step::Hello || g_step == Step::Joining || g_step == Step::Loading || g_step == Step::InWorld;
    }
    return false;
}

bool hosting() { return g_role == Role::Host && g_step == Step::Hosting && g_lobby != 0; }
bool guest() { return g_role == Role::Guest; }
bool active() { return g_role != Role::None; }
bool idle() { return g_role == Role::None; }
bool invader() { return g_role == Role::Guest && g_invade; }
double homeward_time() { return g_homeward_at; }
void clear_homeward_time() { g_homeward_at = 0; }
const std::vector<PeerInfo>& peers() { return g_peers; }

void remember_rest_point() {
    if (!game::in_game() || !game::own_world()) return;
    RestPoint r;
    if (!game::position(&r.pos, &r.angle)) return;
    r.map = game::map_id();
    r.bonfire = game::last_bonfire();
    r.valid = r.map != -1;
    g_rest = r;
}

void note_guest_respawn(int bonfire) {
    if (g_role != Role::Guest || g_invade || bonfire < 0) return;
    g_guest_bonfire = bonfire;
    if (const uint32_t gm = game::game_man()) *ptr<int>(gm + game::kGmLastBonfire) = bonfire;
}

void note_guest_rest(int object_id) {
    if (g_role != Role::Guest || g_invade) return;
    if (object_id <= 100000 || object_id >= 30000000) {
        LOG_INFO("[coop] Rest object %d is not a bonfire; respawn unchanged", object_id);
        return;
    }
    note_guest_respawn(object_id + 1000);
    LOG_INFO("[coop] Guest respawn bonfire is now %d (rested at object %d)", object_id + 1000, object_id);
}

int guest_respawn_bonfire() { return (g_role == Role::Guest && !g_invade) ? g_guest_bonfire : -1; }

// FrpgMenuDlgFloatingPCGauge::update (0xFA8C90, vtable 0x011CAFEC) reads the companion's name from the
// menu slot and sets it on both name widgets (0xFA8BAB and 0xFA8BDF call 0x789050). That helper is also
// how every other menu sets text, so only these two calls are retargeted. The name itself is 16 wide
// characters; the longer line lives in our buffers.
constexpr uint32_t kNameSet = 0x00789050;
constexpr uint32_t kFloatingNameA = 0x00FA8BAB;
constexpr uint32_t kFloatingNameB = 0x00FA8BDF;
void* g_name_set = nullptr;
// Three buffers per companion. One update sets two widgets, and a setter that keeps the pointer must see a
// different buffer on the next update or the soul level and ping stay at whatever the first frame copied.
wchar_t g_nameplate[4][3][96] = {};
int g_nameplate_n[4] = {};

bool read_shown_name(const wchar_t* at, wchar_t* out, int cap) {
    if (!at || cap < 2) return false;
    const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(at));
    for (int i = 0; i < cap - 1; ++i) {
        uint32_t word = 0;
        if (!read32(base + static_cast<uint32_t>(i) * 2, &word)) return false;
        out[i] = static_cast<wchar_t>(word & 0xFFFF);
        if (!out[i]) return i > 0;
    }
    out[cap - 1] = 0;
    return true;
}

bool utf8_is(const wchar_t* shown, const std::string& utf8) {
    if (utf8.empty() || utf8.size() > 48) return false;
    wchar_t buf[49];
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, buf, 49);
    return n > 1 && wcscmp(shown, buf) == 0;
}

// The bar's current name, or null when this string is not a companion we know.
const wchar_t* __stdcall nameplate_for(const wchar_t* shown) {
    if (g_role == Role::None) return nullptr;
    wchar_t name[24] = {};
    if (!read_shown_name(shown, name, 24) || wcslen(name) > 16) return nullptr;
    int slot = 0;
    for (const auto& p : g_peers) {
        if (slot >= 4) break;
        if (now() - p.last_seen > 10.0) continue;
        const int mine = slot++;
        if (!utf8_is(name, p.name) && (p.char_name.empty() || wcscmp(name, p.char_name.c_str()) != 0)) continue;
        const int mode = g_cfg.overhead_display;
        if (mode == 0) return nullptr;  // the game's own name
        int ping = p.ping_ms;
        // The host does not measure a round trip. A guest's own measurement is the host's ping.
        if (g_role == Role::Guest && p.id == g_host) ping = g_ping_ms;
        wchar_t ms[16] = L"--ms";
        if (ping > 0) swprintf_s(ms, L"%dms", ping);
        const int side = g_nameplate_n[mine] % 3;
        g_nameplate_n[mine] = side + 1;
        wchar_t* out = g_nameplate[mine][side];
        constexpr size_t kCap = sizeof(g_nameplate[0][0]) / sizeof(wchar_t);
        switch (mode) {
            case 1: swprintf_s(out, kCap, L"(%ls, %ls)", name, ms); break;
            case 2: swprintf_s(out, kCap, L"(%ls, SL: %d)", name, p.soul_level); break;
            case 3: swprintf_s(out, kCap, L"(%ls, deaths: %d)", name, p.deaths); break;
            default: swprintf_s(out, kCap, L"(%ls, SL: %d, %ls)", name, p.soul_level, ms); break;
        }
        return out;
    }
    return nullptr;
}

// ecx is the name widget. The wide string is the one stack argument (the callee pops it).
__declspec(naked) void hk_floating_name() {
    __asm {
        push ecx
        push edx
        push dword ptr [esp + 12]
        call nameplate_for
        test eax, eax
        jz keep
        mov dword ptr [esp + 12], eax
    keep:
        pop edx
        pop ecx
        jmp g_name_set
    }
}

bool install_nameplate() {
    g_name_set = reinterpret_cast<void*>(kNameSet);
    const bool a = redirect_call(kFloatingNameA, kNameSet, reinterpret_cast<void*>(&hk_floating_name));
    const bool b = redirect_call(kFloatingNameB, kNameSet, reinterpret_cast<void*>(&hk_floating_name));
    if (!a || !b) return false;
    LOG_INFO("[coop] Companion names on the floating health bar can show more (OverheadPlayerDisplay)");
    return true;
}

std::vector<std::wstring> panel_lines(int mode) {
    std::vector<std::wstring> out;
    if (g_role == Role::None) return out;
    std::wstring head;
    if (g_role == Role::Host) {
        head = g_step == Step::Hosting   ? L"Prepare to Co-op: hosting"
               : g_step == Step::Closing ? L"Prepare to Co-op: closing..."
                                         : L"Prepare to Co-op: creating session...";
    } else if (g_step == Step::InWorld) {
        head = (g_invade ? L"Invading " : L"In ") + text::widen(steam::persona(g_host)) + L"'s world";
    } else if (g_step == Step::GoingHome) {
        head = L"Returning home...";
    } else {
        head = g_invade ? L"Prepare to Co-op: invading..." : L"Prepare to Co-op: joining...";
    }
    if (world::in_limbo()) head += L"  (limbo)";
    out.push_back(head);
    const auto& st = rules::state();
    if (st.active && (st.world_rules || st.infection || st.taunt)) {
        std::wstring rules_line = L"  ";
        if (st.world_rules == 1) rules_line += L"Allies hostile  ";
        if (st.world_rules == 2) rules_line += L"Allies in teams  ";
        if (st.infection) rules_line += L"Cursed world  ";
        if (st.taunt) rules_line += L"Invaders taunted";
        out.push_back(rules_line);
    }
    auto detail = [mode](int sl, int deaths, int ping) {
        wchar_t b[96] = L"";
        const std::wstring p = ping < 0 ? L"--" : std::to_wstring(ping);
        switch (mode) {
            case 1: swprintf_s(b, L"  %ls ms", p.c_str()); break;
            case 2: swprintf_s(b, L"  SL %d", sl); break;
            case 3: swprintf_s(b, L"  deaths %d", deaths); break;
            case 4: swprintf_s(b, L"  SL %d  %ls ms", sl, p.c_str()); break;
            default: break;
        }
        return std::wstring(b);
    };
    out.push_back(L"You" + detail(game::soul_level(), game::death_count(), g_role == Role::Host ? 0 : g_ping_ms));
    for (const auto& p : g_peers) {
        if (now() - p.last_seen > 10.0) continue;
        std::wstring line = text::widen(p.name);
        if (p.invader) line += L" (invader)";
        if (p.dead) line += L" (dead)";
        line += detail(p.soul_level, p.deaths, p.id == g_host || g_role == Role::Host ? p.ping_ms : -1);
        out.push_back(line);
    }
    return out;
}

}  // namespace coop::session
