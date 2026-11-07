// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The seamless co-op session: finding and joining players with the same password, hosting, the host's world
// rules, following the host's travel, leaving. Runs on the game thread from the per-frame tick.

#include <cstdint>
#include <string>
#include <vector>

namespace coop::session {

// The most players a session can hold (MaxPlayers): the game's own is 4 (players.cpp makes room for more).
constexpr int kMaxSessionPlayers = 8;

struct Config {
    std::string password;
    bool allow_invaders = true;
    bool invasion_rewards = true;  // Humanity for the side that wins an invasion
    int invasion_cooldown_s = 180; // InvasionCooldown: after an invasion of our world, no other invader for this long
    // ExtraInvaders (1.6.0): our world takes at most its co-op players (the host and the guests) plus this many invaders
    // at once; 1 at the least.
    int extra_invaders = 1;
    bool seal_boss_fogs = false;   // SealBossFogsDuringInvasions: the boss fogs stay shut while an invader is here
    // Enemy health, damage, posture, boss health, damage, posture scaling (percent per extra player): the host's
    // go to its guests (world::set_host_scaling).
    int scaling[6] = {40, 0, 25, 115, 0, 40};
    bool death_debuffs = false;  // each death in a session adds a Crimson Blossom (up to five)
    bool bloody_mary = true;     // 1.5.0: each co-op death gives everyone a Bloody Mary (the host's setting decides)
    bool vicinity_scaling = true;  // 1.5.0: each enemy scaled for the co-op players whose games run it (the host's)
    // A companion's floating health bar: 0 name only, 1 ping, 2 soul level, 3 deaths, 4 soul level and ping, 5 soul
    // level, ping and the smoothing their character is drawn with.
    int overhead_display = 5;
    int max_players = 4;  // MaxPlayers (2 .. kMaxSessionPlayers), no more than the game holds (players.cpp)
    int net_rate = 6;     // NetworkTickRate: the session's updates a second (netrate.h), sent to the others
    bool share_items = true;  // ShareDroppedItems (itemshare.cpp): the host's decides for its session
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
    double pos_at = 0;       // when pos and map last came (Info, every second)
    int ping_ms = -1;        // their own round trip to their host, as they report it (Info)
    int rtt_ms = -1;         // our own round trip to them (kPing / kPong, 1.4.0), -1 none yet
    double rtt_at = 0;       // when it was last measured
    int rtt_min = -1;        // the lowest and highest since player_net_watch last read them (-1 none)
    int rtt_max = -1;
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
    uint32_t hurt_list_sent = 0;  // the list of our world's hurt enemies last sent them while they were down
    int bones = -1;               // the Homeward Bones they hold, as they report it (1.3.0), -1 not reported
    bool disguised = false;       // Chameleon is on them (1.3.0): their floating health bar is hidden
    int max_players = 0;          // the players their game can hold (1.4.0, MaxPlayers), 0 not reported (4)
    double arrived_at = 0;        // host: when they last arrived in our world (kArrived), 0 not yet
    // The floor collision their own game has them on (1.5.0, InfoMore5): its map and its place in that map's
    // collisions, the same in every game; -1 none (loading) or not reported. When it came.
    int floor_map = -1;
    int floor_index = -1;
    double floor_at = 0;
    std::string mod;              // their Prepare to Co-op version, from their hello (a host's guests and invaders)
};

void configure(const Config& cfg);
// The tag that keeps the game's own lobbies among players with this password.
std::string game_tag(const std::string& password);
void tick();

// Requests (items, hotkeys). Each does nothing (and says why in the log) when it does not apply.
void host();         // Blessed Eye Orb
void join();         // Crystal Eye Orb
void invade();       // Chaos Eye Orb; while hosting (1.6.0), the whole party invades together (again: calls it off)
// The Chaos Eye Orb can lead our party into another world now: we host, and no invader is in our world (1.6.0). Why
// not when it cannot go is said on use (a guest of an older version, a boss fight, a teammate down, ...).
bool group_invade_open();
void leave();        // Abyssal Eye Orb
void close_world();  // the host's Black Separation Crystal: every other player goes home, the session ends
void cycle_rules();  // Ominous Tome: none -> allies hostile -> teams -> none
void taunt();        // Dried Fingers
void curse_world();  // Cursed Pendant: curses our own world (offline or hosting), or lifts the curse
// A Homeward Bone used in a session (one bone from every player), the Darksign (every soul and all humanity from every
// player, taken when the trip goes) or the Homeward miracle (no cost but the cast): the host and every guest go to the
// host's last bonfire, whoever used it. The host's own takes the travel slot at once and goes on the next tick; false
// (and the host told why) when no trip can go now. A co-op guest's asks the host's game (kHomewardAsk), which checks
// and goes the same way (after its own menu closes) or tells the guest why not; true once asked.
enum HomewardKind { kHomeBone, kHomeDarksign, kHomeMiracle };
bool homeward(HomewardKind kind);
// We may start that trip: we host, or we are a co-op guest in the world of a host that takes guests' (1.3.0).
bool homeward_open();
// Host: a trip of the party's started less than 5 s ago (its load begins frames later) or a Homeward trip waits to go:
// no other trip can start (homeward and a guest's travel are refused). Guest: our Homeward ask is still open (the
// host's game has not answered it, or its trip waits for the host's menu). Either way the Homeward miracle is closed.
bool trip_slot_busy();

// Item rules (Remastered Seamless Co-op's): a session is established (we host one or we are in one).
bool in_session();
// Whose world we are in: our own Steam id at home, the host's in theirs (0 when not known).
uint64_t world_owner();
bool hosting();
bool guest();
// Summon signs (1.5.0): a co-op player's Vigil Soapstone in a session leaves one where they stand, one per player
// (a new one replaces the last); the Death Knell can take anyone else there (knell.cpp).
struct SummonSign {
    uint64_t owner = 0;
    std::wstring name;  // the owner's character name
    int map = -1;       // area << 24 | block << 16
    float pos[3] = {};
    float angle = 0;
    int floor_map = -1;  // the floor its owner stood on (rules::floor_by_id), -1 not known
    int floor_index = -1;
};
// The other co-op players' signs (not ours).
std::vector<SummonSign> summon_signs();
// The Death Knell's view shows this player's sign (0: none): it glows gold here instead of white.
void set_knell_sign(uint64_t owner);
// Our Vigil Soapstone: our sign where we stand, in place of the last. Null when placed, else why not.
const char* place_summon_sign();
// Why the Vigil Soapstone cannot be used now (null: it can).
const char* sign_refusal();
// The Death Knell took us somewhere: a Bloody Mary for the session (the host counts it; a guest's game tells the
// host's). `sign`: to a summon sign. Added: our count went up (host); Asked: the host's game was told, and decides
// (knell_bloody_mary_answer); None: nothing (five already, no session). Counted with BloodyMary off too: the setting
// is about deaths, and a host of 1.5.0 counts a guest's knell either way.
enum class KnellMary { None, Added, Asked };
KnellMary knell_bloody_mary(bool sign);
// After Asked: 1 once the host's count went up, -1 when it did not within a few seconds (or nothing was asked), 0 still
// waiting.
int knell_bloody_mary_answer();
// VicinityScaling: how many other co-op players' games run the enemy with this net handle (their kNear lists).
int near_count(uint32_t handle);
// We stand in the host's world as a co-op guest: arrived, not invading, not on the way home.
bool coop_guest_in_world();
bool active();       // any role, including searching and joining
bool idle();         // no role at all (the session items may start one)
bool invader();      // we invade
// Another player's character here: 1 when that player's game has said they fell (their death message or status),
// 0 when it says they live, -1 when the character is not matched to a player. `name` gets the player's name.
int remote_player_down(uint32_t chr, std::string* name = nullptr);
// The updates a second our game sends (netrate.h): the host's NetworkTickRate in its session (for a guest or an
// invader, as the host's state says, once in the host's world; a host of 1.2.0 or older says nothing, which leaves the
// game's own rate).
int net_rate();
// The boss fogs stay shut for the host and the guests while an invader is in the world: the host's
// SealBossFogsDuringInvasions (a guest takes it from the host's state; a host of 1.2.0 or older says nothing: open).
bool seal_boss_fogs();

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
// The Darksign is held back only by a lost boss fight's reload that is pending (it takes everyone to their bonfires
// anyway): not by a boss fight, an invader (it sends them home) or a teammate who is down.
bool darksign_held();
// A lost boss fight's reload is pending: ours, or a living co-op teammate's.
bool reload_hold();
// What holds the host's Homeward Bone back: travel's holds, except a boss fight (the bone is the way out of one, as
// in single player; a teammate who fell in it no longer holds it either).
int bone_hold();
// The line that tells the player why (a hold other than kHoldNone).
const wchar_t* travel_hold_text(int hold);

}  // namespace coop::session
