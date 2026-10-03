#pragma once

// The seamless co-op session: finding and joining players with the same password, hosting, following the
// host, leaving. Runs on the game thread from the per-frame tick.

#include <cstdint>
#include <string>
#include <vector>

namespace coop::session {

struct Config {
    std::string password;
    bool allow_invaders = true;
    int max_players = 4;  // PTDE keeps three remote players (GameDataMan+0xC holds 3 x 0x4C0)
};

struct PeerInfo {
    uint64_t id = 0;
    std::string name;
    int soul_level = 0;
    int deaths = 0;
    int map = -1;
    float pos[3] = {};
    int ping_ms = -1;
    bool invader = false;
    bool dead = false;
    int light = -1;
    double last_seen = 0;
};

void configure(const Config& cfg);
// The tag that keeps the game's own lobbies among players with this password.
std::string game_tag(const std::string& password);
void tick();

// Requests (items, hotkeys).
void host();
void join();
void invade();
void leave();
void toggle_invaders();
void taunt();
void misfortune();

// The player panel: a heading, then one line per player with the OverheadPlayerDisplay details
// (0 names, 1 ping, 2 soul level, 3 deaths, 4 soul level and ping). Empty outside a session.
std::vector<std::wstring> panel_lines(int mode);

bool hosting();
bool guest();
bool active();
const std::vector<PeerInfo>& peers();

}  // namespace coop::session
