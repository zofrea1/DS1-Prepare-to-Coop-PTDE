#pragma once

// The seamless co-op session: finding and joining players with the same password, hosting, the host's world
// rules, following the host's travel, leaving. Runs on the game thread from the per-frame tick.

#include <cstdint>
#include <string>
#include <vector>

namespace coop::session {

struct Config {
    std::string password;
    bool allow_invaders = true;
    bool death_debuffs = false;  // each death in a session adds a Crimson Blossom (up to five)
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
    std::wstring char_name;  // their character's name (matches them to their character in our world)
    double alive_since = 0;  // when they last reported coming back alive
    int gait = -1;           // their walk/run/sprint as they report it (motion::Gait), -1 unknown
    double gait_at = 0;      // when they last reported it
};

void configure(const Config& cfg);
// The tag that keeps the game's own lobbies among players with this password.
std::string game_tag(const std::string& password);
void tick();

// Requests (items, hotkeys). Each does nothing (and says why in the log) when it does not apply.
void host();         // Blessed Eye Orb
void join();         // Crystal Eye Orb
void invade();       // Chaos Eye Orb
void leave();        // Abyssal Eye Orb
void close_world();  // the host's Black Separation Crystal: every other player goes home, the session ends
void cycle_rules();  // Ominous Tome: none -> allies hostile -> teams -> none
void taunt();        // Dried Fingers
void curse_world();  // Cursed Pendant
// The host used a Homeward Bone in a session: the host and every guest go to the host's last bonfire.
void host_homeward();

// Item rules (Remastered Seamless Co-op's): a session is established (we host one or we are in one).
bool in_session();
bool hosting();
bool guest();
bool active();       // any role, including searching and joining
bool idle();         // no role at all (the session items may start one)
bool invader();      // we invade
// Host: when the last Homeward Bone was used (GetTickCount64 seconds; 0 none).
double homeward_time();
void clear_homeward_time();

// The player panel: a heading, then one line per player with the OverheadPlayerDisplay details
// (0 names, 1 ping, 2 soul level, 3 deaths, 4 soul level and ping). Empty outside a session.
std::vector<std::wstring> panel_lines(int mode);
const std::vector<PeerInfo>& peers();

// The host's arrival point for guests: where the host last rested at a bonfire (set on every rest).
void remember_rest_point();

}  // namespace coop::session
