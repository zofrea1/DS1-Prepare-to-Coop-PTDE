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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace coop::session {

namespace {

// ---------------------------------------------------------------- wire format
constexpr uint32_t kMagic = 0x31435350;  // "PSC1"
constexpr uint32_t kProtocol = 3;

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
    kAnim = 17,        // everyone -> everyone: an event animation we played (bonfire rest, bonfire warp)
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
    int32_t light;  // light SpEffects the player carries (bit mask)
    char name[64];
    wchar_t char_name[17];  // character name: matches the player to their character in our world
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
};
struct GaitMsg {
    Header h;
    int8_t gait;
};
struct AnimMsg {
    Header h;
    uint8_t kind;
    int32_t anim;
    int32_t a2;
    int32_t a3;
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

// ---------------------------------------------------------------- state
enum class Role { None, Host, Guest };
enum class Step {
    Idle,
    HostLobby,     // creating our lobby
    Hosting,
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
std::vector<Pending> g_pending;
std::vector<PeerInfo> g_peers;
int g_ping_ms = -1;
std::vector<std::pair<int, bool>> g_flag_queue;
int g_host_coop_players = 1;
RestPoint g_rest;
// Host: Homeward Bone used (seconds), see tick.
double g_homeward_at = 0;
// Guest: the host's Homeward Bone, warp to this bonfire at g_follow_at.
int g_follow_bonfire = -1;
double g_follow_at = 0;
// Guest bookkeeping.
int g_home_bonfire = -1;
bool g_home_warp_sent = true;  // GoingHome: the warp home was requested (or the game makes its own)
int g_home_map = -1;
int g_host_map = -1;
int g_host_bonfire = -1;
bool g_callbacks_set = false;
// A Black Separation Crystal was used (1 guest, 2 host); handled on the next tick, outside the player's update.
int g_separation = 0;
bool g_anim_log = false;  // animation sync diagnostic lines in the log (session only)
// Which player each other character in our world belongs to (kept while both exist, see update_remote_players).
std::map<uint32_t, uint64_t> g_chr_peer;
// Our gait as last sent (motion sync).
int g_sent_gait = -1;
double g_last_gait = 0;

double now() { return GetTickCount64() / 1000.0; }

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

template <typename T>
void fill_header(T* m, MsgType t) {
    m->h.magic = kMagic;
    m->h.type = t;
}

template <typename T>
bool send_to(uint64_t to, T& msg, bool reliable = true) {
    return steam::send(to, &msg, sizeof(msg), reliable);
}

void broadcast_raw(const void* data, uint32_t size, bool reliable = true, uint64_t except = 0) {
    if (!g_lobby) return;
    const uint64_t me = steam::self();
    for (uint64_t m : steam::lobby_members(g_lobby)) {
        if (m != me && m != except) steam::send(m, data, size, reliable);
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

bool peer_is_invader(uint64_t id) {
    for (const auto& p : g_peers) {
        if (p.id == id) return p.invader;
    }
    return false;
}

void reset_rules() {
    auto& st = rules::state();
    st = rules::State{};
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
    g_homeward_at = 0;
    g_follow_bonfire = -1;
    reset_rules();
    set_step(Step::Idle);
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
        steam::set_lobby_data(lobby, "ptdesc_ver", std::to_string(kProtocol).c_str());
        steam::set_lobby_data(lobby, "ptdesc_key", g_key.c_str());
        steam::set_lobby_data(lobby, "ptdesc_name", steam::persona(steam::self()).c_str());
        steam::set_lobby_data(lobby, "ptdesc_invade", g_cfg.allow_invaders ? "1" : "0");
        steam::set_lobby_data(lobby, "ptdesc_taunt", "0");
        auto& st = rules::state();
        st.active = true;
        st.host = true;
        set_step(Step::Hosting);
        LOG_INFO("[coop] Hosting a seamless session (lobby %016llx)", static_cast<unsigned long long>(lobby));
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
    send_to(p.from, w);
    LOG_INFO("[coop] Answered %s (%s): status %u", steam::persona(p.from).c_str(), p.invade ? "invader" : "guest",
             w.status);
}

void host_tick() {
    if (g_step != Step::Hosting) return;
    host_ensure_game_session();
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
    h.protocol = kProtocol;
    h.invade = g_invade ? 1 : 0;
    h.soul_level = game::soul_level();
    strncpy_s(h.name, steam::persona(steam::self()).c_str(), _TRUNCATE);
    steam::accept(g_host);
    send_to(g_host, h);
    ++g_hello_tries;
    set_step(Step::Hello);
}

void guest_search() {
    set_step(Step::Searching);
    std::vector<std::pair<std::string, std::string>> filters = {{"ptdesc_ver", std::to_string(kProtocol)}};
    if (g_invade) {
        filters.push_back({"ptdesc_invade", "1"});
    } else {
        filters.push_back({"ptdesc_key", g_key});
    }
    steam::find_lobbies(filters, 50, [](std::vector<uint64_t> found) {
        if (g_step != Step::Searching) return;
        const uint64_t me = steam::self();
        found.erase(std::remove_if(found.begin(), found.end(),
                                   [me](uint64_t l) { return steam::lobby_owner(l) == me; }),
                    found.end());
        if (found.empty()) {
            finish(kMsgNoOne);
            return;
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
                finish(kMsgConnectTimeout);
                return;
            }
            g_lobby = lobby;
            g_host = steam::lobby_owner(lobby);
            LOG_INFO("[coop] Entered %s's lobby", steam::persona(g_host).c_str());
            g_hello_tries = 0;
            guest_send_hello();
        });
    });
}

void apply_host_rules(int world_rules, bool infection, bool taunt) {
    auto& st = rules::state();
    st.world_rules = world_rules;
    st.infection = infection;
    st.taunt = taunt;
}

void guest_on_welcome(const Welcome& w) {
    if (g_step != Step::Hello) return;
    if (w.status == kBusy && g_hello_tries < 8) {
        set_step(Step::Hello);
        g_step_since = now() - 17.0;  // retry in 3 s
        return;
    }
    if (w.status != kOk) {
        LOG_INFO("[coop] The host refused us (status %u)", w.status);
        finish(w.status == kFull || w.status == kRefused ? kMsgNoOne : kMsgConnectTimeout);
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
}

void guest_tick() {
    const double in_step = now() - g_step_since;
    switch (g_step) {
        case Step::Searching:
        case Step::LobbyJoin:
            if (in_step > 20.0) finish(g_invade ? kMsgBreakInTimeout : kMsgJoinTimeout);
            break;
        case Step::Hello:
            if (in_step > 20.0) {
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
    broadcast(i, false);
}

std::wstring event_name(const PlayerEvent& e) {
    char name[65];
    std::memcpy(name, e.name, 64);
    name[64] = 0;
    return text::widen(name);
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
    switch (h.type) {
        case kHello: {
            Hello m;
            if (!as(&m) || g_role != Role::Host || g_step != Step::Hosting) return;
            if (m.protocol != kProtocol) {
                Welcome w{};
                fill_header(&w, kWelcome);
                w.status = kVersion;
                send_to(from, w);
                return;
            }
            steam::accept(from);
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
            p->last_seen = now();
            break;
        }
        case kArrived: {
            PlayerEvent m;
            if (!as(&m)) return;
            if (g_role == Role::Host) {
                if (m.who != from) return;
                broadcast(m, true, from);  // tell the other guests
            } else if (from != g_host) {
                return;
            }
            if (m.who == steam::self()) return;
            say(fmt(m.invader ? kMsgInvaded : kMsgJoined, event_name(m)));
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
            for (int i = 0; i < n; ++i) rules::apply_remote_flag(m.f[i].id, m.f[i].on != 0);
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
            if (PeerInfo* p = peer(m.who)) p->dead = true;
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
        case kBye: {
            if (from == g_host && g_role == Role::Guest) {
                guest_go_home(kMsgHostClosed, "the host closed the session");
            } else if (g_role == Role::Host) {
                const PeerInfo* p = peer(from);
                PlayerEvent e = player_event(kLeft, from, p->invader, p->name);
                broadcast(e, true, from);
                say(fmt(p->invader ? kMsgInvaderLeft : kMsgLeft, text::widen(p->name)));
                g_peers.erase(std::remove_if(g_peers.begin(), g_peers.end(),
                                             [from](const PeerInfo& q) { return q.id == from; }),
                              g_peers.end());
            }
            break;
        }
        case kLeft: {
            PlayerEvent m;
            if (from != g_host || g_role != Role::Guest || !as(&m) || m.who == steam::self()) return;
            say(fmt(m.invader ? kMsgInvaderLeft : kMsgLeft, event_name(m)));
            const uint64_t who = m.who;
            g_peers.erase(std::remove_if(g_peers.begin(), g_peers.end(),
                                         [who](const PeerInfo& q) { return q.id == who; }),
                          g_peers.end());
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
            for (const auto& [chr, id] : g_chr_peer) {
                if (id != from) continue;
                motion::Anim a{m.kind, m.anim, m.a2, m.a3};
                static int lines = 0;
                if (++lines <= 200) {
                    LOG_INFO("[coop] %s plays animation %d (%s): shown on their character %08X",
                             steam::persona(from).c_str(), m.anim, m.kind ? "forced" : "event", chr);
                }
                motion::play_remote_anim(chr, a);
            }
            break;
        }
        case kPing: {
            Simple m;
            if (!as(&m)) return;
            m.h.type = kPong;
            send_to(from, m, false);
            break;
        }
        case kPong: {
            Simple m;
            if (!as(&m)) return;
            const uint32_t rtt = GetTickCount() - m.value;
            if (from == g_host) g_ping_ms = static_cast<int>(rtt);
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
                if (member != me && !peer_is_invader(member)) steam::send(member, &m, bytes, true);
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
    s.invader = p->invader;
    s.alive = !p->dead && p->alive_since > 0 && now() - p->alive_since >= 2.0 && now() - p->last_seen <= 5.0;
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

void set_callbacks() {
    if (g_callbacks_set) return;
    g_callbacks_set = true;
    world::set_rest_handler(&send_rest);
    world::set_death_handler(&send_death);
    rules::set_flag_listener(&queue_flag);
    rules::set_bonfire_warp_listener(&send_bonfire_warp);
    rules::set_bonfire_lit_listener(&send_bonfire_lit);
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
            for (const auto& p : g_peers) {
                if (p.id == user) say(fmt(p.invader ? kMsgInvaderLeft : kMsgLeft, text::widen(p.name)));
            }
            g_peers.erase(std::remove_if(g_peers.begin(), g_peers.end(),
                                         [user](const PeerInfo& q) { return q.id == user; }),
                          g_peers.end());
        }
    });
    steam::on_p2p_failed([](uint64_t peer_id, int error) {
        LOG_INFO("[coop] P2P with %016llx failed (%d)", static_cast<unsigned long long>(peer_id), error);
    });
}

}  // namespace

std::string game_tag(const std::string& password) { return "g" + password_key(password); }

void configure(const Config& cfg) {
    g_cfg = cfg;
    g_cfg.max_players = std::max(2, std::min(4, g_cfg.max_players));
    g_key = password_key(g_cfg.password);
}

void tick() {
    if (!steam::ready()) return;
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
    update_remote_players();
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
                m.a2 = a.a2;
                m.a3 = a.a3;
                broadcast(m);
                static int lines = 0;
                if (++lines <= 200) LOG_INFO("[coop] Sent our animation %d to the others", a.anim);
            }
        }
        motion::clear_remote_gaits();
        if (g_role != Role::None && !game::loading()) {
            for (const auto& [chr, id] : g_chr_peer) {
                for (const auto& p : g_peers) {
                    if (p.id != id || p.invader) continue;
                    const bool fresh = p.gait >= 0 && now() - p.gait_at < 1.5;
                    motion::set_remote_gait(chr, fresh ? static_cast<motion::Gait>(p.gait) : motion::kUnknown);
                }
            }
        }
    }
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
            for (const auto& p : g_peers) {
                if (now() - p.last_seen > 5.0 || p.light <= 0) continue;
                lights.push_back({{p.pos[0], p.pos[1], p.pos[2]}, p.light});
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
    Simple bye{};
    fill_header(&bye, kBye);
    broadcast(bye);
    if (g_role == Role::Host) {
        rules::state().leaving = true;
        if (game::session_state() != 0) game::leave_session();
        finish(L"");
        LOG_INFO("[coop] Session disbanded");
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
    g_last_state = 0;  // tell the guests now
}

void taunt() {
    if (!hosting()) return;
    rules::state().taunt = true;
    steam::set_lobby_data(g_lobby, "ptdesc_taunt", "1");
    LOG_INFO("[coop] Invaders are taunted into this world");
    g_last_state = 0;
}

void curse_world() {
    if (!hosting()) return;
    rules::state().infection = true;
    LOG_INFO("[coop] The world is cursed (pure black World Tendency)");
    g_last_state = 0;
}

void host_homeward() {
    if (!hosting()) return;
    g_homeward_at = now();
}

bool in_session() {
    if (g_role == Role::Host) return g_step == Step::Hosting;
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

std::vector<std::wstring> panel_lines(int mode) {
    std::vector<std::wstring> out;
    if (g_role == Role::None) return out;
    std::wstring head;
    if (g_role == Role::Host) {
        head = g_step == Step::Hosting ? L"Seamless co-op: hosting" : L"Seamless co-op: creating session...";
    } else if (g_step == Step::InWorld) {
        head = (g_invade ? L"Invading " : L"In ") + text::widen(steam::persona(g_host)) + L"'s world";
    } else if (g_step == Step::GoingHome) {
        head = L"Returning home...";
    } else {
        head = g_invade ? L"Seamless co-op: invading..." : L"Seamless co-op: joining...";
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
