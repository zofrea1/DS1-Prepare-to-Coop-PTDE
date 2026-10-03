#include "session.h"

#include "game.h"
#include "hook.h"
#include "log.h"
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
constexpr uint32_t kProtocol = 1;

enum MsgType : uint8_t {
    kHello = 1,    // guest/invader -> host: please let me in
    kWelcome = 2,  // host -> joiner: the session invitation and where to appear
    kState = 3,    // host -> all, every second
    kInfo = 4,     // everyone -> everyone, every second (overhead display, messages)
    kWarp = 5,     // host -> guests: I travelled, follow me to this bonfire
    kRest = 6,     // anyone -> all: rested at a bonfire, the world resets
    kBye = 7,      // leaving the session
    kPing = 8,
    kPong = 9,
    kDead = 10,    // died
    kFlags = 11,   // event flags changed in the session's world (guest -> host -> guests)
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
    uint8_t misfortune;    // Pendant of Misfortune is active
    uint8_t allow_invaders;
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
    int32_t light;  // light SpEffect the player carries (-1 none)
    char name[64];
};
struct WarpMsg {
    Header h;
    int32_t bonfire;
    int32_t map;
};
struct Simple {
    Header h;
    uint32_t value;
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
std::map<uint64_t, uint32_t> g_ping_sent;
int g_ping_ms = -1;
std::vector<std::pair<int, bool>> g_flag_queue;
bool g_taunt = false;
int g_host_coop_players = 1;
bool g_host_misfortune = false;
// Guest bookkeeping.
int g_home_bonfire = -1;
int g_home_map = -1;
int g_host_map = -1;
int g_host_bonfire = -1;
int g_last_map = -1;
bool g_was_loading = false;
bool g_callbacks_set = false;

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

template <typename T>
void broadcast(T& msg, bool reliable = true) {
    if (!g_lobby) return;
    const uint64_t me = steam::self();
    for (uint64_t m : steam::lobby_members(g_lobby)) {
        if (m != me) steam::send(m, &msg, sizeof(msg), reliable);
    }
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
    reset_rules();
    set_step(Step::Idle);
}

// ---------------------------------------------------------------- host
void host_open_lobby() {
    set_step(Step::HostLobby);
    steam::create_lobby(g_cfg.max_players, [](bool ok, uint64_t lobby) {
        if (!ok) {
            finish(L"Could not open a co-op session (Steam lobby).");
            return;
        }
        g_lobby = lobby;
        steam::set_lobby_data(lobby, "ptdesc", "1");
        steam::set_lobby_data(lobby, "ptdesc_ver", std::to_string(kProtocol).c_str());
        steam::set_lobby_data(lobby, "ptdesc_key", g_key.c_str());
        steam::set_lobby_data(lobby, "ptdesc_name", steam::persona(steam::self()).c_str());
        steam::set_lobby_data(lobby, "ptdesc_invade", g_cfg.allow_invaders ? "1" : "0");
        auto& st = rules::state();
        st.active = true;
        st.host = true;
        set_step(Step::Hosting);
        say(L"Seamless co-op session open. Players with your password can join.");
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

void host_answer(const Pending& p) {
    Welcome w{};
    fill_header(&w, kWelcome);
    w.status = kOk;
    int players = 1;
    for (const auto& peer_info : g_peers) players += peer_info.invader ? 0 : 1;
    if (!p.invade && players >= g_cfg.max_players) w.status = kFull;
    if (p.invade && !g_cfg.allow_invaders) w.status = kRefused;
    // No invasions in the Undead Asylum or Firelink Shrine (as in ds1sc).
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
        game::Vec3 pos{};
        float angle = 0;
        game::position(&pos, &angle);
        w.map = game::map_id();
        w.pos[0] = pos.x;
        w.pos[1] = pos.y;
        w.pos[2] = pos.z;
        w.angle = angle;
        w.bonfire = game::last_bonfire();
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
    // The host travelled (a load): guests follow to the host's bonfire.
    const bool loading = game::loading();
    if (g_was_loading && !loading && game::in_game()) {
        const int map = game::map_id();
        if (map != g_last_map && g_last_map != -1) {
            WarpMsg m{};
            fill_header(&m, kWarp);
            m.bonfire = game::last_bonfire();
            m.map = map;
            broadcast(m);
            LOG_INFO("[coop] Host arrived in map %08X; guests follow to bonfire %d", map, m.bonfire);
        }
        g_last_map = map;
    }
    if (!loading && game::in_game()) g_last_map = game::map_id();
    g_was_loading = loading;
    if (now() - g_last_state > 1.0) {
        g_last_state = now();
        StateMsg s{};
        fill_header(&s, kState);
        s.map = game::map_id();
        s.bonfire = game::last_bonfire();
        s.players = static_cast<uint8_t>(game::member_count());
        int coop = 1;
        for (const auto& p : g_peers) {
            if (now() - p.last_seen > 10.0) continue;
            if (p.invader) {
                s.invader_present = 1;
            } else {
                ++coop;
            }
        }
        s.coop_players = static_cast<uint8_t>(coop);
        s.misfortune = world::misfortune() ? 1 : 0;
        s.allow_invaders = g_cfg.allow_invaders ? 1 : 0;
        rules::state().invader_present = s.invader_present != 0;
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
            finish(g_invade ? L"No world to invade was found." : L"No co-op session with your password was found.");
            return;
        }
        uint64_t pick = g_invade ? found[GetTickCount() % found.size()] : found.front();
        if (g_invade) {
            for (uint64_t l : found) {
                if (steam::lobby_data(l, "ptdesc_taunt") == "1") {
                    pick = l;
                    break;
                }
            }
        }
        set_step(Step::LobbyJoin);
        steam::join_lobby(pick, [](bool ok, uint64_t lobby) {
            if (g_step != Step::LobbyJoin) return;
            if (!ok) {
                finish(L"Could not enter the co-op session.");
                return;
            }
            g_lobby = lobby;
            g_host = steam::lobby_owner(lobby);
            say(L"Found " + text::widen(steam::persona(g_host)) + L"'s world. Joining...");
            g_hello_tries = 0;
            guest_send_hello();
        });
    });
}

void guest_on_welcome(const Welcome& w) {
    if (g_step != Step::Hello) return;
    if (w.status == kBusy && g_hello_tries < 8) {
        set_step(Step::Hello);
        g_step_since = now() - 17.0;  // retry in 3 s
        return;
    }
    if (w.status != kOk) {
        finish(w.status == kFull ? L"That world is full." : L"The host could not take you in.");
        return;
    }
    if (!game::in_game() || !game::own_world() || game::loading()) {
        finish(L"Joining needs you in your own world.");
        return;
    }
    const int type = game::chr_type();
    if (type != 0 && type != 8) {
        finish(L"Joining needs you in your own body (not a phantom).");
        return;
    }
    auto& st = rules::state();
    st.active = true;
    st.guest = true;
    st.invader = g_invade;
    st.own_chr_type = type;
    g_home_bonfire = game::last_bonfire();
    g_home_map = game::map_id();
    g_host_map = w.map;
    g_host_bonfire = w.bonfire;
    const game::Vec3 pos{w.pos[0], w.pos[1], w.pos[2]};
    if (!game::join_from_invitation(w.invitation, w.invitation_size, w.map, pos, w.angle)) {
        finish(L"The session invitation could not be used.");
        return;
    }
    // Deaths in the host's world respawn at the host's last bonfire (the guest has not rested there yet).
    if (w.bonfire >= 0 && !g_invade) *ptr<int>(game::game_man() + game::kGmLastBonfire) = w.bonfire;
    set_step(Step::Joining);
    LOG_INFO("[coop] Joining %s: map %08X bonfire %d, own type %d, home bonfire %d",
             steam::persona(g_host).c_str(), w.map, w.bonfire, type, g_home_bonfire);
}

void guest_go_home(const std::wstring& why) {
    say(why);
    auto& st = rules::state();
    st.leaving = true;
    if (game::session_state() != 0) game::leave_session();
    game::set_join_step(0);
    if (g_lobby) steam::leave_lobby(g_lobby);
    g_lobby = 0;
    if (!game::own_world() && game::in_game()) {
        const uint32_t gm = game::game_man();
        if (g_home_bonfire >= 0) {
            *ptr<int>(gm + game::kGmLastBonfire) = g_home_bonfire;
            world::warp_now(game::bonfire_map(g_home_bonfire), -1);
        } else {
            world::warp_now(g_home_map, -1);
        }
        if (st.own_chr_type >= 0) *ptr<int>(game::game_data_man() + 0x60) = st.own_chr_type;
        set_step(Step::GoingHome);
    } else {
        finish(L"");
    }
}

void guest_tick() {
    const double in_step = now() - g_step_since;
    switch (g_step) {
        case Step::Searching:
        case Step::LobbyJoin:
            if (in_step > 20.0) finish(L"Searching for a session timed out.");
            break;
        case Step::Hello:
            if (in_step > 20.0) {
                if (g_hello_tries < 8) {
                    guest_send_hello();
                } else {
                    finish(L"The host did not answer.");
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
                finish(L"Joining the host's session failed.");
            }
            break;
        }
        case Step::Loading:
            if (game::session_state() != 6 && in_step > 5.0) {
                guest_go_home(L"Lost the host while joining. Returning home.");
            } else if (game::join_step() == 4 && game::in_game() && !game::own_world()) {
                set_step(Step::InWorld);
                say(L"Joined " + text::widen(steam::persona(g_host)) + L"'s world.");
            } else if (in_step > 180.0) {
                guest_go_home(L"Joining took too long. Returning home.");
            }
            break;
        case Step::InWorld:
            if (game::session_state() != 6 && !game::loading()) {
                if (g_invade) {
                    // The game sends a finished invader home itself (its death or the host's); give it time.
                    say(L"The invasion is over.");
                    rules::state().leaving = true;
                    if (g_lobby) steam::leave_lobby(g_lobby);
                    g_lobby = 0;
                    set_step(Step::GoingHome);
                } else {
                    guest_go_home(L"The session with " + text::widen(steam::persona(g_host)) +
                                  L" ended. Returning home.");
                }
            }
            break;
        case Step::GoingHome:
            if (g_invade && in_step > 10.0 && game::in_game() && !game::own_world() && !game::loading()) {
                g_invade = false;  // the game did not send us home: do it ourselves
                guest_go_home(L"Returning home.");
                break;
            }
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

void guest_on_warp(const WarpMsg& m) {
    if (g_step != Step::InWorld || m.bonfire < 0) return;
    g_host_bonfire = m.bonfire;
    *ptr<int>(game::game_man() + game::kGmLastBonfire) = m.bonfire;
    if (game::map_id() == m.map) return;
    say(L"Following " + text::widen(steam::persona(g_host)) + L"...");
    world::warp_now(game::bonfire_map(m.bonfire), -1);
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
    i.dead = world::local_dead() ? 1 : 0;
    i.light = world::local_lights();
    strncpy_s(i.name, steam::persona(steam::self()).c_str(), _TRUNCATE);
    broadcast(i, false);
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
            g_host_misfortune = m.misfortune != 0;
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
            p->dead = m.dead != 0;
            p->light = m.light;
            p->last_seen = now();
            break;
        }
        case kRest: {
            if (g_role != Role::None) {
                say(text::widen(peer(from)->name) + L" rested at a bonfire. The world is restored.");
                world::remote_rest();
            }
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
            if (from_guest) {
                // pass a guest's progress on to the other guests
                const uint64_t me = steam::self();
                for (uint64_t member : steam::lobby_members(g_lobby)) {
                    if (member != me && member != from) steam::send(member, data, size, true);
                }
            }
            break;
        }
        case kDead: {
            // The host fell while we invade: the game's own invader victory (HostDead) runs here.
            if (from == g_host && g_invade && g_step == Step::InWorld) {
                LOG_INFO("[coop] The host died: invasion victory");
                game::call_lua("HostDead");
            }
            break;
        }
        case kWarp: {
            WarpMsg m;
            if (from == g_host && as(&m)) guest_on_warp(m);
            break;
        }
        case kBye: {
            if (from == g_host && g_role == Role::Guest) {
                guest_go_home(text::widen(steam::persona(g_host)) + L" closed the session. Returning home.");
            } else if (g_role == Role::Host) {
                const PeerInfo* p = peer(from);
                say(text::widen(p->name) + L" left.");
                g_peers.erase(std::remove_if(g_peers.begin(), g_peers.end(),
                                             [from](const PeerInfo& q) { return q.id == from; }),
                              g_peers.end());
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
    Simple r{};
    fill_header(&r, kRest);
    r.value = static_cast<uint32_t>(game::last_bonfire());
    broadcast(r);
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
                bool invader = false;
                for (const auto& p : g_peers) invader |= p.id == member && p.invader;
                if (member != me && !invader) steam::send(member, &m, bytes, true);
            }
        } else if (g_host) {
            steam::send(g_host, &m, bytes, true);
        }
    }
}

void set_callbacks() {
    if (g_callbacks_set) return;
    g_callbacks_set = true;
    world::set_rest_handler(&send_rest);
    rules::set_flag_listener(&queue_flag);
    steam::on_lobby_member([](uint64_t lobby, uint64_t user, bool entered) {
        if (lobby != g_lobby) return;
        if (entered) {
            steam::accept(user);
            if (g_role == Role::Host) LOG_INFO("[coop] %s entered the lobby", steam::persona(user).c_str());
            return;
        }
        if (g_role == Role::Guest && user == g_host) {
            guest_go_home(text::widen(steam::persona(g_host)) + L" left. Returning home.");
        } else if (g_role == Role::Host) {
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
    if (g_role == Role::Host) host_tick();
    if (g_role == Role::Guest) guest_tick();
    rules::tick();
    {
        const auto& st = rules::state();
        world::set_session(st.active, st.invader, st.invader_present);
        int coop = 1;
        bool all_dead = true;
        int others = 0;
        if (g_role == Role::Host) {
            coop = g_host_coop_players;
        } else if (g_role == Role::Guest && g_step == Step::InWorld) {
            coop = g_host_coop_players;
            world::set_misfortune(g_host_misfortune);
        }
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
    // The host's death reaches invaders (co-op guests are not sent home by it).
    static bool host_was_dead = false;
    const bool dead = world::local_dead();
    if (g_role == Role::Host && dead && !host_was_dead) {
        Simple d{};
        fill_header(&d, kDead);
        const uint64_t me = steam::self();
        for (const auto& p : g_peers) {
            if (p.invader && p.id != me) send_to(p.id, d);
        }
    }
    host_was_dead = dead;
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
        say(L"Already in a co-op session.");
        return;
    }
    if (!game::in_game() || !game::own_world()) {
        say(L"Open a co-op session from your own world.");
        return;
    }
    g_role = Role::Host;
    g_last_map = game::map_id();
    host_open_lobby();
}

void join() {
    if (g_role != Role::None) {
        say(L"Already in a co-op session.");
        return;
    }
    if (!game::in_game() || !game::own_world()) {
        say(L"Join from your own world.");
        return;
    }
    g_role = Role::Guest;
    g_invade = false;
    say(L"Searching for a co-op session...");
    guest_search();
}

void invade() {
    if (g_role != Role::None) {
        say(L"Already in a session.");
        return;
    }
    if (!game::in_game() || !game::own_world()) {
        say(L"Invade from your own world.");
        return;
    }
    g_role = Role::Guest;
    g_invade = true;
    say(L"Searching for a world to invade...");
    guest_search();
}

void leave() {
    if (g_role == Role::None) {
        say(L"Not in a co-op session.");
        return;
    }
    Simple bye{};
    fill_header(&bye, kBye);
    broadcast(bye);
    if (g_role == Role::Host) {
        rules::state().leaving = true;
        if (game::session_state() != 0) game::leave_session();
        finish(L"Co-op session closed.");
        return;
    }
    if (g_step == Step::InWorld || g_step == Step::Loading) {
        guest_go_home(L"Leaving the session. Returning home.");
    } else {
        rules::state().leaving = true;
        if (game::session_state() != 0) game::leave_session();
        finish(L"Stopped joining.");
    }
}

void toggle_invaders() {
    if (g_role != Role::Host || !g_lobby) {
        say(L"Only the host of a session can change its rules.");
        return;
    }
    g_cfg.allow_invaders = !g_cfg.allow_invaders;
    steam::set_lobby_data(g_lobby, "ptdesc_invade", g_cfg.allow_invaders ? "1" : "0");
    if (!g_cfg.allow_invaders) {
        g_taunt = false;
        steam::set_lobby_data(g_lobby, "ptdesc_taunt", "0");
    }
    say(g_cfg.allow_invaders ? L"Invaders may now enter this world." : L"Invaders can no longer enter this world.");
}

void taunt() {
    if (g_role != Role::Host || !g_lobby) {
        say(L"Only the host of a session can call invaders.");
        return;
    }
    if (!g_cfg.allow_invaders) {
        say(L"This world does not allow invaders (use the Tome of World Rules).");
        return;
    }
    g_taunt = true;
    steam::set_lobby_data(g_lobby, "ptdesc_taunt", "1");
    say(L"Invaders will seek out this world.");
}

void misfortune() {
    if (g_role != Role::Host) {
        say(L"Only the host of a session can draw misfortune on the world.");
        return;
    }
    world::set_misfortune(true);
    say(L"The enemies of this world grow stronger until the next rest.");
}

std::vector<std::wstring> panel_lines(int mode) {
    std::vector<std::wstring> out;
    if (g_role == Role::None) return out;
    std::wstring head;
    if (g_role == Role::Host) {
        head = L"Seamless co-op: hosting";
    } else if (g_step == Step::InWorld) {
        head = (g_invade ? L"Invading " : L"In ") + text::widen(steam::persona(g_host)) + L"'s world";
    } else if (g_step == Step::GoingHome) {
        head = L"Returning home...";
    } else {
        head = L"Seamless co-op: connecting...";
    }
    if (world::in_limbo()) head += L"  (limbo)";
    out.push_back(head);
    auto detail = [mode](int sl, int deaths, int ping) {
        wchar_t b[96] = L"";
        switch (mode) {
            case 1: swprintf_s(b, L"  %d ms", ping); break;
            case 2: swprintf_s(b, L"  SL %d", sl); break;
            case 3: swprintf_s(b, L"  deaths %d", deaths); break;
            case 4: swprintf_s(b, L"  SL %d  %d ms", sl, ping); break;
            default: break;
        }
        if (ping < 0 && (mode == 1 || mode == 4)) {
            std::wstring w(b);
            const size_t at = w.find(std::to_wstring(ping) + L" ms");
            if (at != std::wstring::npos) w.replace(at, std::to_wstring(ping).size() + 3, L"-- ms");
            return w;
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

bool hosting() { return g_role == Role::Host; }
bool guest() { return g_role == Role::Guest; }
bool active() { return g_role != Role::None; }
const std::vector<PeerInfo>& peers() { return g_peers; }

}  // namespace coop::session
