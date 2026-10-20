// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
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
    bool invasion_rewards = true;  // Humanity for the side that wins an invasion
    bool death_debuffs = false;  // each death in a session adds a Crimson Blossom (up to five)
    // A companion's floating health bar: 0 name only, 1 ping, 2 soul level, 3 deaths, 4 soul level and ping.
    int overhead_display = 4;
    int max_players = 4;  // PTDE keeps three remote players (GameDataMan+0xC holds 3 x 0x4C0)
    // Hidden test keys: pretend to speak another protocol / be another version (0 / empty: the real ones).
    int test_protocol = 0;
    std::string test_version;
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
    int ceremony = 0;        // the bonfire animation they report looping (their sit), 0 none
    double anim_at = 0;      // when one of their event animations last arrived
    int event_anim = -1;     // the event animation their own character holds (1.2.0), 0 none, -1 not reported
    double event_free_since = 0;  // since when they have reported holding none (0: they hold one, or unknown)
    int death_events = 0;    // kDead messages about them this session
    bool boss_fight = false; // a boss gauge is shown in their game (1.1.0 players report it)
    bool team_respawn = false;  // their respawn after a team wipe waits for the others' (kTeamRespawn)
    uint32_t respawn = 0;       // where that respawn is for their current death (session.cpp, kRespawnNone ...)
    uint8_t reset = 0;          // their world's reload after a lost boss fight (kResetNone ...), from kResetPhase
    uint32_t reset_rest = 0;    // the rest that phase answers (kRest's id)
    bool went_kept = false;     // their respawn went (respawn) with a reload that keeps the world's dead
    double went_over_at = 0;    // when a respawn of theirs last went with a reload that starts the world over
    uint32_t dead_list_sent = 0;  // the list of our world's dead last sent them while they were down (0 none)
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
void curse_world();  // Cursed Pendant: curses our own world (offline or hosting), or lifts the curse
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

// Kept for the session text. The on-screen box that used to draw these lines is not shown; soul level and
// ping are appended to the name on each companion's floating health bar instead.
std::vector<std::wstring> panel_lines(int mode);
const std::vector<PeerInfo>& peers();
// The game draws a companion's name from the player-data string. This replaces that pointer, while the
// floating bar is updated, with "(name, SL: N, Pms)".
bool install_nameplate();
// An invader sees the floating health bars of the host and its guests, as they see each other's.
bool install_invader_bars();

// The host's arrival point for guests: where the host last rested at a bonfire (set on every rest).
void remember_rest_point();

// Guest: a death in the host's world respawns at this bonfire. `object_id` is the bonfire object
// Util_RequestRegene was given; the respawn id is that plus 1000. Ignored unless we are a co-op guest.
void note_guest_rest(int object_id);
// The same, with an id that is already a respawn bonfire (the host's, on join or when following a Homeward Bone).
void note_guest_respawn(int bonfire);
int guest_respawn_bonfire();

// A team wipe (every co-op player has fallen): the respawn warps wait for each other, so that every game reloads
// its world at once. Asked when our respawn warp starts while we are dead: true holds the warp (world.cpp). Then
// asked every frame: true lets it go.
bool team_respawn_wait();
bool team_respawn_go();
// Our respawn warp goes now (every death, wipe or not): the others stop waiting for it. `kept`: its reload keeps the
// world's dead (deadlist.cpp); the others' later reloads follow it (wipe_reload).
void note_respawn_went(bool kept);
// Our death's reload is a team wipe's, which starts the world over (else it keeps the world's dead). It follows a
// teammate whose respawn went before ours: one whose reload kept the world means ours keeps it, one whose reload
// started it over (during our death, `fell_at`) is part of the same wipe. With none gone, it is a wipe when every
// other co-op player is down.
bool wipe_reload(double fell_at);
// A rest after a lost boss fight (world.cpp): our world's reload is pending, waiting (our menu is open, or a fallen
// player is not at their respawn warp yet) or ready. Every living player whose reload is pending goes once none of
// them still waits (resets_ready), and the fallen's respawn warps wait until none is pending (team_respawn_wait),
// so that every game reloads at once. The others hear each change at once.
constexpr uint8_t kResetNone = 0, kResetWaiting = 1, kResetReady = 2;
void set_reset_phase(uint8_t phase);
// Our reload phase, for the last rest (kResetPhase, reliable): on each change, and once after every rest.
void report_reset_phase();
// No other living co-op player's reload after a lost boss fight still waits (each is ready, gone, or none).
bool resets_ready();
// A co-op player (from 1.1.0 on) whose reload phase for the last rest has not come yet (their name), else empty.
std::string reset_phase_missing();
// Every fallen co-op player (from 1.1.0 on) is at their respawn warp or past it.
bool fallen_at_respawn();
// Someone waits on our pending reload: a fallen co-op player held at their respawn warp, or a living one whose own
// reload is ready.
bool others_wait_on_reset();

// Bonfire travel takes the whole party, so it waits while anyone's game is in the middle of something a load would
// cut short: a boss fight (ours or a standing teammate's), an invasion, a reload after a lost boss fight (ours or a
// teammate's), or a teammate who is down (their respawn goes first, through its own waits). kHoldNone: travel is open.
enum TravelHold : int { kHoldNone = 0, kHoldBossFight, kHoldInvader, kHoldReload, kHoldMateDown };
int travel_hold();
// The line that tells the player why (a hold other than kHoldNone).
const wchar_t* travel_hold_text(int hold);

}  // namespace coop::session
