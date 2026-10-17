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
#include "spectate.h"
#include "steam.h"
#include "text.h"
#include "world.h"
#include "version.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>

namespace coop::session {

namespace {

// ---------------------------------------------------------------- wire format
constexpr uint32_t kMagic = 0x31435350;  // "PSC1"
// Players of the same protocol play together (see same_version). 1.0.0 and 1.1.0 both speak 8 and play together:
// the messages are the same, 1.1.0's hello only adds its version at the end. Bump it when a release can no longer
// play with the older ones; the version notices then tell the players why they cannot connect.
constexpr uint32_t kProtocol = 8;
// Whether players also need the same Prepare to Co-op version, not only the same protocol. Off for now, so 1.0.0
// and 1.1.0 players meet; the version is still published and sent, for a later release to compare.
constexpr bool kRequireSameVersion = false;
// What this game speaks and reports: kProtocol and PREPARE_TO_COOP_VERSION, unless the hidden INI keys TestProtocol /
// TestVersion pretend otherwise (TestProtocol shows the version notices between two copies of the same build;
// TestVersion only does with kRequireSameVersion).
uint32_t g_protocol = kProtocol;
std::string g_version = PREPARE_TO_COOP_VERSION;
// Lobby data. "ptdesc_ver" with 8 is what Prepare to Co-op 1.0.0 searches for, so every host keeps publishing it:
// after a protocol bump a 1.0.0 player still finds a newer host, is refused, and the host is told who tried. The
// protocol a host really speaks is "ptdesc_proto" and its Prepare to Co-op version "ptdesc_mod" (both from 1.1.0 on;
// a lobby without them is 1.0.0's, protocol 8). Searches do not filter by version, so a session of another version is
// reported to the player instead of being hidden.
constexpr const char* kLegacyProtocol = "8";
// Dried Fingers. 1.0.0 invaders go to a world with "ptdesc_taunt" = 1 before any other, so with few players one
// such host would draw every 1.0.0 invader. Hosts from 1.1.0 on publish "ptdesc_fingers" instead, which only 1.1.0
// invaders read, and make the world kFingersWeight times as likely to be picked as one without (pick_invade_world).
constexpr double kFingersWeight = 1.5;
// Steam's room in our lobby: more than the game's four players, so that lobby members who never join (players of
// other games that use Spacewar, or knocks that fail) cannot fill it. The game's own limit is kept by host_answer.
constexpr int kLobbyRoom = 16;

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
    kTeamRespawn = 21, // a team wipe (1.1.0): where my respawn is (guest -> host, host -> guests)
};

// kBossFight (1.1.0): a co-op guest waits for the host's or a guest's boss fight to end (1.0.0 takes it as a refusal).
enum WelcomeStatus : uint32_t { kOk = 0, kFull = 1, kBusy = 2, kRefused = 3, kVersion = 4, kBossFight = 5 };

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
    char name[64];     // the sender's character name (UTF-8) from 1.1.0 on; 1.0.0 sends its Steam name
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
    char name[64];          // as in Hello: the character name from 1.1.0 on, 1.0.0's Steam name
    wchar_t char_name[17];  // character name: matches the player to their character in our world
    int32_t ceremony;       // the bonfire animation the player is looping (the sit), 0 none
};
// Added in 1.1.0 behind Info, which keeps protocol 8: an older player reads the Info part and ignores the rest,
// and an older player's shorter Info leaves these at their defaults.
struct InfoMore {
    Info info;
    uint8_t boss_fight;    // a boss gauge is shown in this player's game (the host keeps invaders out meanwhile)
    uint8_t team_respawn;  // this player's respawn after a team wipe waits for the others' (kTeamRespawn)
    uint8_t respawn;       // where that respawn is for the death this player reports (kRespawnNone ...)
    uint8_t reserved[5];
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
    char name[64];  // the name to show: a character name from 1.1.0 on (1.0.0 puts Steam names here)
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
constexpr const wchar_t* kMsgBossFight = L"That world is in a boss fight. Try again when it is over";
constexpr const wchar_t* kMsgTryAnother = L"Trying another world...";
constexpr const wchar_t* kMsgReturnHome = L"Returning to your world...";
constexpr const wchar_t* kMsgHostClosed = L"The host closed their world. Returning to your world...";
constexpr const wchar_t* kMsgWorldClosed = L"Your world is closed. Guests return to their worlds";
constexpr const wchar_t* kMsgWorldOpen = L"Your world is open";
constexpr const wchar_t* kMsgTeamRespawn = L"Waiting for the other players...";  // a team wipe (1.1.0)
constexpr const wchar_t* kMsgMateBossFight = L"{} is fighting a boss: no resting until the fight is decided";

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
// Guest: the other worlds the search found, tried in turn when the one picked cannot take us (try_next_world).
std::vector<uint64_t> g_worlds;
int g_world_tries = 0;   // worlds tried for this search
int g_join_gen = 0;      // the current lobby join (an answer for an earlier one is stale)
bool g_last_busy = false;  // the host's last answer was "busy"
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
// Key items a co-op guest gained in the host's world. Keys picked up there were missing from the guest's own
// inventory later (the Asylum's F2 East Key and Big Pilgrim's Key, 2026-10-16: both used or received in the
// host's world, absent from the save; the Estus from the same talk stayed). Where they are lost is not known yet,
// so the key items are logged at every step, and keys gained there that are gone after the trip home are given
// back.
std::vector<int32_t> g_keys_arrival;  // held when we arrived in the host's world
std::vector<int32_t> g_keys_seen;     // as last logged there
std::vector<int32_t> g_keys_carried;  // gained there and held when we left
double g_keys_polled = 0;
double g_keys_home_at = 0;            // when we were first home again with keys to check (0 not yet)
// Our gait as last sent (motion sync).
int g_sent_gait = -1;
double g_last_gait = 0;
// Host, while disbanding: when it began and whether the game's own leave was asked for.
double g_close_at = 0;
bool g_close_left = false;
bool g_close_forced = false;  // a guest was still in the game session when it was ended
// We invade and the co-op team fell: the invasion is won, and our own death a moment later (both fell) is no
// defeat. Or we fell first (g_invade_lost): a wipe reported a moment later is no victory.
bool g_invade_won = false;
bool g_invade_lost = false;
bool g_invade_open = false;  // host: our lobby tells invaders' searches we take invaders (ptdesc_invade)
// Host: since when we have had no character (0: we have one), and whether the lobby is hidden for it.
double g_away_since = 0;
bool g_hidden = false;
// We invade and are in the host's world: the other players' health bars are shown (hk_bar_type; set every tick).
uint8_t g_invader_bars = 0;

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

std::string utf8(const std::wstring& w) {
    char out[256];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out, sizeof(out), nullptr, nullptr);
    return n > 0 ? std::string(out) : std::string();
}

// The name the other players see for us: our character's name (the Steam name only while no character is loaded).
std::wstring own_name() {
    const std::wstring name = party::local_name();
    return name.empty() ? text::widen(steam::persona(steam::self())) : name;
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

// Players can play together only with the same protocol (and, with kRequireSameVersion, the same Prepare to Co-op
// version).
bool same_version(const PeerVersion& v) {
    return v.protocol == g_protocol && (!kRequireSameVersion || v.mod == g_version);
}

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

// The name we show for another player: their character's name, or their Steam name until they have reported it.
// PeerInfo.name stays the Steam name, for the log.
std::wstring shown_name(const PeerInfo& p) { return p.char_name.empty() ? text::widen(p.name) : p.char_name; }

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

uint32_t chr_of_peer(uint64_t id);

// A co-op player of our session (the host or a guest; invaders do not count). The lobby can also hold players who
// never got in, and a refused joiner still broadcasts its status: a host counts the players it admitted, everyone
// else the host, players whose character is here, and the players the game lists as session members (`members`,
// party::remotes; the game's record holds the Steam name, cut to 16 characters).
bool coop_member(const PeerInfo& p, const std::vector<party::Remote>& members) {
    if (p.invader || now() - p.last_seen > 10.0) return false;
    if (g_role == Role::Host) return admitted(p.id);
    if (p.id == g_host || chr_of_peer(p.id)) return true;
    const std::wstring steam_name = text::widen(p.name);
    for (const auto& m : members) {
        if (m.name.empty()) continue;
        if (m.name == p.char_name || m.name == steam_name) return true;
        if (m.name.size() >= 16 && steam_name.compare(0, m.name.size(), m.name) == 0) return true;
    }
    return false;
}

// We invade: the host has fallen and so has every co-op guest. Since 1.1.0 the host's death alone does not end an
// invasion while a guest still stands (the fallen spectate, spectate.cpp): the co-op players win or lose together.
bool coop_team_down() {
    const std::vector<party::Remote> members = party::remotes();
    bool host_down = false;
    int down = 0;
    for (const auto& p : g_peers) {
        if (!coop_member(p, members)) continue;
        if (!p.dead) return false;
        ++down;
        if (p.id == g_host) host_down = true;
    }
    // As many fallen as the host counts co-op players (its state: the host and its co-op guests): a guest this game
    // has not matched to anyone yet (just after we arrived) counts as standing, not as gone.
    return host_down && down >= g_host_coop_players;
}

// An invasion has one outcome, whichever comes first: the co-op team's wipe (invasion_won) or our own death
// (send_death). Our death is noticed in world::tick, after the messages, so the last co-op player's kDead or the
// half-second check could come first in a mutual kill and announce victory after the failure.
bool invasion_undecided() { return g_invade && g_step == Step::InWorld && !g_invade_won && !g_invade_lost; }

void invasion_won() {
    // Fallen already, with the report of it still to come this frame (send_death): that is the outcome.
    if (world::local_dead()) return;
    LOG_INFO("[coop] Every co-op player of this world has fallen: invasion victory");
    g_invade_won = true;
    say(kMsgBreakInSuccess);
    game::call_lua("HostDead");  // the game's own invader victory
}

// ---------------------------------------------------------------- a team wipe reloads every game at once
// After a team wipe every game reloads its world, and the boss and the enemies start over as in single player, but
// only if no game is in its new world while another is still in the old one: the network goes on following the old
// world's game for an enemy, and the new world's flags go into the old one (docs: "A team wipe reloads every game
// at once"). So the respawn warps wait for each other (world.cpp holds WarpNextStageKick after "YOU DIED"). Each
// player reports where its respawn is for the death it reports: kRespawnNone (not at the warp yet), kRespawnWaiting
// (at the warp, waiting for the team), kRespawnWent (the warp went; until they stand in the world again). The host
// waits for every fallen guest still at kRespawnNone; a guest waits for the host's kRespawnWent. Each change is a
// message (kTeamRespawn), and the player report repeats it every second, also through the load.
constexpr uint32_t kRespawnNone = 0, kRespawnWaiting = 1, kRespawnWent = 2;
constexpr double kHostWait = 15.0;   // the host waits this long for the guests' deaths to reach their warp
constexpr double kGuestWait = 25.0;  // a guest waits this long for the host's warp (a host gone silent)
uint32_t g_respawn = kRespawnNone;   // ours, for our current death
bool g_respawn_waiting = false;
double g_respawn_since = 0;
double g_respawn_told = 0;  // guest: when we last told the host our warp waits
// Guest: when the host last said its warp went (message or player report). Kept apart from the host's phase, which
// its first report after standing up sets back to none: both can arrive in one poll, after a stall.
double g_host_went_at = -100;

// Every other co-op player has fallen (and there is one). The dead stay dead in their reports until their
// respawned character exists (send_info), so a wipe lasts through the loads.
bool team_wiped() {
    const std::vector<party::Remote> members = party::remotes();
    int n = 0;
    for (const auto& p : g_peers) {
        if (!coop_member(p, members)) continue;
        if (!p.dead) return false;
        ++n;
    }
    return n > 0;
}

// A player's report for their current death: it only moves forward (None -> Waiting -> Went) until they live again.
void note_peer_respawn(PeerInfo* p, uint32_t phase) {
    if (p && p->dead && phase > p->respawn && phase <= kRespawnWent) p->respawn = phase;
}

// Host: the fallen co-op guests that will say where their respawn is, and the ones of them not at their warp yet.
// A guest whose warp already went for this death (it died alone, or gave up waiting) is not waited for.
void respawn_guests(int* fallen, std::vector<const PeerInfo*>* not_ready) {
    const std::vector<party::Remote> members = party::remotes();
    for (const auto& p : g_peers) {
        if (!coop_member(p, members) || !p.team_respawn || !p.dead || p.respawn == kRespawnWent) continue;
        ++*fallen;
        if (p.respawn == kRespawnNone && not_ready) not_ready->push_back(&p);
    }
}

void respawn_signal(uint32_t phase) {
    g_respawn = phase;
    Simple m{};
    fill_header(&m, kTeamRespawn);
    m.value = phase;
    if (g_role == Role::Host) {
        broadcast(m);
    } else if (g_host) {
        send_to(g_host, m);
        g_respawn_told = now();
    }
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
    g_invade_won = false;
    g_invade_lost = false;
    g_respawn_waiting = false;
    g_respawn = kRespawnNone;
    g_host_went_at = -100;
    g_away_since = 0;
    g_hidden = false;
    g_worlds.clear();
    g_world_tries = 0;
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
    steam::create_lobby(std::max(kLobbyRoom, g_cfg.max_players), [](bool ok, uint64_t lobby) {
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
        g_invade_open = g_cfg.allow_invaders;
        steam::set_lobby_data(lobby, "ptdesc_taunt", "0");  // stays 0: see kFingersWeight
        steam::set_lobby_data(lobby, "ptdesc_fingers", "0");
        steam::set_lobby_joinable(lobby, true);
        g_away_since = 0;
        g_hidden = false;
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

// Host: the co-op guests in our world, the ones host_answer admitted. A joiner's hello puts them in g_peers before
// the answer, and one told kBusy (we spectate, or we are between maps) knocks again every few seconds without
// getting in: counted, they kept an invasion from ending after every co-op player had fallen (coop_team_down
// needs this many) and enemies scaled for a player who was not here.
int coop_guest_count() {
    int n = 0;
    for (const auto& p : g_peers) {
        if (!p.invader && admitted(p.id) && now() - p.last_seen <= 10.0) ++n;
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
        say(fmt(p.invader ? kMsgInvaderLost : kMsgLost, shown_name(p)));
        PlayerEvent e = player_event(kLeft, id, p.invader, utf8(shown_name(p)));
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

// A boss gauge is shown in our game or in a co-op guest's (1.1.0 guests report it).
bool boss_fight_in_world() {
    if (world::boss_fight()) return true;
    for (const auto& p : g_peers) {
        if (!p.invader && p.boss_fight && admitted(p.id) && now() - p.last_seen <= 5.0) return true;
    }
    return false;
}

// Where an invader arrives in our world, as the game's own invasions do: the black invasion point of the play
// region we stand in (the collision's PlayRegionID, PlayerIns+0x284; one point per play region in the maps'
// MSBs). Where our play region has none (connecting stretches the multiplayer walls used to close, Firelink
// Shrine, the Undead Asylum), the nearest point within kNearPoint; with none in reach, on the host (false).
constexpr float kNearPoint = 50.0f;

bool invader_arrival(game::InvasionPoint* ip) {
    game::Vec3 me{};
    const int region = game::play_region();
    if (!game::position(&me, nullptr) || !game::invasion_point(region, me, kNearPoint, ip)) {
        LOG_INFO("[coop] No invasion point for play region %d or within %.0f m: the invader arrives on the host",
                 region, kNearPoint);
        return false;
    }
    LOG_INFO("[coop] The invader arrives at invasion point %d (%s, %.0f m away, map %08X)", ip->id,
             ip->exact ? "our play region's" : "the nearest", std::sqrt(ip->dist), ip->map);
    return true;
}

void host_answer(const Pending& p) {
    Welcome w{};
    fill_header(&w, kWelcome);
    w.status = kOk;
    // The game's limit for guests and invaders together (it keeps three remote players). Steam's lobby has more
    // room (kLobbyRoom), so this is what keeps the count; it used to be the lobby's size. Until 1.1.0 a second
    // check counted every non-invader in g_peers: that held players still knocking, and turned a joiner away as
    // full while there was room.
    int in_session = 1;
    for (uint64_t id : g_admitted) in_session += id == p.from ? 0 : 1;
    if (in_session >= g_cfg.max_players) w.status = kFull;
    if (p.invade && !g_cfg.allow_invaders) w.status = kRefused;
    // Nobody joins a boss fight (ours or a guest's). An invader tries another world. A guest is told to come back
    // after it: one who arrived mid-fight ran the boss's opening in its own game on a boss already fighting (the
    // Asylum Demon's jump from its perch, from wherever it stood, through the floor; 2026-10-17 test). Remastered
    // Seamless Co-op also keeps invaders out of the Undead Asylum and Firelink Shrine; Prepare to Co-op lets them in.
    if (w.status == kOk && boss_fight_in_world()) {
        w.status = p.invade ? kRefused : kBossFight;
        LOG_INFO("[coop] %s knocked during a boss fight: %s", p.invade ? "An invader" : "A guest",
                 p.invade ? "refused" : "told to come back after it");
    }
    // Between maps or at the title screen there is no world to arrive in yet, and none while our death waits
    // (spectating): the joiner asks again.
    if (w.status == kOk && (!game::in_game() || game::loading() || !game::own_world() || world::local_dead())) {
        w.status = kBusy;
    }
    std::vector<uint8_t> inv;
    if (w.status == kOk) {
        // join type: -1 white (co-op, own body kept by the guest's rules), -2 black (invader)
        if (!game::build_invitation(p.invade ? -2 : -1, 0, &inv) || inv.size() > sizeof(w.invitation)) {
            w.status = kBusy;
        }
    }
    if (w.status == kOk) {
        const auto& st = rules::state();
        // Guests arrive at the host's last bonfire (where the host last rested); invaders at a black invasion point
        // (invader_arrival), else on the host.
        const int bonfire = game::last_bonfire();
        game::InvasionPoint ip;
        if (!p.invade && g_rest.valid && g_rest.bonfire == bonfire) {
            w.map = g_rest.map;
            w.pos[0] = g_rest.pos.x;
            w.pos[1] = g_rest.pos.y;
            w.pos[2] = g_rest.pos.z;
            w.angle = g_rest.angle;
        } else if (p.invade && invader_arrival(&ip)) {
            w.map = ip.map;
            w.pos[0] = ip.pos.x;
            w.pos[1] = ip.pos.y;
            w.pos[2] = ip.pos.z;
            w.angle = ip.angle;
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
        // The host, the admitted guests, and a joining guest (admitted just below).
        const int joining = !p.invade && !admitted(p.from) ? 1 : 0;
        w.coop_players = static_cast<uint8_t>(1 + coop_guest_count() + joining);
        w.invitation_size = static_cast<uint32_t>(inv.size());
        std::memcpy(w.invitation, inv.data(), inv.size());
    }
    set_admitted(p.from, w.status == kOk);
    send_to(p.from, w);
    const PeerInfo* who = nullptr;
    for (const auto& q : g_peers) who = q.id == p.from ? &q : who;
    LOG_INFO("[coop] Answered %s (%s, character '%s'): status %u", steam::persona(p.from).c_str(),
             p.invade ? "invader" : "guest", who ? utf8(who->char_name).c_str() : "", w.status);
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

// A host at the title screen has no world to join, yet its lobby is still found, and every joiner or invader who
// picked it waited through "busy" answers. Once we have had no character for kAwayHide s, the lobby is hidden: Steam
// lists no lobby that cannot be joined, for any version of the mod. It is shown again as soon as we are back in the
// world. Loading screens are shorter than that; one that is not only hides the session for a moment.
constexpr double kAwayHide = 10.0;

void host_hide_while_away() {
    if (game::in_game()) {
        g_away_since = 0;
        if (g_hidden) {
            g_hidden = false;
            steam::set_lobby_joinable(g_lobby, true);
            LOG_INFO("[coop] Back in the world: our session can be found again");
        }
        return;
    }
    if (g_away_since == 0) g_away_since = now();
    if (!g_hidden && now() - g_away_since >= kAwayHide) {
        g_hidden = true;
        steam::set_lobby_joinable(g_lobby, false);
        LOG_INFO("[coop] No character for %.0f s (title screen): our session is hidden from searches", kAwayHide);
    }
}

// Invaders' searches skip a world that would refuse them: ptdesc_invade reads "0" while a boss fight is on (and
// with AllowInvaders off). 1.0.0 invaders search on the same key.
void host_invasions_open() {
    static double last = 0;
    if (now() - last < 1.0) return;
    last = now();
    const bool open = g_cfg.allow_invaders && !boss_fight_in_world();
    if (open == g_invade_open) return;
    g_invade_open = open;
    steam::set_lobby_data(g_lobby, "ptdesc_invade", open ? "1" : "0");
    LOG_INFO("[coop] A boss fight %s: invaders %s our world", open ? "is over" : "is on",
             open ? "can find" : "cannot find");
}

void host_tick() {
    if (g_step == Step::Closing) {
        host_close_tick();
        return;
    }
    if (g_step != Step::Hosting) return;
    host_hide_while_away();
    host_invasions_open();
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
        // Admitted only: an invader that knocked and was refused still broadcasts its status for a moment.
        for (const auto& p : g_peers) invader |= p.invader && admitted(p.id) && now() - p.last_seen <= 10.0;
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
    strncpy_s(h.name, utf8(own_name()).c_str(), _TRUNCATE);
    strncpy_s(h.version, g_version.c_str(), _TRUNCATE);
    steam::accept(g_host);
    send_to(g_host, h);
    ++g_hello_tries;
    set_step(Step::Hello);
}

// An invader's pick among the worlds found: at random, a world with Dried Fingers kFingersWeight times as likely as
// one without. A 1.0.0 host with Dried Fingers says so in "ptdesc_taunt", a newer one in "ptdesc_fingers".
uint64_t pick_invade_world(const std::vector<uint64_t>& found) {
    static std::mt19937_64 rng = [] {
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        return std::mt19937_64(static_cast<uint64_t>(t.QuadPart) ^ steam::self());
    }();
    std::vector<double> weight;
    double total = 0;
    int fingers = 0;
    for (uint64_t l : found) {
        const bool f = steam::lobby_data(l, "ptdesc_fingers") == "1" || steam::lobby_data(l, "ptdesc_taunt") == "1";
        fingers += f ? 1 : 0;
        weight.push_back(f ? kFingersWeight : 1.0);
        total += weight.back();
    }
    double r = std::uniform_real_distribution<double>(0.0, total)(rng);
    size_t i = 0;
    while (i + 1 < found.size() && r >= weight[i]) r -= weight[i++];
    LOG_INFO("[coop] %zu world(s) to invade, %d with Dried Fingers; picked one %s", found.size(), fingers,
             weight[i] > 1.0 ? "with Dried Fingers" : "without");
    return found[i];
}

// The world we picked cannot take us (full, refused, busy, gone or silent): the next one the search found is
// tried, up to kMaxWorldTries worlds. Not once the host has let us in, and not for a knock that only tells a host
// of another version why we cannot come in.
constexpr int kMaxWorldTries = 5;

bool can_try_another() { return !g_mismatch.active && !g_worlds.empty() && g_world_tries < kMaxWorldTries; }

// How often we knock on one world before trying another: a host answering "busy" (between maps) gets more tries
// than one that does not answer at all. With no other world to try, 8 as before.
int knock_limit(bool busy) { return !can_try_another() ? 8 : busy ? 4 : 2; }

void join_world(uint64_t lobby);

bool try_next_world(const char* why) {
    if (!can_try_another()) return false;
    LOG_INFO("[coop] %s's world: %s; trying another (%zu more found)", g_host ? steam::persona(g_host).c_str() : "?",
             why, g_worlds.size());
    if (g_lobby) steam::leave_lobby(g_lobby);
    if (g_host) steam::close(g_host);
    for (const auto& p : g_peers) steam::close(p.id);
    g_lobby = 0;
    g_host = 0;
    g_host_heard = 0;
    g_peers.clear();
    say(kMsgTryAnother);
    join_world(g_invade ? pick_invade_world(g_worlds) : g_worlds.front());
    return true;
}

// Enters a world's lobby and knocks (the hello). The world leaves the list of the others to try.
void join_world(uint64_t pick) {
    g_worlds.erase(std::remove(g_worlds.begin(), g_worlds.end(), pick), g_worlds.end());
    ++g_world_tries;
    const int gen = ++g_join_gen;
    set_step(Step::LobbyJoin);
    steam::join_lobby(pick, [gen](bool ok, uint64_t lobby) {
        // The answer for a lobby we gave up on: leave it if Steam let us in after all.
        if (gen != g_join_gen || g_step != Step::LobbyJoin) {
            if (ok && lobby && lobby != g_lobby) steam::leave_lobby(lobby);
            return;
        }
        if (!ok) {
            if (g_mismatch.active) {
                finish(L"");
                show_join_mismatch(g_mismatch.host);
                return;
            }
            if (!try_next_world("could not enter its lobby")) finish(kMsgConnectTimeout);
            return;
        }
        g_lobby = lobby;
        g_host = steam::lobby_owner(lobby);
        g_host_heard = now();
        LOG_INFO("[coop] Entered %s's lobby", steam::persona(g_host).c_str());
        g_hello_tries = 0;
        g_last_busy = false;
        guest_send_hello();
    });
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
        const uint64_t pick = g_invade ? pick_invade_world(found) : found.front();
        g_worlds = found;
        g_world_tries = 0;
        join_world(pick);
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
    if (w.status == kBusy && g_hello_tries < knock_limit(true)) {
        g_last_busy = true;
        set_step(Step::Hello);
        g_step_since = now() - 17.0;  // retry in 3 s
        return;
    }
    if (w.status == kBossFight) {
        LOG_INFO("[coop] The host's world is in a boss fight");
        if (try_next_world("in a boss fight")) return;
        finish(kMsgBossFight);
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
        if (try_next_world(w.status == kFull ? "full" : w.status == kRefused ? "refused" : "busy")) return;
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
    st.host_map = w.map;
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

std::string key_text(const std::vector<int32_t>& v) {
    std::string s;
    for (int32_t id : v) s += (s.empty() ? "" : " ") + std::to_string(id);
    return s.empty() ? "none" : s;
}

bool holds(const std::vector<int32_t>& v, int32_t id) { return std::find(v.begin(), v.end(), id) != v.end(); }

void guest_go_home(const std::wstring& why, const char* reason) {
    LOG_INFO("[coop] Going home (%s; session state %d, step %d)", reason, game::session_state(), game::join_step());
    if (!g_invade && g_step == Step::InWorld) {
        const std::vector<int32_t> now_keys = items::key_items();
        g_keys_carried.clear();
        for (int32_t id : now_keys) {
            if (!holds(g_keys_arrival, id)) g_keys_carried.push_back(id);
        }
        g_keys_home_at = 0;
        LOG_INFO("[coop] Key items as we leave: %s (gained here: %s)", key_text(now_keys).c_str(),
                 key_text(g_keys_carried).c_str());
        for (int32_t id : g_keys_carried) items::keep_key_for_home(id);  // in case the last poll missed one
        LOG_INFO("[coop] Key range as we leave: %s", items::key_layout().c_str());
    }
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
    if (!g_invade) {
        g_keys_arrival = items::key_items();
        g_keys_seen = g_keys_arrival;
        g_keys_polled = now();
        LOG_INFO("[coop] Key items on arrival: %s", key_text(g_keys_arrival).c_str());
        LOG_INFO("[coop] Key range on arrival: %s", items::key_layout().c_str());
    }
    if (g_invade) {
        int defeat = g_host_coop_players;
        say(fmt(kMsgBreakInMission, std::to_wstring(defeat)));
    } else {
        say(kMsgGuestMission);
    }
    PlayerEvent e = player_event(kArrived, steam::self(), g_invade, utf8(own_name()));
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
            if (in_step > 20.0 && !try_next_world("its lobby could not be entered")) {
                finish(g_invade ? kMsgBreakInTimeout : kMsgJoinTimeout);
            }
            break;
        case Step::Hello:
            if (in_step > 20.0 && g_mismatch.active) {
                finish(L"");
                show_join_mismatch(g_mismatch.host);
            } else if (in_step > 20.0) {
                if (g_hello_tries < knock_limit(g_last_busy)) {
                    guest_send_hello();
                } else if (!try_next_world(g_last_busy ? "busy" : "no answer")) {
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
            if (!g_invade && now() - g_keys_polled >= 2.0 && !game::loading()) {
                g_keys_polled = now();
                const std::vector<int32_t> k = items::key_items();
                if (k != g_keys_seen && !k.empty()) {
                    LOG_INFO("[coop] Key items here now: %s (on arrival: %s)", key_text(k).c_str(),
                             key_text(g_keys_arrival).c_str());
                    g_keys_seen = k;
                    // A key gained here goes into the own world's key array as well (items::keep_key_for_home).
                    for (int32_t id : k) {
                        if (holds(g_keys_arrival, id)) continue;
                        LOG_INFO("[coop] Key item %d, gained here, %s", id,
                                 items::keep_key_for_home(id) ? "is kept for home as well"
                                                              : "could not be kept for home (given back there)");
                    }
                }
            }
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
    InfoMore more{};
    Info& i = more.info;
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
        g_respawn = kRespawnNone;
    }
    i.dead = dead ? 1 : 0;
    i.light = world::local_lights();
    strncpy_s(i.name, utf8(own_name()).c_str(), _TRUNCATE);
    wcsncpy_s(i.char_name, party::local_name().c_str(), _TRUNCATE);
    i.ceremony = motion::local_loop_anim();
    more.boss_fight = world::boss_fight() ? 1 : 0;
    more.team_respawn = 1;
    more.respawn = static_cast<uint8_t>(dead ? g_respawn : kRespawnNone);
    broadcast(more, false);
}

// The player an event is about, by their character's name when we know them (a 1.0.0 player's events carry its
// Steam name), else by the name the event carries.
std::wstring event_name(const PlayerEvent& e) {
    for (const auto& p : g_peers) {
        if (p.id == e.who && !p.char_name.empty()) return p.char_name;
    }
    char name[65];
    std::memcpy(name, e.name, 64);
    name[64] = 0;
    return text::widen(name);
}

// Host: an event passed on to the guests names its player by their character, as far as we know it.
void name_event(PlayerEvent* e) {
    for (const auto& p : g_peers) {
        if (p.id == e->who && !p.char_name.empty()) strncpy_s(e->name, utf8(p.char_name).c_str(), _TRUNCATE);
    }
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
            // From 1.1.0 on the hello names the character (PeerInfo.name keeps the Steam name); 1.0.0's names the
            // Steam account.
            if (size >= sizeof(m)) {
                if (p->char_name.empty()) p->char_name = text::widen(m.name);
            } else {
                p->name = m.name;
            }
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
            if (m.map != -1) rules::state().host_map = m.map;
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
            m.char_name[16] = 0;
            // 1.0.0 sends its Steam name here, 1.1.0 on the character name again; PeerInfo.name is the Steam name.
            if (text::widen(m.name) != m.char_name) p->name = m.name;
            p->soul_level = m.soul_level;
            p->deaths = m.deaths;
            p->map = m.map;
            std::memcpy(p->pos, m.pos, sizeof(p->pos));
            p->ping_ms = m.ping_ms;
            p->invader = m.invader != 0;
            const bool dead = m.dead != 0;
            if (!dead && (p->dead || p->alive_since == 0)) p->alive_since = now();
            if (dead && !p->dead) p->respawn = kRespawnNone;  // a death we had not heard of
            p->dead = dead;
            p->light = m.light;
            p->char_name = m.char_name;
            p->ceremony = motion::synced_anim(m.ceremony) ? m.ceremony : 0;
            InfoMore more{};
            const bool more_ok = as(&more);
            p->boss_fight = more_ok && more.boss_fight != 0;
            p->team_respawn = more_ok && more.team_respawn != 0;
            if (more_ok && more.respawn == kRespawnWent && g_role == Role::Guest && from == g_host) g_host_went_at = now();
            if (!dead) p->respawn = kRespawnNone;
            if (more_ok) note_peer_respawn(p, more.respawn);
            p->last_seen = now();
            break;
        }
        case kArrived: {
            PlayerEvent m;
            if (!as(&m)) return;
            if (g_role == Role::Host) {
                if (m.who != from) return;
                name_event(&m);
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
                name_event(&m);
                broadcast(m, true, from);
            } else if (from != g_host) {
                return;
            }
            if (PeerInfo* p = peer(m.who)) {
                p->dead = true;
                p->respawn = kRespawnNone;  // a new death: its respawn has not reached the warp yet
                ++p->death_events;  // their character here gets rebuilt (party::tick)
            }
            say(fmt(m.invader ? kMsgInvaderDied : kMsgDied, event_name(m)));
            // We invade and the last co-op player fell (the host alone is not enough while a guest stands).
            if (invasion_undecided() && coop_team_down()) invasion_won();
            break;
        }
        case kTeamRespawn: {
            Simple m;
            if (!as(&m) || g_invade || (g_role == Role::Guest && from != g_host)) return;
            // Every death's warp says it went (note_respawn_went); only a wipe waits on it.
            if (g_role == Role::Guest && m.value == kRespawnWent) g_host_went_at = now();
            PeerInfo* p = peer(from);
            const uint32_t was = p->respawn;
            note_peer_respawn(p, m.value);
            if (p->respawn != was && (p->respawn == kRespawnWaiting || g_respawn_waiting)) {
                LOG_INFO("[coop] Team wipe: %s's respawn %s", steam::persona(from).c_str(),
                         p->respawn == kRespawnWaiting ? "waits" : "went");
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
                PlayerEvent e = player_event(kLeft, from, p->invader, utf8(shown_name(*p)));
                broadcast(e, true, from);
                say(fmt(p->invader ? kMsgInvaderLeft : kMsgLeft, shown_name(*p)));
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
    g_respawn = kRespawnNone;
    if (g_role == Role::None || !g_lobby) return;
    if (invasion_undecided()) {
        g_invade_lost = true;
        LOG_INFO("[coop] We fell while invading: invasion failed");
        say(kMsgBreakInFailed);
    }
    // The death debuff (Remastered Seamless Co-op): a Crimson Blossom per death in a session, up to five.
    if (g_cfg.death_debuffs && !g_invade && items::count(items::kBlossom) < 5) items::give(items::kBlossom, 1);
    PlayerEvent e = player_event(kDead, steam::self(), g_invade, utf8(own_name()));
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

// A session member's record holds the Steam name, cut to 16 characters (see coop_member): a member whose character
// is missing is known only by that.
const PeerInfo* peer_by_member_name(const std::wstring& name) {
    if (name.empty()) return nullptr;
    for (const auto& p : g_peers) {
        if (now() - p.last_seen > 10.0) continue;
        const std::wstring steam_name = text::widen(p.name);
        if (name == steam_name || (name.size() >= 16 && steam_name.compare(0, name.size(), name) == 0)) return &p;
    }
    return nullptr;
}

party::PeerState peer_state(uint32_t chr, const std::wstring& name) {
    party::PeerState s;
    const PeerInfo* p = chr ? peer_by_chr(chr) : nullptr;
    if (!p) p = peer_by_char_name(name);
    if (!p) p = peer_by_member_name(name);
    if (!p) return s;
    s.known = true;
    s.id = p->id;
    s.invader = p->invader;
    s.alive = !p->dead && p->alive_since > 0 && now() - p->alive_since >= 2.0 && now() - p->last_seen <= 5.0;
    s.dead = p->dead;
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
            if (g_step == Step::Hello && try_next_world("the host left its lobby")) return;
            guest_go_home(kMsgReturnHome, "the host left the lobby");
        } else if (g_role == Role::Host) {
            const bool member = admitted(user);
            for (const auto& p : g_peers) {
                if (p.id != user) continue;
                say(fmt(p.invader ? kMsgInvaderLeft : kMsgLeft, shown_name(p)));
                if (member) {
                    // The guests see it too (an invader sent home by the game leaves without a Bye).
                    PlayerEvent e = player_event(kLeft, user, p.invader, utf8(shown_name(p)));
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
            LOG_INFO("[coop] Key items of this character: %s", key_text(items::key_items()).c_str());
            LOG_INFO("[coop] Key range of this character: %s", items::key_layout().c_str());
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
    // Back home with keys gained in the host's world: three seconds in, any of them that is gone is given back.
    if (!g_keys_carried.empty() && g_role == Role::None && game::in_game() && !game::loading() && game::own_world()) {
        if (g_keys_home_at == 0) {
            g_keys_home_at = now();
            LOG_INFO("[coop] Key range on the first frame home: %s", items::key_layout().c_str());
        }
        if (now() - g_keys_home_at >= 3.0) {
            const std::vector<int32_t> k = items::key_items();
            LOG_INFO("[coop] Key items back home: %s", key_text(k).c_str());
            LOG_INFO("[coop] Key range back home: %s", items::key_layout().c_str());
            for (int32_t id : g_keys_carried) {
                if (holds(k, id)) continue;
                items::give(id, 1);
                LOG_INFO("[coop] Key item %d, gained in the host's world, was gone after the trip home: given back (%s)",
                         id, items::count(id) > 0 ? "now held" : "could not be given");
            }
            g_keys_carried.clear();
            g_keys_home_at = 0;
        }
    }
    g_invader_bars = g_role == Role::Guest && g_invade && g_step == Step::InWorld ? 1 : 0;
    text::tick();
    // Characters of the other players: rebuilt after loads and after a death. An invader's game too: a co-op
    // guest who fell and came back stayed an invisible, untouchable dead copy there with only their health bar
    // showing (2026-10-16 invasions; the game never sees a player come back within one session).
    party::tick(g_step == Step::Hosting || g_step == Step::InWorld, &peer_state);
    if (g_step == Step::Hosting || g_step == Step::InWorld) cutscene_tick();
    update_remote_players();
    {
        std::vector<rules::PeerSpot> spots;
        if (g_step == Step::Hosting || g_step == Step::InWorld) {
            for (const auto& p : g_peers) {
                if (p.map == -1 || now() - p.last_seen > 3.0) continue;
                rules::PeerSpot s;
                std::memcpy(s.pos, p.pos, sizeof(s.pos));
                s.map = p.map;
                s.dead = p.dead;
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
        if (g_role == Role::Host || (g_role == Role::Guest && g_step == Step::InWorld)) coop = g_host_coop_players;
        world::set_players(coop);
        // The other co-op players, five times a second (the list asks the game for each member's character).
        static double team_at = 0;
        if (now() - team_at >= 0.2) {
            team_at = now();
            std::vector<spectate::Teammate> team;
            bool all_dead = true;
            const PeerInfo* boss_mate = nullptr;  // a living teammate with a boss gauge up (rules: bonfires wait)
            if (g_role != Role::None && !g_invade && !game::loading()) {
                const std::vector<party::Remote> members = party::remotes();
                for (const auto& p : g_peers) {
                    if (!coop_member(p, members)) continue;
                    all_dead &= p.dead;
                    if (!boss_mate && !p.dead && p.boss_fight && now() - p.last_seen <= 5.0) boss_mate = &p;
                    spectate::Teammate t;
                    t.id = p.id;
                    t.name = shown_name(p);
                    t.chr = chr_of_peer(p.id);
                    t.alive = !p.dead;
                    team.push_back(t);
                }
            }
            spectate::set_teammates(team);
            world::set_everyone_dead(!team.empty() && all_dead);
            if (!game::loading()) {
                auto& st = rules::state();
                if (boss_mate && !st.teammate_boss_fight) {
                    LOG_INFO("[coop] %s is in a boss fight: bonfires wait until it is decided", boss_mate->name.c_str());
                    if (!world::local_dead() && !world::boss_fight()) say(fmt(kMsgMateBossFight, shown_name(*boss_mate)));
                } else if (!boss_mate && st.teammate_boss_fight) {
                    LOG_INFO("[coop] No teammate is in a boss fight any more: bonfires can be used");
                }
                st.teammate_boss_fight = boss_mate != nullptr;
            }
        }
        if (invasion_undecided()) {
            static double last_down = 0;
            if (now() - last_down > 0.5) {
                last_down = now();
                if (coop_team_down()) invasion_won();  // also a last guest who left instead of falling
            }
        }
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
    steam::set_lobby_data(g_lobby, "ptdesc_fingers", "1");  // not ptdesc_taunt: see kFingersWeight
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

const PeerInfo* host_peer() {
    for (const auto& p : g_peers) {
        if (p.id == g_host) return &p;
    }
    return nullptr;
}

bool team_respawn_wait() {
    if (g_role == Role::None || g_invade || !g_lobby || !team_wiped()) return false;
    if (g_role == Role::Guest) {
        if (g_step != Step::InWorld) return false;
        const PeerInfo* host = host_peer();
        if (!host || !host->team_respawn) return false;  // a host from before 1.1.0 would never answer
        if (host->respawn == kRespawnWent) {
            LOG_INFO("[coop] Team wipe: the host's game already reloads; ours follows at once");
            return false;
        }
        respawn_signal(kRespawnWaiting);
        LOG_INFO("[coop] Team wipe: our respawn waits for the host's, so that every game reloads at once");
    } else {
        int fallen = 0;
        std::vector<const PeerInfo*> not_ready;
        respawn_guests(&fallen, &not_ready);
        if (!fallen) return false;  // nobody to wait for: guests from before 1.1.0, or their warps already went
        if (not_ready.empty()) {
            LOG_INFO("[coop] Team wipe: every player's respawn already waits; every game reloads now");
            return false;
        }
        respawn_signal(kRespawnWaiting);
        LOG_INFO("[coop] Team wipe: our respawn waits for %u player(s), so that every game reloads at once",
                 static_cast<unsigned>(not_ready.size()));
    }
    g_respawn_waiting = true;
    g_respawn_since = now();
    say(kMsgTeamRespawn);
    return true;
}

bool team_respawn_go() {
    if (!g_respawn_waiting) return true;
    const double waited = now() - g_respawn_since;
    std::string why;
    if (g_role == Role::None || !g_lobby) {
        why = "the session ended";
    } else if (g_role == Role::Host) {
        int fallen = 0;
        std::vector<const PeerInfo*> not_ready;
        respawn_guests(&fallen, &not_ready);
        if (not_ready.empty()) {
            why = "every player's respawn waits";
        } else if (waited >= kHostWait) {
            why = "no word from";
            for (const PeerInfo* p : not_ready) why += " " + utf8(shown_name(*p));
        }
    } else {
        // Only the host's warp lets ours go (another guest up again says nothing about it): its "went", or the host
        // standing again since our wait began, which it cannot do before its warp has gone.
        const PeerInfo* host = host_peer();
        if (!host) {
            why = "the host is gone";
        } else if (host->respawn == kRespawnWent || g_host_went_at >= g_respawn_since) {
            why = "the host's game reloads";
        } else if (!host->dead && host->alive_since >= g_respawn_since) {
            why = "the host is up again (its warp went)";
        } else if (waited >= kGuestWait) {
            why = "no word from the host";
        } else if (now() - g_respawn_told >= 1.0) {
            respawn_signal(kRespawnWaiting);  // again, in case it was missed
        }
    }
    if (why.empty()) return false;
    g_respawn_waiting = false;
    LOG_INFO("[coop] Team wipe: our world reloads after %.1f s (%s)", waited, why.c_str());
    return true;
}

void note_respawn_went() {
    if (g_role == Role::None || g_invade || !g_lobby || g_respawn == kRespawnWent) return;
    if (g_role == Role::Guest && g_step != Step::InWorld) return;
    respawn_signal(kRespawnWent);
}

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

// The bar's current name, or null when this string is not a companion we know. The game writes the player's Steam
// name there (its member record, member[n] in the "Character names" log line); the bar shows the character's name.
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
        const std::wstring shown_as = shown_name(p).substr(0, 16);
        const wchar_t* who = shown_as.c_str();
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
            case 0: swprintf_s(out, kCap, L"%ls", who); break;
            case 1: swprintf_s(out, kCap, L"(%ls, %ls)", who, ms); break;
            case 2: swprintf_s(out, kCap, L"(%ls, SL: %d)", who, p.soul_level); break;
            case 3: swprintf_s(out, kCap, L"(%ls, deaths: %d)", who, p.deaths); break;
            default: swprintf_s(out, kCap, L"(%ls, SL: %d, %ls)", who, p.soul_level, ms); break;
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

// The floating health bars of the other players (0xFA89C0, one widget per slot, data at MenuMan+0x8F4 + 0x20 *
// slot, filled by 0x1012C30) are hidden while our own character is a dark spirit (chr type 2, or 0xC): vanilla
// invaders see no bars at all. The host and its guests see each other's, so an invader now sees theirs too: the
// test at 0xFA8A11 skips the hide while we invade. A remote dark spirit's bar stays hidden (the slot's own flag at
// +0x1C), as in vanilla.
constexpr uint32_t kBarTypeTest = 0x00FA8A11;
uint32_t g_bar_hide = 0x00FA8A1E;  // xor bl,bl ; mov [esp+14h],bl: no bars
uint32_t g_bar_next = 0x00FA8A24;  // the next test (cmp ecx,0Dh), with ecx = our chr type

__declspec(naked) void hk_bar_type() {
    __asm {
        mov ecx, dword ptr [eax + 0x70]
        cmp ecx, 2
        je dark
        cmp ecx, 0Ch
        je dark
        jmp dword ptr [g_bar_next]
    dark:
        cmp byte ptr [g_invader_bars], 0
        jne keep
        jmp dword ptr [g_bar_hide]
    keep:
        jmp dword ptr [g_bar_next]
    }
}

bool install_invader_bars() {
    // mov ecx,[eax+70h] ; cmp ecx,2 ; je +5 ; cmp ecx,0Ch ; jne +6
    static const uint8_t k[13] = {0x8B, 0x48, 0x70, 0x83, 0xF9, 0x02, 0x74, 0x05, 0x83, 0xF9, 0x0C, 0x75, 0x06};
    if (!bytes_match(kBarTypeTest, k, sizeof(k))) return false;
    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_bar_type) - (kBarTypeTest + 5));
    std::memcpy(jmp + 1, &rel, 4);
    if (!patch(kBarTypeTest, k, jmp, sizeof(jmp))) return false;
    LOG_INFO("[coop] Invaders see the host's and guests' health bars");
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
        std::wstring host_name = text::widen(steam::persona(g_host));
        for (const auto& p : g_peers) host_name = p.id == g_host ? shown_name(p) : host_name;
        head = (g_invade ? L"Invading " : L"In ") + host_name + L"'s world";
    } else if (g_step == Step::GoingHome) {
        head = L"Returning home...";
    } else {
        head = g_invade ? L"Prepare to Co-op: invading..." : L"Prepare to Co-op: joining...";
    }
    if (world::in_limbo()) head += L"  (limbo)";
    if (spectate::holding()) head += L"  (spectating)";
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
        std::wstring line = shown_name(p);
        if (p.invader) line += L" (invader)";
        if (p.dead) line += L" (dead)";
        line += detail(p.soul_level, p.deaths, p.id == g_host || g_role == Role::Host ? p.ping_ms : -1);
        out.push_back(line);
    }
    return out;
}

}  // namespace coop::session
