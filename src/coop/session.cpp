// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "session.h"

#include "deadlist.h"
#include "drops.h"
#include "game.h"
#include "hook.h"
#include "items.h"
#include "knell.h"
#include "log.h"
#include "motion.h"
#include "netrate.h"
#include "players.h"
#include "party.h"
#include "partylist.h"
#include "rules.h"
#include "spectate.h"
#include "steam.h"
#include "itemshare.h"
#include "summons.h"
#include "talk.h"
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
#include <set>
#include <random>

namespace coop::session {

namespace {

// ---------------------------------------------------------------- wire format
constexpr uint32_t kMagic = 0x31435350;  // "PSC1"
// Players of the same protocol play together (see same_version). 1.0.0 to 1.4.0 all speak 8 and play together: the
// messages are the same, 1.1.0's hello only adds its version at the end. Bump it when a release can no longer play
// with the older ones; the version notices then tell the players why they cannot connect. 1.5.0 speaks 9: its
// scaling by vicinity, the Bloody Mary and the Death Knell's price need every game of the session to have them, so
// every player updates (the user's decision, 2026-11-06). 1.1.0 to 1.4.0 compare only the protocol (their
// kRequireSameVersion is false too): an older invader that finds only 1.5.0 worlds is told "Can't invade: these
// worlds have Prepare to Co-op 1.5.0" without knocking, an older guest is refused (kVersion) and told "Can't join",
// and the host is told who tried. 1.0.0 knocks (it finds us by "ptdesc_ver") and only says "Failed to connect".
constexpr uint32_t kProtocol = 9;
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
// invaders read, and make the world kFingersWeight times as likely to be picked as one without (pick_invade_world):
// 150% more likely since 1.7.0 (65% from 1.4.0, 50% before; the invader's game decides, so an older invader still uses
// its own). While they are in effect InvasionCooldown is off (taunt, invasion_over), until the session ends and the
// rules reset.
constexpr double kFingersWeight = 2.5;
// Level matching (1.7.0): a preference, never a filter, since there are too few players for a filter. A host
// publishes the soul levels of its world's co-op players ("ptdesc_levels": "45,52,60"). An invader's pick weighs each
// world found by how close its closest player is to the invader: a player within the invader's level range (from 10%
// below the invader's soul level to 20 + 10% above it, the range the original game's Red Eye Orb used) gives the
// world the whole pull, kLevelPull times as likely as a world far from it; the pull fades with every level outside the
// range (by e^-1 every kLevelFade, at least 10% of the invader's level). Dried Fingers multiply on top of it. A world
// that publishes no levels (a host before 1.7.0) gets kLevelUnknown, between near and far. Only the worlds that can
// take an invader now are weighed (cooldown, boss fight and invader limit are passed over first), so a single world
// is still invaded as before. A group invasion (1.6.0) uses its highest soul level, so a strong player cannot bring
// low-level friends to reach low-level worlds.
constexpr double kLevelPull = 8.0;
constexpr double kLevelUnknown = 2.5;
constexpr double kLevelFade = 10.0;
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
    kDeadList = 22,    // 1.1.0: my world's dead characters, to a fallen teammate (host -> guest, guest -> host)
    kResetPhase = 23,  // 1.1.0, everyone -> everyone: my reload after a lost boss fight, for this rest (ResetPhase)
    kHostWorld = 24,   // 1.2.0, host -> co-op guests: what the host's world has (key items, Lordvessel, warp bonfires)
    kTravel = 25,      // 1.2.0, guest -> host: my bonfire warp, take everyone to this bonfire (TravelMsg)
    kTravelAnswer = 26,  // 1.2.0, host -> that guest: the travel waits for the host's menu, or why it cannot go
    kNpcPhantoms = 27,   // 1.3.0, host -> guests and invaders: the NPC phantoms in the host's party (summons.cpp)
    kHurtList = 28,      // 1.3.0: my world's hurt enemies, to a fallen teammate (as kDeadList; deadlist.cpp)
    kSentHome = 29,      // 1.3.0, host -> an invader: the host's Darksign sends you home (no outcome)
    kWorldGlobals = 30,  // 1.3.0, host -> a co-op guest: the host's world state below the map flags (flags 0-1999)
    kAnimDone = 31,      // 1.3.0, anyone -> all (through the host): an event's animation on a character I move ended
    kHomewardAsk = 32,   // 1.3.0, co-op guest -> host: my Homeward Bone, Darksign or Homeward miracle (Simple: its
                         // HomewardKind); the host's trip takes everyone (a host whose StateMore4 says it takes them)
    kNpcTalk = 33,       // 1.3.0, anyone -> all (through the host): an NPC says a line in my game, or stops (talk.cpp)
    kEventLead = 34,     // 1.4.0, anyone -> all (through the host): my game moves this character through an event's
                         // animation until it ends (EventLeadMsg; rules.cpp, lead_event_anim)
    kNpcSummons = 35,    // 1.4.0, co-op guest -> host: the NPC phantoms my own game summoned (NpcPhantomsMsg); the
                         // host takes each into its party as its own (the world's) once that NPC is in its world, and
                         // names them in its list (kNpcPhantoms) meanwhile
    kItemDrop = 36,      // 1.4.0, co-op player -> all (through the host): an item I dropped lies on your floor too
                         // (ItemDropMsg; itemshare.cpp), and the host's list to a guest who arrives
    kItemClaim = 37,     // 1.4.0, co-op guest -> host: I would pick up this shared drop (ItemKeyMsg)
    kItemTaken = 38,     // 1.4.0, host -> co-op guests: this shared drop is that player's (ItemTakenMsg); off the floor
    kBonfires = 39,      // 1.4.0, host -> co-op guests: our bonfires' kindling (world::BonfirePoints each), to a guest
                         // who arrived and to all whenever it changes; a guest's game takes it while in our world
    kBonfireKindle = 40, // 1.4.0, co-op guest -> host: I kindled this bonfire of your world (BonfireMsg)
    kBossSouls = 41,     // 1.4.0, co-op player -> all (through the host): my game paid a boss's souls
                         // (BossSoulsMsg), for a player whose game did not run the boss's defeat (world.cpp)
    kSign = 42,          // 1.5.0, co-op player -> all (through the host): my summon sign (SignMsg), on a change and
                         // every kSignResend; one not heard of for kSignLapse is gone
    kKnellPrice = 43,    // 1.5.0, co-op guest -> host: my Death Knell's price, a Bloody Mary for the session
    kNear = 44,          // 1.5.0, co-op player -> everyone: the enemies my game runs (VicinityScaling; a u16 count, then
                         // that many net handles), on a change and every kNearResend; one not heard again for
                         // kNearLapse counts for nothing
    // 1.6.0, group invasions (the section "group invasions" below). Older games ignore them: a group goes only with
    // players of 1.6.0 or later, and only into the world of a host of 1.6.0 or later (which keeps the invader limit).
    kGroupSearch = 45,   // a host -> its co-op guests: our search for a world the whole party can invade began or
                         // ended without one (GroupSearchMsg)
    kGroupAsk = 46,      // a group's host, in another world's lobby beside its own -> that world's host: are there places
                         // for these invaders? (GroupAskMsg)
    kGroupAnswer = 47,   // that host -> the asker: a WelcomeStatus (GroupAnswerMsg); kOk holds the places kGroupHoldFor
    kGroupGo = 48,       // the group's host -> its co-op guests: we invade that world together; home first (GroupGoMsg)
    kGroupOff = 49,      // the group's host -> the group: its own knock failed, the invasion is off; we gather again
    kGroupBack = 50,     // the group's host -> the group, home again: our world is open, this lobby (GroupBackMsg); lobby 0
                         // while the host is still out (joining, invading, on its way home): keep waiting
    kGroupHere = 51,     // a player of the group -> its host, until back in the host's world: still out, keep sending
    kGroupComing = 52,   // 1.7.0, host -> its co-op guests: a group of `value` invaders is coming in (kMsgGroupComing)
    kKnellSpy = 53,      // 1.7.0, an invader -> the host (-> that guest): my Death Knell watches you (KnellSpyMsg)
};
// Older games ignore message types they do not know: a 1.1.0 host never answers a travel request (the guest is
// told after kTravelWait), and a 1.1.0 guest neither gets the host's keys nor asks to travel.

// kBossFight (1.1.0): a co-op guest waits for the host's or a guest's boss fight to end (1.0.0 takes it as a refusal);
// from 1.3.0 an invader of 1.3.0 or later gets it too (older invaders get kRefused). kCooldown (1.3.0, invaders of
// 1.3.0 or later): the world was invaded lately (InvasionCooldown), WelcomeMore says for how long still.
enum WelcomeStatus : uint32_t { kOk = 0, kFull = 1, kBusy = 2, kRefused = 3, kVersion = 4, kBossFight = 5, kCooldown = 6 };
// The first version whose invaders know kBossFight and kCooldown.
constexpr const char* kRefusalsVersion = "1.3.0";
// From 1.4.0 a co-op guest holds back its own flags until its copy of our world is ours (copy_hold_tick).
constexpr const char* kHoldsCopyVersion = "1.4.0";

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
// 1.3.0, behind Welcome as StateMore is behind StateMsg: an older joiner reads the Welcome part, and an older host's
// Welcome leaves wait_s 0.
struct WelcomeMore {
    Welcome w;
    uint32_t wait_s;  // kCooldown: the seconds until the world takes invaders again
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
// 1.3.0, behind StateMsg as InfoMore is behind Info: an older guest reads the StateMsg part.
struct StateMore {
    StateMsg state;
    uint8_t net_rate;  // the host's NetworkTickRate: the updates a second every game of the session sends
};
// Behind StateMore the same way: a guest of an earlier 1.3.0 reads StateMore, and such a host's StateMore leaves the
// fogs open.
struct StateMore2 {
    StateMore more;
    uint8_t seal_fogs;  // the host's SealBossFogsDuringInvasions: 1 the boss fogs stay shut while an invader is here
};
// And behind StateMore2: the host's enemy and boss scaling (EnemyHealthScaling ... BossPostureScaling), which its
// guests use in its world. A host without it leaves each guest its own.
struct StateMore3 {
    StateMore2 more2;
    int16_t scaling[6];
};
// And behind StateMore3: the host takes its co-op guests' Homeward Bones, Darksign and Homeward miracle (kHomewardAsk).
struct StateMore4 {
    StateMore3 more3;
    uint8_t guest_homeward;
};
// 1.5.0, behind StateMore4: the session's Bloody Marys (0..5), the host's count. An older host leaves it 0.
struct StateMore5 {
    StateMore4 more4;
    uint8_t bloody;
};
// 1.5.0, behind StateMore5: 1 while the host's VicinityScaling is on for its session (host_vicinity): each enemy is
// scaled for the co-op players whose games run it (kNear). An older host leaves it 0: every enemy for the session.
struct StateMore6 {
    StateMore5 more5;
    uint8_t vicinity;
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
// and an older player's shorter Info leaves these at their defaults. InfoMore2 (1.2.0) adds to it the same way: a
// 1.1.0 player reads InfoMore, and a 1.1.0 player's InfoMore leaves event_anim unknown.
struct InfoMore {
    Info info;
    uint8_t boss_fight;    // a boss gauge is shown in this player's game (the host keeps invaders out meanwhile)
    uint8_t team_respawn;  // this player's respawn after a team wipe waits for the others' (kTeamRespawn)
    uint8_t respawn;       // where that respawn is for the death this player reports (kRespawnNone ...)
    uint8_t reset;         // this player's world's reload after a lost boss fight (kResetNone ..., world.cpp)
    int32_t boss;          // the boss on this player's gauge (its entity), 0 none: its fight began in every game
                           // that holds it, also where the fight's own runs stop short (rules::note_teammate_boss)
};
struct InfoMore2 {
    InfoMore more;
    int32_t event_anim;  // the event animation the sender's own character holds (motion::held_event_anim), 0 none
};
// event_anim 2 (no animation of the game's): the sender is in a cutscene (1.5.0). Their position and map do not go
// together meanwhile: the crow's flight out of the Asylum put the player at Firelink's landing point 26 s before
// their map changed, and a Death Knell to them loaded into the Asylum there and fell through it (2026-11-04). Older
// players take it for an event animation they hold, as they should.
constexpr int32_t kReportCutscene = 2;
// event_anim 3 (none of the game's either): the sender is in a load and has no character (1.5.0). It used to report
// 0 then, and a living load (a Homeward Bone, bonfire travel, the load after the crow's flight) counted as standing
// with no event animation: 0.3 s into it the others' games took the sender for settled and made their missing
// character during the load, one that was then kept (Grok dcb9359e). Older players take it for a held event animation.
constexpr int32_t kReportLoading = 3;
// 1.3.0, behind InfoMore2 the same way.
struct InfoMore3 {
    InfoMore2 more2;
    int32_t bones;  // the Homeward Bones the sender holds (each one the host uses takes one from every player)
};
struct InfoMore4 {
    InfoMore3 more3;
    uint8_t disguised;  // Chameleon is on the sender (SpEffect 1500): the others hide their floating health bar
    uint8_t max_players;  // 1.4.0: the players the sender's game can hold (MaxPlayers, players.cpp); 0 = the game's 4
    uint8_t spare[2];
};
// 1.5.0, behind InfoMore4 the same way: the floor collision the sender's game has them on, as its map and its place
// among that map's collisions (rules::floor_id; the same in every game, which loads the same map file). A game that
// spectates them across a map boundary has no floor of its own for their character there (its physics never touched
// that map's collisions, which were not loaded): with this it draws and streams the world from where they stand.
struct InfoMore5 {
    InfoMore4 more4;
    int32_t floor_map;    // -1 none
    int32_t floor_index;
};
struct BonfireMsg {
    Header h;
    int32_t bonfire;
};
// kHomeward: BonfireMsg (all that 1.2.0 reads and sends) and what the trip costs every guest (1.3.0).
struct HomewardMsg {
    Header h;
    int32_t bonfire;
    uint8_t darksign;  // the Darksign's trip: every soul and all humanity are lost
    uint8_t bone;      // a Homeward Bone's trip: one bone goes from every player
};
struct Simple {
    Header h;
    uint32_t value;
};
// kBossSouls: the souls my game paid, and the defeat flag of the boss they were for.
struct BossSoulsMsg {
    Header h;
    uint32_t souls;
    int32_t flag;
};
struct SignMsg {
    Header h;
    uint64_t owner;
    uint32_t serial;   // a new one for each sign the owner places
    int32_t map;       // area << 24 | block << 16
    float pos[3];
    float angle;
    uint8_t active;    // 0: the owner has no sign any more
};
// 1.5.0 (the Death Knell's view on a summon sign): the floor its owner stood on, named as kInfoMore5 names a floor (map
// and place among that map's collisions), so a game that loads the sign's map for the view finds the floor under it
// as it finds a far teammate's. After the whole of SignMsg: a game before reads that and no more; one without these
// fields (a shorter message, or no tag) has none.
constexpr uint32_t kSignFloorTag = 0x524C4653;  // "SFLR"
struct SignFloorMsg {
    SignMsg m;
    int32_t floor_map;    // -1 none
    int32_t floor_index;
    uint32_t tag;         // kSignFloorTag
};
struct KnellPriceMsg {
    Header h;
    uint8_t sign;  // the bell took them to a summon sign
};
// kRest's value is the rest's id (1.1.0: random, set by the one who rests and passed on by the host; 1.0.0: their
// bonfire, or 0 passed on by a 1.0.0 host). Each player's reload phase is sent for the rest it answers.
struct ResetPhase {
    Header h;
    uint32_t rest;
    uint8_t phase;
};
// kHostWorld: what the host's world has, sent when it changes and to each co-op guest who arrives. A guest carries
// there the key items of their own that the host holds too (items::travel_key), and its warp list offers the
// bonfires of the host's world.
constexpr int kHostKeysMax = 64;  // the inventory's key range holds 64
struct HostWorldMsg {
    Header h;
    uint8_t lordvessel;  // the host owns the Lordvessel (flag 710): bonfire travel is open in that world
    uint32_t warps;      // bit i: the host has rested at the warp list's i-th bonfire (rules::own_warps)
    uint8_t keys;
    int32_t key[kHostKeysMax];
};
// kNpcPhantoms: sent when the host's NPC phantoms change and every few seconds while there are any (a player who
// comes in later gets them too). Each is named by its entity id, which every game shares.
constexpr int kNpcPhantomsMax = 4;
struct NpcPhantomsMsg {
    Header h;
    uint8_t count;
    struct {
        int32_t entity;
        uint32_t handle;  // its character handle in the host's game (packet 48 names it so)
        int32_t kind;     // its character type as a phantom
        int32_t f1, f2;   // its party entry's data
    } p[kNpcPhantomsMax];
};
struct TravelMsg {
    Header h;
    int32_t bonfire;  // the warp's point (WarpNextStage_Bonfire's argument)
    uint8_t spot;     // the guest's Warp row was that of a place that travels without the Lordvessel (flag 717)
};
enum TravelAnswer : uint32_t {
    kTravelWaiting = 1,    // the host has a menu open: the travel goes when it closes
    kTravelBossFight = 2,  // someone is fighting a boss
    kTravelNoVessel = 3,   // the host's world has no Lordvessel yet
    kTravelNotThere = 4,   // the host has not rested at that bonfire
    kTravelBusy = 5,       // the host is loading, has fallen or is away
    kTravelTimedOut = 6,   // the host's menu stayed open
    kTravelInvader = 7,    // an invader is in the world (bonfires are out of use)
    kTravelMateDown = 8,   // a teammate is down: their respawn goes first
    kTravelReload = 9,     // a game reloads after a lost boss fight
    kTravelNoBone = 10,    // (1.3.0, to a guest's Homeward Bone) a co-op player holds no Homeward Bone
};
// kTravelAnswer with kTravelNoBone (1.3.0): the answer and the co-op player who holds no Homeward Bone (the host's own
// id when it is the host), so that the guest's screen names them.
struct TravelAnswerMsg {
    Simple simple;
    uint64_t who;
};
struct PlayerEvent {
    Header h;
    uint64_t who;
    uint8_t invader;
    char name[64];  // the name to show: a character name from 1.1.0 on (1.0.0 puts Steam names here)
    uint8_t lost;  // kLeft: the host dropped them after their connection went quiet; kDead of an invader (1.1.0):
                   // this death lost the invasion (the co-op team's reward)
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
struct AnimDoneMsg {
    Header h;
    int32_t entity;  // the character's entity id
    int32_t anim;    // the animation the event waited for (rules.cpp, follow_wait)
};
struct EventLeadMsg {
    Header h;
    uint32_t handle;  // the character's net handle (chr+0x78), the same in every game
    int32_t entity;   // its entity id if the sender knows it, else -1 (log)
    int32_t anim;
    uint8_t start;    // 1 the move began, 0 it is over
    uint64_t leader;  // the game that moves it (Steam id); the host relays it
};
struct ItemDropMsg {
    Header h;
    uint64_t origin;  // who dropped it: the drop is (origin, seq) in every game
    uint32_t seq;
    int32_t map;      // where it lies (area << 24 | block << 16)
    uint8_t entry[itemshare::kEntryBytes];  // the item manager's entry (the item, its count and durability, the spot)
};
struct ItemKeyMsg {
    Header h;
    uint64_t origin;
    uint32_t seq;
};
struct ItemTakenMsg {
    Header h;
    uint64_t origin;
    uint32_t seq;
    uint64_t taker;  // the player who has it now
};
struct NpcTalkMsg {
    Header h;
    int32_t talk;    // the NPC's talk id (its MSB part's; not its entity id)
    int32_t line;    // the TalkParam row the line began with
    int32_t row;     // the TalkParam row it says now (the line, or a later page of it)
    int32_t a2;      // TalkToPlayer's other two arguments
    int32_t a3;
    uint8_t kind;    // talk::Kind: a page, the line cut short, or a line begun
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
// 1.4.0, behind Hello: an older host copies the Hello part and ignores the rest.
struct HelloMore {
    Hello hello;
    uint8_t max_players;  // the players the joiner's game can hold (MaxPlayers); an older joiner's game holds 4
    uint8_t spare[3];
};
// 1.6.0, group invasions.
constexpr int kGroupMax = kMaxSessionPlayers;
struct GroupSearchMsg {
    Header h;
    uint8_t searching;  // 1 the search began, 0 it ended without a world
    uint8_t size;       // the group's players
};
struct GroupAskMsg {
    Header h;
    uint32_t protocol;
    char version[16];
    uint8_t max_players;      // the least any game of the group holds (MaxPlayers)
    uint8_t count;
    uint64_t ids[kGroupMax];  // the group's players, its host first
};
// kKnellSpy: the co-op player an invader's Death Knell view shows.
struct KnellSpyMsg {
    Header h;
    uint64_t target;
    uint64_t watcher;  // the invader (the host passes it on as it came)
};
struct GroupAnswerMsg {
    Header h;
    uint32_t status;  // a WelcomeStatus
    uint32_t wait_s;  // kCooldown: the seconds until the world takes invaders again
    uint8_t places;   // the invaders the world took before this answer held any
};
struct GroupGoMsg {
    Header h;
    uint64_t lobby;  // the world the group invades (its host holds a place for each of `ids`)
    uint8_t count;
    uint64_t ids[kGroupMax];
    uint64_t host;   // that world's host: each of the group accepts their session at once (steam::accept)
};
struct GroupBackMsg {
    Header h;
    uint64_t lobby;  // the group's host's world, open again
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
// A group invasion (1.6.0) is coming in: its first player's knock was let in (1.7.0, kGroupComing).
constexpr const wchar_t* kMsgGroupComing = L"Beware! A band of {} evil spirits is breaking into this world!";
// An invader's Death Knell view is on us (1.7.0, kKnellSpy; at most once every kKnellSpyEvery).
constexpr const wchar_t* kMsgKnellSpied = L"Your Death Knell tolls uneasily... an invader is spying on you!";
constexpr const wchar_t* kMsgLeft = L"{} has left";
constexpr const wchar_t* kMsgInvaderLeft = L"Evil spirit {} has left";
constexpr const wchar_t* kMsgLost = L"{} lost the connection";
constexpr const wchar_t* kMsgInvaderLost = L"Evil spirit {} lost the connection";
constexpr const wchar_t* kMsgHostLost = L"Lost the connection to the host. Returning to your world...";
constexpr const wchar_t* kMsgRulesNone = L"World rules: allies cannot harm each other";
constexpr const wchar_t* kMsgRulesHostile = L"World rules: allies can harm each other";
constexpr const wchar_t* kMsgRulesTeams = L"World rules: the host against the guests";
constexpr const wchar_t* kMsgRulesFreeForAll = L"World rules: free for all";
constexpr const wchar_t* kMsgRulesNeedsNew = L"Free for all needs every player on 1.4.0 or newer";
// The first version that knows free for all (world rules 3): an older game takes it for no rules at all.
constexpr const char* kFreeForAllVersion = "1.4.0";
constexpr const wchar_t* kMsgTaunt = L"Invaders are taunted into this world";
constexpr const wchar_t* kMsgCursed = L"This world is cursed";
constexpr const wchar_t* kMsgCurseLifted = L"The curse on this world is lifted";
constexpr const wchar_t* kMsgCurseFaded = L"The curse on this world has faded";  // a curse of a previous game
constexpr const wchar_t* kMsgDied = L"{} has died";
constexpr const wchar_t* kMsgInvaderDied = L"Evil spirit {} has died";
constexpr const wchar_t* kMsgGuestStart = L"Joining another world as a guest...";
constexpr const wchar_t* kMsgGuestMission = L"Joined another world!";
constexpr const wchar_t* kMsgHostWarpOut = L"The power of the homeward bone consumes you...";
constexpr const wchar_t* kMsgDarksignOut = L"The Darksign consumes your souls and humanity...";
constexpr const wchar_t* kMsgMiracleOut = L"The power of Homeward consumes you...";
constexpr const wchar_t* kMsgBoneBy = L"{}'s Homeward Bone: its power consumes you...";
constexpr const wchar_t* kMsgDarksignBy = L"{}'s Darksign consumes your souls and humanity...";
constexpr const wchar_t* kMsgMiracleBy = L"{}'s Homeward consumes you...";
constexpr const wchar_t* kMsgHomewardHostMenu = L"{} wants to go home: close the menu to take everyone";
constexpr const wchar_t* kMsgNoBone = L"Everyone needs a Homeward Bone: {} has none";
constexpr const wchar_t* kMsgNoBoneAnyone = L"Everyone needs a Homeward Bone";
constexpr const wchar_t* kMsgDarksignInvader = L"The Darksign was used in this world. Returning to your world...";
constexpr const wchar_t* kMsgDarksignSends = L"The Darksign sends the invader away";
constexpr const wchar_t* kMsgBreakInStart = L"Invading another world as an evil spirit";
constexpr const wchar_t* kMsgBreakInMission = L"Invaded another world! Defeat {} players";
constexpr const wchar_t* kMsgBreakInSuccess = L"Invasion success! Returning to your world";
constexpr const wchar_t* kMsgBreakInFailed = L"Invasion failed... Returning to your world";
constexpr const wchar_t* kMsgRewardInvader = L"Invasion reward: {} Humanity";
constexpr const wchar_t* kMsgRewardTeam = L"The invader is defeated. Reward: {} Humanity";
constexpr const wchar_t* kMsgRewardTeamMany = L"The invaders are defeated. Reward: {} Humanity";
constexpr const wchar_t* kMsgHostTimeout = L"Failed to create session";
constexpr const wchar_t* kMsgJoinTimeout = L"Failed to find session";
constexpr const wchar_t* kMsgBreakInTimeout = L"Failed to find session to invade";
constexpr const wchar_t* kMsgConnectTimeout = L"Failed to connect";
constexpr const wchar_t* kMsgNoOne = L"No sessions found";
constexpr const wchar_t* kMsgBossFight = L"That world is in a boss fight. Try again when it is over";
constexpr const wchar_t* kMsgCooldown = L"That world was invaded recently. Invaders can enter again in {}";
constexpr const wchar_t* kMsgWorldFull = L"That world is full";
constexpr const wchar_t* kMsgWorldBusy = L"That world's host is loading or has fallen. Try again in a moment";
constexpr const wchar_t* kMsgNoInvaders = L"That world takes no invaders right now";
constexpr const wchar_t* kMsgNoWorldInvade = L"No world can take an invader right now: {}";
constexpr const wchar_t* kMsgNoWorldJoin = L"No world can take you right now: {}";
constexpr const wchar_t* kMsgNextOpens = L"The next one takes invaders again in {}";
constexpr const wchar_t* kMsgJoinRetry = L"The connection fell through. Trying again...";
constexpr const wchar_t* kMsgTryAnother = L"Trying another world...";
constexpr const wchar_t* kMsgReturnHome = L"Returning to your world...";
constexpr const wchar_t* kMsgHostClosed = L"The host closed their world. Returning to your world...";
constexpr const wchar_t* kMsgWorldClosed = L"Your world is closed. Guests return to their worlds";
constexpr const wchar_t* kMsgWorldOpen = L"Your world is open";
constexpr const wchar_t* kMsgTeamRespawn = L"Waiting for the other players...";  // a team wipe (1.1.0)
constexpr const wchar_t* kMsgMateBossFight = L"{} is fighting a boss: no resting or travel until it is decided";
constexpr const wchar_t* kMsgTravelAsked = L"{} is taking everyone to a bonfire";
constexpr const wchar_t* kMsgTravelHostMenu = L"{} wants to travel: close the menu to take everyone";
constexpr const wchar_t* kMsgTravelFollowMenu = L"The others are travelling: close the menu to follow";
constexpr const wchar_t* kMsgTravelFollowSit = L"The others are travelling: leave the bonfire to follow";
constexpr const wchar_t* kMsgTravelWaiting = L"Waiting for {} to close the menu...";
constexpr const wchar_t* kMsgTravelNoVessel = L"The host's world has no Lordvessel yet: no travel";
constexpr const wchar_t* kMsgTravelNotThere = L"The host has not rested at that bonfire yet";
constexpr const wchar_t* kMsgTravelBusy = L"The host cannot travel right now";
constexpr const wchar_t* kMsgTripBusy = L"No travel right now";
constexpr const wchar_t* kMsgTravelTimedOut = L"The host did not close the menu: no travel";
constexpr const wchar_t* kMsgTravelOld = L"The host's game cannot travel for guests (Prepare to Co-op 1.2.0 can)";
constexpr const wchar_t* kMsgKeysLent = L"Key items the host also holds are with you in this world";
constexpr const wchar_t* kMsgFellJoining = L"You fell before reaching the other world: the join is called off";
// 1.6.0, group invasions.
constexpr const wchar_t* kMsgGroupSearch = L"Looking for a world your group of {} can invade...";
constexpr const wchar_t* kMsgGroupSearchSolo = L"Looking for a world to invade. Your world stays open";
constexpr const wchar_t* kMsgGroupSearchMate = L"{} is looking for a world to invade with everyone";
constexpr const wchar_t* kMsgGroupNoneMate = L"{} found no world your group can invade";
constexpr const wchar_t* kMsgGroupNone = L"No world can take your group right now";
constexpr const wchar_t* kMsgGroupNoneWhy = L"No world can take your group right now: {}";
constexpr const wchar_t* kMsgGroupNoneSolo = L"No world can take an invader right now";
constexpr const wchar_t* kMsgGroupNoneSoloWhy = L"No world can take an invader right now: {}";
constexpr const wchar_t* kMsgGroupCalledOff = L"The search for a world to invade is called off";
constexpr const wchar_t* kMsgGroupNotNow = L"You cannot invade from your world right now";
constexpr const wchar_t* kMsgGroupInvader = L"Not while an invader is in your world";
constexpr const wchar_t* kMsgGroupBoss = L"Not during a boss fight";
constexpr const wchar_t* kMsgGroupTravel = L"Not while the party travels";
constexpr const wchar_t* kMsgGroupFallen = L"Not while you are fallen";
constexpr const wchar_t* kMsgGroupOld = L"{} needs Prepare to Co-op 1.6.0 to invade together";
constexpr const wchar_t* kMsgGroupWait = L"Wait until {} stands in your world";
constexpr const wchar_t* kMsgGroupGoLeader = L"Your group invades another world. Your world closes for now";
constexpr const wchar_t* kMsgGroupGoSolo = L"Invading another world. Your world closes for now";
constexpr const wchar_t* kMsgGroupGoMember = L"Your group invades another world. Going home first...";
constexpr const wchar_t* kMsgGroupJoining = L"Joining your group's invasion...";
constexpr const wchar_t* kMsgGroupOff = L"Your group's invasion is called off";
constexpr const wchar_t* kMsgGroupBack = L"Your world is open again: your group is coming back";
constexpr const wchar_t* kMsgGroupBackSolo = L"Your world is open again";
constexpr const wchar_t* kMsgGroupWaiting = L"Waiting for {} to open their world again...";
constexpr const wchar_t* kMsgGroupReturning = L"Returning to {}'s world...";
constexpr const wchar_t* kMsgGroupNotBack = L"{} did not open their world again";
constexpr const wchar_t* kMsgGroupGathering = L"Your group is still on its way back";
constexpr const wchar_t* kMsgGroupIncoming = L"Not while invaders are on their way to your world";
constexpr const wchar_t* kMsgGroupOffInvader = L"An invader came into your world: the search is called off";
constexpr const wchar_t* kMsgGroupOffIncoming = L"Invaders are on their way to your world: the search is called off";

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
    bool knows_refusals = false;  // 1.3.0 or later (kRefusalsVersion): an invader is told kBossFight and kCooldown
    int max_players = 4;          // what the joiner's game can hold (HelloMore)
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
// Our death and the event animation we hold, as our last report said: a change of either goes out at once, and once
// more a moment later in case that one is lost, not with the next report of the second (1.5.0; see kPeerAliveFor).
bool g_info_dead = false;
int g_info_event = 0;
int g_info_floor_map = -1;  // our floor as our last report gave it (a change goes out within kFloorReportGap)
int g_info_floor_index = -1;
constexpr double kFloorReportGap = 0.25;
double g_info_again_at = 0;
double g_last_ping = 0;
int g_hello_tries = 0;
int g_search_waves = 0;
int g_search_gen = 0;
// Guest: the other worlds the search found, tried in turn when the one picked cannot take us (try_next_world).
std::vector<uint64_t> g_worlds;
int g_world_tries = 0;   // worlds tried for this search
int g_join_gen = 0;      // the current lobby join (an answer for an earlier one is stale)
bool g_last_busy = false;  // the host's last answer was "busy"
// Guest: why the worlds of this search could not take us (their answers, and the worlds an invader's search passed
// over for InvasionCooldown), for the line shown when none could (finish_search).
struct Refusals {
    int cooldown = 0, boss = 0, full = 0, busy = 0, refused = 0;
    int quiet = 0;  // a group's ask that had no answer in kGroupAskWait (not "loading or fallen": they never said)
    double open_at = 0;  // the soonest a world on cooldown takes invaders again (now())
};
Refusals g_refusals;
// Guest: this world's join has had its one retry (retry_join).
bool g_join_retried = false;
// Join timings (1.6.0, the user's: every wait of the mod's own between a search or a knock and the arrival is about a
// quarter of what it was, for guests and invaders alike). What the game's own join takes (its steps and its load) is
// the game's. A busy host is asked four times as often, with as much patience in all when there is no other world.
constexpr double kSearchRetryAfter = 2.0;  // a lobby list that has not come is asked for again (was 8 s)
constexpr double kSearchGiveUp = 5.0;      // no lobby list at all (was 20 s)
constexpr double kLobbyJoinGiveUp = 5.0;   // a world's lobby that does not let us in: the next world (was 20 s)
constexpr double kHelloWait = 5.0;         // a knock with no answer is sent again (was 20 s)
constexpr double kBusyRetry = 0.75;        // a host that answered busy is asked again (was 3 s)
constexpr double kJoiningGiveUp = 12.0;    // the game's join that never gets under way (was 40 s)
constexpr double kLoadingGrace = 1.5;      // the game's session gone this long into the load: the join ended (was 5 s)
constexpr double kInvaderHomeWait = 2.5;   // the game's own trip home for a finished invader, before ours (was 10 s)
constexpr double kPendingBusy = 5.0;       // host: a knock waiting for our game's session is answered busy (was 20 s)
bool g_join_seen = false;  // the game's session left state 0 since our join was handed to it (Step::Joining)
double g_join_leave_seen = 0;  // Step::Loading: since when the game has been leaving our join at home (0 not)
std::vector<Pending> g_pending;
std::vector<PeerInfo> g_peers;
// Host: the players it let in (a Welcome with kOk). Steam passes on packets from anyone in the lobby, so the host
// takes world changes only from these. Kept apart from g_peers, which also holds players not let in.
std::vector<uint64_t> g_admitted;
// Host: each co-op guest's own NPC phantoms (their kNpcSummons), until they leave.
std::map<uint64_t, std::vector<summons::Phantom>> g_guest_npcs;
// Host: InvasionCooldown. The invaders that arrived in our world (kArrived) until they leave it; the end of an
// invasion, whichever way the invader goes (set_admitted), closes the world to invaders until g_invade_open_at. It
// outlives the session: a host who closes the world and opens it again is still on cooldown.
std::vector<uint64_t> g_invaders_in;
double g_invade_open_at = 0;
// A player whose connection went quiet is dropped (nothing heard for kLostQuiet s after Steam reported the link
// lost, or for kLostSilent s in any case): the host tells the others and stops sending to them; a guest goes
// home. Nothing is counted while this game itself was not running (a machine asleep, a long stall).
constexpr double kLostQuiet = 10.0;
constexpr double kLostSilent = 60.0;
std::map<uint64_t, double> g_p2p_failed;  // when Steam last reported the link to a player lost
std::vector<uint64_t> g_lost;             // host: dropped players, until they ask to join again (kDropped)
double g_host_heard = 0;                  // guest: the last message from the host (not counting kPong)
double g_quiet_grace_until = 0;           // no drops before this (we were not running)
int g_ping_ms = -1;  // guest: our own round trip to the host (what our Info reports)
// Our round trips. Every kPingEvery s each player times each other player of the session (kPing, answered by kPong
// with the ping's number, which every version sends back as it came), and their floating health bar shows the newest.
// Until 1.4.0 a guest timed its host alone, every 2 s with one ping in flight, and every other bar showed that
// player's own ping to their host from its Info, sent once a second: a figure 2 to 3 s old on the host's screen, and
// between two guests not the round trip between them at all (each game sends its player's updates to every other).
constexpr double kPingEvery = 0.25;
struct PingOut {
    uint32_t seq = 0;
    uint64_t to = 0;
    int64_t qpc = 0;
};
constexpr uint32_t kPingSlots = 128;  // 7 other players at 4 a second: an answer up to 4.5 s late still matches
PingOut g_pings[kPingSlots];
uint32_t g_ping_seq = 0;
std::vector<std::pair<int, bool>> g_flag_queue;
int g_host_coop_players = 1;
RestPoint g_rest;
// Host: a Homeward Bone or the Darksign used (seconds, 0 none), its trip still to go (host_tick). It holds the
// travel slot from the use on (start_homeward).
double g_homeward_at = 0;
HomewardKind g_homeward_kind = kHomeBone;
uint64_t g_homeward_bumped = 0;  // the guest whose waiting travel it replaced (answered if the trip cannot go)
uint64_t g_homeward_by = 0;      // the co-op guest who used it (kHomewardAsk), 0 ourselves
double g_homeward_told = 0;      // when our menu holding up that guest's trip was last pointed out (0 not yet)
bool g_host_homeward = false;    // guest: the host takes our Homeward Bones, Darksign and miracle (StateMore4)
bool g_host_vicinity = false;    // guest: the host's VicinityScaling is on for its session (StateMore6)
bool host_vicinity();
double g_homeward_asked = 0;     // guest: when our Homeward ask went to the host's game (0: none open; homeward_asked)
bool g_homeward_waits = false;   // guest: the host's game said that ask waits for its menu (kTravelWaiting)
// Guest: the host's Homeward Bone, warp to this bonfire at g_follow_at.
int g_follow_bonfire = -1;
double g_follow_at = 0;
// Guest: the host's Homeward Bone came while we were loading (no character to take ours from): taken once we stand.
bool g_bone_owed = false;
int g_host_net_rate = 0;  // guest: the host's NetworkTickRate (StateMore), 0 not heard
bool g_host_seal_fogs = false;  // guest: the host's SealBossFogsDuringInvasions (StateMore)
bool g_host_scaling = false;    // guest: the host's scaling is in use (world::set_host_scaling)
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
// When a cutscene of ours was last loading or playing. Its end moves us into its map in place (0xEE21F0 ->
// 0xDE36E0: the crow lands, the player runs a few steps, no loading screen; then the event's "In Firelink..." tip),
// which need not be the same frame. The guest set off the crow and skipped first; the host skipped later, and the
// follow warp went the frame the host's cutscene ended: a loading screen to Firelink's bonfire, no landing, no tip,
// the rest of the crow's event cut off. Afterwards the host's talk with the Crestfallen Warrior turned no page by
// itself (the voice never reported its end, 0x6D13F0) until the host pressed on, while the guest's game, moved by
// the cutscene, played the same talk normally (2026-10-29); the cut-short ending is the one difference found. A
// cutscene of ours into the map now has kCutMoveGrace after its end to move us; the warp is only for one that never
// does (the 2026-10-13 crow, whose arrival point was not loaded in the host's game).
double g_cut_busy_at = 0;
bool g_cut_end_told = false;  // the end of our cutscene was logged for this follow
constexpr double kCutMoveGrace = 20.0;
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
// The session member record's name (the Steam name, cut to 16 characters) of each player whose character is matched
// above: what the game writes on their floating health bar (nameplate_for), whatever Steam tells us of the name.
std::vector<std::pair<std::wstring, uint64_t>> g_member_names;
// Key items of a co-op guest in the host's world. Every load in another player's world empties the key array the
// inventory uses there (MoveMapStep 0xEFC6C3 -> 0x51C520 with clear 1), so a guest arrives without keys, and a death
// or a travel there takes the keys gained in that world as well. Keys gained there are written into the own world's
// array too (items::keep_key_for_home) and are put back after such a load. A guest also carries the keys of their
// own that the host's world has (kHostWorld): a guest who had the Big Pilgrim's Key at home got no second one when
// the Asylum Demon fell in a world without it (their own lot flag was taken), while the host got theirs
// (2026-10-19 test). Keys gained there that are gone after the trip home are given back.
std::vector<int32_t> g_keys_arrival;  // held when we arrived in the host's world
std::vector<int32_t> g_keys_seen;     // as last logged there
std::vector<int32_t> g_keys_gained;   // gained in the host's world this visit (put back after a load there)
// The Crimson Blossom is a key item (goodsType 1), so the key poll took a guest's Blossom for a key gained in the
// host's world: it was written into the own world's array for home and listed for the give-back there (2026-10-29,
// "gained here: 2012 2011 389007"); the 3 s wither at home took it away again, after it had been seen. No co-op item
// is a key of a world. A guest's Blossoms wither on the first frame home; the host's when it closes the world.
bool world_key(int32_t id) { return id < items::kFirst || id > items::kLast; }
bool g_blossoms_home = false;  // a guest going home: the Blossoms wither on the first frame there
std::vector<int32_t> g_keys_lent;     // ours that the host's world has too, carried there (not gained)
std::vector<int32_t> g_keys_carried;  // gained there and held when we left
double g_keys_polled = 0;
double g_keys_home_at = 0;            // when we were first home again with keys to check (0 not yet)
bool g_keys_lent_told = false;        // the player was told once this visit that keys came along
// A guest: what the host's world has (kHostWorld), from a 1.2.0 host.
bool g_host_world_known = false;
std::vector<int32_t> g_host_keys;
// Host: what our world has, as last sent to the guests (kHostWorld), and when it was last checked.
std::vector<uint8_t> g_world_sent;
double g_world_polled = 0;
// Bonfire travel (kTravel). Host: a guest's travel waiting for our menu to close (bonfire -1 none). Guest: our
// warp asked the host (0 not asked, cleared by the answer or the host's travel); the host's travel waits for our
// menu to close (-1 none).
constexpr double kTravelWait = 5.0;   // a guest's request without any answer: the host's game cannot do it
constexpr double kTravelMenu = 60.0;  // the host keeps a guest's travel this long while its menu stays open
constexpr double kTravelNag = 8.0;    // how often a menu that holds up a travel is pointed out
constexpr double kTravelGap = 5.0;    // host: after a trip starts, no other for this long (its load begins frames later)
uint64_t g_travel_from = 0;
double g_travel_started = -100;       // host: when our last trip (ours or a guest's) started
int g_travel_bonfire = -1;
bool g_travel_spot = false;
double g_travel_since = 0;
double g_travel_told = 0;
double g_travel_asked = 0;
int g_follow_travel = -1;
double g_follow_told = 0;  // a follow (travel or Homeward Bone) last pointed out the menu holding it up
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
double g_invade_won_at = 0;  // when the co-op team's wipe won the invasion (now())
// After a won invasion the invader leaves this long after the victory. The game's own HostDead sends an invader home
// only when its script's timer runs out (21 s on 2026-11-11), and a co-op team's wipe keeps the session: the host and
// guests reload together and stood at their bonfire again 15 s after the last fall, with the invader still in their
// world and able to hit them. 4 s shows "Invasion success!" and lets the game's own victory run first.
constexpr double kWonLeaveAfter = 4.0;
bool g_invade_lost = false;
bool g_team_fell = false;  // we invade and fell while another invader stood: our outcome is the team's
// Invasion rewards (InvasionRewards): Humanity for the side that won, given once we stand in the world again, an
// invader's only back home. Kept through finish(), which comes before an invader is home.
constexpr int32_t kHumanity = 500;  // the Humanity item (goods)
int g_reward = 0;                   // Humanity still to give
bool g_reward_home = false;         // wait for our own world
const wchar_t* g_reward_text = nullptr;
// When the last Humanity was added to it. Several invaders of one team lose together, each death message a moment after
// the other: the host's two came 0.2 s apart and gave two rewards of one, each with its own item window and line, the
// second over the first, so the host saw "Reward: 1 Humanity" for the two (2026-11-07 14:40:43). They are given as one
// once none has come for kRewardGather.
double g_reward_at = 0;
constexpr double kRewardGather = 1.0;
bool g_invade_open = false;  // host: our lobby tells invaders' searches we take invaders (ptdesc_invade)
// An invader drinks from the Estus Flask (the game's own rule forbids it; coop.cpp allows it) with at most
// kInvaderEstus charges: more are set aside on arrival and given back once home, so an invasion costs only what
// was drunk. Kept in memory only: a crash or quit during the invasion leaves the flask at the cap until a rest.
constexpr int kInvaderEstus = 5;
constexpr int32_t kHomewardBone = 330;
// Chameleon (sorcery 3550: its bullet 2700 puts SpEffect 1500 on the caster, which enemies neither see nor hear).
constexpr int kChameleonEffect = 1500;
int g_estus_kept = 0;  // charges set aside for the trip home
// Host: since when we have had no character (0: we have one), and whether the lobby is hidden for it.
double g_away_since = 0;
bool g_hidden = false;
// Host: steam::reconnects() when our lobby was made, and the game's own session was lost while we hosted (see
// host_lobby_watch).
int g_lobby_reconnects = 0;
int g_game_session_last = 0;  // the game's session state at the last check
bool g_game_session_lost = false;
// We invade and are in the host's world: the other players' health bars are shown (hk_bar_type; set every tick).
uint8_t g_invader_bars = 0;

// Group invasions (1.6.0; the section "group invasions" below). Every game of a group keeps this through the sessions
// the group goes through: the host's own world, the world it invades, and the host's world again.
enum class GroupPhase {
    None,
    Searching,  // the group's host looks for a world and asks each found in turn; its world stays open
    Leaving,    // a world said yes: the host closes its world, the guests go home; then each knocks there
    Joining,    // knocking on that world, joining it, loading into it
    Invading,   // in that world, as evil spirits
    Returning,  // home after it: the host opens its world again, the others wait to hear its lobby
    Rejoining,  // the host: waits for the others in its world; a guest: joins the host's world again
};
struct Group {
    GroupPhase phase = GroupPhase::None;
    bool lead = false;             // we are the group's host
    uint64_t leader = 0;           // the group's host (ourselves when lead)
    std::wstring leader_name;
    std::vector<uint64_t> members;  // the group's players, the host first
    uint64_t target = 0;           // the lobby of the world the group invades
    uint64_t back = 0;             // a guest: the host's lobby once its world is open again (kGroupBack)
    double since = 0;              // when the phase began
    double ready_since = 0;        // home and standing since (a knock or a return waits a moment), 0 not
    bool told = false;             // this phase's line was shown
    // The host, searching.
    int gen = 0;                   // the current lobby list or lobby join (an older answer is stale)
    int waves = 0;                 // lobby lists asked for
    bool listing = false;
    double listed_at = 0;
    std::vector<uint64_t> worlds;  // found with room for us, still to ask
    int asks = 0;
    bool ask_pending = false;      // entering a world's lobby to ask, or waiting for its answer
    uint64_t asking = 0;           // the lobby we are in to ask (beside our own)
    uint64_t asking_host = 0;
    double asked_at = 0;
    Refusals refusals;             // why the worlds asked could not take us
    int older = 0;                 // worlds found that are not 1.6.0 (they keep no invader limit)
    // Gathering again (kGroupBack, kGroupHere). The host: when it last sent its kGroupBack, and when each other player
    // last said they are still out. A guest: when the host was last heard from, and when we last said we are out.
    double back_sent = 0;
    std::map<uint64_t, double> member_heard;
    double heard_at = 0;
    double beat_sent = 0;
};
Group g_group;
// Host: the places held for a group that asked (kGroupAsk) until its players come in or kGroupHoldFor passes. The
// invader limit and MaxPlayers count them, for anyone else who knocks meanwhile. A place its player has not taken
// by then is anyone's again (the user's design: 45 s); that player may still knock, as any invader does.
struct GroupHold {
    uint64_t leader = 0;
    std::vector<uint64_t> ids;
    double until = 0;
    int size = 0;            // the group's players, as asked (1.7.0: the warning's count)
    bool announced = false;  // the warning that they are coming was given (group_coming)
};
std::vector<GroupHold> g_group_holds;
constexpr double kGroupHoldFor = 45.0;
// The Death Knell's spying (1.7.0): an invader's game tells a co-op player it watches at most this often, and the
// one watched sees it at most this often.
constexpr double kKnellSpyEvery = 60.0;
// A player of the group who is not home and standing to knock this long after the group went gives up and gathers
// again with the others. Longer than the hold: a slow load still knocks, and gets in if the world has room by then.
constexpr double kGroupLeaveFor = 120.0;
// The group's host waits this long for a world's lobby and its answer before asking the next.
// An answer takes a fraction of a second, but a first packet Steam did not let through at once comes 3.4 s later
// (a session request that came before the asked host accepted us; joins show it too, 2026-11-07): 2 s missed every
// ask that came that way, and the asker's leave then closed the link before it came. Was 8 s, then 2 s.
constexpr double kGroupAskWait = 4.0;
// Home and standing this long before a group's player knocks or comes back (a quarter of what they were, 1.6.0).
constexpr double kGroupLeadKnockAfter = 0.15;
constexpr double kGroupKnockAfter = 0.4;
constexpr double kGroupLeadReopenAfter = 0.25;
constexpr double kGroupRejoinAfter = 0.4;
constexpr double kGroupBackResend = 0.75;  // the host's lobby to a player of the group not back yet (was 3 s)
// Gathering again after the invasion (Grok review a55ef919). The players of a group need not get home together: a
// guest whose knock failed is home at once while the host invades on, and a host whose connection dropped is home
// while the others fight on. So neither side waits for a fixed time from its own return. While out (joining, invading,
// on the way home) the host tells the others every kGroupBeat that it is (kGroupBack with lobby 0), and each other
// player not yet back in the host's world tells the host the same (kGroupHere); a guest waiting at home gives up only
// after kGroupQuietFor without a word from the host, and the host, its world open again, stops sending its lobby to a
// player not heard from for as long.
constexpr double kGroupBeat = 5.0;
constexpr double kGroupQuietFor = 90.0;
constexpr const char* kGroupVersion = "1.6.0";

double now() { return GetTickCount64() / 1000.0; }

// ---------------------------------------------------------------- the Bloody Mary (1.5.0)
// The host counts the session's co-op deaths (its own and its co-op guests', up to five) while BloodyMary is on, and
// every Death Knell's price whatever BloodyMary says (the setting is about deaths; the knell's price is its own), and
// says the count in every State; any rest and a team wipe set it back to none. world.cpp raises the enemies' health
// with it and coop.cpp keeps the Bloody Mary items in step.
constexpr int kMaxBloody = 5;
int g_bloody = 0;       // host: the count
int g_host_bloody = 0;  // guest: the host's
bool g_bloody_on = true;

// True when the count went up. `knell`: a Death Knell's price, counted with BloodyMary off too.
bool bloody_add(const char* who, const char* what = "fell", bool knell = false) {
    if ((!g_bloody_on && !knell) || g_bloody >= kMaxBloody) return false;
    ++g_bloody;
    g_last_state = 0;  // the guests hear it now, so their enemies grow when the host's do
    LOG_INFO("[coop] Bloody Mary: %s %s; the session has %d", who, what, g_bloody);
    return true;
}
// Guest: our Death Knell's Bloody Mary asked of the host (kKnellPrice) at this time (0 none), and the host's count
// then: the price's line says the enemies grow hardier only once the host's count is seen going up.
double g_knell_mary_at = 0;
int g_knell_mary_from = 0;
constexpr double kKnellMaryWait = 3.0;

void bloody_reset(const char* why) {
    if (g_bloody == 0) return;
    LOG_INFO("[coop] Bloody Mary: none any more (%s; there were %d)", why, g_bloody);
    g_bloody = 0;
    g_last_state = 0;
}
// After a session ends, the party is put back to solo for a while (see settle_after_session).
double g_settle_until = 0;
// Guest: waiting for the host's map flags since this time (0 not waiting), and the flags we changed meanwhile,
// which the snapshot (taken before the host heard of them) would otherwise undo here.
double g_world_wait = 0;
// Guest: from the welcome until our copy of the host's world is the host's, our game sends the others nothing of
// what it does there: no flag (queue_flag) and no run of a map event (rules::hold_event_runs). That copy is the
// host's from the host's map flags on arrival, 2 s on (kSettle) for what the load still had in flight; without
// them (a host before 1.4.0 sends them only then, after 1.2.0 always) 10 s after arrival. The bulk goes one way:
// the host's world into the guest's copy, never the guest's into the host's (2026-11-02, Darksim's arrival).
double g_copy_settles_at = 0;  // when our copy is the host's (0: not yet known)
bool g_copy_hold = false;      // our game's runs and flags are held back now
int g_held_flags = 0;
constexpr double kCopySettle = 2.0;
constexpr double kCopyFallback = 10.0;
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

bool peer_is_invader(uint64_t id);

// To the co-op players only (reliable): what an invader's game must not take, also from a host of 1.4.0 to an
// invader of an older version that would (a bonfire's first light).
template <typename T>
void broadcast_coop(T& msg, uint64_t except = 0) {
    if (!g_lobby) return;
    const uint64_t me = steam::self();
    for (uint64_t m : steam::lobby_members(g_lobby)) {
        if (m != me && m != except && !is_lost(m) && !peer_is_invader(m)) steam::send(m, &msg, sizeof(msg), true);
    }
}

// Steam knows another player's name only once it has heard of them: a joiner's hello can come before they are in our
// lobby, and Steam then answers "[unknown]". Kept, that name matched nothing for the rest of the session: the host's
// floating health bars kept the guest's Steam name instead of their character, soul level and ping, and the host's
// game made a missing guest's character at once instead of waiting until they were up (2026-10-28).
bool name_known(const std::string& name) { return !name.empty() && name != "?" && name != "[unknown]"; }

PeerInfo* peer(uint64_t id) {
    for (auto& p : g_peers) {
        if (p.id != id) continue;
        if (!name_known(p.name)) {
            const std::string now_known = steam::persona(id);
            if (name_known(now_known)) p.name = now_known;
        }
        return &p;
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

// Host: when InvasionCooldown ends, in Steam's clock, for the invaders' searches ("ptdesc_cooldown"; 0 open). Every
// game reads the same clock, so a search can pass over the world without knocking (issue_search).
void publish_cooldown() {
    if (g_role != Role::Host || !g_lobby) return;
    const double left = g_invade_open_at - now();
    const uint32_t t = steam::server_time();
    const uint32_t until = left > 0 && t ? t + static_cast<uint32_t>(std::ceil(left)) : 0;
    steam::set_lobby_data(g_lobby, "ptdesc_cooldown", std::to_string(until).c_str());
}

// Host: an invader who arrived in our world has left it (won, lost, sent home, gone). Once the last one has gone, the
// invasion is over and InvasionCooldown begins; until then another invader may still join the ones here (fallen
// invaders stay to spectate while one of them stands, so their places stay taken).
void invasion_over(uint64_t id) {
    const auto it = std::find(g_invaders_in.begin(), g_invaders_in.end(), id);
    if (it == g_invaders_in.end()) return;
    g_invaders_in.erase(it);
    if (!g_invaders_in.empty()) {
        LOG_INFO("[coop] %s has left the invasion; %zu invader(s) still here", steam::persona(id).c_str(),
                 g_invaders_in.size());
        return;
    }
    if (g_cfg.invasion_cooldown_s <= 0) return;
    // Dried Fingers ask for invaders: no cooldown while they are in effect (to the end of the session, a close over an
    // invader included: finish() ends the invasion before the rules reset). The next session has InvasionCooldown again.
    if (rules::state().taunt) {
        LOG_INFO("[coop] The invasion is over (%s left last); Dried Fingers: no invasion cooldown",
                 steam::persona(id).c_str());
        return;
    }
    g_invade_open_at = now() + g_cfg.invasion_cooldown_s;
    publish_cooldown();
    LOG_INFO("[coop] The invasion is over (%s left last): no other invader for %d s (InvasionCooldown)",
             steam::persona(id).c_str(), g_cfg.invasion_cooldown_s);
}

// Every way a player leaves the session ends here (their leave, Bye, drop, a refused knock).
void set_admitted(uint64_t id, bool on) {
    g_admitted.erase(std::remove(g_admitted.begin(), g_admitted.end(), id), g_admitted.end());
    g_guest_npcs.erase(id);
    if (on) {
        g_admitted.push_back(id);
    } else {
        invasion_over(id);
    }
}

bool peer_is_invader(uint64_t id) {
    for (const auto& p : g_peers) {
        if (p.id == id) return p.invader;
    }
    return false;
}

uint32_t chr_of_peer(uint64_t id);
const PeerInfo* peer_by_chr(uint32_t chr);

// A co-op player of our session (the host or a guest; invaders do not count). The lobby can also hold players who
// never got in, and a refused joiner still broadcasts its status: a host counts the players it admitted, everyone
// else the host, players whose character is here, and the players the game lists as session members (`members`,
// party::remotes; the game's record holds the Steam name, cut to 16 characters).
bool in_member_records(const PeerInfo& p, const std::vector<party::Remote>& members);

bool coop_member(const PeerInfo& p, const std::vector<party::Remote>& members) {
    if (p.invader || now() - p.last_seen > 10.0) return false;
    if (g_role == Role::Host) return admitted(p.id);
    if (p.id == g_host || chr_of_peer(p.id)) return true;
    return in_member_records(p, members);
}

// The game lists the player among the session's members: by the member's Steam id, else by name (its records hold
// Steam names, cut to 16 characters).
bool in_member_records(const PeerInfo& p, const std::vector<party::Remote>& members) {
    const std::wstring steam_name = text::widen(p.name);
    for (const auto& m : members) {
        if (m.steam_id && m.steam_id == p.id) return true;
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

// Invaders fight as one team (1.3.0, with a host of 1.3.0 or later): an invader who falls while another invader of
// this world still stands does not die yet but spectates them (spectate.cpp), and the invasion is decided for all of
// them at once: every co-op player down (they all win, the fallen too), or every invader down (they all lose, and the
// co-op team earns a Humanity for each). Fallen invaders stay in the session meanwhile, so no new one takes their
// place, and InvasionCooldown starts once the last invader has gone (invasion_over).
constexpr const char* kTeamInvasionVersion = "1.3.0";

bool team_invasion_host() {
    return g_invade && g_lobby && compare_versions(lobby_version(g_lobby).mod, kTeamInvasionVersion) >= 0;
}

// Another invader in this world: in the session (their character is here, or the game lists them; a refused knock
// still broadcasts for a moment), heard from lately.
bool invader_teammate(const PeerInfo& p, const std::vector<party::Remote>& members) {
    return p.invader && p.id != steam::self() && now() - p.last_seen <= 5.0 &&
           (chr_of_peer(p.id) || in_member_records(p, members));
}

// Another invader of this world stands (as their own game last reported it).
bool invader_teammate_standing() {
    const std::vector<party::Remote> members = party::remotes();
    for (const auto& p : g_peers) {
        if (invader_teammate(p, members) && !p.dead) return true;
    }
    return false;
}

void invasion_won() {
    // Fallen already, with the report of it still to come this frame (send_death): that is the outcome. A fallen
    // invader who spectates a standing one wins with them; their held death then takes them home.
    const bool fallen = world::local_dead();
    if (fallen && !g_team_fell) return;
    LOG_INFO("[coop] Every co-op player of this world has fallen: invasion victory%s", fallen ? " (we had fallen)" : "");
    g_invade_won = true;
    g_invade_won_at = now();
    say(kMsgBreakInSuccess);
    if (!fallen) game::call_lua("HostDead");  // the game's own invader victory
    if (g_cfg.invasion_rewards) {  // one per co-op player defeated, as "Defeat {} players" counted them
        g_reward += std::max(1, g_host_coop_players);
        g_reward_at = now();
        g_reward_home = true;
        g_reward_text = kMsgRewardInvader;
    }
}

// We fell while another invader stood, and now every invader has fallen: the invasion is lost for us too. The host
// is told (kDead with lost = kLostLater: no second "has died"), and its co-op team earns this invader's Humanity.
constexpr uint8_t kLostLater = 2;

PlayerEvent player_event(MsgType type, uint64_t who, bool invader, const std::string& name);

void invasion_team_lost() {
    g_invade_lost = true;
    LOG_INFO("[coop] Every invader of this world has fallen: invasion failed");
    say(kMsgBreakInFailed);
    PlayerEvent e = player_event(kDead, steam::self(), true, utf8(own_name()));
    e.lost = kLostLater;
    if (g_host) send_to(g_host, e);
}

// The invaders' team wipe (1.7.0): we invade a world of 1.3.0 or later (team invasions), the invasion is not won, and
// every other invader of this world has fallen, at least one of them: whether they came as a group or one by one, a
// fallen invader spectates the others until the last one falls, and then every one of them sees YOUR TEAM DIED, as a
// co-op team does. Asked by the death text (deathtext.cpp) while we are down: the last invader's own fall is counted
// by its game, and the ones who fell before are released (spectate.cpp) once the last one's fall is reported.
bool invaders_all_fell() {
    if (!g_invade || g_step != Step::InWorld || g_invade_won || !team_invasion_host()) return false;
    const std::vector<party::Remote> members = party::remotes();
    int fallen = 0;
    for (const auto& p : g_peers) {
        if (!invader_teammate(p, members)) continue;
        if (!p.dead) return false;  // one still stands: our fall waits (spectate.cpp), or it is not a team's
        ++fallen;
    }
    return fallen > 0;
}

// The reward counts what reached the inventory: a full stack takes none (or only some, one at a time), and the rest
// is tried again later.
void reward_tick() {
    static double retry_at = 0;
    if (g_reward <= 0 || !game::in_game() || game::loading() || !game::player() || world::local_dead()) return;
    if (g_reward_home && (!game::own_world() || g_role != Role::None)) return;
    if (now() < retry_at || now() - g_reward_at < kRewardGather) return;
    const int before = items::count(kHumanity);
    if (!items::give(kHumanity, g_reward)) {
        for (int i = 0; i < g_reward; ++i) {
            if (!items::give(kHumanity, 1)) break;
        }
    }
    const int given = std::clamp(items::count(kHumanity) - before, 0, g_reward);
    items::announce(items::kGoodsCategory, kHumanity, given);
    if (given < g_reward) {
        retry_at = now() + 30.0;
        LOG_INFO("[coop] Reward: %d of %d Humanity fit in the inventory; the rest is tried again in 30 s", given,
                 g_reward);
    }
    if (given > 0) {
        // The co-op side's reward for more than one invader: one line for all of them.
        const wchar_t* text = g_reward_text == kMsgRewardTeam && given > 1 ? kMsgRewardTeamMany : g_reward_text;
        say(fmt(text, std::to_wstring(given)));
    }
    g_reward -= given;
    if (g_reward <= 0) {
        g_reward = 0;
        g_reward_home = false;
    }
}

// ---------------------------------------------------------------- a death keeps the world's dead
// While a co-op teammate is down (fallen, on the way to their bonfire, or loading there), the host sends a fallen
// guest its list of the world's dead characters, and a guest sends a fallen host its own, each time it changes: the
// reload of the one who fell keeps them dead (deadlist.cpp). Older versions ignore the message (their reload then
// brings the world's dead back in their game, as before).
uint32_t list_hash(const std::vector<uint8_t>& v) {
    uint32_t h = 2166136261u;
    for (uint8_t b : v) h = (h ^ b) * 16777619u;
    return h ? h : 1;
}

void share_dead_lists() {
    static double at = 0;
    if (now() - at < 0.5) return;
    at = now();
    if (g_role == Role::None || g_invade || !g_lobby || game::loading() || !game::player() || world::local_dead()) {
        return;
    }
    if (g_role == Role::Guest && g_step != Step::InWorld) return;
    const std::vector<party::Remote> members = party::remotes();
    std::vector<uint8_t> msg, hurt_msg;
    uint32_t hash = 0, hurt_hash = 0;
    bool built = false;
    for (auto& p : g_peers) {
        if (!coop_member(p, members) || !p.dead || (g_role == Role::Guest && p.id != g_host)) {
            p.dead_list_sent = 0;
            p.hurt_list_sent = 0;
            continue;
        }
        if (!built) {
            built = true;
            const std::vector<uint8_t> list = deadlist::build();
            if (list.empty()) return;
            Header h{kMagic, kDeadList};
            msg.resize(sizeof(h) + list.size());
            std::memcpy(msg.data(), &h, sizeof(h));
            std::memcpy(msg.data() + sizeof(h), list.data(), list.size());
            hash = list_hash(list);
            // Their reload puts back the health of the enemies hurt here (1.3.0; older games ignore it).
            const std::vector<uint8_t> hurt = deadlist::build_hurt();
            if (!hurt.empty()) {
                Header hh{kMagic, kHurtList};
                hurt_msg.resize(sizeof(hh) + hurt.size());
                std::memcpy(hurt_msg.data(), &hh, sizeof(hh));
                std::memcpy(hurt_msg.data() + sizeof(hh), hurt.data(), hurt.size());
                hurt_hash = list_hash(hurt);
            }
        }
        if (p.dead_list_sent != hash &&
            steam::send(p.id, msg.data(), static_cast<uint32_t>(msg.size()), true)) {
            p.dead_list_sent = hash;
        }
        if (!hurt_msg.empty() && p.hurt_list_sent != hurt_hash &&
            steam::send(p.id, hurt_msg.data(), static_cast<uint32_t>(hurt_msg.size()), true)) {
            p.hurt_list_sent = hurt_hash;
        }
    }
}

// ---------------------------------------------------------------- NPC phantoms (summons.cpp)
// The co-op guests' summons as their games reported them, one per NPC (summons.cpp takes each into our party).
std::vector<summons::Phantom> guests_npcs() {
    std::vector<summons::Phantom> out;
    auto same = [](const summons::Phantom& a, const summons::Phantom& b) {
        return a.entity >= 0 ? a.entity == b.entity : a.handle == b.handle;
    };
    for (uint64_t id : g_admitted) {
        const auto it = g_guest_npcs.find(id);
        if (it == g_guest_npcs.end() || peer_is_invader(id)) continue;
        for (const summons::Phantom& ph : it->second) {
            if (std::none_of(out.begin(), out.end(), [&](const auto& o) { return same(o, ph); })) out.push_back(ph);
        }
    }
    return out;
}

// Host: the NPC phantoms in our party go to every admitted player (co-op guests and invaders) when they change and
// every 3 s while there are any; an empty list once when the last has gone.
std::vector<uint8_t> g_npc_sent;
double g_npc_sent_at = 0;

// Our list as it stands (ours, the guests' summons among them once our party holds them, then those it does not
// hold yet), sent when it changed or every 3 s while there are any; `at_once`: sent now whatever it is (a team
// wipe's dismissal, just before our load: we send nothing while loading, and the others would rebuild the
// phantoms from the list they hold).
void publish_npc_list(bool at_once) {
    NpcPhantomsMsg m{};
    fill_header(&m, kNpcPhantoms);
    // Ours, the guests' summons among them once our party holds them; then those it does not hold yet.
    summons::set_guest_summons(guests_npcs());
    std::vector<summons::Phantom> list = summons::ours();
    const std::vector<summons::Phantom> pending = summons::guest_summons_pending();
    list.insert(list.end(), pending.begin(), pending.end());
    for (const summons::Phantom& ph : list) {
        if (m.count >= kNpcPhantomsMax) break;
        auto& e = m.p[m.count++];
        e.entity = ph.entity;
        e.handle = ph.handle;
        e.kind = ph.kind;
        e.f1 = ph.f1;
        e.f2 = ph.f2;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(&m);
    const std::vector<uint8_t> now_bytes(bytes, bytes + sizeof(m));
    const bool changed = now_bytes != g_npc_sent;
    if (!at_once && !changed && (m.count == 0 || now() - g_npc_sent_at < 3.0)) return;
    if (changed) {
        static std::string logged = "none";
        std::string ids;
        for (int i = 0; i < m.count; ++i) ids += (ids.empty() ? "" : " ") + std::to_string(m.p[i].entity);
        if (ids.empty()) ids = "none";
        if (ids != logged) LOG_INFO("[coop] Our NPC phantoms: %s%s", ids.c_str(), at_once ? " (sent at once)" : "");
        logged = ids;
    }
    g_npc_sent = now_bytes;
    g_npc_sent_at = now();
    for (uint64_t id : g_admitted) {
        if (!is_lost(id)) steam::send(id, &m, sizeof(m), true);
    }
}

void host_npc_tick() {
    static double polled = 0;
    if (now() - polled < 0.5 || !game::in_game() || game::loading() || !game::own_world()) return;
    polled = now();
    publish_npc_list(false);
}

std::vector<summons::Phantom> npc_list(const uint8_t* data, uint32_t size) {
    NpcPhantomsMsg m{};
    std::memcpy(&m, data, std::min<size_t>(size, sizeof(m)));
    std::vector<summons::Phantom> list;
    for (int i = 0; i < m.count && i < kNpcPhantomsMax; ++i) {
        summons::Phantom ph;
        ph.entity = m.p[i].entity;
        ph.handle = m.p[i].handle;
        ph.kind = m.p[i].kind;
        ph.f1 = m.p[i].f1;
        ph.f2 = m.p[i].f2;
        list.push_back(ph);
    }
    return list;
}

void on_npc_phantoms(const uint8_t* data, uint32_t size) { summons::note_host_list(npc_list(data, size)); }

// A co-op guest: the NPC phantoms our own game summoned (a sign touched in our game, a black sign's invader) go to
// the host when they change and every 3 s while there are any; an empty list once when the last has gone. A host
// before 1.4.0 ignores them: it has a summon of ours only if its game had that NPC loaded when our packet 48 came.
std::vector<uint8_t> g_mine_sent;
double g_mine_sent_at = 0;

void guest_npc_tick() {
    static double polled = 0;
    if (now() - polled < 0.5 || !g_host) return;
    polled = now();
    NpcPhantomsMsg m{};
    fill_header(&m, kNpcSummons);
    for (const summons::Phantom& ph : summons::mine()) {
        if (m.count >= kNpcPhantomsMax) break;
        auto& e = m.p[m.count++];
        e.entity = ph.entity;
        e.handle = ph.handle;
        e.kind = ph.kind;
        e.f1 = ph.f1;
        e.f2 = ph.f2;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(&m);
    const std::vector<uint8_t> now_bytes(bytes, bytes + sizeof(m));
    const bool changed = now_bytes != g_mine_sent;
    if (!changed && (m.count == 0 || now() - g_mine_sent_at < 3.0)) return;
    if (changed && (m.count > 0 || !g_mine_sent.empty())) {
        std::string ids;
        for (int i = 0; i < m.count; ++i) ids += (ids.empty() ? "" : " ") + std::to_string(m.p[i].entity);
        LOG_INFO("[coop] The NPC phantoms we summoned, for the host: %s", ids.empty() ? "none" : ids.c_str());
    }
    g_mine_sent = now_bytes;
    g_mine_sent_at = now();
    send_to(g_host, m);
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
// With "went" (message and report): that reload keeps the world's dead (deadlist.cpp); without, it starts the world
// over. A later death's reload follows it (wipe_reload).
constexpr uint32_t kRespawnKept = 0x10, kRespawnPhase = 0x0F;
constexpr double kHostWait = 15.0;   // the host waits this long for the guests' deaths to reach their warp
constexpr double kGuestWait = 25.0;  // a guest waits this long for the host's warp (a host gone silent)
uint32_t g_respawn = kRespawnNone;   // ours, for our current death
bool g_respawn_kept = false;         // our current death's reload keeps the world's dead
bool g_respawn_waiting = false;
double g_respawn_since = 0;
double g_respawn_told = 0;  // guest: when we last told the host our warp waits
// Guest: when the host last said its warp went (message or player report). Kept apart from the host's phase, which
// its first report after standing up sets back to none: both can arrive in one poll, after a stall.
double g_host_went_at = -100;
// A rest after a lost boss fight (world.cpp): living players' worlds reload at their bonfires, as the fallen's do,
// all at once (set_reset_phase). A fallen player waits as long as a teammate who reports a pending reload is heard
// from (coop_member: within 10 s), however long that teammate stays in the bonfire menu; that teammate is told so.
uint8_t g_reset = kResetNone;  // our reload
uint32_t g_rest_id = 0;        // the last rest this game took (ours or another player's): kRest's value
bool g_reset_hold = false;     // our respawn waits for teammates' reloads

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

// A living co-op teammate whose world's reload after a lost boss fight is at least at `phase`.
const PeerInfo* resetting_mate(uint8_t phase) {
    const std::vector<party::Remote> members = party::remotes();
    for (const auto& p : g_peers) {
        if (coop_member(p, members) && !p.dead && p.reset >= phase) return &p;
    }
    return nullptr;
}

// A player's report for their current death: it only moves forward (None -> Waiting -> Went) until they live again.
void note_peer_respawn(PeerInfo* p, uint32_t value) {
    const uint32_t phase = value & kRespawnPhase;
    if (!p || !p->dead || phase <= p->respawn || phase > kRespawnWent) return;
    p->respawn = phase;
    if (phase != kRespawnWent) return;
    p->went_kept = (value & kRespawnKept) != 0;
    if (!p->went_kept) p->went_over_at = now();
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
    m.value = phase | (phase == kRespawnWent && g_respawn_kept ? kRespawnKept : 0);
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
    knell::left_session("the session ended");
    // A host closing its world over an invader: that invasion is over too (InvasionCooldown).
    const std::vector<uint64_t> invaders = g_invaders_in;
    for (uint64_t id : invaders) invasion_over(id);
    g_invaders_in.clear();
    g_group_holds.clear();  // places held for a group that was to invade our world (1.6.0)
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
    g_homeward_by = 0;
    g_host_homeward = false;
    g_host_vicinity = false;
    g_homeward_asked = 0;
    g_follow_bonfire = -1;
    g_bone_owed = false;
    g_host_net_rate = 0;
    g_host_seal_fogs = false;
    if (g_host_scaling) world::set_host_scaling(nullptr);
    g_host_scaling = false;
    g_follow_travel = -1;
    g_travel_bonfire = -1;
    g_travel_asked = 0;
    g_host_world_known = false;
    g_host_keys.clear();
    g_world_sent.clear();
    rules::set_host_world(false, false, 0);
    g_guest_bonfire = -1;
    g_cut_map = -1;
    g_cut_own_map = -1;
    g_cut_follow_map = -1;
    g_close_at = 0;
    g_close_left = false;
    g_close_forced = false;
    g_invade_won = false;
    g_invade_won_at = 0;
    g_invade_lost = false;
    g_team_fell = false;
    g_respawn_waiting = false;
    g_reset_hold = false;
    g_reset = kResetNone;
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
void publish_invader_places();
void publish_world_levels();

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
        steam::set_lobby_data(lobby, "ptdesc_boss", "0");
        steam::set_lobby_data(lobby, "ptdesc_items", g_cfg.share_items ? "1" : "0");
        steam::set_lobby_joinable(lobby, true);
        publish_cooldown();  // a cooldown from our last session goes on
        publish_invader_places();
        publish_world_levels();
        g_away_since = 0;
        g_hidden = false;
        g_lobby_reconnects = steam::reconnects();
        g_game_session_last = game::session_state();
        g_game_session_lost = false;
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
    if (g_game_session_last == 3 && state != 3 && !g_game_session_lost) {
        g_game_session_lost = true;
        LOG_INFO("[coop] The game's own session was lost while we host (state %d)", state);
    }
    g_game_session_last = state;
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
    // As for a join (guest_on_welcome): the lone-member timer of a session we left a moment ago must not end this one.
    if (const float left = game::stop_idle_timer()) {
        LOG_INFO("[coop] The game's timer from our last session had %.1f s left: stopped before our new one", left);
    }
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

// ---------------------------------------------------------------- the invader limit (1.6.0)
// The user's design: our world takes at most as many invaders at once as it has co-op players (the host and the co-op
// guests: N) plus ExtraInvaders, which is 1 at the least, so never fewer than N + 1: a host alone meets two invaders at
// most, a party of three four. ExtraInvaders raises it for larger groups of invaders; it cannot lower it. Fallen
// invaders who spectate a standing one keep their places (team invasions, 1.3.0), and so do the places held for a group
// that asked (kGroupAsk). MaxPlayers still holds for everyone together. Older hosts keep no such limit, so groups invade
// only worlds of 1.6.0 or later.

// The places of a hold whose players came in are taken (they count as invaders here now); a hold none of whose players
// came within kGroupHoldFor is given up.
void prune_group_holds() {
    for (auto& g : g_group_holds) {
        g.ids.erase(std::remove_if(g.ids.begin(), g.ids.end(), [](uint64_t id) { return admitted(id); }), g.ids.end());
    }
    g_group_holds.erase(std::remove_if(g_group_holds.begin(), g_group_holds.end(),
                                       [](const GroupHold& g) {
                                           if (g.ids.empty()) return true;
                                           if (now() < g.until) return false;
                                           LOG_INFO("[coop] The %zu invader place(s) held for %s's group are given up "
                                                    "(they did not come within %.0f s)",
                                                    g.ids.size(), steam::persona(g.leader).c_str(), kGroupHoldFor);
                                           return true;
                                       }),
                        g_group_holds.end());
}

// A place is held for this player.
bool holds_place(uint64_t id) {
    prune_group_holds();
    for (const auto& g : g_group_holds) {
        if (std::find(g.ids.begin(), g.ids.end(), id) != g.ids.end()) return true;
    }
    return false;
}

// The places held for players who have not come in yet, `except`'s own not counted.
int held_places(uint64_t except) {
    prune_group_holds();
    int n = 0;
    for (const auto& g : g_group_holds) {
        for (uint64_t id : g.ids) n += id != except ? 1 : 0;
    }
    return n;
}

// The invaders in our world (admitted and heard from lately, fallen ones too), `except` not counted.
int invaders_here(uint64_t except) {
    int n = 0;
    for (const auto& p : g_peers) {
        if (p.invader && p.id != except && admitted(p.id) && now() - p.last_seen <= 10.0) ++n;
    }
    return n;
}

// The most invaders our world takes at once.
int invader_cap() { return 1 + coop_guest_count() + g_cfg.extra_invaders; }

// The players our session can hold with a joiner whose game holds `joiner_max`: every game in the session needs room
// for everyone (MaxPlayers, players.cpp). `in_session` gets the players in it: us and everyone admitted but `except`.
int session_room(int joiner_max, uint64_t except, int* in_session) {
    int n = 1;
    int room = std::min(g_cfg.max_players, joiner_max);
    for (uint64_t id : g_admitted) {
        if (id == except) continue;
        ++n;
        int theirs = 4;
        for (const PeerInfo& q : g_peers) {
            if (q.id == id && q.max_players > 0) theirs = q.max_players;
        }
        room = std::min(room, theirs);
    }
    *in_session = n;
    return room;
}

// How many more invaders our world takes now, by the invader limit and by MaxPlayers, the held places taken off: our
// lobby says it ("ptdesc_inv_room"), so that a search of 1.6.0 passes over a world without room (a group's, one
// without room for all of it).
int invader_places() {
    int in_session = 1;
    const int room = session_room(g_cfg.max_players, 0, &in_session);
    const int held = held_places(0);
    const int by_limit = invader_cap() - invaders_here(0) - held;
    const int by_room = room - in_session - held;
    return std::max(0, std::min(by_limit, by_room));
}

void publish_invader_places() {
    static uint64_t lobby = 0;
    static int last = -1;
    if (!g_lobby) return;
    const int n = invader_places();
    if (lobby == g_lobby && n == last) return;
    lobby = g_lobby;
    last = n;
    steam::set_lobby_data(g_lobby, "ptdesc_inv_room", std::to_string(n).c_str());
}

// The soul levels of our world's co-op players, ours first, for invaders' level matching (1.7.0, kLevelPull):
// "ptdesc_levels" = "45,52". Written when it changes (a level-up, a guest coming or going), checked every second.
void publish_world_levels() {
    static uint64_t lobby = 0;
    static std::string last;
    if (!g_lobby) return;
    const int own = game::soul_level();
    std::string s = own > 0 ? std::to_string(own) : std::string();
    const std::vector<party::Remote> members = party::remotes();
    for (const auto& p : g_peers) {
        if (!coop_member(p, members) || p.soul_level <= 0) continue;
        if (!s.empty()) s += ',';
        s += std::to_string(p.soul_level);
    }
    if (s.empty() || (lobby == g_lobby && s == last)) return;
    lobby = g_lobby;
    last = s;
    steam::set_lobby_data(g_lobby, "ptdesc_levels", s.c_str());
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
// Shrine, the Undead Asylum), the nearest point of our own map within kNearPoint across and kNearRise up or down;
// with none in reach, on the host (false). Until 1.3.0 that was any point within 50 m in a straight line: from
// Firelink, the New Londo Ruins' points below it, and the invader stood behind a lift the host had not used yet.
constexpr float kNearPoint = 30.0f;
constexpr float kNearRise = 6.0f;

bool invader_arrival(game::InvasionPoint* ip) {
    game::Vec3 me{};
    const int region = game::play_region();
    const int map = game::map_id();
    if (!game::position(&me, nullptr) || !game::invasion_point(region, me, map, kNearPoint, kNearRise, ip)) {
        LOG_INFO("[coop] No invasion point for play region %d, nor one of map %08X within %.0f m (%.0f m up or down): "
                 "the invader arrives on the host", region, map, kNearPoint, kNearRise);
        return false;
    }
    LOG_INFO("[coop] The invader arrives at invasion point %d (%s, %.0f m away, map %08X)", ip->id,
             ip->exact ? "our play region's" : "the nearest", std::sqrt(ip->dist), ip->map);
    return true;
}

void send_world_globals(uint64_t to);
void send_world_flags(uint64_t to);
void send_live_drops(uint64_t to);
void send_bonfires(uint64_t to);
bool guests_know_free_for_all();

// A group invasion (1.6.0) is on its way in: its first player's knock was just let in (host_answer), before any of
// them has arrived. The world is warned, the host and its co-op guests (1.7.0, the user's wish: they are about to be
// invaded by a band, so some shock to it), with the number of invaders the group holds places for. Once per group,
// and only for two or more (a host alone who invades with the orb is a group of one).
void group_coming(uint64_t id) {
    for (auto& g : g_group_holds) {
        if (g.announced || std::find(g.ids.begin(), g.ids.end(), id) == g.ids.end()) continue;
        g.announced = true;
        if (g.size < 2) return;
        LOG_INFO("[coop] %s's group of %d invaders is coming in: the world is warned", steam::persona(g.leader).c_str(),
                 g.size);
        say(fmt(kMsgGroupComing, std::to_wstring(g.size)));
        Simple m{};
        fill_header(&m, kGroupComing);
        m.value = static_cast<uint32_t>(g.size);
        const uint64_t me = steam::self();
        for (const auto& p : g_peers) {
            if (!p.invader && p.id != me && admitted(p.id)) send_to(p.id, m);
        }
        return;
    }
}

// An invader's Death Knell watches us (1.7.0, the user's wish: the co-op players should know they are spied on). Shown
// at most once every kKnellSpyEvery, however many invaders watch: the one watched is not flooded.
void knell_spied(uint64_t by) {
    static double last = -1e9;
    if (now() - last < kKnellSpyEvery) {
        LOG_INFO("[coop] %s's Death Knell watches us (told %.0f s ago)", steam::persona(by).c_str(), now() - last);
        return;
    }
    last = now();
    LOG_INFO("[coop] %s's Death Knell watches us: we are told", steam::persona(by).c_str());
    say(kMsgKnellSpied);
}

void host_answer(const Pending& p) {
    Welcome w{};
    fill_header(&w, kWelcome);
    w.status = kOk;
    // The game's limit for guests and invaders together (it keeps three remote players). Steam's lobby has more
    // room (kLobbyRoom), so this is what keeps the count; it used to be the lobby's size. Until 1.1.0 a second
    // check counted every non-invader in g_peers: that held players still knocking, and turned a joiner away as
    // full while there was room.
    // NPC phantoms do not count: four players always fit, with NPCs on top. The party list has four entries, so with
    // four players and an NPC one character has none in each game (no floating health bar for them); a 1.2.0 host's
    // world held four players and an NPC so (2026-10-25).
    // Above the game's four every game in the session needs room for one more (MaxPlayers, players.cpp): one that
    // holds four could not make the fifth player's character, nor the fifth anyone else's.
    // The places held for a group that is coming (1.6.0) count as taken, for co-op guests as for invaders, except the
    // joiner's own: one of that group takes the place held for them, whatever the invader limit says by now.
    int in_session = 1;
    const int room = session_room(p.max_players, p.from, &in_session);
    const int held = held_places(p.from);
    const bool holder = p.invade && holds_place(p.from);
    if (in_session + held >= room) w.status = kFull;
    // The invader limit (1.6.0): our co-op players plus ExtraInvaders.
    const int limit = invader_cap();
    const int invaders = invaders_here(p.from);
    const bool limited = w.status == kOk && p.invade && !holder && invaders + held + 1 > limit;
    if (limited) w.status = kFull;
    if (p.invade && !g_cfg.allow_invaders) w.status = kRefused;
    // InvasionCooldown: no invader for a while after an invasion of our world ended. Checked before a boss fight: the
    // wait is the least the invader has to wait in any case.
    uint32_t wait = 0;
    if (w.status == kOk && p.invade && now() < g_invade_open_at) {
        wait = static_cast<uint32_t>(std::ceil(g_invade_open_at - now()));
        w.status = p.knows_refusals ? kCooldown : kRefused;
        LOG_INFO("[coop] An invader knocked during the invasion cooldown (%u s left): refused", wait);
    }
    // Nobody joins a boss fight (ours or a guest's). An invader tries another world. A guest is told to come back
    // after it: one who arrived mid-fight ran the boss's opening in its own game on a boss already fighting (the
    // Asylum Demon's jump from its perch, from wherever it stood, through the floor; 2026-10-17 test). Remastered
    // Seamless Co-op also keeps invaders out of the Undead Asylum and Firelink Shrine; Prepare to Co-op lets them in.
    if (w.status == kOk && boss_fight_in_world()) {
        w.status = p.invade && !p.knows_refusals ? kRefused : kBossFight;
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
    if (w.status == kOk && holder) {
        LOG_INFO("[coop] %s takes the place held for their group", steam::persona(p.from).c_str());
        group_coming(p.from);
        prune_group_holds();
    }
    WelcomeMore more{};
    more.w = w;
    more.wait_s = wait;
    send_to(p.from, more);
    // The world's state and its map flags go with the welcome, so the guest's game runs our world's events on them
    // from its first frame here (and again on arrival, for what changed during the load). Until 1.4.0 the map flags
    // came on arrival only, and a guest's game ran our world's events through the load on its copy of our maps as
    // it was: its own progress, or another world it had visited (2026-11-02: Darksim's, at Firelink).
    if (w.status == kOk && !p.invade) {
        send_world_globals(p.from);
        send_world_flags(p.from);
    }
    const PeerInfo* who = nullptr;
    for (const auto& q : g_peers) who = q.id == p.from ? &q : who;
    // Why, in words: an older player's game holds four (1.3.0 and before), so it is told "full" by a session of four
    // players, or of four and an invader (2026-11-01: an invader of 1.3.0 knocked seven times on a session of five,
    // each time answered full and gone from the lobby within a second, as its game then looks for another world).
    char why[160] = "";
    switch (w.status) {
        case kOk: std::snprintf(why, sizeof(why), "welcome"); break;
        case kFull:
            if (limited) {
                std::snprintf(why, sizeof(why), "full: %d invader(s) here and %d place(s) held, the limit is %d (%d "
                              "co-op player(s) + ExtraInvaders %d)", invaders, held, limit, limit - g_cfg.extra_invaders,
                              g_cfg.extra_invaders);
            } else {
                std::snprintf(why, sizeof(why), "full: %d player(s) here and %d place(s) held, room for %d (their game "
                              "holds %d)", in_session, held, room, p.max_players);
            }
            break;
        case kBusy: std::snprintf(why, sizeof(why), "busy (a load, the title screen, or our player is down)"); break;
        case kRefused: std::snprintf(why, sizeof(why), "refused"); break;
        case kBossFight: std::snprintf(why, sizeof(why), "a boss fight"); break;
        case kCooldown: std::snprintf(why, sizeof(why), "invasion cooldown, %u s left", wait); break;
        default: break;
    }
    LOG_INFO("[coop] Answered %s (%s, character '%s'): status %u, %s", steam::persona(p.from).c_str(),
             p.invade ? "invader" : "guest", who ? utf8(who->char_name).c_str() : "", w.status, why);
}

// The party counts the game keeps beside the session (PartyMemberInfo +0x10/+0x14, slot +0x7C). The game
// brings them down itself when a guest's character is taken out of the party. They are only put right here
// when a guest never left and the session had to be ended over them: HasParty would keep Quit grey.
void settle_party_counts() {
    const uint32_t gm = game::game_man();
    uint32_t party = 0;
    if (!gm || !read32(gm + game::kGmParty, &party) || party < 0x10000) return;
    // An NPC phantom of ours stays summoned when the session ends (the Black Separation Crystal closed our world
    // over Solaire: his health bar went, he stayed, 2026-10-23): the party is put back to one alone with it.
    if (summons::settle_party(party)) return;
    uint32_t n10 = 0, n14 = 0, slot = 0;
    read32(party + 0x10, &n10);
    read32(party + 0x14, &n14);
    read32(party + partylist::field(0x7C), &slot);
    if (static_cast<int>(n10) <= 1 && static_cast<int>(n14) <= 1 && static_cast<int>(slot) == -1) return;
    if (static_cast<int>(n10) > 1) *ptr<int>(party + 0x10) = 1;
    if (static_cast<int>(n14) > 1) *ptr<int>(party + 0x14) = 1;
    if (static_cast<int>(slot) != -1) *ptr<int>(party + partylist::field(0x7C)) = -1;
    LOG_INFO("[coop] Party counts settled after the session (were %d/%d, slot %d)", static_cast<int>(n10),
             static_cast<int>(n14), static_cast<int>(slot));
}

// Once a session is over (the game session is solo and no other player's character is left in our world) the
// party is put back to a solo game's: counts 1, no slot, no member handle but our own. The game lowers the counts
// as each member's character goes, but not in every way a session ends (a guest's own leave, a host left alone),
// and a party that still counts two keeps HasPartyMembers true: the bonfire is unusable, talk refuses service and
// the menu treats the game as multiplayer. Run for a while after every session (see tick).
void settle_after_session() {
    if (game::session_state() != 0 || game::loading() || !game::remote_player_chrs().empty()) return;
    const uint32_t gm = game::game_man();
    uint32_t party = 0;
    if (gm && read32(gm + game::kGmParty, &party) && party > 0x10000 && summons::settle_party(party)) return;
    settle_party_counts();
    if (gm && party > 0x10000) summons::solo_party(party);
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

// The party's four entries, written whenever they change: which members the count +0x10 stands for (0xEA2510's
// entry types: 0 our own character, 1 another player, 2 an NPC). After the second disband of the 2026-10-24 test
// only Solaire's entry was left, so the Black Separation Crystal stayed grey, and the log could not say whose
// entry had gone.
void log_party_entries(uint32_t party) {
    static std::string previous;
    static int lines = 0;
    std::string s;
    char one[48];
    for (int i = 0; i < partylist::entries(); ++i) {
        const uint32_t e = party + 0x18 + i * 0x14;
        const uint32_t h = peek32(e);
        if (h == 0xFFFFFFFF) continue;
        const uint32_t type = peek32(e + 4);
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s[%d] %08X %s/%d", s.empty() ? "" : ", ", i, h,
                    type == 0 ? "self" : type == 1 ? "player" : type == 2 ? "NPC" : "?",
                    static_cast<int>(peek32(e + 8)));
        s += one;
    }
    if (s.empty()) s = "none";
    if (s == previous) return;
    previous = s;
    if (++lines > 300) return;
    const uint32_t me = game::player();
    LOG_INFO("[coop] Party entries: %s (our character %08X)", s.c_str(), me ? peek32(me + 4) : 0);
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
            static_cast<int>(peek32(party + partylist::field(0x7C))));
        log_party_entries(party);
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
            put(" dialog %d res %d/%d kind %d", static_cast<int>(peek32(dlg)), static_cast<int>(peek32(dlg + 0x18)),
                static_cast<int>(peek32(dlg + 0x1C)), static_cast<int>(peek32(dlg + 0xC)));
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
        world::session_over_blossoms("the host closed the world");
        finish(L"");
        return;
    }
    const int members = game::member_count();
    // Hosting (3): once alone, or when a guest has not left after six seconds (1.5 s when our group leaves to invade:
    // its guests went home the moment they heard, and the knock waits on this close). Still being created or failed
    // (1, 2, 5): after a moment, the same leave clears it. Leaving (7) is the game's own leave in progress.
    const bool group_going = g_group.lead && g_group.phase == GroupPhase::Leaving;
    const bool hosting_done = state == 3 && (members <= 1 || waited > (group_going ? 1.5 : 6.0));
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

// Two checks in a row (kLobbyCheck s apart) that find us out of our lobby: we are out of it for good (steam::in_lobby).
constexpr double kLobbyCheck = 5.0;

bool out_of_lobby() {
    static double last = 0;
    static int misses = 0;
    static uint64_t lobby = 0;
    if (lobby != g_lobby) {
        lobby = g_lobby;
        misses = 0;
    }
    if (now() - last < kLobbyCheck) return false;
    last = now();
    if (!g_lobby || !steam::online()) return false;  // nothing to judge until Steam is back
    misses = steam::in_lobby(g_lobby) ? 0 : misses + 1;
    return misses >= 2;
}

// Host: our lobby is how other players find our world and how our messages reach them. A network change or a lost
// connection to Steam takes us out of it, and Steam deletes a lobby its last member has left: the world could no
// longer be found while everything else still said we hosted, and the host had to close it and open it again
// (2026-10-28: the host's game lost its own session at 21:34 and made it again; a search at 21:50 found no world).
// A new lobby is made once we are out of ours, or, while no other player is with us, once Steam is back after a
// loss or the game's own session had to be made again (both cost a lobby nobody is in nothing).
void host_lobby_watch() {
    const char* why = nullptr;
    if (out_of_lobby()) {
        why = "we are no longer in it";
    } else if (steam::online() && (steam::reconnects() != g_lobby_reconnects || g_game_session_lost) &&
               game::session_state() == 3) {
        bool alone = true;
        for (const auto& p : g_peers) alone &= !(admitted(p.id) && now() - p.last_seen <= 10.0);
        if (alone) {
            why = g_game_session_lost ? "the game's own session was lost" : "the connection to Steam was lost";
        } else {
            g_lobby_reconnects = steam::reconnects();  // the players with us still reach us: it is kept
            g_game_session_lost = false;
        }
    }
    if (!why) return;
    LOG_INFO("[coop] Our session's lobby may be gone (%s): a new one is made", why);
    if (g_lobby) steam::leave_lobby(g_lobby);
    g_lobby = 0;
    host_open_lobby();
}

// Invaders' searches skip a world that would refuse them: ptdesc_invade reads "0" while a boss fight is on (and
// with AllowInvaders off). 1.0.0 to 1.2.0 invaders search on that key. From 1.3.0 "ptdesc_boss" reads "1" during the
// fight of a world that takes invaders otherwise: a 1.3.0 invader's search finds every world and tells the invader
// why none could take them (issue_search).
// A world whose own group looks for a world to invade (1.6.0) stays open to invaders all the while, as the user wants:
// only the group's going closes it (group_go).
void host_invasions_open() {
    static double last = 0;
    if (now() - last < 1.0) return;
    last = now();
    publish_invader_places();
    publish_world_levels();
    const bool fight = boss_fight_in_world();
    const bool open = g_cfg.allow_invaders && !fight;
    if (open == g_invade_open) return;
    g_invade_open = open;
    steam::set_lobby_data(g_lobby, "ptdesc_invade", open ? "1" : "0");
    steam::set_lobby_data(g_lobby, "ptdesc_boss", g_cfg.allow_invaders && fight ? "1" : "0");
    LOG_INFO("[coop] A boss fight %s: invaders %s our world", open ? "is over" : "is on",
             open ? "can find" : "cannot find");
}

// ---------------------------------------------------------------- bonfire travel and the host's world
// Host: what our world has, for the co-op guests (kHostWorld), when it changed (checked every 2 s).
void host_world_tick() {
    if (now() - g_world_polled < 2.0 || !game::in_game() || game::loading() || !game::own_world()) return;
    g_world_polled = now();
    HostWorldMsg m{};
    fill_header(&m, kHostWorld);
    m.lordvessel = rules::own_lordvessel() ? 1 : 0;
    m.warps = rules::own_warps();
    const std::vector<int32_t> keys = items::key_items();
    m.keys = static_cast<uint8_t>(std::min<size_t>(keys.size(), kHostKeysMax));
    std::copy(keys.begin(), keys.begin() + m.keys, m.key);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&m);
    if (std::vector<uint8_t>(bytes, bytes + sizeof(m)) == g_world_sent) return;
    g_world_sent.assign(bytes, bytes + sizeof(m));
    for (uint64_t id : g_admitted) {
        if (!peer_is_invader(id) && !is_lost(id)) steam::send(id, &m, sizeof(m), true);
    }
}

uint64_t bone_missing();

// A refused Homeward Bone names who has none (TravelAnswerMsg); every other answer is the Simple one.
void answer_travel(uint64_t to, uint32_t why) {
    TravelAnswerMsg a{};
    fill_header(&a.simple, kTravelAnswer);
    a.simple.value = why;
    if (why != kTravelNoBone) {
        send_to(to, a.simple);
        return;
    }
    a.who = bone_missing();
    send_to(to, a);
}

// Host: why no trip of the party's can start now (0: one can): a travel, a Homeward Bone's (or the Homeward
// miracle's) or the Darksign's. The Darksign is the way out of anything: a boss fight, an invasion, a teammate who is
// down; only a lost fight's reload, which takes everyone to their bonfires anyway, and our own game's state hold it.
// The Homeward Bone is the way out of a boss fight, as in single player (session::bone_hold). Both still go when we
// are the one who is down (a guest used it): our death goes ahead to our last bonfire, the trip's (homeward_go).
enum TripKind { kTripTravel, kTripBone, kTripDarksign };
uint32_t trip_refusal(TripKind kind) {
    if (!game::in_game() || game::loading() || rules::cutscene_playing() || rules::cutscene_loading()) {
        return kTravelBusy;
    }
    const bool fallen = world::local_dead();
    if (fallen && kind == kTripTravel) return kTravelBusy;
    if (kind == kTripDarksign) return reload_hold() ? kTravelReload : 0;
    int hold = kind == kTripBone ? bone_hold() : travel_hold();
    if (fallen && hold == kHoldNone && !world::boss_fight() && !rules::state().teammate_boss_fight) {
        hold = kHoldMateDown;  // we are the teammate who is down: outside a boss fight the bone waits for our respawn
    }
    switch (hold) {
        case kHoldBossFight: return kTravelBossFight;
        case kHoldInvader: return kTravelInvader;
        case kHoldReload: return kTravelReload;
        case kHoldMateDown: return kTravelMateDown;
        default: break;
    }
    return 0;
}

// Host: why our world cannot travel to that bonfire now (0: it can). A guest's travel is checked against our own
// world, so a guest cannot take the party further than the host's own warp could, and waits as the host's own warp
// does (session::travel_hold). A trip that started a moment ago still has its load ahead: no other goes meanwhile.
uint32_t travel_refusal(int bonfire, bool spot) {
    if (trip_slot_busy()) return kTravelBusy;
    if (const uint32_t why = trip_refusal(kTripTravel)) return why;
    if (!spot && !rules::own_lordvessel()) return kTravelNoVessel;
    if (!rules::warp_destination(bonfire)) return kTravelNotThere;
    return 0;
}

// Host: a guest's travel goes once no menu of ours is open and we are not in a bonfire ceremony (we are taken there
// with everyone). A warp of our own that starts meanwhile (its animation is such a ceremony) replaces it.
void host_travel_tick() {
    if (g_travel_bonfire < 0) return;
    if (!admitted(g_travel_from)) {
        g_travel_bonfire = -1;
        return;
    }
    const std::wstring who = shown_name(*peer(g_travel_from));
    if (const uint32_t why = travel_refusal(g_travel_bonfire, g_travel_spot)) {
        LOG_INFO("[coop] The travel to bonfire %d for %s cannot go any more (%u)", g_travel_bonfire,
                 steam::persona(g_travel_from).c_str(), why);
        answer_travel(g_travel_from, why);
        g_travel_bonfire = -1;
        return;
    }
    const int anim = motion::remote_open_anim(game::player());
    if (anim == 7725) return;  // our own warp is under way (Lua_Warp): it replaces this travel (send_bonfire_warp)
    if (world::menu_open() || anim != 0) {
        if (now() - g_travel_since > kTravelMenu) {
            LOG_INFO("[coop] The travel for %s is dropped: our menu stayed open", steam::persona(g_travel_from).c_str());
            answer_travel(g_travel_from, kTravelTimedOut);
            g_travel_bonfire = -1;
        } else if (now() - g_travel_told >= kTravelNag) {
            if (g_travel_told == 0) answer_travel(g_travel_from, kTravelWaiting);
            g_travel_told = now();
            text::show(fmt(kMsgTravelHostMenu, who));
        }
        return;
    }
    const int bonfire = g_travel_bonfire;
    g_travel_bonfire = -1;
    g_travel_started = now();
    LOG_INFO("[coop] Travelling to bonfire %d for %s; guests follow", bonfire, steam::persona(g_travel_from).c_str());
    say(fmt(kMsgTravelAsked, who));
    BonfireMsg b{};
    fill_header(&b, kBonfireWarp);
    b.bonfire = bonfire;
    broadcast(b);
    rules::remote_bonfire_warp(bonfire);
}

void host_travel_request(uint64_t from, const TravelMsg& m) {
    uint32_t why = travel_refusal(m.bonfire, m.spot != 0);
    if (!why && g_travel_bonfire >= 0 && g_travel_from != from) why = kTravelBusy;  // another guest's goes first
    LOG_INFO("[coop] %s asks to travel to bonfire %d%s: %s (%u)", steam::persona(from).c_str(), m.bonfire,
             m.spot ? " from a warp spot" : "", why ? "refused" : "everyone goes", why);
    if (why) {
        answer_travel(from, why);
        return;
    }
    g_travel_from = from;
    g_travel_bonfire = m.bonfire;
    g_travel_spot = m.spot != 0;
    g_travel_since = now();
    g_travel_told = 0;
    host_travel_tick();
}

std::wstring guest_without_bone();
bool start_homeward(HomewardKind kind, uint64_t by);

const char* homeward_name(HomewardKind kind) {
    return kind == kHomeDarksign ? "Darksign" : kind == kHomeMiracle ? "Homeward miracle" : "Homeward Bone";
}

// Host: our own Homeward Bone's, Darksign's or Homeward miracle's trip cannot go (refused at the use or when it was
// to go). A guest's is answered with the reason instead (kTravelAnswer).
void homeward_refused(uint32_t why, HomewardKind kind) {
    LOG_INFO("[coop] Host %s: no trip (%u)", homeward_name(kind), why);
    switch (why) {
        case kTravelBossFight: say(travel_hold_text(kHoldBossFight)); break;
        case kTravelInvader: say(travel_hold_text(kHoldInvader)); break;
        case kTravelMateDown: say(travel_hold_text(kHoldMateDown)); break;
        case kTravelReload: say(travel_hold_text(kHoldReload)); break;
        case kTravelNoBone: {
            const std::wstring who = guest_without_bone();
            say(who.empty() ? std::wstring(L"Nothing interesting happens") : fmt(kMsgNoBone, who));
            break;
        }
        default: say(kMsgTripBusy); break;
    }
}

// Host: a co-op guest without a Homeward Bone, as they report it, else null. A player of 1.2.0 or older does not report
// their bones: they neither hold the bone back nor pay one.
const PeerInfo* mate_without_bone() {
    const std::vector<party::Remote> members = party::remotes();
    for (const auto& p : g_peers) {
        if (coop_member(p, members) && p.bones == 0) return &p;
    }
    return nullptr;
}

// Host: that guest's name, else empty.
std::wstring guest_without_bone() {
    const PeerInfo* p = mate_without_bone();
    return p ? shown_name(*p) : L"";
}

// Host: the co-op player without a Homeward Bone (their id; ours first), else 0: a bone's trip cannot go.
uint64_t bone_missing() {
    if (items::count(kHomewardBone) < 1) return steam::self();
    const PeerInfo* p = mate_without_bone();
    return p ? p->id : 0;
}

// Host: the Darksign sends every invader home first (message 29; a 1.2.0 invader ignores it and gets the host's
// goodbye 5 s later, which it takes as the session's end). An invader sent home this way neither wins nor loses.
std::vector<std::pair<uint64_t, double>> g_sent_home;  // invaders told to go, and when

void send_invaders_home() {
    Simple m{};
    fill_header(&m, kSentHome);
    int n = 0;
    for (uint64_t id : g_admitted) {
        if (!peer_is_invader(id)) continue;
        send_to(id, m);
        g_sent_home.push_back({id, now()});
        LOG_INFO("[coop] Darksign: %s, the invader, is sent home", steam::persona(id).c_str());
        ++n;
    }
    if (n) say(kMsgDarksignSends);
}

void sent_home_tick() {
    for (auto it = g_sent_home.begin(); it != g_sent_home.end();) {
        if (!admitted(it->first)) {
            it = g_sent_home.erase(it);
        } else if (now() - it->second > 5.0) {
            Simple bye{};
            fill_header(&bye, kBye);
            send_to(it->first, bye);
            LOG_INFO("[coop] The invader %s is still here 5 s after the Darksign: told the session is over for them",
                     steam::persona(it->first).c_str());
            it = g_sent_home.erase(it);
        } else {
            ++it;
        }
    }
}

// Host: the trip of a Homeward Bone, the Darksign or the Homeward miracle goes, unless something now holds the party's
// trips back (the item or spell can be used only when nothing does, but its animation takes a moment). Its cost is
// taken only when the trip goes: the Darksign takes every soul and all humanity from the host and from every guest
// (each game its own), a Homeward Bone one bone from each; the miracle costs only its cast. The game's UseGoods, which
// would use the bone up, is skipped in a session, so a refused bone costs nothing (a refused miracle is cast).
// A guest's (kHomewardAsk) is the same trip, to our last bonfire, at the same costs; on our screen it says whose.

// Host: the waiting trip cannot go. Whoever used it is told why (we on screen, a guest by kTravelAnswer), and so is
// the guest whose travel it replaced.
void homeward_cancel(uint32_t why) {
    const HomewardKind kind = g_homeward_kind;
    const uint64_t by = g_homeward_by, bumped = g_homeward_bumped;
    g_homeward_at = 0;
    g_homeward_kind = kHomeBone;
    g_homeward_by = 0;
    g_homeward_bumped = 0;
    if (by) {
        LOG_INFO("[coop] %s's %s: no trip (%u)", steam::persona(by).c_str(), homeward_name(kind), why);
        if (admitted(by)) answer_travel(by, why);
    } else {
        homeward_refused(why, kind);
    }
    if (bumped && bumped != by && admitted(bumped)) answer_travel(bumped, why == kTravelNoBone ? kTravelBusy : why);
}

void homeward_go(bool ready) {
    const HomewardKind kind = g_homeward_kind;
    const bool darksign = kind == kHomeDarksign, bone = kind == kHomeBone;
    const uint64_t by = g_homeward_by;
    const int bonfire = game::last_bonfire();
    uint32_t why = ready ? trip_refusal(darksign ? kTripDarksign : kTripBone) : kTravelBusy;
    if (!why && bonfire < 0) why = kTravelNotThere;
    if (!why && bone && bone_missing()) why = kTravelNoBone;
    if (why) {
        homeward_cancel(why);
        return;
    }
    g_homeward_at = 0;
    g_homeward_kind = kHomeBone;
    g_homeward_bumped = 0;
    g_homeward_by = 0;
    if (by) {
        const std::wstring who = shown_name(*peer(by));
        say(fmt(darksign ? kMsgDarksignBy : bone ? kMsgBoneBy : kMsgMiracleBy, who));
    }
    if (darksign) {
        send_invaders_home();
        LOG_INFO("[coop] Darksign: %d souls and %d humanity lost", game::souls(), game::humanity());
        game::set_souls(0);
        game::set_humanity(0);
    } else if (bone) {
        const int taken = items::take(kHomewardBone, 1);
        LOG_INFO("[coop] Homeward Bone: %d taken from us (%d left); one goes from every guest", taken,
                 items::count(kHomewardBone));
    }
    g_travel_started = now();
    HomewardMsg m{};
    fill_header(&m, kHomeward);
    m.bonfire = bonfire;
    m.darksign = darksign ? 1 : 0;
    m.bone = bone ? 1 : 0;
    broadcast(m);
    LOG_INFO("[coop] %s %s: everyone goes to bonfire %d", by ? steam::persona(by).c_str() : "Host", homeward_name(kind),
             bonfire);
    if (world::local_dead()) {
        // We are down (a guest's Darksign, or a guest's bone in a boss fight): our held death goes ahead now, as a
        // fallen guest's does. Its respawn is at our last bonfire, the trip's.
        LOG_INFO("[coop] We are down: our death goes ahead, its respawn at that bonfire");
        spectate::trip_release();
        world::trip_release();
        return;
    }
    world::warp_now(game::bonfire_map(bonfire), -1);
}

// Host: a guest's trip goes once no menu of ours is open and we are not in a bonfire animation (sitting, kindling),
// as a guest's travel does (host_travel_tick): we are taken with everyone. It waits up to kTravelMenu.
bool homeward_menu_wait() {
    if (!g_homeward_by) return false;
    const bool ceremony = motion::remote_open_anim(game::player()) != 0;
    if (!ceremony && !world::menu_open()) return false;
    if (now() - g_homeward_at > kTravelMenu) {
        LOG_INFO("[coop] %s's %s is dropped: our menu stayed open", steam::persona(g_homeward_by).c_str(),
                 homeward_name(g_homeward_kind));
        homeward_cancel(kTravelTimedOut);
        return true;
    }
    if (now() - g_homeward_told >= kTravelNag) {
        if (g_homeward_told == 0 && admitted(g_homeward_by)) answer_travel(g_homeward_by, kTravelWaiting);
        g_homeward_told = now();
        text::show(fmt(kMsgHomewardHostMenu, shown_name(*peer(g_homeward_by))));
    }
    return true;
}

void host_tick() {
    if (g_step == Step::Closing) {
        host_close_tick();
        return;
    }
    if (g_step != Step::Hosting) return;
    host_world_tick();
    host_npc_tick();
    sent_home_tick();
    host_travel_tick();
    host_hide_while_away();
    host_invasions_open();
    host_ensure_game_session();
    host_lobby_watch();
    if (g_step != Step::Hosting) return;  // a new lobby is being made
    drop_quiet_players();
    const bool ready = game::session_state() == 3;
    for (size_t i = 0; i < g_pending.size();) {
        if (ready) {
            host_answer(g_pending[i]);
            g_pending.erase(g_pending.begin() + i);
        } else if (now() - g_pending[i].since > kPendingBusy) {
            Welcome w{};
            fill_header(&w, kWelcome);
            w.status = kBusy;
            send_to(g_pending[i].from, w);
            g_pending.erase(g_pending.begin() + i);
        } else {
            ++i;
        }
    }
    // A Homeward Bone or the Darksign in a session (Remastered Seamless Co-op): the host goes to their last
    // bonfire and the guests follow there, once our game can take the trip (within 2 s of the use).
    if (g_homeward_at > 0) {
        const bool ready = game::in_game() && !game::loading();
        if (g_homeward_by && !admitted(g_homeward_by)) {
            // The guest who used it has left (their Bye is read before this tick): no trip and no cost for anyone.
            LOG_INFO("[coop] %s's %s is dropped: they left", steam::persona(g_homeward_by).c_str(),
                     homeward_name(g_homeward_kind));
            homeward_cancel(kTravelBusy);
        } else if (ready && homeward_menu_wait()) {
            // a guest's trip waits for our menu
        } else if (ready || now() - g_homeward_at >= 2.0) {
            homeward_go(ready);
        }
    }
    if (now() - g_last_state > 1.0) {
        g_last_state = now();
        const auto& st = rules::state();
        StateMore6 more6{};
        StateMore5& more5 = more6.more5;
        StateMore4& more4 = more5.more4;
        StateMore3& more3 = more4.more3;
        StateMore2& more2 = more3.more2;
        StateMore& more = more2.more;
        StateMsg& s = more.state;
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
        more.net_rate = static_cast<uint8_t>(g_cfg.net_rate);
        more2.seal_fogs = g_cfg.seal_boss_fogs ? 1 : 0;
        for (int i = 0; i < 6; ++i) {
            more3.scaling[i] = static_cast<int16_t>(std::max(-1000, std::min(1000, g_cfg.scaling[i])));
        }
        more4.guest_homeward = 1;
        more5.bloody = static_cast<uint8_t>(g_bloody);
        more6.vicinity = host_vicinity() ? 1 : 0;
        rules::state().invader_present = invader;
        g_host_coop_players = coop;
        broadcast(more6);
    }
}

// ---------------------------------------------------------------- guest
void guest_send_hello() {
    HelloMore more{};
    more.max_players = static_cast<uint8_t>(g_cfg.max_players);
    Hello& h = more.hello;
    fill_header(&h, kHello);
    h.protocol = g_protocol;
    h.invade = g_invade ? 1 : 0;
    h.soul_level = game::soul_level();
    strncpy_s(h.name, utf8(own_name()).c_str(), _TRUNCATE);
    strncpy_s(h.version, g_version.c_str(), _TRUNCATE);
    steam::accept(g_host);
    send_to(g_host, more);
    ++g_hello_tries;
    set_step(Step::Hello);
}

// The soul levels a world's lobby publishes for its co-op players (1.7.0, "ptdesc_levels"); empty when it publishes
// none (a host before 1.7.0).
std::vector<int> lobby_levels(uint64_t lobby) {
    std::vector<int> out;
    const std::string s = steam::lobby_data(lobby, "ptdesc_levels");
    const char* p = s.c_str();
    while (*p) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p) break;
        if (v > 0 && v < 1000) out.push_back(static_cast<int>(v));
        p = *end == ',' ? end + 1 : end;
    }
    return out;
}

// How far (in soul levels) the world's closest player is from our level range, 0 inside it; -1 when it publishes no
// levels. The range is the original game's Red Eye Orb's: 10% below our level to 20 + 10% above it.
double level_gap(int sl, const std::vector<int>& levels) {
    if (levels.empty() || sl <= 0) return -1.0;
    const double lo = sl * 0.9, hi = sl * 1.1 + 20.0;
    double best = 1e9;
    for (int v : levels) best = std::min(best, v < lo ? lo - v : v > hi ? v - hi : 0.0);
    return best;
}

// The level matching's weight for a world (kLevelPull inside our range, fading to 1 far outside it).
double level_weight(int sl, double gap) {
    if (gap < 0) return kLevelUnknown;
    const double fade = std::max(kLevelFade, sl * 0.1);
    return 1.0 + (kLevelPull - 1.0) * std::exp(-gap / fade);
}

// The soul level an invasion is matched by: ours, or a group's highest (its members as their games report it).
int invasion_level() {
    int sl = game::soul_level();
    if (g_group.lead) {
        for (uint64_t id : g_group.members) {
            for (const auto& p : g_peers) {
                if (p.id == id && p.soul_level > sl) sl = p.soul_level;
            }
        }
    }
    return sl;
}

// An invader's pick among the worlds found: at random, weighed by level matching (level_weight) and Dried Fingers
// (kFingersWeight). A 1.0.0 host with Dried Fingers says so in "ptdesc_taunt", a newer one in "ptdesc_fingers".
uint64_t pick_invade_world(const std::vector<uint64_t>& found) {
    static std::mt19937_64 rng = [] {
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        return std::mt19937_64(static_cast<uint64_t>(t.QuadPart) ^ steam::self());
    }();
    const int sl = invasion_level();
    std::vector<double> weight;
    double total = 0;
    int fingers = 0, in_range = 0, unknown = 0;
    for (uint64_t l : found) {
        const bool f = steam::lobby_data(l, "ptdesc_fingers") == "1" || steam::lobby_data(l, "ptdesc_taunt") == "1";
        const double gap = level_gap(sl, lobby_levels(l));
        fingers += f ? 1 : 0;
        in_range += gap == 0.0 ? 1 : 0;
        unknown += gap < 0 ? 1 : 0;
        weight.push_back(level_weight(sl, gap) * (f ? kFingersWeight : 1.0));
        total += weight.back();
    }
    double r = std::uniform_real_distribution<double>(0.0, total)(rng);
    size_t i = 0;
    while (i + 1 < found.size() && r >= weight[i]) r -= weight[i++];
    const bool picked_fingers =
        steam::lobby_data(found[i], "ptdesc_fingers") == "1" || steam::lobby_data(found[i], "ptdesc_taunt") == "1";
    const std::string levels = steam::lobby_data(found[i], "ptdesc_levels");
    const double picked_gap = level_gap(sl, lobby_levels(found[i]));
    LOG_INFO("[coop] %zu world(s) to invade at SL %d: %d with a player in our level range, %d without levels, %d with "
             "Dried Fingers; picked one %s Dried Fingers, levels %s (%s), chance %.0f%%",
             found.size(), sl, in_range, unknown, fingers, picked_fingers ? "with" : "without",
             levels.empty() ? "unknown" : levels.c_str(),
             picked_gap < 0 ? "not published" : picked_gap == 0 ? "in range" : "outside the range",
             total > 0 ? 100.0 * weight[i] / total : 100.0);
    return found[i];
}

// The world we picked cannot take us (full, refused, busy, gone or silent): the next one the search found is
// tried, up to kMaxWorldTries worlds. Not once the host has let us in, and not for a knock that only tells a host
// of another version why we cannot come in.
constexpr int kMaxWorldTries = 5;

bool can_try_another() { return !g_mismatch.active && !g_worlds.empty() && g_world_tries < kMaxWorldTries; }

// How often we knock on one world before trying another: a host answering "busy" (between maps, its player down) gets
// more tries than one that does not answer at all. With no other world to try, 8, and 32 for "busy" (kBusyRetry apart,
// 1.6.0: the 24 s of patience the 8 tries 3 s apart gave, asked four times as often).
int knock_limit(bool busy) { return !can_try_another() ? (busy ? 32 : 8) : busy ? 4 : 2; }

void join_world(uint64_t lobby);

// A world of this search could not take us (`status`, a WelcomeStatus; `wait_s` for kCooldown), counted in `r`.
void note_refusal_to(Refusals& r, uint32_t status, uint32_t wait_s) {
    switch (status) {
        case kCooldown: {
            ++r.cooldown;
            const double at = now() + wait_s;
            if (wait_s > 0 && (r.open_at == 0 || at < r.open_at)) r.open_at = at;
            break;
        }
        case kBossFight: ++r.boss; break;
        case kFull: ++r.full; break;
        case kBusy: ++r.busy; break;
        default: ++r.refused; break;
    }
}

void note_refusal(uint32_t status, uint32_t wait_s) { note_refusal_to(g_refusals, status, wait_s); }

std::wstring minutes_seconds(int s) {
    wchar_t b[16];
    swprintf_s(b, L"%d:%02d", s / 60, s % 60);
    return b;
}

// No world of this search could take us. When the worlds said why (InvasionCooldown, a boss fight, full, a host
// loading or fallen), the player is told that, with the wait until the first world on cooldown opens; else `fallback`.
void finish_search(const wchar_t* fallback) {
    const Refusals r = g_refusals;
    const int worlds = r.cooldown + r.boss + r.full + r.busy + r.refused;
    const int wait = r.cooldown && r.open_at > 0 ? std::max(1, static_cast<int>(std::ceil(r.open_at - now()))) : 0;
    if (worlds > 0) {
        LOG_INFO("[coop] No world could take us: %d invaded recently (the next takes invaders in %d s), %d in a boss "
                 "fight, %d full, %d busy, %d refused", r.cooldown, wait, r.boss, r.full, r.busy, r.refused);
    }
    if (worlds == 0) {
        finish(fallback);
    } else if (worlds == 1) {
        finish(r.cooldown ? fmt(kMsgCooldown, minutes_seconds(wait))
               : r.boss   ? std::wstring(kMsgBossFight)
               : r.full   ? std::wstring(kMsgWorldFull)
               : r.busy   ? std::wstring(kMsgWorldBusy)
               : g_invade ? std::wstring(kMsgNoInvaders)
                          : std::wstring(fallback));
    } else {
        std::wstring parts;
        auto add = [&parts](int n, const wchar_t* what) {
            if (n > 0) parts += (parts.empty() ? L"" : L", ") + std::to_wstring(n) + L" " + what;
        };
        add(r.cooldown, L"invaded recently");
        add(r.boss, L"in a boss fight");
        add(r.full, L"full");
        add(r.busy, L"loading or fallen");
        const bool invade = g_invade;  // finish clears it
        add(r.refused, invade ? L"closed to invaders" : L"refused");
        finish(fmt(invade ? kMsgNoWorldInvade : kMsgNoWorldJoin, parts));
        if (wait) say(fmt(kMsgNextOpens, minutes_seconds(wait)));
    }
}

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
    g_join_retried = false;
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
            if (!try_next_world("could not enter its lobby")) finish_search(kMsgConnectTimeout);
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
        // Every world (each version's lobby says "ptdesc"), not only those with ptdesc_invade "1" as until 1.2.0: a
        // world in a boss fight is found too, so the invader can be told why it cannot take them.
        filters.push_back({"ptdesc", "1"});
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
        // A world that takes no invaders at all (AllowInvaders off, or an older host's boss fight) is left out.
        if (g_invade) {
            found.erase(std::remove_if(found.begin(), found.end(),
                                       [](uint64_t l) {
                                           return steam::lobby_data(l, "ptdesc_invade") != "1" &&
                                                  steam::lobby_data(l, "ptdesc_boss") != "1";
                                       }),
                        found.end());
        }
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
        // A 1.3.0 host's world in a boss fight (ptdesc_boss) or invaded lately (ptdesc_cooldown: until when, in Steam's
        // clock) is passed over without a knock and counted for the line the invader sees when no world can take them,
        // with how long until the first world on cooldown opens. The cooldown goes first, as in host_answer: a world in
        // a boss fight keeps its cooldown running, and that wait is the least the invader still has.
        if (g_invade) {
            const uint32_t t = steam::server_time();
            // A 1.6.0 host's world at its invader limit ("ptdesc_inv_room" 0) is passed over the same way; an older
            // host says nothing of it, and its answer decides.
            std::vector<uint64_t> open;
            int cooling = 0, fighting = 0, full = 0;
            for (uint64_t l : found) {
                const uint32_t until = static_cast<uint32_t>(
                    std::strtoul(steam::lobby_data(l, "ptdesc_cooldown").c_str(), nullptr, 10));
                if (t && until > t) {
                    note_refusal(kCooldown, until - t);
                    ++cooling;
                } else if (steam::lobby_data(l, "ptdesc_invade") != "1") {
                    note_refusal(kBossFight, 0);
                    ++fighting;
                } else if (steam::lobby_data(l, "ptdesc_inv_room") == "0") {
                    note_refusal(kFull, 0);
                    ++full;
                } else {
                    open.push_back(l);
                }
            }
            if (cooling || fighting || full) {
                LOG_INFO("[coop] Worlds passed over: %d invaded recently (InvasionCooldown), %d in a boss fight, %d with "
                         "as many invaders as they take", cooling, fighting, full);
            }
            if (open.empty()) {
                finish_search(kMsgNoOne);
                return;
            }
            found.swap(open);
        }
        const uint64_t pick = g_invade ? pick_invade_world(found) : found.front();
        g_worlds = found;
        g_world_tries = 0;
        join_world(pick);
    });
}

void guest_search() {
    g_search_waves = 0;
    g_refusals = Refusals{};
    set_step(Step::Searching);
    issue_search();
}

const wchar_t* rules_text(int world_rules) {
    return world_rules == 1   ? kMsgRulesHostile
           : world_rules == 2 ? kMsgRulesTeams
           : world_rules == 3 ? kMsgRulesFreeForAll
                              : kMsgRulesNone;
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

void guest_on_welcome(const Welcome& w, uint32_t wait_s) {
    if (g_step != Step::Hello) return;
    if (w.status == kBusy && g_hello_tries < knock_limit(true)) {
        g_last_busy = true;
        set_step(Step::Hello);
        g_step_since = now() - (kHelloWait - kBusyRetry);  // the next knock in kBusyRetry
        return;
    }
    if (w.status == kBossFight || w.status == kCooldown) {
        if (w.status == kBossFight) {
            LOG_INFO("[coop] The host's world is in a boss fight");
        } else {
            LOG_INFO("[coop] The host's world was invaded recently: it takes invaders again in %u s", wait_s);
        }
        note_refusal(w.status, wait_s);
        if (try_next_world(w.status == kBossFight ? "in a boss fight" : "invaded recently")) return;
        finish_search(kMsgBossFight);
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
        note_refusal(w.status, 0);
        if (try_next_world(w.status == kFull ? "full" : w.status == kRefused ? "refused" : "busy")) return;
        finish_search(w.status == kFull || w.status == kRefused ? kMsgNoOne : kMsgConnectTimeout);
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
    // The lone-member timer of the session we left last (game::kSessIdleTimer) would end this join 10 s after that
    // leave, before its load: stopped first.
    if (const float left = game::stop_idle_timer()) {
        LOG_INFO("[coop] The game's timer from our last session had %.1f s left (it would have ended this join): "
                 "stopped", left);
    }
    if (!game::join_from_invitation(w.invitation, w.invitation_size, w.map, pos, w.angle)) {
        finish(kMsgConnectTimeout);
        return;
    }
    // Deaths in the host's world respawn at the host's last bonfire (until the guest rests at one there).
    if (w.bonfire >= 0 && !g_invade) *ptr<int>(game::game_man() + game::kGmLastBonfire) = w.bonfire;
    g_join_seen = false;
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
    knell::left_session("the trip home started");
    g_blossoms_home = true;
    if (!g_invade && g_step == Step::InWorld) {
        const std::vector<int32_t> now_keys = items::key_items();
        g_keys_carried.clear();
        for (int32_t id : now_keys) {
            if (world_key(id) && !holds(g_keys_arrival, id) && !holds(g_keys_lent, id)) g_keys_carried.push_back(id);
        }
        for (int32_t id : g_keys_gained) {
            if (!holds(g_keys_carried, id)) g_keys_carried.push_back(id);  // gone from here after a load
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

// What guest_on_welcome set up for a join that did not happen, taken back: our own last bonfire (a co-op guest's death
// respawns at the host's), the join step and the guest's or invader's rules. Until 1.4.0 a join that ran out of time
// left the host's bonfire as our own last one.
void undo_join_setup() {
    if (!g_invade && g_home_bonfire >= 0) *ptr<int>(game::game_man() + game::kGmLastBonfire) = g_home_bonfire;
    game::set_join_step(0);
    reset_rules();
}

// The game session of our join ended before its load, with us still at home: we knock once more. In the 2026-10-25
// test the first invasion of a world after an invasion of it always fell through this way (the game's own leave,
// 0.5 to 3.5 s into the join), and the player's own retry always got in. Still in the host's lobby, so the hello goes
// at once; the host answers with a new invitation. Once per world (join_world), then home as before.
bool retry_join() {
    if (g_join_retried || !g_host || !g_lobby || game::session_state() != 0 || !game::in_game() || game::loading() ||
        !game::own_world()) {
        return false;
    }
    g_join_retried = true;
    LOG_INFO("[coop] The join fell through before the load (session state 0, join step %d): asking %s again",
             game::join_step(), steam::persona(g_host).c_str());
    // As before the join: guest_on_welcome put the host's bonfire into ours (a co-op guest's deaths respawn there),
    // and set the rules that make us a guest or an invader.
    if (!g_invade && g_home_bonfire >= 0) *ptr<int>(game::game_man() + game::kGmLastBonfire) = g_home_bonfire;
    game::set_join_step(0);
    reset_rules();
    g_hello_tries = 0;
    g_last_busy = false;
    say(kMsgJoinRetry);
    guest_send_hello();
    return true;
}

void guest_arrived() {
    set_step(Step::InWorld);
    // Our respawn in the host's world: the host's bonfire (or our own rest there). A rest at home while the join loaded
    // wrote our own bonfire into the game's last bonfire since guest_on_welcome set it.
    if (!g_invade && g_guest_bonfire >= 0) {
        if (const uint32_t gm = game::game_man()) *ptr<int>(gm + game::kGmLastBonfire) = g_guest_bonfire;
    }
    if (!g_invade) {
        g_keys_arrival = items::key_items();
        g_keys_seen = g_keys_arrival;
        g_keys_gained.clear();
        g_keys_lent.clear();
        g_keys_lent_told = false;
        g_keys_polled = now();
        LOG_INFO("[coop] Key items on arrival: %s", key_text(g_keys_arrival).c_str());
        LOG_INFO("[coop] Key range on arrival: %s", items::key_layout().c_str());
    }
    if (g_invade) {
        const int charges = items::estus_charges();
        if (charges > kInvaderEstus && items::set_estus_charges(kInvaderEstus)) {
            g_estus_kept += charges - kInvaderEstus;
            LOG_INFO("[coop] Estus Flask: %d charges for the invasion, %d set aside until we are home", kInvaderEstus,
                     charges - kInvaderEstus);
        } else if (charges > kInvaderEstus) {
            LOG_ERROR("[coop] Estus Flask: could not set it to %d charges for the invasion (it has %d)", kInvaderEstus,
                      charges);
        }
        int defeat = g_host_coop_players;
        say(fmt(kMsgBreakInMission, std::to_wstring(defeat)));
    } else {
        say(kMsgGuestMission);
        knell::joined();  // the summon arrival, once we stand (1.5.0)
    }
    PlayerEvent e = player_event(kArrived, steam::self(), g_invade, utf8(own_name()));
    if (g_host) send_to(g_host, e);
    if (!g_invade) {
        g_world_wait = now();
        g_world_ours.clear();
    }
    g_copy_settles_at = now() + kCopyFallback;
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

// Host: our world's state below the map flags (the bosses killed, the bells, every NPC's state) for a co-op guest,
// with the welcome (before their game loads our world and runs its events) and again on arrival.
void send_world_globals(uint64_t to) {
    const std::vector<uint8_t> snap = rules::world_global_snapshot();
    if (snap.empty()) {
        LOG_ERROR("[coop] Could not read our world's state (flags 0-1999) for %s", steam::persona(to).c_str());
        return;
    }
    std::vector<uint8_t> msg(sizeof(Header) + snap.size());
    Header h{kMagic, kWorldGlobals};
    std::memcpy(msg.data(), &h, sizeof(h));
    std::memcpy(msg.data() + sizeof(h), snap.data(), snap.size());
    if (!steam::send(to, msg.data(), static_cast<uint32_t>(msg.size()), true)) {
        LOG_ERROR("[coop] Could not send our world's state to %s", steam::persona(to).c_str());
    }
}

// Guest: the host's world state replaces what our copy of their world had (see rules::apply_world_globals).
void on_world_globals(const uint8_t* data, uint32_t size) {
    std::string changes;
    const int changed = rules::apply_world_globals(data, size, &changes);
    drops::flags_rewritten();
    world::defeat_rebase();
    if (changed < 0) {
        LOG_ERROR("[coop] The host's world state did not fit (%u bytes); our copy stays as it was", size);
    } else if (changed > 0) {
        LOG_INFO("[coop] Took the host's world state (flags 0-1999): %d differed from our copy (%s%s)", changed,
                 changes.c_str(), changed > 24 ? " ..." : "");
    } else {
        LOG_INFO("[coop] Took the host's world state (flags 0-1999): our copy had it already");
    }
}

// Guest: the host's map flags replace our copy of the host's world (every map, not only the one we joined in).
void on_world_flags(const uint8_t* data, uint32_t size) {
    const int written = rules::apply_world_flag_snapshot(data, size);
    drops::flags_rewritten();
    world::defeat_rebase();
    if (g_step == Step::InWorld && written >= 0) g_copy_settles_at = now() + kCopySettle;
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

// A guest in the host's world: the key items that belong in the key array there and are missing from it are
// added (`held`: what it holds now). Keys gained in this world come back after a load emptied the array, and with
// a 1.2.0 host the keys of our own world that the host's world has too come along (items::travel_key only: never
// one an event takes back, which would then be both handed over here and still ours at home).
void guest_carry_keys(const std::vector<int32_t>& held) {
    std::vector<int32_t> want;
    for (int32_t id : g_keys_gained) {
        if (items::travel_key(id)) want.push_back(id);
    }
    if (g_host_world_known) {
        const std::vector<int32_t> own = items::own_key_items();
        for (int32_t id : g_host_keys) {
            if (items::travel_key(id) && holds(own, id) && !holds(want, id)) want.push_back(id);
        }
    }
    bool lent_now = false, given = false;
    for (int32_t id : want) {
        if (holds(held, id)) continue;
        const bool gained = holds(g_keys_gained, id);
        if (!items::give(id, 1)) {
            static int lines = 0;
            if (++lines <= 20) LOG_INFO("[coop] Key item %d could not be put in our inventory here", id);
            continue;
        }
        given = true;
        if (!gained && !holds(g_keys_lent, id)) {
            g_keys_lent.push_back(id);
            lent_now = true;
        }
        LOG_INFO("[coop] Key item %d is with us in the host's world: %s", id,
                 gained ? "gained here, back after a load" : "ours, and the host's world has it too");
    }
    if (lent_now && !g_keys_lent_told) {
        g_keys_lent_told = true;
        say(kMsgKeysLent);
    }
    if (given) g_keys_seen = items::key_items();
}

// A guest follows the host (its travel, its Homeward Bone) once we stand in the world: not under a menu (the bonfire
// menu's shops, the box, the level up) or a bonfire ceremony, not in a cutscene, and alive. A fallen guest respawns
// first, through every wait of a respawn (limbo, the team, the lost fight's reload), and follows once back on their
// feet; the host does not travel while it knows of a fallen teammate. True while the follow waits; a menu or a
// ceremony is pointed out every kTravelNag s.
bool follow_waits() {
    if (!game::in_game() || game::loading() || world::local_dead() || rules::cutscene_playing() ||
        rules::cutscene_loading()) {
        return true;
    }
    const bool ceremony = motion::remote_open_anim(game::player()) != 0;
    if (!ceremony && !world::menu_open()) return false;
    if (now() - g_follow_told >= kTravelNag) {
        if (g_follow_told == 0) LOG_INFO("[coop] Following the host waits for our menu to close");
        g_follow_told = now();
        text::show(ceremony ? kMsgTravelFollowSit : kMsgTravelFollowMenu);
    }
    return true;
}

// A guest: the host's travel (kBonfireWarp), followed as follow_waits allows. Our own warp's request that nobody
// answers came to a host that cannot travel for guests.
void guest_travel_tick() {
    if (g_travel_asked > 0 && now() - g_travel_asked > kTravelWait) {
        g_travel_asked = 0;
        LOG_INFO("[coop] Our travel request got no answer: the host's game does not travel for guests");
        say(kMsgTravelOld);
    }
    if (g_follow_travel < 0 || follow_waits()) return;
    const int bonfire = g_follow_travel;
    g_follow_travel = -1;
    LOG_INFO("[coop] Following the host to bonfire %d", bonfire);
    rules::remote_bonfire_warp(bonfire);
}

void guest_tick() {
    const double in_step = now() - g_step_since;
    // A player who falls in their own world while joining (before the game's move into the host's world) never gets
    // there: the death's reload replaces that move. Aimed at the host's bonfire as a guest's death is, it stood them
    // at that bonfire of their OWN world, half in the session, until they left (2026-10-19 19:56). The join is called
    // off at once, before the death's warp, which then takes them to their own last bonfire as any death does.
    if ((g_step == Step::Joining || g_step == Step::Loading) && game::in_game() && !game::loading() &&
        game::own_world() && world::local_dead()) {
        if (g_home_bonfire >= 0) *ptr<int>(game::game_man() + game::kGmLastBonfire) = g_home_bonfire;
        guest_go_home(kMsgFellJoining, "we fell before reaching the other world");
        return;
    }
    switch (g_step) {
        case Step::Searching:
            // One lobby-list request hung after the previous session's P2P failure. Ask once more
            // without restarting the wait; an empty answer still finishes as "No sessions found".
            if (g_search_waves < 2 && in_step > kSearchRetryAfter) {
                LOG_INFO("[coop] Still searching; asking Steam again");
                issue_search();
            }
            if (in_step > kSearchGiveUp) finish(g_invade ? kMsgBreakInTimeout : kMsgJoinTimeout);
            break;
        case Step::LobbyJoin:
            if (in_step > kLobbyJoinGiveUp && !try_next_world("its lobby could not be entered")) {
                finish_search(g_invade ? kMsgBreakInTimeout : kMsgJoinTimeout);
            }
            break;
        case Step::Hello:
            if (in_step > kHelloWait && g_mismatch.active) {
                finish(L"");
                show_join_mismatch(g_mismatch.host);
            } else if (in_step > kHelloWait) {
                if (g_hello_tries < knock_limit(g_last_busy)) {
                    guest_send_hello();
                } else {
                    if (g_last_busy) note_refusal(kBusy, 0);
                    if (!try_next_world(g_last_busy ? "busy" : "no answer")) finish_search(kMsgConnectTimeout);
                }
            }
            break;
        case Step::Joining: {
            const int s = game::session_state();
            if (s != 0) g_join_seen = true;
            if (s == 6) {
                g_join_leave_seen = 0;
                set_step(Step::Loading);
            } else if (s == 0 && g_join_seen) {
                // The game's own join began and ended again before its move into the other world (its idle timer
                // gave up on a host who fell as we came: 2026-11-01, an invasion that then waited out the 40 s below
                // with nothing left to wait for). Ask that world once more (it says if its player is down), else the
                // next world, else we are done.
                LOG_INFO("[coop] The game ended our join before its load (join step %d)", game::join_step());
                if (!retry_join()) {
                    undo_join_setup();
                    if (!try_next_world("the join fell through")) finish(kMsgConnectTimeout);
                }
            } else if (s == 5 || in_step > kJoiningGiveUp) {
                rules::state().leaving = true;
                if (s != 0) game::leave_session();
                undo_join_setup();
                finish(kMsgConnectTimeout);
            }
            break;
        }
        case Step::Loading:
            if (game::session_state() != 6 && in_step > kLoadingGrace) {
                // The game's own leave of our join before its move (still at home: join step 2, the idle timer at
                // 0xD2D0EC) is still going (state 7): the retry needs the session back at 0, so it waits for that
                // (at most 2.5 s; the leave took 0.5 s) instead of giving up at once (2026-11-07 13:33:57, a group's
                // second invader, whose join began 2 s after the first invader's into the same world).
                const bool home = game::in_game() && !game::loading() && game::own_world();
                if (home && !g_join_retried && game::session_state() != 0) {
                    if (g_join_leave_seen == 0) {
                        g_join_leave_seen = now();
                        LOG_INFO("[coop] The game is leaving our join before its load (session state %d); once it is "
                                 "done we knock again", game::session_state());
                    }
                    if (now() - g_join_leave_seen < 2.5) break;
                }
                g_join_leave_seen = 0;
                if (!retry_join()) guest_go_home(kMsgConnectTimeout, "the game session ended while loading");
            } else if (game::join_step() == 4 && game::in_game() && !game::own_world()) {
                guest_arrived();
            } else if (in_step > 180.0) {
                guest_go_home(kMsgConnectTimeout, "loading took over 3 minutes");
            }
            break;
        case Step::InWorld:
            // A won invasion: home now, not on the game's own (much later) timer. A fallen invader is taken home by
            // their held death instead (invasion_won).
            if (g_invade && g_invade_won && g_invade_won_at > 0 && now() - g_invade_won_at >= kWonLeaveAfter &&
                !world::local_dead()) {
                g_invade_won_at = 0;
                guest_go_home(L"", "the invasion was won: every co-op player of this world fell");
                break;
            }
            if (!g_invade && now() - g_keys_polled >= 2.0 && !game::loading() && game::in_game()) {
                g_keys_polled = now();
                const std::vector<int32_t> k = items::key_items();
                if (k != g_keys_seen && !k.empty()) {
                    LOG_INFO("[coop] Key items here now: %s (on arrival: %s)", key_text(k).c_str(),
                             key_text(g_keys_arrival).c_str());
                    g_keys_seen = k;
                    // A key gained here goes into the own world's key array as well (items::keep_key_for_home).
                    for (int32_t id : k) {
                        if (!world_key(id) || holds(g_keys_arrival, id) || holds(g_keys_lent, id) ||
                            holds(g_keys_gained, id)) {
                            continue;
                        }
                        g_keys_gained.push_back(id);
                        LOG_INFO("[coop] Key item %d, gained here, %s", id,
                                 items::keep_key_for_home(id) ? "is kept for home as well"
                                                              : "could not be kept for home (given back there)");
                    }
                }
                guest_carry_keys(k);
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
            // Our own connection to Steam dropped (a network change): we are out of the host's lobby, so the host
            // no longer sends to us. Home now, not after the minute of silence.
            if (out_of_lobby()) {
                guest_go_home(kMsgHostLost, "we are no longer in the host's lobby (our connection to Steam dropped)");
                break;
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
            if (g_bone_owed && game::in_game() && !game::loading()) {
                g_bone_owed = false;
                const int taken = items::take(kHomewardBone, 1);
                LOG_INFO("[coop] The host's Homeward Bone that came during our load: %d of ours taken (%d left)", taken,
                         items::count(kHomewardBone));
            }
            // The host's Homeward Bone: follow after a moment.
            if (g_follow_bonfire >= 0 && now() >= g_follow_at && !follow_waits()) {
                const int bonfire = g_follow_bonfire;
                g_follow_bonfire = -1;
                g_guest_bonfire = bonfire;
                *ptr<int>(game::game_man() + game::kGmLastBonfire) = bonfire;
                rules::arm_trip_reload();
                world::warp_now(game::bonfire_map(bonfire), -1);
            }
            guest_travel_tick();
            break;
        case Step::GoingHome:
            if (g_invade && in_step > kInvaderHomeWait && game::in_game() && !game::own_world() && !game::loading()) {
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
// The event animation our report gives: the one our character holds (1 held with no id known: still "held", -1 is
// "not reported"), kReportCutscene while a cutscene plays or waits for its map, kReportLoading during a load (still
// kReportCutscene for a load that follows a cutscene), so the others count us as back only once our character stands
// here and reports a real 0.
int32_t reported_event_anim() {
    static bool cutscene = false;  // the last report was a cutscene's
    if (rules::cutscene_playing() || rules::cutscene_loading()) {
        cutscene = true;
        return kReportCutscene;
    }
    if (game::loading()) return cutscene ? kReportCutscene : kReportLoading;
    cutscene = false;
    const uint32_t me = game::player();
    const int held = me ? motion::held_event_anim(me) : 0;
    return held < 0 ? 1 : held;
}

// Our own floor collision as the report gives it (-1 none).
void own_floor_id(int* map, int* index) {
    *map = -1;
    *index = -1;
    const uint32_t me = game::player();
    uint32_t hit = 0;
    if (!me || game::loading() || !read32(me + 0x29C, &hit) || !hit || !rules::floor_id(hit, map, index)) {
        *map = -1;
        *index = -1;
    }
}

void send_info() {
    InfoMore5 more5{};
    InfoMore4& more4 = more5.more4;
    InfoMore3& more3 = more4.more3;
    InfoMore2& more2 = more3.more2;
    InfoMore& more = more2.more;
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
    g_info_dead = dead;
    i.light = world::local_lights();
    strncpy_s(i.name, utf8(own_name()).c_str(), _TRUNCATE);
    wcsncpy_s(i.char_name, party::local_name().c_str(), _TRUNCATE);
    i.ceremony = motion::local_loop_anim();
    more.boss_fight = world::boss_fight() ? 1 : 0;
    more.team_respawn = 1;
    more.respawn = static_cast<uint8_t>(
        dead ? g_respawn | (g_respawn == kRespawnWent && g_respawn_kept ? kRespawnKept : 0) : kRespawnNone);
    more.reset = g_reset;  // not read from 1.1.0 on: kResetPhase carries it, reliably and for its rest
    more.boss = rules::own_gauge_boss();
    const uint32_t me = game::player();
    more2.event_anim = reported_event_anim();
    g_info_event = more2.event_anim;
    more3.bones = items::count(kHomewardBone);
    more4.disguised = me && !game::loading() && world::has_effect(me, kChameleonEffect) ? 1 : 0;
    more4.max_players = static_cast<uint8_t>(g_cfg.max_players);
    own_floor_id(&g_info_floor_map, &g_info_floor_index);
    more5.floor_map = g_info_floor_map;
    more5.floor_index = g_info_floor_index;
    broadcast(more5, false);
}

// Our floor differs from what our last report said.
bool floor_changed() {
    int map = -1, index = -1;
    own_floor_id(&map, &index);
    return map != g_info_floor_map || index != g_info_floor_index;
}

// What send_info would report now for our death and our event animation differs from what it last sent.
bool info_changed() {
    bool dead = g_info_dead;
    if (world::local_dead()) {
        dead = true;
    } else if (game::player() && !game::loading()) {
        dead = false;
    }
    return dead != g_info_dead || reported_event_anim() != g_info_event;
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

// What the game keeps on another player's character here that decides whether it is updated, collides and
// takes animations, for the log: [chr+0xC] is its map entity (+8 state, 2 active; +0x14 bit 0 dead or disabled,
// bit 1 under the entity [+0x1C]), chr+0x1FD the update flags (bit 0 fixed rate, bit 3 visible), chr+0xF4 the
// update rate, chr+0x29C its floor, [[chr+0x28]+0x1C] its physics.
struct CopyLook {
    uint32_t ent_state = 0xFFFFFFFF;  // -1: no entity
    uint32_t ent_flags = 0;
    uint32_t f1fd = 0, f1fe = 0, f3c4 = 0;
    int32_t rate = 0;
    int hp = 0;
    bool floor = false, phys = false;
    int held = 0;  // motion::held_event_anim
    // The fields a change of is logged: not the rate, the visible bit or the HP beyond alive or not.
    uint64_t sig() const {
        uint64_t v = ent_state & 0xFF;
        v = v << 8 | (ent_flags & 0xFF);
        v = v << 8 | (f1fd & 0xE7);
        v = v << 16 | (f1fe & 0xFFFF);
        v = v << 4 | (floor ? 8 : 0) | (phys ? 4 : 0) | (hp > 0 ? 2 : 0) | (held != 0 ? 1 : 0);
        return v ^ (static_cast<uint64_t>(f3c4) << 32);
    }
};

CopyLook look_at(uint32_t chr) {
    CopyLook c;
    uint32_t ent = 0, ctrl = 0, phys = 0, floor = 0, v = 0;
    uint8_t b = 0;
    if (read32(chr + 0xC, &ent) && ent && read32(ent + 8, &v)) {
        c.ent_state = v;
        if (read8(ent + 0x14, &b)) c.ent_flags = b;
    }
    if (read8(chr + 0x1FD, &b)) c.f1fd = b;
    if (read32(chr + 0x1FC, &v)) c.f1fe = v >> 16;
    if (read32(chr + 0x3C4, &v)) c.f3c4 = v;
    if (read32(chr + 0xF4, &v)) c.rate = static_cast<int32_t>(v);
    if (read32(chr + 0x2D4, &v)) c.hp = static_cast<int>(v);
    c.floor = read32(chr + 0x29C, &floor) && floor;
    c.phys = read32(chr + 0x28, &ctrl) && ctrl && read32(ctrl + 0x1C, &phys) && phys;
    c.held = motion::held_event_anim(chr);
    return c;
}

std::string describe(const CopyLook& c) {
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "entity %d/%02X, 1FD %02X, 1FE %04X, 3C4 %08X, rate %d, HP %d, floor %s, physics %s, event animation %d",
                  static_cast<int32_t>(c.ent_state), c.ent_flags, c.f1fd, c.f1fe, c.f3c4, c.rate, c.hp,
                  c.floor ? "yes" : "none", c.phys ? "yes" : "none", c.held);
    return buf;
}

// How another player's character is drawn here, every 15 s, when it looks uneven: the updates taken, the longest gap,
// the "stalls" (an update that came only after the copy had covered the one before, so it stood until it came), the
// smoothing, and whether their gait (walk, run, sprint) is fresh. For the 2026-11-01 report of a guest who rejoined
// after two invasions: on every other screen they walked or ran, stopped and went on, over and over (not the sliding of
// before: their body could not be pushed through).
// The smoothing, and the two things it follows (the jitter, and the late tail: the latest an update came lately),
// with our round trips to the player over the same 15 s: until the 2026-11-02 test the line came only above 60 ms, and
// a minute after the Asylum Demon with the smoothing up on a guest's screen and the ping low left nothing in the log.
void player_net_watch() {
    static double last = 0;
    static int lines = 0;
    if (now() - last < 15.0 || lines >= 150 || g_role == Role::None || game::loading()) return;
    const double window = last > 0 ? now() - last : 0;
    last = now();
    if (window <= 0) return;
    for (const auto& [chr, id] : g_chr_peer) {
        uint32_t manip = 0;
        netrate::CopyStats st;
        if (!read32(chr + 0x2C, &manip) || !manip || !netrate::copy_stats(manip, &st)) continue;
        PeerInfo* p = nullptr;
        for (auto& q : g_peers) p = q.id == id ? &q : p;
        if (!p) continue;
        const int rtt_min = p->rtt_min, rtt_max = p->rtt_max;
        p->rtt_min = p->rtt_max = -1;
        const bool gait_fresh = p->gait >= 0 && now() - p->gait_at < 1.5;
        const double rate = st.updates / window;
        const bool uneven = st.stalls >= 8 || st.smoothing_ms > 35 || st.jitter_ms > 15 || st.max_gap_ms > 250 ||
                            (!gait_fresh && p->gait_at > 0);
        if (!uneven) continue;
        ++lines;
        char rtt[48] = "none answered";
        if (rtt_min > 0) std::snprintf(rtt, sizeof(rtt), "%d to %d ms", rtt_min, rtt_max);
        LOG_INFO("[coop] %s's character here, last %.0f s: %.1f updates a second (mean gap %d ms, longest %d ms), %d "
                 "stalls (%d in a slow frame of ours), smoothing %d ms (jitter %d ms, late tail %d ms), round trip %s, "
                 "gait %d %s", p->name.c_str(), window, rate, st.gap_ms, st.max_gap_ms, st.stalls, st.stalls_slow,
                 st.smoothing_ms, st.jitter_ms, st.tail_ms, rtt, p->gait,
                 gait_fresh ? "(fresh)" : p->gait_at > 0 ? "(stale: the game's own guess)" : "(never heard)");
    }
}

// A player's character here far from where their own game has them (their Info, every second), in our map, for a
// while: on 2026-11-02 the laptop host in New Londo could not see mmamun, whose character there was made and alive
// all along, after he rode the Firelink lift down while the host's game left its lift where it was (rules.cpp,
// kCrossLifts). Logged once each time it begins, with where the character stands and where they are.
void stray_copy_watch() {
    struct Stray {
        double since = 0;
        bool told = false;
    };
    static std::map<uint64_t, Stray> strays;
    static double last = 0;
    static int lines = 0;
    if (now() - last < 1.0) return;
    last = now();
    if (g_role == Role::None || game::loading()) {
        strays.clear();
        return;
    }
    constexpr float kFar = 30.0f;
    constexpr double kFor = 6.0;
    const int here = game::map_id();
    std::map<uint64_t, Stray> seen;
    for (const auto& [chr, id] : g_chr_peer) {
        const PeerInfo* p = nullptr;
        for (const auto& q : g_peers) p = q.id == id ? &q : p;
        game::Vec3 at{};
        if (!p || p->dead || p->map != here || now() - p->pos_at > 2.0 || !game::chr_position(chr, &at)) continue;
        const float dx = at.x - p->pos[0], dy = at.y - p->pos[1], dz = at.z - p->pos[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < kFar) continue;
        Stray s = strays.count(id) ? strays[id] : Stray{};
        if (s.since == 0) s.since = now();
        if (!s.told && now() - s.since >= kFor && lines < 40) {
            s.told = true;
            ++lines;
            LOG_INFO("[coop] %s's character here stands %.0f m from where their game has them, for %.0f s now: at "
                     "(%.1f, %.1f, %.1f), they are at (%.1f, %.1f, %.1f) in map %08X", p->name.c_str(), d,
                     now() - s.since, at.x, at.y, at.z, p->pos[0], p->pos[1], p->pos[2], static_cast<uint32_t>(here));
        }
        seen[id] = s;
    }
    strays.swap(seen);
}

// The other players' characters are updated every frame wherever they stand, as the event command "Set Network
// Update Rate: Always" (2004[34], 0xD6D780) does for the Hellkite: chr+0x1FD bit 0 fixes the rate chr+0xF4 at 0.
// Otherwise WorldChrMan's update (0xE3F7D7..0xE3F8D7) gives a character off screen a lower rate and one whose state
// object is not active ([chr+0xC]+8 != 2) none at all (-1): no update, so an event animation it was playing never
// reaches its end (the auto stop 0xE6B180 runs in the update). Vanilla never needed more: a phantom stays in the
// host's area. Across the Firelink / Undead Burg seam the host's event animation 7520 (20:10:28, the guest still in
// Firelink) was left open on the guest's copy of the host, which then had no collision and showed no attacks after
// both stood together again (2026-10-19 test). Such a copy (any event animation, not only the bonfire ones above) is
// also cleared once its player reports holding none (InfoMore2), or after 30 s for a player who does not report it.
// The rate is fixed only on a character made 2 s ago or more: the game sets a new one up in its first frames
// (its entity not active yet, rate -1), as it would without us. (A copy made while its player still got up at their
// bonfire was the one with no collision, both times: party.cpp now waits for that.)
// Each character's state is written to the log when it is first seen and when it changes, and a character we stand
// inside of (it should push us away) is pointed out with it.
std::map<uint32_t, double> g_copy_seen_at;  // another player's character here -> when it was first seen (the upkeep's)

// A character made just now is set up by the game in its first frames (its entity not active, rate -1 for about
// 0.1 s): its update rate is fixed once its entity is active and it has its physics, kCopyBuilt after it was first
// seen, or after kCopyRateFallback whatever it shows (1.5.0; until then 2 s for every character, and the copy of a
// player who just came back moved in jerks until then, at the game's rate 5).
constexpr double kCopyBuilt = 0.5;
constexpr double kCopyRateFallback = 2.0;

void remote_characters_upkeep() {
    static double last = 0;
    if (now() - last < 0.25) return;
    last = now();
    struct Watch {
        double first = 0;
        bool told = false;       // its first line written (once we know whose it is, or after 1.5 s)
        uint64_t sig = 0;
        int inside = 0;          // checks in a row we stood inside it
        double told_inside = -100;
    };
    static std::map<uint32_t, Watch> watch;
    if (g_role == Role::None || game::loading()) {
        watch.clear();  // a load makes every character anew, maybe at the same address
        g_copy_seen_at.clear();
        return;
    }
    static int rate_lines = 0, look_lines = 0, inside_lines = 0;
    std::map<uint32_t, Watch> seen;
    const uint32_t me = game::player();
    game::Vec3 at{};
    float angle = 0;
    const bool me_free = me && !world::local_dead() && motion::held_event_anim(me) == 0 && game::position(&at, &angle);
    for (uint32_t chr : game::remote_player_chrs()) {
        const PeerInfo* p = peer_by_chr(chr);
        const std::string who = p ? p->name : "another player";
        const auto it = watch.find(chr);
        Watch w = it == watch.end() ? Watch{} : it->second;
        CopyLook c = look_at(chr);
        if (it == watch.end()) w.first = now();
        if (!w.told && (p || now() - w.first >= 1.5)) {
            w.told = true;
            w.sig = c.sig();
            if (++look_lines <= 300) {
                char state[96] = "their state not known";
                if (p) {
                    std::snprintf(state, sizeof(state), "%s, up %.1f s, event animation %d", p->dead ? "dead" : "alive",
                                  !p->dead && p->alive_since > 0 ? now() - p->alive_since : 0.0, p->event_anim);
                }
                LOG_INFO("[coop] %s's character %08X is here (made %.1f s ago; %s): %s", who.c_str(), chr,
                         now() - w.first, state, describe(c).c_str());
            }
        } else if (w.told && c.sig() != w.sig) {
            w.sig = c.sig();
            if (++look_lines <= 300) {
                LOG_INFO("[coop] %s's character %08X now: %s", who.c_str(), chr, describe(c).c_str());
            }
        }
        const double age = now() - w.first;
        const bool built = age >= kCopyBuilt && c.ent_state == 2 && c.phys;
        if ((built || age >= kCopyRateFallback) && !((c.f1fd & 1) && c.rate == 0)) {
            *ptr<uint8_t>(chr + 0x1FD) = static_cast<uint8_t>(c.f1fd | 1);
            *ptr<int32_t>(chr + 0xF4) = 0;
            if (++rate_lines <= 20) {
                LOG_INFO("[coop] %s's character %08X is updated every frame from now on (its rate was %d)", who.c_str(),
                         chr, c.rate);
            }
        }
        // Two characters that collide stand 0.5 m or more apart (their capsules).
        game::Vec3 pos{};
        if (me_free && p && !p->dead && p->ceremony == 0 && p->event_anim <= 0 && c.held == 0 && c.hp > 0 &&
            game::chr_position(chr, &pos)) {
            const float dx = pos.x - at.x, dz = pos.z - at.z;
            const float apart = std::sqrt(dx * dx + dz * dz);
            w.inside = apart < 0.3f && std::fabs(pos.y - at.y) < 1.0f ? w.inside + 1 : 0;
            if (w.inside >= 2 && now() - w.told_inside > 20.0 && ++inside_lines <= 20) {
                w.told_inside = now();
                LOG_INFO("[coop] We stand inside %s's character %08X (%.2f m apart), made %.0f s ago: it may not "
                         "collide; %s", who.c_str(), chr, apart, now() - w.first, describe(c).c_str());
            }
        } else {
            w.inside = 0;
        }
        seen[chr] = w;
    }
    watch.swap(seen);
    g_copy_seen_at.clear();
    for (const auto& [chr, w] : watch) g_copy_seen_at[chr] = w.first;
    static std::map<uint32_t, double> held_since;  // characters seen holding an event animation, since when
    static std::map<uint32_t, double> cleared_at;
    static int lines = 0;
    std::map<uint32_t, double> held_now;
    for (const auto& p : g_peers) {
        if (p.dead || now() - p.last_seen > 3.0 || now() - p.anim_at < 3.0) continue;
        const uint32_t chr = chr_of_peer(p.id);
        if (!chr) continue;
        const int held = motion::held_event_anim(chr);
        if (held == 0 || (p.ceremony != 0 && held == p.ceremony)) continue;
        const auto it = held_since.find(chr);
        const double since = it == held_since.end() ? now() : it->second;
        held_now[chr] = since;
        const double held_for = now() - since;
        const bool stuck = p.event_anim >= 0
                               ? p.event_anim == 0 && p.event_free_since > 0 && now() - p.event_free_since >= 3.0 &&
                                     held_for >= 3.0
                               : p.ceremony == 0 && held_for >= 30.0;
        if (!stuck || now() - cleared_at[chr] < 5.0) continue;
        cleared_at[chr] = now();
        if (motion::play_remote_anim(chr, motion::Anim{1, 0, 0}) && ++lines <= 60) {
            LOG_INFO("[coop] %s's character still held event animation %d for %.0f s (%s): cleared", p.name.c_str(), held,
                     held_for, p.event_anim >= 0 ? "they hold none" : "they do not report theirs");
        }
    }
    held_since.swap(held_now);
}

// ---------------------------------------------------------------- summon signs (1.5.0)
// A co-op player's Vigil Soapstone, in a session, leaves a summon sign where they stand, one per player (a new one
// replaces the last); the Death Knell can take anyone else there (knell.cpp). Each player sends their own on a change
// and every kSignResend, through the host as the other world news, so a player who joins later has them within that;
// one not heard of for kSignLapse is gone (its owner left), and the end of the session takes them all.
constexpr double kSignResend = 5.0;
constexpr double kSignLapse = 16.0;
struct SignInfo {
    uint32_t serial = 0;
    int map = -1;  // -1: none
    float pos[3] = {};
    float angle = 0;
    double heard = 0;
    int floor_map = -1;  // the floor its owner stood on (-1 not known)
    int floor_index = -1;
};
SignInfo g_my_sign;
double g_my_sign_sent = 0;
std::map<uint64_t, SignInfo> g_signs;  // the others', by owner
// Each sign is drawn with the game's own summon sign effect (rules::effect_at, id 100, as the game's signs are) in
// every game where its block is in use, ours as well; the game makes no sign of its own (the Vigil Soapstone is our
// item: the White Sign Soapstone's sign, its UseGoods effect, would offer us to the game's own summoning, and that
// soapstone stays the game's). Its handle is the game's and must not move while the effect lasts: the map's nodes do
// not.
constexpr int kSignEffect = 100;
// The sign the Death Knell's view shows glows gold here, with the Warrior of Sunlight's sign effect (118), so its
// player can see which sign it is and where (1.5.0: the view's prompt never showed during the ring's held crouch, and
// the camera stays with the player for a sign).
constexpr int kKnellSignEffect = 118;
struct SignFx {
    alignas(4) uint8_t handle[0x18] = {};
    bool on = false;
    uint32_t serial = 0;
    int effect = 0;  // the effect it was drawn with
};
std::map<uint64_t, SignFx> g_sign_fx;  // by owner (ours under our own id)
uint64_t g_knell_sign = 0;             // the sign the Death Knell's view shows (0 none)
bool g_sign_fx_now = false;            // redraw at once (the view's sign changed)

bool coop_in_world() {
    return (g_role == Role::Host && (g_step == Step::Hosting || g_step == Step::Closing)) ||
           (g_role == Role::Guest && !g_invade && g_step == Step::InWorld);
}

// We invade, standing in the host's world (1.5.0: the co-op players' summon signs are ours to see and to go to with the
// Death Knell, the user's design).
bool invader_in_world() { return g_role == Role::Guest && g_invade && g_step == Step::InWorld; }

// To the invaders in our world (the host's game): the co-op players' summon signs.
template <typename T>
void send_to_invaders(T& msg, uint64_t except = 0) {
    if (!g_lobby) return;
    const uint64_t me = steam::self();
    for (uint64_t m : steam::lobby_members(g_lobby)) {
        if (m != me && m != except && !is_lost(m) && peer_is_invader(m)) steam::send(m, &msg, sizeof(msg), true);
    }
}

// ---------------------------------------------------------------- enemy scaling by vicinity (1.5.0)
// With the host's VicinityScaling each enemy is scaled for the co-op players whose games run it (world.cpp, the
// scaling by vicinity). Every game must give an enemy the same max HP, so each co-op player's game tells every other
// game, the invaders' too, which enemies it runs (kNear: their net handles, the same in every game), and each game
// counts the same lists. Sent straight to everyone, as kInfo is. A list not heard again for kNearLapse (a player who
// left or lost the link) counts for nothing; a load sends an empty one.
constexpr double kNearResend = 2.0;  // our list again this often, changed or not
constexpr double kNearGap = 0.5;     // a changed list goes no sooner than this after the last one
constexpr double kNearLapse = 6.0;
constexpr size_t kNearMax = 1024;
struct NearList {
    std::vector<uint16_t> handles;  // sorted
    double heard = 0;
};
std::map<uint64_t, NearList> g_near;  // the other co-op players' lists
std::vector<uint16_t> g_near_sent;    // ours, as last sent
double g_near_sent_at = 0;

// Host: VicinityScaling for our session. On while every other player's game counts the lists (1.5.0 or later): an
// older game scales every enemy for the whole session, and the games would part on the enemies' health. An invader's
// too (Grok review 064550aa): it is never counted, but an older one still scales every enemy for all the co-op
// players, and its copies, hit by the same numbers, outlive the others' where the host's HP report does not reach.
// Protocol 9 keeps every older version out of the session; this stays for a game that says otherwise (TestVersion).
bool host_vicinity() {
    if (!g_cfg.vicinity_scaling) return false;
    for (const auto& p : g_peers) {
        if (!admitted(p.id) || now() - p.last_seen > 10.0) continue;
        if (compare_versions(p.mod, "1.5.0") < 0) return false;
    }
    return true;
}

void send_near(const std::vector<uint16_t>& list) {
    std::vector<uint8_t> msg(sizeof(Header) + 2 + list.size() * 2);
    const Header h{kMagic, kNear};
    std::memcpy(msg.data(), &h, sizeof(h));
    const uint16_t n = static_cast<uint16_t>(list.size());
    std::memcpy(msg.data() + sizeof(h), &n, 2);
    if (n) std::memcpy(msg.data() + sizeof(h) + 2, list.data(), list.size() * 2);
    broadcast_raw(msg.data(), static_cast<uint32_t>(msg.size()));
    g_near_sent = list;
    g_near_sent_at = now();
}

void near_tick() {
    const bool invader_here = g_role == Role::Guest && g_invade && g_step == Step::InWorld;
    if (!coop_in_world() && !invader_here) {
        g_near.clear();
        g_near_sent.clear();
        g_near_sent_at = 0;
        return;
    }
    if (invader_here) return;  // an invader's game counts the co-op players' lists and sends none
    std::vector<uint16_t> list;
    world::near_handles(&list);  // empty while we load
    if (list.size() > kNearMax) list.resize(kNearMax);
    const double t = now();
    if (t - g_near_sent_at >= kNearResend || (list != g_near_sent && t - g_near_sent_at >= kNearGap)) send_near(list);
}

void on_near(uint64_t from, const uint8_t* data, uint32_t size) {
    uint16_t n = 0;
    if (size < sizeof(Header) + 2) return;
    std::memcpy(&n, data + sizeof(Header), 2);
    if (size < sizeof(Header) + 2 + n * 2u) return;
    NearList& l = g_near[from];
    const bool first = l.heard == 0;
    l.handles.resize(n);
    if (n) std::memcpy(l.handles.data(), data + sizeof(Header) + 2, n * 2u);
    std::sort(l.handles.begin(), l.handles.end());
    l.heard = now();
    if (first) {
        LOG_INFO("[coop] Scaling by vicinity: %s's game runs %d enemies (its first list)", steam::persona(from).c_str(),
                 static_cast<int>(n));
    }
}

void send_sign() {
    SignFloorMsg x{};
    SignMsg& m = x.m;
    fill_header(&m, kSign);
    m.owner = steam::self();
    m.serial = g_my_sign.serial;
    m.map = g_my_sign.map;
    std::copy(std::begin(g_my_sign.pos), std::end(g_my_sign.pos), m.pos);
    m.angle = g_my_sign.angle;
    m.active = g_my_sign.map != -1 ? 1 : 0;
    x.floor_map = g_my_sign.floor_map;
    x.floor_index = g_my_sign.floor_index;
    x.tag = kSignFloorTag;
    if (g_role == Role::Host) {
        broadcast_coop(x);
        send_to_invaders(x);
    } else if (g_host) {
        send_to(g_host, x);
    }
    g_my_sign_sent = now();
}

void sign_fx_end(SignFx& fx) {
    if (fx.on) rules::effect_end(fx.handle);
    fx.on = false;
}

// The effects follow the signs, twice a second; a load ends them all (they are made again where their block is in use).
void sign_fx_tick() {
    static double last = 0;
    if (now() - last < 0.5 && !game::loading() && !g_sign_fx_now) return;
    last = now();
    g_sign_fx_now = false;
    const bool show = (coop_in_world() || invader_in_world()) && game::in_game() && !game::loading() && game::player();
    std::map<uint64_t, const SignInfo*> want;
    if (show) {
        if (g_my_sign.map != -1) want[steam::self()] = &g_my_sign;
        for (const auto& [owner, sign] : g_signs) want[owner] = &sign;
    }
    for (auto it = g_sign_fx.begin(); it != g_sign_fx.end();) {
        const auto w = want.find(it->first);
        const bool keep = w != want.end() && rules::map_in_use(w->second->map);
        const int effect = it->first == g_knell_sign ? kKnellSignEffect : kSignEffect;
        if (!keep || it->second.serial != (w == want.end() ? 0 : w->second->serial) ||
            (it->second.on && it->second.effect != effect)) {
            sign_fx_end(it->second);
        }
        if (w == want.end()) {
            it = g_sign_fx.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [owner, sign] : want) {
        SignFx& fx = g_sign_fx[owner];
        if (fx.on || !rules::map_in_use(sign->map)) continue;
        const float at[3] = {sign->pos[0], sign->pos[1] + 0.05f, sign->pos[2]};
        fx.effect = owner == g_knell_sign ? kKnellSignEffect : kSignEffect;
        fx.on = rules::effect_at(fx.handle, fx.effect, at);
        if (!fx.on && fx.effect != kSignEffect) {
            static int gold_lines = 0;
            if (++gold_lines <= 5) LOG_INFO("[coop] Summon sign: the gold effect %d was not made; white", fx.effect);
            fx.effect = kSignEffect;
            fx.on = rules::effect_at(fx.handle, fx.effect, at);
        }
        fx.serial = sign->serial;
        static int lines = 0;
        if (++lines <= 60) {
            LOG_INFO("[coop] Summon sign of %s %s at (%.1f, %.1f, %.1f)%s",
                     owner == steam::self() ? "ours" : steam::persona(owner).c_str(), fx.on ? "drawn" : "NOT drawn",
                     at[0], at[1], at[2], fx.effect == kKnellSignEffect ? " in gold (the Death Knell shows it)" : "");
        }
    }
}

void signs_tick() {
    sign_fx_tick();
    if (!coop_in_world() && !invader_in_world()) {
        if (g_my_sign.map != -1 || !g_signs.empty()) {
            LOG_INFO("[coop] Summon signs: ours and %d of the others' gone (the session ended)",
                     static_cast<int>(g_signs.size()));
        }
        g_my_sign = SignInfo{};
        g_signs.clear();
        return;
    }
    if (g_my_sign.map != -1 && now() - g_my_sign_sent >= kSignResend) send_sign();
    for (auto it = g_signs.begin(); it != g_signs.end();) {
        if (now() - it->second.heard > kSignLapse) {
            LOG_INFO("[coop] Summon sign of %s gone (not heard of for %.0f s)", steam::persona(it->first).c_str(),
                     kSignLapse);
            it = g_signs.erase(it);
        } else {
            ++it;
        }
    }
}

void on_sign(uint64_t from, const SignMsg& m, int floor_map, int floor_index) {
    if (m.owner == steam::self()) return;
    const std::string who = steam::persona(m.owner);
    if (!m.active) {
        if (g_signs.erase(m.owner)) LOG_INFO("[coop] %s's summon sign is gone", who.c_str());
        return;
    }
    SignInfo& s = g_signs[m.owner];
    const bool fresh = s.serial != m.serial || s.map != m.map;
    s.serial = m.serial;
    s.map = m.map;
    std::copy(std::begin(m.pos), std::end(m.pos), s.pos);
    s.angle = m.angle;
    s.heard = now();
    s.floor_map = floor_map;
    s.floor_index = floor_index;
    if (fresh) {
        LOG_INFO("[coop] %s's summon sign at (%.1f, %.1f, %.1f) in map %08X%s", who.c_str(), m.pos[0], m.pos[1],
                 m.pos[2], static_cast<uint32_t>(m.map), from != m.owner ? " (through the host)" : "");
    }
}

// ---------------------------------------------------------------- the Death Knell's animations on others' characters
// A summon arrival (a join's, or the Death Knell's; knell.cpp) and the knell's price are started on the player's
// character here when it can take them. A joining player's character is made here a moment after their game plays the
// arrival (0.2 s after the message in the 2026-11-03 test: "their character is not here"), and a character made a
// moment ago is not updated yet (entity not active, rate -1 or 5 until remote_characters_upkeep fixes it 2 s on), so
// an event animation started on it then is not played, or never ends (no collision, no attacks; see above). So the
// animation waits for the character: active, with its physics, and given kKnellCopyBuilt more to be built when it was
// made just now (its age as the upkeep or this wait first saw it); its update rate is then fixed as the upkeep does,
// and the animation started. A character that was here already takes it at once. One started late ends when its
// player's does (their report, sent the moment it changes), and one not started before theirs would be over is let
// go: a late arrival would hold their character in place while they walk away.
// The Death Knell's arrival comes the moment its player warped, before their next position does (2026-11-05: started
// on a character still 230 m back where they rang it): the arrival's effect (the white glow, the Praise the Sun light)
// was made there and the character then jumped to them without it ("the animation will play but not the effect").
// An arrival on a character that was here already waits for it to jump (kKnellJump from where it stood when the
// message came), at most kKnellJumpWait; a joining player's is made where they arrive.
// A looped one whose player's report says they no longer hold it is not started (a stop that came before their
// character here was ready).
struct KnellAnimWait {
    int anim = 0;
    double came = 0;          // the message
    uint8_t loop = 0;
    uint32_t chr = 0;         // the character we first saw for them while waiting
    double chr_seen_at = 0;   // when (the upkeep's own first sight is only four times a second)
    bool from_set = false;    // where that character stood when the message came (it was here already)
    game::Vec3 from{};
};
constexpr double kKnellJump = 2.0;       // m: a warp, not a step
// The knell animations that come with a warp (the arrivals), as against the price's stagger and the ring's crouch.
bool knell_warp_anim(int anim) { return anim == 6950 || anim == 6951 || anim == 6952; }
constexpr double kKnellJumpWait = 0.4;   // s: a warp too short to tell
std::map<uint64_t, KnellAnimWait> g_knell_waits;
// A character is built a moment after it is first seen (its entity active, its physics there): 0.1 s more before an
// event animation goes on it (1.5.0 first gave it 0.5 s, which showed as lag on a join).
constexpr double kKnellCopyBuilt = 0.1;
// An arrival started late on a character made after it came: it ends when its player's does (their report says they
// hold no event animation any more), so the character does not stand there after them.
struct KnellLate {
    uint32_t chr = 0;
    int anim = 0;
    double started = 0;
    bool seen_held = false;  // their report showed an event animation since: a 0 after that is the end of theirs
};
std::map<uint64_t, KnellLate> g_knell_late;
constexpr double kKnellLateAfter = 0.25;
constexpr int kStaggerAnim = 2010;  // the knell's price: never late enough to matter, and it ends by itself
// The Death Knell's crouch (motion kHold / kRelease, 1.5.0): while its player's view is open their character here is
// held where its animation is, as theirs is (motion::hold_anims), so the others see them down in the ring's crouch. Let
// go by their release, or kKnellHoldLimit after the hold (their view's own limit is two minutes), or when their
// character here is another (made again). A release that comes while their character is missing here for a moment
// waits for it (`release`): dropping the record then left the character held for good once it was back.
struct KnellHeld {
    uint32_t chr = 0;
    double at = 0;
    bool release = false;
};
std::map<uint64_t, KnellHeld> g_knell_held;
constexpr double kKnellHoldLimit = 130.0;
void knell_held_tick() {
    static int lines = 0;
    for (auto it = g_knell_held.begin(); it != g_knell_held.end();) {
        const uint32_t chr = chr_of_peer(it->first);
        // Not in this world this frame: keep the record. Their character is often missing for a frame, and writing
        // the old one after it was freed is not safe. A new character is another object, and was never held.
        if (!chr) {
            if (now() - it->second.at > kKnellHoldLimit) it = g_knell_held.erase(it);
            else ++it;
            continue;
        }
        if (chr != it->second.chr) {
            it = g_knell_held.erase(it);
            continue;
        }
        if (it->second.release) {
            const bool done = motion::hold_anims(chr, false);
            if (++lines <= 30) {
                LOG_INFO("[coop] Knell hold on %08X let go: their release came while the character was not here%s", chr,
                         done ? "" : " (FAILED)");
            }
            it = g_knell_held.erase(it);
            continue;
        }
        if (now() - it->second.at > kKnellHoldLimit) {
            motion::hold_anims(chr, false);
            if (++lines <= 30) LOG_INFO("[coop] Knell hold on %08X let go: no release came in %.0f s", chr, kKnellHoldLimit);
            it = g_knell_held.erase(it);
            continue;
        }
        // The sender's own game holds the speed again whenever the game sets it back (knell.cpp hold_tick). One
        // write here does not last when the same thing happens to their character in this game.
        if (!motion::anims_held(chr)) {
            motion::hold_anims(chr, true);
            if (++lines <= 30) {
                LOG_INFO("[coop] Knell hold on %08X: the game set the speed back; held again", chr);
            }
        }
        ++it;
    }
}

void knell_late_tick() {
    static int lines = 0;
    for (auto it = g_knell_late.begin(); it != g_knell_late.end();) {
        KnellLate& l = it->second;
        const PeerInfo* p = nullptr;
        for (const auto& q : g_peers) {
            if (q.id == it->first) p = &q;
        }
        if (!p || chr_of_peer(it->first) != l.chr || motion::held_event_anim(l.chr) != l.anim ||
            now() - l.started > motion::knell_anim_seconds(l.anim) + 2.0) {
            it = g_knell_late.erase(it);
            continue;
        }
        if (p->event_anim != 0) {
            l.seen_held = true;
        } else if (l.seen_held && now() - l.started > 0.3) {
            if (motion::play_remote_anim(l.chr, motion::Anim{1, 0, 0}) && ++lines <= 60) {
                LOG_INFO("[coop] %s: knell animation %d ended with theirs (%.2f s after it started here)",
                         p->name.c_str(), l.anim, now() - l.started);
            }
            it = g_knell_late.erase(it);
            continue;
        }
        ++it;
    }
}

void knell_anims_tick() {
    if (g_role == Role::None || game::loading()) {
        g_knell_waits.clear();
        g_knell_late.clear();
        // A load frees the characters. The session ending does not: let a character that is still here walk again.
        if (!game::loading()) {
            for (const auto& kv : g_knell_held) {
                const uint32_t chr = chr_of_peer(kv.first);
                if (chr && chr == kv.second.chr) motion::hold_anims(chr, false);
            }
        }
        g_knell_held.clear();
        if (game::loading()) motion::forget_held();
        return;
    }
    if (!g_knell_held.empty()) knell_held_tick();
    if (!g_knell_late.empty()) knell_late_tick();
    if (g_knell_waits.empty()) return;
    static int lines = 0;
    for (auto it = g_knell_waits.begin(); it != g_knell_waits.end();) {
        KnellAnimWait& w = it->second;
        PeerInfo* p = nullptr;
        for (auto& q : g_peers) {
            if (q.id == it->first) p = &q;
        }
        const char* name = p ? p->name.c_str() : "another player";
        const double age = now() - w.came;
        if (w.loop && p && age > 2.0 && p->event_anim >= 0 && p->event_anim != w.anim) {
            if (++lines <= 60) {
                LOG_INFO("[coop] %s: knell animation %d not shown (they no longer hold it; their character here was not "
                         "ready in %.1f s)", name, w.anim, age);
            }
            it = g_knell_waits.erase(it);
            continue;
        }
        if (!p || age > motion::knell_anim_seconds(w.anim) + 0.5) {
            if (++lines <= 60) {
                LOG_INFO("[coop] %s: knell animation %d not shown (their character here was not ready in %.1f s)", name,
                         w.anim, age);
            }
            it = g_knell_waits.erase(it);
            continue;
        }
        const uint32_t chr = chr_of_peer(it->first);
        if (chr != w.chr) {
            w.chr = chr;
            w.chr_seen_at = now();
        }
        const CopyLook c = chr ? look_at(chr) : CopyLook{};
        if (!chr || c.ent_state != 2 || c.hp <= 0 || !c.phys) {
            ++it;
            continue;
        }
        const auto seen = g_copy_seen_at.find(chr);
        const double copy_age = std::max(seen == g_copy_seen_at.end() ? 0.0 : now() - seen->second, now() - w.chr_seen_at);
        const bool fixed = (c.f1fd & 1) && c.rate == 0;  // here a while: the upkeep fixed its rate
        if (!fixed && copy_age < kKnellCopyBuilt) {
            ++it;
            continue;
        }
        if (!fixed) {
            *ptr<uint8_t>(chr + 0x1FD) = static_cast<uint8_t>(c.f1fd | 1);
            *ptr<int32_t>(chr + 0xF4) = 0;
        }
        // A Death Knell arrival on a character that was here when it came: once it has jumped to where they arrived.
        bool jumped = true;
        if (w.from_set && knell_warp_anim(w.anim) && age < kKnellJumpWait) {
            game::Vec3 at{};
            jumped = game::chr_position(chr, &at) && (at.x - w.from.x) * (at.x - w.from.x) +
                                                             (at.y - w.from.y) * (at.y - w.from.y) +
                                                             (at.z - w.from.z) * (at.z - w.from.z) >
                                                         kKnellJump * kKnellJump;
        }
        if (!jumped) {
            ++it;
            continue;
        }
        if (!motion::play_remote_anim(chr, motion::Anim{0, w.anim, w.loop})) {
            ++it;
            continue;
        }
        p->anim_at = now();  // the stuck-animation clean-up above leaves it be for a while
        if (age > kKnellLateAfter && w.anim != kStaggerAnim) {
            g_knell_late[it->first] = KnellLate{chr, w.anim, now(), p->event_anim != 0};
        }
        if (++lines <= 60) {
            LOG_INFO("[coop] %s: knell animation %d shown on their character %08X (%.2f s after it came%s)", name,
                     w.anim, chr, age,
                     !fixed       ? "; the character was just made"
                     : w.from_set ? (age < kKnellJumpWait ? "; once it jumped to where they arrived"
                                                          : "; it did not jump: a short warp, or none")
                                  : "");
        }
        it = g_knell_waits.erase(it);
    }
}

void knell_anim_came(uint64_t from, int anim, uint8_t loop) {
    g_knell_late.erase(from);  // a new one of theirs replaces a late one still playing
    KnellAnimWait w{anim, now(), loop};
    // Their character here already (a Death Knell's arrival): where it stands now.
    const uint32_t chr = chr_of_peer(from);
    if (chr && knell_warp_anim(anim) && game::chr_position(chr, &w.from)) w.from_set = true;
    g_knell_waits[from] = w;
    knell_anims_tick();
}

// ---------------------------------------------------------------- group invasions (1.6.0)
// The user's design: a host with its world open, with co-op guests or alone, uses the Chaos Eye Orb (Ctrl+F4) and the
// whole party invades another world together. The world stays open while the search runs, to co-op players and
// invaders alike (the user's design: a search is no shelter from invasion), and an invasion of it comes first: an
// invader let in, or another group on its way in (places our world holds for it), calls the search off, and keeps a
// new one from starting while it lasts. A co-op player who comes in meanwhile stays out of the group.
//
// The search lists the worlds an invader's search finds and keeps those whose host is of 1.6.0 or later (it keeps the
// invader limit; an older one takes any number) and has room for the whole group by its lobby's "ptdesc_inv_room".
// Each is asked in turn: our game enters its lobby beside our own and asks its host (kGroupAsk), who checks what it
// checks for a knock, with its invader limit and MaxPlayers for the whole group, and holds the group's places for
// kGroupHoldFor (45 s; a place not taken by then is anyone's again) or says why not (kGroupAnswer). Once one says yes
// (and still no invader is in our world) our world stops taking invaders and closes: its guests are told first (kGroupGo:
// the world and the group) and go home, as a join leaves from one's own world, and the host's close then finds them
// gone. Each knocks there as an evil spirit, the host as soon as its world is closed, a guest once it stands at home,
// and takes the place held for it; there they invade as one team (an invasion has one outcome for all its invaders,
// 1.3.0). Back home, the host opens its world again and sends its new lobby to each of the others (kGroupBack, straight
// to them: no lobby joins them any more), and each joins it as a co-op guest once it stands at home. A knock of the
// host's that does not get it there calls the invasion off for those not let in yet (kGroupOff), and the party gathers
// again the same way.

const char* group_phase_name(GroupPhase p) {
    switch (p) {
        case GroupPhase::None: return "none";
        case GroupPhase::Searching: return "searching";
        case GroupPhase::Leaving: return "leaving";
        case GroupPhase::Joining: return "joining";
        case GroupPhase::Invading: return "invading";
        case GroupPhase::Returning: return "returning";
        case GroupPhase::Rejoining: return "rejoining";
    }
    return "?";
}

void set_group_phase(GroupPhase p, const char* why) {
    if (g_group.phase == p) return;
    LOG_INFO("[coop] Group invasion: %s -> %s (%s)", group_phase_name(g_group.phase), group_phase_name(p), why);
    if (p == GroupPhase::None) {
        if (g_group.asking) steam::leave_lobby(g_group.asking);
        g_group = Group{};
        return;
    }
    g_group.phase = p;
    g_group.since = now();
    g_group.ready_since = 0;
    g_group.told = false;
}

bool in_group(uint64_t id) {
    return std::find(g_group.members.begin(), g_group.members.end(), id) != g_group.members.end();
}

// To the group's players but us (reliable).
template <typename T>
void send_group(T& msg) {
    const uint64_t me = steam::self();
    for (uint64_t id : g_group.members) {
        if (id != me) send_to(id, msg);
    }
}

const PeerInfo* find_peer(uint64_t id) {
    for (const auto& p : g_peers) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

std::string group_names() {
    std::string s;
    for (uint64_t id : g_group.members) s += (s.empty() ? "" : ", ") + steam::persona(id);
    return s;
}

// Why our party cannot invade together now (empty: it can); `guests` gets the co-op guests who go along. Before the
// search (no group yet) every co-op guest must be able to go: of 1.6.0, arrived, standing, not in a load. Once the
// search runs only the group's own are checked: a co-op player who comes in meanwhile is no reason to stop it (they
// stay out of the group and are sent home by the close as any guest is), and one of the group who left is let go.
// Our world stays open to invaders while the search runs (the user's design: a search is no shelter from invasion),
// and an invasion of it comes first (the user's: two parties after a fight should have it): an invader let into our
// world, or places our world holds for another group on its way (kGroupAsk), keeps the search from starting and calls
// a running one off, so no invasion of ours is ever cut short by our group leaving.
std::wstring group_refusal(std::vector<uint64_t>* guests) {
    if (!hosting() || !game::in_game() || game::loading() || !game::own_world() || !game::player()) {
        return kMsgGroupNotNow;
    }
    const bool everyone = g_group.phase == GroupPhase::None;
    if (world::local_dead()) return kMsgGroupFallen;
    if (rules::state().invader_present || !g_invaders_in.empty() || invaders_here(0) > 0) return kMsgGroupInvader;
    if (held_places(0) > 0) return kMsgGroupIncoming;
    if (boss_fight_in_world()) return kMsgGroupBoss;
    if (trip_slot_busy() || g_travel_bonfire >= 0) return kMsgGroupTravel;
    for (uint64_t id : g_admitted) {
        const PeerInfo* p = find_peer(id);
        if (!p || p->invader || now() - p->last_seen > 10.0) continue;
        if (!everyone && !in_group(id)) continue;
        if (compare_versions(p->mod, kGroupVersion) < 0) return fmt(kMsgGroupOld, shown_name(*p));
        if (p->arrived_at == 0 || p->dead || p->event_anim == kReportLoading || p->event_anim == kReportCutscene) {
            return fmt(kMsgGroupWait, shown_name(*p));
        }
        if (guests) guests->push_back(id);
    }
    return L"";
}

// The least any game of the group holds: the world we invade needs room in every one of them.
int group_max_players() {
    int least = g_cfg.max_players;
    for (uint64_t id : g_group.members) {
        if (id == steam::self()) continue;
        const PeerInfo* p = find_peer(id);
        least = std::min(least, p && p->max_players > 0 ? p->max_players : 4);
    }
    return least;
}

// What the worlds found said, as finish_search puts it for one invader.
std::wstring group_none_line() {
    const Refusals& r = g_group.refusals;
    const bool solo = g_group.members.size() <= 1;
    std::wstring parts;
    auto add = [&parts](int n, const wchar_t* what) {
        if (n > 0) parts += (parts.empty() ? L"" : L", ") + std::to_wstring(n) + L" " + what;
    };
    add(r.cooldown, L"invaded recently");
    add(r.boss, L"in a boss fight");
    add(r.full, solo ? L"full" : L"without room for you all");
    add(r.busy, L"loading or fallen");
    add(r.quiet, L"not answering");
    add(r.refused, L"closed to invaders");
    add(g_group.older, L"on an older version");
    if (parts.empty()) return solo ? kMsgGroupNoneSolo : kMsgGroupNone;
    return fmt(solo ? kMsgGroupNoneSoloWhy : kMsgGroupNoneWhy, parts);
}

// The search is over without a world, or called off: the others hear it, and our world takes invaders again
// (host_invasions_open, within a second).
void group_search_over(const std::wstring& line, const char* why) {
    GroupSearchMsg m{};
    fill_header(&m, kGroupSearch);
    m.searching = 0;
    m.size = static_cast<uint8_t>(g_group.members.size());
    send_group(m);
    const Refusals r = g_group.refusals;
    set_group_phase(GroupPhase::None, why);
    if (!line.empty()) say(line);
    if (r.cooldown && r.open_at > 0) {
        say(fmt(kMsgNextOpens, minutes_seconds(std::max(1, static_cast<int>(std::ceil(r.open_at - now()))))));
    }
}

// Something in our world calls the running search off (`why`, group_refusal's line): an invasion of it, in words of
// its own; anything else after "called off".
void group_call_off(const std::wstring& why) {
    if (why == kMsgGroupInvader) {
        group_search_over(kMsgGroupOffInvader, "an invader came into our world");
    } else if (why == kMsgGroupIncoming) {
        group_search_over(kMsgGroupOffIncoming, "another group is on its way into our world");
    } else {
        say(kMsgGroupCalledOff);
        group_search_over(why, "our world changed");
    }
}

void group_ask_next();

void group_list() {
    const int gen = ++g_group.gen;
    ++g_group.waves;
    g_group.listing = true;
    g_group.listed_at = now();
    steam::find_lobbies({{"ptdesc", "1"}}, 50, [gen](std::vector<uint64_t> found) {
        if (gen != g_group.gen || !g_group.lead || g_group.phase != GroupPhase::Searching) return;
        g_group.listing = false;
        const uint64_t me = steam::self();
        const int size = static_cast<int>(g_group.members.size());
        const uint32_t t = steam::server_time();
        std::vector<uint64_t> open;
        int closed = 0;
        for (uint64_t l : found) {
            if (l == g_lobby || steam::lobby_owner(l) == me) continue;
            const bool takes = steam::lobby_data(l, "ptdesc_invade") == "1";
            if (!takes && steam::lobby_data(l, "ptdesc_boss") != "1") {
                ++closed;  // takes no invaders at all (AllowInvaders off, or an older host's boss fight)
                continue;
            }
            const PeerVersion v = lobby_version(l);
            if (!same_version(v) || compare_versions(v.mod, kGroupVersion) < 0) {
                ++g_group.older;
                continue;
            }
            const uint32_t until =
                static_cast<uint32_t>(std::strtoul(steam::lobby_data(l, "ptdesc_cooldown").c_str(), nullptr, 10));
            if (t && until > t) {
                note_refusal_to(g_group.refusals, kCooldown, until - t);
            } else if (!takes) {
                note_refusal_to(g_group.refusals, kBossFight, 0);
            } else if (std::atoi(steam::lobby_data(l, "ptdesc_inv_room").c_str()) < size) {
                note_refusal_to(g_group.refusals, kFull, 0);
            } else {
                open.push_back(l);
            }
        }
        const Refusals& r = g_group.refusals;
        LOG_INFO("[coop] Group invasion: %zu world(s) found, %zu with room for our %d; passed over %d of an older "
                 "version, %d invaded recently, %d in a boss fight, %d without room, %d that take no invaders",
                 found.size(), open.size(), size, g_group.older, r.cooldown, r.boss, r.full, closed);
        if (open.empty()) {
            group_search_over(group_none_line(), "no world has room for us");
            return;
        }
        g_group.worlds = open;
        group_ask_next();
    });
}

void group_ask_next() {
    if (g_group.asking) {
        steam::leave_lobby(g_group.asking);
        g_group.asking = 0;
        g_group.asking_host = 0;
    }
    g_group.ask_pending = false;
    if (g_group.worlds.empty() || g_group.asks >= kMaxWorldTries) {
        group_search_over(group_none_line(), "no world said yes");
        return;
    }
    const uint64_t pick = pick_invade_world(g_group.worlds);
    g_group.worlds.erase(std::remove(g_group.worlds.begin(), g_group.worlds.end(), pick), g_group.worlds.end());
    ++g_group.asks;
    const int gen = ++g_group.gen;
    g_group.ask_pending = true;
    g_group.asked_at = now();
    steam::join_lobby(pick, [gen](bool ok, uint64_t lobby) {
        if (gen != g_group.gen || !g_group.lead || g_group.phase != GroupPhase::Searching) {
            if (ok && lobby && lobby != g_lobby) steam::leave_lobby(lobby);
            return;
        }
        if (!ok) {
            LOG_INFO("[coop] Group invasion: could not enter that world's lobby to ask; the next");
            note_refusal_to(g_group.refusals, kBusy, 0);
            group_ask_next();
            return;
        }
        g_group.asking = lobby;
        g_group.asking_host = steam::lobby_owner(lobby);
        g_group.asked_at = now();
        GroupAskMsg m{};
        fill_header(&m, kGroupAsk);
        m.protocol = g_protocol;
        strncpy_s(m.version, g_version.c_str(), _TRUNCATE);
        m.max_players = static_cast<uint8_t>(group_max_players());
        m.count = static_cast<uint8_t>(std::min<size_t>(g_group.members.size(), kGroupMax));
        for (int i = 0; i < m.count; ++i) m.ids[i] = g_group.members[i];
        steam::accept(g_group.asking_host);
        send_to(g_group.asking_host, m);
        LOG_INFO("[coop] Group invasion: asked %s for %u invader place(s)", steam::persona(g_group.asking_host).c_str(),
                 m.count);
    });
}

// Host: the Chaos Eye Orb (Ctrl+F4) while our world is open.
void group_invade() {
    if (g_group.lead && g_group.phase == GroupPhase::Searching) {
        group_search_over(kMsgGroupCalledOff, "the Chaos Eye Orb again");
        return;
    }
    if (g_group.phase != GroupPhase::None || !hosting()) {
        LOG_INFO("[coop] Invade request ignored (%s)", g_group.phase != GroupPhase::None
                                                             ? "a group invasion of ours is under way"
                                                             : "our world is not open yet");
        if (g_group.lead && g_group.phase == GroupPhase::Rejoining) say(kMsgGroupGathering);
        return;
    }
    std::vector<uint64_t> guests;
    const std::wstring why = group_refusal(&guests);
    if (!why.empty()) {
        say(why);
        LOG_INFO("[coop] Group invasion: not now (%s)", utf8(why).c_str());
        return;
    }
    g_group = Group{};
    g_group.lead = true;
    g_group.leader = steam::self();
    g_group.leader_name = own_name();
    g_group.members.push_back(steam::self());
    g_group.members.insert(g_group.members.end(), guests.begin(), guests.end());
    set_group_phase(GroupPhase::Searching, "the Chaos Eye Orb while hosting");
    const int size = static_cast<int>(g_group.members.size());
    say(size > 1 ? fmt(kMsgGroupSearch, std::to_wstring(size)) : std::wstring(kMsgGroupSearchSolo));
    GroupSearchMsg m{};
    fill_header(&m, kGroupSearch);
    m.searching = 1;
    m.size = static_cast<uint8_t>(size);
    send_group(m);
    LOG_INFO("[coop] Group invasion: looking for a world for our %d player(s): %s", size, group_names().c_str());
    group_list();
}

// A world said yes: our world closes and the group goes.
void group_go() {
    std::vector<uint64_t> guests;
    const std::wstring why = group_refusal(&guests);
    if (!why.empty()) {
        // Our world changed while the world was asked (an invader came in, a boss fight, a guest who fell): the places
        // held there lapse by themselves.
        group_call_off(why);
        return;
    }
    // Those the places are held for who can still go; one who came in during the search after the ask stays out.
    std::vector<uint64_t> going{steam::self()};
    for (uint64_t id : guests) {
        if (in_group(id)) going.push_back(id);
    }
    g_group.members = going;
    g_group.target = g_group.asking;
    const uint64_t target_host = g_group.asking_host;
    steam::leave_lobby(g_group.asking);  // we knock there again once our world is closed
    g_group.asking = 0;
    g_group.asking_host = 0;
    g_group.ask_pending = false;
    GroupGoMsg m{};
    fill_header(&m, kGroupGo);
    m.lobby = g_group.target;
    m.count = static_cast<uint8_t>(std::min<size_t>(going.size(), kGroupMax));
    for (int i = 0; i < m.count; ++i) m.ids[i] = going[i];
    m.host = target_host;
    send_group(m);
    LOG_INFO("[coop] Group invasion: %s's world holds our %zu place(s); our world closes and we go (%s)",
             steam::persona(target_host).c_str(), going.size(), group_names().c_str());
    say(going.size() > 1 ? kMsgGroupGoLeader : kMsgGroupGoSolo);
    // Only now does our world stop taking invaders: the close below hides its lobby and answers no knock (a knock
    // waiting for its answer is dropped). No invader is in it (group_refusal just said so).
    set_group_phase(GroupPhase::Leaving, "a world holds our places");
    leave();  // our close: the Bye, then our game session ends once the guests are gone (host_close_tick)
}

void group_on_answer(uint64_t from, const GroupAnswerMsg& m) {
    if (!g_group.lead || g_group.phase != GroupPhase::Searching || !g_group.asking || from != g_group.asking_host) {
        return;
    }
    LOG_INFO("[coop] Group invasion: %s answered %u (%u invader place(s) open before ours%s)",
             steam::persona(from).c_str(), m.status, m.places, m.status == kCooldown ? ", invaded recently" : "");
    if (m.status == kOk) {
        group_go();
        return;
    }
    if (m.status == kVersion) {
        ++g_group.older;
    } else {
        note_refusal_to(g_group.refusals, m.status, m.wait_s);
    }
    group_ask_next();
}

// Host: another world's group asks for places (kGroupAsk). The same checks as a knock (host_answer), with the invader
// limit and MaxPlayers for the whole group; a yes holds the places for kGroupHoldFor. No answer while we cannot say
// (no session): the asker's wait runs out.
void group_on_ask(uint64_t from, const GroupAskMsg& m) {
    if (g_role != Role::Host || g_step != Step::Hosting) return;
    char v[sizeof(m.version) + 1] = {};
    std::memcpy(v, m.version, sizeof(m.version));
    PeerVersion them;
    them.protocol = m.protocol;
    them.mod = v;
    const int count = std::max(1, std::min<int>(m.count, kGroupMax));
    GroupAnswerMsg a{};
    fill_header(&a, kGroupAnswer);
    a.status = kOk;
    // A world whose own group is searching takes invaders and groups too: the places a yes holds call its own search
    // off (group_refusal), so the group coming in finds it there.
    if (!same_version(them) || compare_versions(them.mod, kGroupVersion) < 0) {
        a.status = kVersion;
    } else if (!g_cfg.allow_invaders) {
        a.status = kRefused;
    } else if (now() < g_invade_open_at) {
        a.status = kCooldown;
        a.wait_s = static_cast<uint32_t>(std::ceil(g_invade_open_at - now()));
    } else if (boss_fight_in_world()) {
        a.status = kBossFight;
    } else if (!game::in_game() || game::loading() || !game::own_world() || world::local_dead()) {
        a.status = kBusy;
    }
    // A group that asks again (its first answer lost, or a new search) gives up the places it held.
    g_group_holds.erase(std::remove_if(g_group_holds.begin(), g_group_holds.end(),
                                       [from](const GroupHold& g) { return g.leader == from; }),
                        g_group_holds.end());
    int in_session = 1;
    const int room = session_room(std::max(4, static_cast<int>(m.max_players)), 0, &in_session);
    const int held = held_places(0);
    const int limit = invader_cap();
    const int invaders = invaders_here(0);
    const int places = std::max(0, std::min(limit - invaders - held, room - in_session - held));
    a.places = static_cast<uint8_t>(std::min(places, 255));
    if (a.status == kOk && count > places) a.status = kFull;
    if (a.status == kOk) {
        GroupHold hold;
        hold.leader = from;
        hold.until = now() + kGroupHoldFor;
        for (int i = 0; i < count; ++i) {
            if (!m.ids[i]) continue;
            hold.ids.push_back(m.ids[i]);
            // Each of them is accepted now, while they are on their way: a knock whose session request comes before
            // Steam says they entered our lobby is let through at once, not 3.4 s later. Accepted only, nothing sent:
            // with bbc197f's no-op sent to them as well (here and at every lobby entry), every ask of the 2026-11-07
            // 15:58-18:03 tests went unanswered and Steam's link to the asked host timed out.
            steam::accept(m.ids[i]);
        }
        hold.size = static_cast<int>(hold.ids.size());
        g_group_holds.push_back(hold);
    }
    send_to(from, a);
    LOG_INFO("[coop] %s asked for %d invader place(s) for their group: %s (%d co-op player(s), %d invader(s) here, %d "
             "place(s) held, limit %d, %d of room %d in use)",
             steam::persona(from).c_str(), count, a.status == kOk ? "held" : "refused", 1 + coop_guest_count(), invaders,
             held, limit, in_session, room);
}

// A co-op guest: our host's group invades another world (kGroupGo). Only a guest who stands in the world goes along.
void group_on_go(uint64_t from, const GroupGoMsg& m) {
    if (g_role != Role::Guest || g_invade || from != g_host || g_step != Step::InWorld || !m.lobby) return;
    const int count = std::max(0, std::min<int>(m.count, kGroupMax));
    const std::vector<uint64_t> ids(m.ids, m.ids + count);
    if (std::find(ids.begin(), ids.end(), steam::self()) == ids.end()) return;
    g_group = Group{};
    g_group.leader = from;
    g_group.leader_name = shown_name(*peer(from));
    g_group.members = ids;
    g_group.target = m.lobby;
    set_group_phase(GroupPhase::Leaving, "our host's group invades another world");
    LOG_INFO("[coop] Group invasion: our host's group invades another world (lobby %016llx, %s's): %s",
             static_cast<unsigned long long>(m.lobby), m.host ? steam::persona(m.host).c_str() : "?",
             group_names().c_str());
    guest_go_home(kMsgGroupGoMember, "our host's group invades another world");
    // That world's host is accepted on our way home (it accepted us at the ask), for anything of theirs that comes first.
    if (m.host) steam::accept(m.host);
}

// The group's host: our knock did not get us there; the others stop theirs.
void group_send_off() {
    Simple m{};
    fill_header(&m, kGroupOff);
    send_group(m);
    LOG_INFO("[coop] Group invasion: our knock did not get us there; the group is told");
}

// A guest: the host's knock did not get it there (kGroupOff). Not yet let in, we do not knock any more; one let in
// already invades on.
void group_on_off() {
    LOG_INFO("[coop] Group invasion: %s's knock did not get them there", utf8(g_group.leader_name).c_str());
    if (g_group.phase == GroupPhase::Leaving) {
        say(kMsgGroupOff);
        set_group_phase(GroupPhase::Returning, "our host's knock failed");
    } else if (g_group.phase == GroupPhase::Joining && g_role == Role::Guest &&
               (g_step == Step::Searching || g_step == Step::LobbyJoin || g_step == Step::Hello)) {
        say(kMsgGroupOff);
        leave();  // the phase follows once our part of that session is over (group_tick)
    }
}

// Home and standing: we knock on the world the group invades, as an evil spirit, on the place held for us.
void group_knock() {
    LOG_INFO("[coop] Group invasion: knocking on the world we invade (lobby %016llx)",
             static_cast<unsigned long long>(g_group.target));
    g_role = Role::Guest;
    g_invade = true;
    g_refusals = Refusals{};
    g_worlds.clear();
    g_world_tries = 0;
    g_mismatch = Mismatch{};
    if (!g_group.lead) say(kMsgGroupJoining);
    set_group_phase(GroupPhase::Joining, "home and standing");
    join_world(g_group.target);
}

// Every frame while a group invasion of ours is under way.
void group_tick() {
    if (g_group.phase == GroupPhase::None) return;
    const double in_phase = now() - g_group.since;
    // Home and standing. The rise at the bonfire after the trip home (6950) is no reason to wait: a join leaves from
    // wherever we stand in our own world, and the load replaces the character.
    const bool home = g_role == Role::None && game::in_game() && !game::loading() && game::own_world() &&
                      game::player() && !world::local_dead();
    if (!home) {
        g_group.ready_since = 0;
    } else if (g_group.ready_since == 0) {
        g_group.ready_since = now();
    }
    const double standing = home ? now() - g_group.ready_since : 0;
    // A guest of the group: the host's messages reach us outside any lobby we share once we are home (every session's
    // end closes the channels to its players).
    if (!g_group.lead && g_group.leader) {
        static double accepted = 0;
        if (now() - accepted >= 2.0) {
            accepted = now();
            steam::accept(g_group.leader);
        }
    }
    // While out, each side says so every kGroupBeat (see kGroupQuietFor): the host until its world is open again (then
    // its kGroupBack carries the lobby), a guest until it is on its way back into the host's world.
    const bool out = g_group.phase == GroupPhase::Joining || g_group.phase == GroupPhase::Invading ||
                     g_group.phase == GroupPhase::Returning || (!g_group.lead && g_group.phase == GroupPhase::Leaving);
    if (out && now() - g_group.beat_sent >= kGroupBeat) {
        g_group.beat_sent = now();
        if (g_group.lead) {
            GroupBackMsg m{};
            fill_header(&m, kGroupBack);
            m.lobby = 0;
            send_group(m);
        } else if (g_group.leader) {
            Simple m{};
            fill_header(&m, kGroupHere);
            send_to(g_group.leader, m);
        }
    }
    switch (g_group.phase) {
        case GroupPhase::Searching: {
            if (!g_group.lead || !hosting()) {
                set_group_phase(GroupPhase::None, "our world closed");
                break;
            }
            static double checked = 0;
            if (now() - checked >= 1.0) {
                checked = now();
                const std::wstring why = group_refusal(nullptr);
                if (!why.empty()) {
                    group_call_off(why);
                    break;
                }
            }
            if (g_group.listing && now() - g_group.listed_at > kSearchRetryAfter) {
                if (g_group.waves < 2) {
                    LOG_INFO("[coop] Group invasion: no lobby list yet; asking Steam again");
                    group_list();
                } else {
                    group_search_over(kMsgBreakInTimeout, "the lobby list never came");
                }
                break;
            }
            if (g_group.ask_pending && now() - g_group.asked_at > kGroupAskWait) {
                LOG_INFO("[coop] Group invasion: no answer from %s within %.0f s; the next world",
                         g_group.asking_host ? steam::persona(g_group.asking_host).c_str() : "a world", kGroupAskWait);
                ++g_group.refusals.quiet;
                ++g_group.gen;  // a late lobby join is stale
                group_ask_next();
            }
            break;
        }
        case GroupPhase::Leaving:
            if (g_group.lead && g_role == Role::Host && g_step == Step::Closing) break;  // our world closes
            if (!g_group.lead && g_role == Role::Guest && g_step == Step::GoingHome) break;  // on our way home
            if (g_role != Role::None) {
                set_group_phase(GroupPhase::None, "we started something else");
                break;
            }
            if (in_phase > kGroupLeaveFor) {
                if (g_group.lead) group_send_off();
                say(kMsgGroupOff);
                set_group_phase(GroupPhase::Returning, "we were not home and standing in time to knock");
                break;
            }
            if (standing >= (g_group.lead ? kGroupLeadKnockAfter : kGroupKnockAfter)) group_knock();
            break;
        case GroupPhase::Joining:
            if (g_role == Role::Guest && g_invade && g_step == Step::InWorld) {
                set_group_phase(GroupPhase::Invading, "we arrived");
            } else if (g_role == Role::None) {
                if (g_group.lead) group_send_off();
                set_group_phase(GroupPhase::Returning, "our knock or join did not get us there");
            } else if (g_role == Role::Host || !g_invade) {
                set_group_phase(GroupPhase::None, "we started something else");
            }
            break;
        case GroupPhase::Invading:
            if (g_role == Role::None) {
                set_group_phase(GroupPhase::Returning, "the invasion is over");
            } else if (g_role == Role::Host || !g_invade) {
                set_group_phase(GroupPhase::None, "we started something else");
            }
            break;
        case GroupPhase::Returning:
            if (g_role == Role::Guest && g_step == Step::GoingHome) break;  // still on our way home
            if (g_role != Role::None) {
                set_group_phase(GroupPhase::None, "we started something else");
                break;
            }
            if (g_group.lead) {
                if (standing < kGroupLeadReopenAfter) break;
                LOG_INFO("[coop] Group invasion: home again; our world opens for the group");
                g_role = Role::Host;
                set_group_phase(GroupPhase::Rejoining, "home again");
                host_open_lobby();
                break;
            }
            if (home && !g_group.told) {
                g_group.told = true;
                if (!g_group.back) say(fmt(kMsgGroupWaiting, g_group.leader_name));
            }
            if (g_group.back && standing >= kGroupRejoinAfter) {
                g_role = Role::Guest;
                g_invade = false;
                g_refusals = Refusals{};
                g_worlds.clear();
                g_world_tries = 0;
                g_mismatch = Mismatch{};
                say(fmt(kMsgGroupReturning, g_group.leader_name));
                const uint64_t lobby = g_group.back;
                set_group_phase(GroupPhase::Rejoining, "the host's world is open again");
                join_world(lobby);
                break;
            }
            // The host may still be out (it invades on): we wait as long as it keeps saying so, and give up only after
            // kGroupQuietFor without a word from it, counted from our own return at the earliest.
            if (now() - std::max(g_group.since, g_group.heard_at) > kGroupQuietFor) {
                say(fmt(kMsgGroupNotBack, g_group.leader_name));
                set_group_phase(GroupPhase::None, "nothing heard from the host for too long");
            }
            break;
        case GroupPhase::Rejoining:
            if (!g_group.lead) {
                if (g_role == Role::Guest && !g_invade && g_step == Step::InWorld) {
                    set_group_phase(GroupPhase::None, "back in the host's world");
                } else if (g_role == Role::None) {
                    set_group_phase(GroupPhase::None, "the way back did not get us there");
                } else if (g_role == Role::Host || g_invade) {
                    set_group_phase(GroupPhase::None, "we started something else");
                }
                break;
            }
            if (g_role != Role::Host) {
                set_group_phase(GroupPhase::None, "our world closed again");
                break;
            }
            if (!hosting()) break;  // our lobby is being made
            if (!g_group.told) {
                g_group.told = true;
                say(g_group.members.size() > 1 ? kMsgGroupBack : kMsgGroupBackSolo);
            }
            {
                // Each player not back yet gets our lobby every kGroupBackResend (0.75 s) for as long as they keep
                // saying they are out (one
                // still invading after we came home waits on), and is let go after kGroupQuietFor without a word from
                // them, counted from our world's opening at the earliest.
                const std::vector<uint64_t> in_lobby = steam::lobby_members(g_lobby);
                const bool resend = now() - g_group.back_sent >= kGroupBackResend;
                if (resend) g_group.back_sent = now();
                int waiting = 0, lost = 0;
                for (uint64_t id : g_group.members) {
                    if (id == steam::self() || admitted(id)) continue;
                    if (std::find(in_lobby.begin(), in_lobby.end(), id) != in_lobby.end()) continue;
                    const auto heard = g_group.member_heard.find(id);
                    const double last = std::max(g_group.since, heard != g_group.member_heard.end() ? heard->second : 0.0);
                    if (now() - last > kGroupQuietFor) {
                        ++lost;
                        continue;
                    }
                    ++waiting;
                    if (resend) {
                        GroupBackMsg m{};
                        fill_header(&m, kGroupBack);
                        m.lobby = g_lobby;
                        send_to(id, m);
                    }
                }
                if (waiting == 0) {
                    set_group_phase(GroupPhase::None, lost ? "everyone still heard from is back in our world"
                                                           : "the whole group is back in our world");
                }
            }
            break;
        case GroupPhase::None:
            break;
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
    // Steam passes on packets from every lobby member, also ones refused or not answered yet. Group invasions (1.6.0):
    // a group's ask comes from a player not in our session (in our lobby only to ask), its answer to our own ask from
    // the host of another world, and a player of our group still out says so while not in our session (each checks who
    // it is from).
    if (g_role == Role::Host && h.type != kHello && h.type != kBye && h.type != kPing && h.type != kGroupAsk &&
        h.type != kGroupAnswer && h.type != kGroupHere && !admitted(from)) {
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
            p->mod = them.mod;
            p->last_seen = now();
            g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                           [from](const Pending& q) { return q.from == from; }),
                            g_pending.end());
            Pending pending{from, m.invade != 0, now(), compare_versions(them.mod, kRefusalsVersion) >= 0};
            if (size >= sizeof(HelloMore)) {
                HelloMore more{};
                std::memcpy(&more, data, sizeof(more));
                if (more.max_players >= 4) pending.max_players = more.max_players;
            }
            p->max_players = pending.max_players;
            g_pending.push_back(pending);
            break;
        }
        case kWelcome: {
            WelcomeMore m{};
            if (from != g_host) return;
            if (as(&m)) {
                guest_on_welcome(m.w, m.wait_s);
            } else if (as(&m.w)) {
                guest_on_welcome(m.w, 0);  // a host of 1.2.0 or older
            }
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
            StateMore more{};
            if (as(&more) && more.net_rate != g_host_net_rate) {
                g_host_net_rate = more.net_rate;
                LOG_INFO("[coop] The host's network rate: %d updates a second", g_host_net_rate);
            }
            StateMore2 more2{};
            const bool seal = as(&more2) && more2.seal_fogs != 0;
            if (seal != g_host_seal_fogs) {
                g_host_seal_fogs = seal;
                LOG_INFO("[coop] The host's boss fogs %s while an invader is in the world (SealBossFogsDuringInvasions)",
                         g_host_seal_fogs ? "stay shut" : "stay open");
            }
            StateMore3 more3{};
            if (as(&more3)) {
                const int scaling[6] = {more3.scaling[0], more3.scaling[1], more3.scaling[2],
                                        more3.scaling[3], more3.scaling[4], more3.scaling[5]};
                world::set_host_scaling(scaling);
                g_host_scaling = true;
            }
            StateMore6 more6{};
            const bool vicinity = as(&more6) && more6.vicinity != 0;
            if (vicinity != g_host_vicinity) {
                g_host_vicinity = vicinity;
                LOG_INFO("[coop] The host's enemy scaling: %s", vicinity ? "by vicinity (VicinityScaling)"
                                                                          : "for the whole session");
            }
            StateMore5 more5{};
            const int bloody = as(&more5) ? std::min<int>(more5.bloody, kMaxBloody) : 0;
            if (bloody != g_host_bloody) {
                LOG_INFO("[coop] The host's Bloody Marys: %d -> %d", g_host_bloody, bloody);
                g_host_bloody = bloody;
            }
            StateMore4 more4{};
            const bool takes = as(&more4) && more4.guest_homeward != 0;
            if (takes != g_host_homeward) {
                g_host_homeward = takes;
                LOG_INFO("[coop] The host's game %s our Homeward Bones, Darksign and Homeward miracle",
                         takes ? "takes" : "does not take");
            }
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
            p->pos_at = now();
            p->ping_ms = m.ping_ms;
            p->invader = m.invader != 0;
            const bool dead = m.dead != 0;
            if (!dead && (p->dead || p->alive_since == 0)) p->alive_since = now();
            if (dead && !p->dead) {  // a death we had not heard of
                p->respawn = kRespawnNone;
                p->went_kept = false;
            }
            p->dead = dead;
            p->light = m.light;
            p->char_name = m.char_name;
            p->ceremony = motion::synced_anim(m.ceremony) ? m.ceremony : 0;
            InfoMore more{};
            const bool more_ok = as(&more);
            InfoMore2 more2{};
            if (as(&more2)) {
                if (more2.event_anim != 0) {
                    p->event_free_since = 0;
                } else if (p->event_anim != 0 || p->event_free_since == 0) {
                    p->event_free_since = now();
                }
                p->event_anim = more2.event_anim;
            }
            InfoMore3 more3{};
            p->bones = as(&more3) ? more3.bones : -1;
            InfoMore4 more4{};
            p->disguised = as(&more4) && more4.disguised != 0;
            if (as(&more4) && more4.max_players >= 4) p->max_players = more4.max_players;
            InfoMore5 more5{};
            if (as(&more5)) {
                p->floor_map = more5.floor_map;
                p->floor_index = more5.floor_index;
                p->floor_at = now();
            }
            p->boss_fight = more_ok && more.boss_fight != 0;
            p->team_respawn = more_ok && more.team_respawn != 0;
            if (more_ok && (more.respawn & kRespawnPhase) == kRespawnWent && g_role == Role::Guest && from == g_host) {
                g_host_went_at = now();
            }
            if (!dead) {
                p->respawn = kRespawnNone;
                p->went_kept = false;
            }
            if (more_ok) note_peer_respawn(p, more.respawn);
            // The reload phase (more.reset) is not taken from here: this report is unreliable, so one from before a
            // rest can come after it (kResetPhase).
            // A fallen player's gauge can outlast our own reload: only a standing teammate's fight is news here.
            if (more_ok && more.boss && !p->invader && !dead && !game::loading()) rules::note_teammate_boss(more.boss);
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
                peer(from)->arrived_at = now();
                if (!m.invader) {
                    send_world_globals(from);
                    send_world_flags(from);
                    send_live_drops(from);
                    send_bonfires(from);
                    // A guest of a version before free for all came into a world with it: back to no rules for all.
                    if (rules::state().world_rules == 3 && !guests_know_free_for_all()) {
                        rules::state().world_rules = 0;
                        say(kMsgRulesNeedsNew);
                        say(rules_text(0));
                        LOG_INFO("[coop] World rules back to 0: a guest who does not know free for all arrived");
                        g_last_state = 0;
                    }
                    g_world_sent.clear();  // what our world has, to the newcomer (and again to the others)
                    g_world_polled = 0;
                } else if (std::find(g_invaders_in.begin(), g_invaders_in.end(), from) == g_invaders_in.end()) {
                    g_invaders_in.push_back(from);  // its leave starts InvasionCooldown
                }
                g_npc_sent.clear();  // our NPC phantoms, to the newcomer at once
            } else if (from != g_host) {
                return;
            }
            if (m.who == steam::self()) return;
            unmark_lost(m.who);
            say(fmt(m.invader ? kMsgInvaded : kMsgJoined, event_name(m)));
            break;
        }
        case kWorldFlags: {
            // With the welcome (before the load) and on arrival.
            if (from != g_host || g_role != Role::Guest || g_invade) return;
            if (g_step != Step::InWorld && g_step != Step::Joining && g_step != Step::Loading) return;
            on_world_flags(data + sizeof(Header), size - static_cast<uint32_t>(sizeof(Header)));
            break;
        }
        case kWorldGlobals: {
            if (from != g_host || g_role != Role::Guest || g_invade) return;
            on_world_globals(data + sizeof(Header), size - static_cast<uint32_t>(sizeof(Header)));
            break;
        }
        case kDeadList: {
            if (g_invade || size <= sizeof(Header)) return;
            const bool from_host = g_role == Role::Guest && from == g_host;
            if (from_host || (g_role == Role::Host && !peer_is_invader(from))) {
                deadlist::note_list(from, from_host, data + sizeof(Header), size - sizeof(Header));
            }
            break;
        }
        case kHurtList: {
            if (g_invade || size <= sizeof(Header)) return;
            const bool from_host = g_role == Role::Guest && from == g_host;
            if (from_host || (g_role == Role::Host && !peer_is_invader(from))) {
                deadlist::note_hurt(from, from_host, data + sizeof(Header), size - sizeof(Header));
            }
            break;
        }
        case kNpcPhantoms: {
            if (g_role != Role::Guest || from != g_host || size < sizeof(Header) + 1) return;
            on_npc_phantoms(data, size);
            break;
        }
        case kNpcSummons: {
            if (g_role != Role::Host || peer_is_invader(from) || size < sizeof(Header) + 1) return;
            g_guest_npcs[from] = npc_list(data, size);
            summons::set_guest_summons(guests_npcs());
            break;
        }
        case kRest: {
            Simple m{};
            if (g_role == Role::None || !as(&m)) return;
            if (g_role == Role::Host) {
                // a guest rested: reset here and tell the other guests (with the rest's id)
                bloody_reset("a guest rested");
                Simple r{};
                fill_header(&r, kRest);
                r.value = m.value;
                broadcast(r, true, from);
            } else if (from != g_host) {
                return;
            }
            g_rest_id = m.value;
            world::remote_rest();
            break;
        }
        case kResetPhase: {
            ResetPhase m{};
            if (!as(&m) || g_invade) return;
            PeerInfo* p = peer(from);
            p->reset = m.phase <= kResetReady ? m.phase : kResetNone;
            p->reset_rest = m.rest;
            break;
        }
        case kFlags: {
            if (size < sizeof(Header) + 2) return;
            FlagsMsg m{};
            std::memcpy(&m, data, std::min<size_t>(size, sizeof(m)));
            const int n = std::min<int>(m.count, kFlagsPerMsg);
            const bool from_guest = g_role == Role::Host;
            if (!from_guest && from != g_host) return;
            // An older guest's first seconds in our world: what its game set then it judged on its own copy, not yet
            // ours. A guest of 1.4.0 holds that back itself and lets go 2 s after our map flags reached it: its first
            // real flags come about 2 s and a round trip after we sent them, inside these 3 s, so they are not
            // dropped here (Grok d6d05cb9). An unknown version counts as older.
            if (from_guest) {
                const PeerInfo* p = nullptr;
                for (const auto& q : g_peers) p = q.id == from ? &q : p;
                if (p && p->arrived_at > 0 && now() - p->arrived_at < 3.0 &&
                    compare_versions(p->mod, kHoldsCopyVersion) < 0) {
                    static int early = 0;
                    if (++early <= 20) {
                        LOG_INFO("[coop] %d flag change(s) from %s not taken: sent in their first 3 s in our world",
                                 n, steam::persona(from).c_str());
                    }
                    return;
                }
            }
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
            // A fallen invader's team lost after their fall (invasion_team_lost): the outcome only.
            if (m.invader && m.lost == kLostLater) {
                if (!g_invade && g_cfg.invasion_rewards) {
                    LOG_INFO("[coop] The invasion of %s is repelled (they had fallen before): a Humanity for us",
                             utf8(event_name(m)).c_str());
                    ++g_reward;
                    g_reward_at = now();
                    g_reward_text = kMsgRewardTeam;
                }
                break;
            }
            if (g_role == Role::Host && !m.invader) bloody_add(utf8(event_name(m)).c_str());
            if (PeerInfo* p = peer(m.who)) {
                p->dead = true;
                p->respawn = kRespawnNone;  // a new death: its respawn has not reached the warp yet
                p->went_kept = false;
                ++p->death_events;  // their character here gets rebuilt (party::tick)
            }
            say(fmt(m.invader ? kMsgInvaderDied : kMsgDied, event_name(m)));
            // We invade and the last co-op player fell (the host alone is not enough while a guest stands).
            if (invasion_undecided() && coop_team_down()) invasion_won();
            // An invader's death that lost its invasion (its game decides): the co-op team won, fallen players too.
            if (m.invader && m.lost && !g_invade && g_cfg.invasion_rewards) {
                LOG_INFO("[coop] The invasion of %s is repelled: a Humanity for us", utf8(event_name(m)).c_str());
                ++g_reward;
                g_reward_at = now();
                g_reward_text = kMsgRewardTeam;
            }
            break;
        }
        case kTeamRespawn: {
            Simple m;
            if (!as(&m) || g_invade || (g_role == Role::Guest && from != g_host)) return;
            // Every death's warp says it went (note_respawn_went); only a wipe waits on it.
            if (g_role == Role::Guest && (m.value & kRespawnPhase) == kRespawnWent) g_host_went_at = now();
            PeerInfo* p = peer(from);
            const uint32_t was = p->respawn;
            note_peer_respawn(p, m.value);
            if (p->respawn != was && (p->respawn == kRespawnWaiting || g_respawn_waiting)) {
                LOG_INFO("[coop] %s: %s's respawn %s",
                         g_reset != kResetNone || g_reset_hold ? "Lost boss fight" : "Team wipe",
                         steam::persona(from).c_str(), p->respawn == kRespawnWaiting ? "waits" : "went");
            }
            break;
        }
        case kBonfireWarp: {
            BonfireMsg m;
            if (from != g_host || !as(&m) || g_step != Step::InWorld || g_invade || m.bonfire < 0) return;
            g_travel_asked = 0;
            g_homeward_asked = 0;   // the host's warp goes instead of any Homeward trip of ours
            g_follow_bonfire = -1;  // the host's latest trip is the one followed
            g_follow_travel = m.bonfire;
            g_follow_told = 0;
            guest_travel_tick();
            break;
        }
        case kTravel: {
            TravelMsg m{};
            if (g_role != Role::Host || g_step != Step::Hosting || !as(&m) || peer_is_invader(from)) return;
            host_travel_request(from, m);
            break;
        }
        case kHomewardAsk: {
            Simple m{};
            if (g_role != Role::Host || g_step != Step::Hosting || !as(&m) || peer_is_invader(from) || !admitted(from) ||
                m.value > kHomeMiracle) {
                return;
            }
            start_homeward(static_cast<HomewardKind>(m.value), from);
            break;
        }
        case kTravelAnswer: {
            Simple m{};
            if (g_role != Role::Guest || from != g_host || g_invade || !as(&m)) return;
            g_travel_asked = 0;
            // Our Homeward ask stays open while its trip waits for the host's menu; any other answer closes it.
            if (m.value == kTravelWaiting) {
                g_homeward_waits = g_homeward_asked > 0;
            } else {
                g_homeward_asked = 0;
            }
            const std::wstring host_name = shown_name(*peer(g_host));
            LOG_INFO("[coop] The host answered our travel or Homeward request: %u", m.value);
            switch (m.value) {
                case kTravelNoBone: {
                    // The host names who has none (TravelAnswerMsg). When it names us, its count of our bones is out of
                    // date: the rule is said without a name.
                    TravelAnswerMsg full{};
                    std::wstring who;
                    if (as(&full) && full.who != steam::self()) {
                        for (const auto& p : g_peers) {
                            if (p.id == full.who) who = shown_name(p);
                        }
                    }
                    say(who.empty() ? std::wstring(kMsgNoBoneAnyone) : fmt(kMsgNoBone, who));
                    break;
                }
                case kTravelWaiting: say(fmt(kMsgTravelWaiting, host_name)); break;
                case kTravelBossFight: say(travel_hold_text(kHoldBossFight)); break;
                case kTravelNoVessel: say(kMsgTravelNoVessel); break;
                case kTravelNotThere: say(kMsgTravelNotThere); break;
                case kTravelTimedOut: say(kMsgTravelTimedOut); break;
                case kTravelInvader: say(travel_hold_text(kHoldInvader)); break;
                case kTravelMateDown: say(travel_hold_text(kHoldMateDown)); break;
                case kTravelReload: say(travel_hold_text(kHoldReload)); break;
                default: say(kMsgTravelBusy); break;
            }
            break;
        }
        case kHostWorld: {
            if (g_role != Role::Guest || from != g_host || g_invade || size < offsetof(HostWorldMsg, key)) return;
            HostWorldMsg m{};
            std::memcpy(&m, data, std::min<size_t>(size, sizeof(m)));
            const size_t fit = (size - offsetof(HostWorldMsg, key)) / sizeof(int32_t);
            const size_t n = std::min<size_t>({m.keys, fit, static_cast<size_t>(kHostKeysMax)});
            std::vector<int32_t> keys(m.key, m.key + n);
            if (!g_host_world_known || keys != g_host_keys) {
                LOG_INFO("[coop] The host's world: key items %s, Lordvessel %s, warp bonfires %05X", key_text(keys).c_str(),
                         m.lordvessel ? "yes" : "no", m.warps);
            }
            g_host_world_known = true;
            g_host_keys = keys;
            rules::set_host_world(true, m.lordvessel != 0, m.warps);
            g_keys_polled = 0;  // carry the keys at the next look
            break;
        }
        case kHomeward: {
            HomewardMsg m{};  // a 1.2.0 host sends the BonfireMsg part only: its bone, at no cost to us
            if (size < sizeof(BonfireMsg)) return;
            std::memcpy(&m, data, std::min<size_t>(size, sizeof(m)));
            if (from != g_host || g_step != Step::InWorld || g_invade || m.bonfire < 0) return;
            const bool miracle = size >= sizeof(HomewardMsg) && !m.darksign && !m.bone;
            if (m.darksign) {
                LOG_INFO("[coop] The host's Darksign: %d souls and %d humanity lost", game::souls(), game::humanity());
                game::set_souls(0);
                game::set_humanity(0);
            } else if (m.bone && !game::in_game()) {
                g_bone_owed = true;
                LOG_INFO("[coop] The host's Homeward Bone came while we load: ours is taken once we stand");
            } else if (m.bone) {
                const int taken = items::take(kHomewardBone, 1);
                LOG_INFO("[coop] The host's Homeward Bone: %d of ours taken (%d left)", taken,
                         items::count(kHomewardBone));
            }
            // A fallen guest follows once back on their feet. A death held for a boss fight would wait for a fight
            // that our own body keeps on screen (the gauge stays while we lie in the arena): it goes ahead now.
            if (world::local_dead() || spectate::holding() || world::in_limbo()) {
                spectate::trip_release();
                world::trip_release();
            }
            say(m.darksign ? kMsgDarksignOut : miracle ? kMsgMiracleOut : kMsgHostWarpOut);
            g_travel_asked = 0;    // our own travel request is answered by this trip
            g_homeward_asked = 0;  // and so is our Homeward ask
            g_follow_travel = -1;  // the host's latest trip is the one followed
            g_follow_told = 0;
            g_follow_bonfire = m.bonfire;
            g_follow_at = now() + 3.0;
            break;
        }
        case kBonfires: {
            if (from != g_host || g_role != Role::Guest || g_invade) return;
            world::set_host_bonfires(data + sizeof(Header), size - static_cast<uint32_t>(sizeof(Header)));
            break;
        }
        case kBossSouls: {
            BossSoulsMsg m;
            if (!as(&m) || g_invade) return;
            if (g_role == Role::Host) {
                if (peer_is_invader(from)) return;
                broadcast_coop(m, from);
            } else if (from != g_host) {
                return;
            }
            LOG_INFO("[coop] %s's game paid boss %d's %u souls", steam::persona(from).c_str(), m.flag, m.souls);
            world::remote_boss_souls(static_cast<int>(m.souls), m.flag);
            break;
        }
        case kBonfireKindle: {
            BonfireMsg m;
            if (!as(&m) || g_role != Role::Host || peer_is_invader(from)) return;
            LOG_INFO("[coop] %s kindled bonfire %d of our world", peer(from)->name.c_str(), m.bonfire);
            world::kindle_for_guest(m.bonfire);
            break;
        }
        case kBonfireLit: {
            BonfireMsg m;
            if (!as(&m)) return;
            if (g_role == Role::Host) {
                if (peer_is_invader(from)) return;
                broadcast_coop(m, from);
            } else if (from != g_host || g_invade) {
                return;
            }
            rules::remote_bonfire_lit(m.bonfire);
            break;
        }
        case kAnimDone: {
            AnimDoneMsg m;
            if (!as(&m) || peer(from)->invader) return;
            if (g_role == Role::Host) {
                broadcast(m, true, from);
            } else if (from != g_host) {
                return;
            }
            if (!g_invade) rules::remote_anim_done(m.entity, m.anim);
            break;
        }
        case kItemDrop: {
            ItemDropMsg m;
            if (!as(&m) || peer(from)->invader || g_invade) return;
            if (g_role == Role::Host) {
                if (m.origin != from || !admitted(from)) return;
                broadcast(m, true, from);
            } else if (from != g_host || g_step != Step::InWorld) {
                return;
            }
            itemshare::Drop d;
            d.origin = m.origin;
            d.seq = m.seq;
            d.map = m.map;
            std::memcpy(d.entry, m.entry, sizeof(d.entry));
            itemshare::remote_drop(d);
            break;
        }
        case kItemClaim: {
            ItemKeyMsg m;
            if (!as(&m) || g_role != Role::Host || peer(from)->invader || !admitted(from)) return;
            itemshare::remote_claim(from, m.origin, m.seq);
            break;
        }
        case kItemTaken: {
            ItemTakenMsg m;
            if (!as(&m) || g_role != Role::Guest || from != g_host || g_invade) return;
            itemshare::remote_taken(m.origin, m.seq, m.taker);
            break;
        }
        case kEventLead: {
            EventLeadMsg m;
            if (!as(&m) || peer(from)->invader) return;
            if (g_role == Role::Host) {
                m.leader = from;
                broadcast(m, true, from);
            } else if (from != g_host) {
                return;
            }
            if (!g_invade) rules::remote_event_lead(m.handle, m.entity, m.anim, m.start != 0, m.leader);
            break;
        }
        case kNpcTalk: {
            NpcTalkMsg m;
            if (!as(&m)) return;
            if (g_role == Role::Host) {
                broadcast(m, true, from);
            } else if (from != g_host || g_step != Step::InWorld) {
                return;
            }
            if (m.kind > talk::kStart) return;
            talk::remote(m.talk, m.line, m.row, m.a2, m.a3, static_cast<talk::Kind>(m.kind));
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
            g_cut_end_told = false;
            break;
        }
        case kBye: {
            if (from == g_host && g_role == Role::Guest) {
                // Already on the way home (1.6.0: the host's group invades, and its kGroupGo came just before): the
                // trip goes on as it is, with its own line, not a second one.
                if (g_step != Step::GoingHome) guest_go_home(kMsgHostClosed, "the host closed the session");
            } else if (g_role == Role::Host && find_peer(from)) {
                // Only from a player who knocked or came in: in another world's lobby to ask for a group's places
                // (1.6.0), our game hears that world's players leave it too.
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
        case kSign: {
            SignFloorMsg x{};
            if (!as(&x.m) || g_role == Role::None) return;
            const SignMsg& m = x.m;
            if (!as(&x) || x.tag != kSignFloorTag) {
                x.floor_map = -1;
                x.floor_index = -1;
                x.tag = kSignFloorTag;  // passed on with the floor unknown
            }
            if (g_role == Role::Host) {
                if (peer_is_invader(from) || m.owner != from) return;
                broadcast_coop(x, from);
                send_to_invaders(x, from);  // an invader's Death Knell may go to it too (1.5.0)
            } else if (from != g_host) {
                return;
            }
            on_sign(from, m, x.floor_map, x.floor_index);
            break;
        }
        case kNear: {
            if (from == steam::self() || peer_is_invader(from)) return;
            on_near(from, data, size);
            break;
        }
        case kKnellPrice: {
            KnellPriceMsg m;
            if (!as(&m) || g_role != Role::Host || peer_is_invader(from)) return;
            const PeerInfo* p = peer(from);
            const std::string who = p && !p->char_name.empty() ? utf8(p->char_name) : steam::persona(from);
            bloody_add(who.c_str(), m.sign ? "rang the Death Knell to a summon sign" : "rang the Death Knell", true);
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
            if (m.kind == motion::kHold || m.kind == motion::kRelease) {
                const uint32_t chr = chr_of_peer(from);
                const bool on = m.kind == motion::kHold;
                const bool done = chr && motion::hold_anims(chr, on);
                const auto held = g_knell_held.find(from);
                if (on && done) {
                    g_knell_held[from] = KnellHeld{chr, now()};
                } else if (!on && !chr && held != g_knell_held.end()) {
                    held->second.release = true;  // let go once their character is here again (knell_held_tick)
                } else if (chr && held != g_knell_held.end()) {
                    g_knell_held.erase(held);  // let go on their character, or a hold that failed on it
                }
                static int lines = 0;
                if (++lines <= 120) {
                    LOG_INFO("[coop] %s: knell crouch %s%s", p->name.c_str(), on ? "held" : "let go",
                             done ? " on their character" : chr ? " (FAILED on their character)" : " (their character is not here)");
                }
                break;
            }
            if (m.kind == 0) {
                if (!motion::synced_anim(m.anim) && !motion::knell_arrival_anim(m.anim)) return;
                p->ceremony = m.loop && motion::synced_anim(m.anim) ? m.anim : 0;
            } else {
                p->ceremony = 0;
            }
            const uint32_t chr = chr_of_peer(from);
            const bool knell = m.kind == 0 && !motion::synced_anim(m.anim) && motion::knell_arrival_anim(m.anim);
            static int lines = 0;
            if (++lines <= 200) {
                LOG_INFO("[coop] %s: %s animation %s %d%s%s", p->name.c_str(), knell ? "knell" : "bonfire",
                         m.kind == 0 ? "start" : "stop", m.kind == 0 ? m.anim : 0, m.kind == 0 && m.loop ? " (loop)" : "",
                         chr ? "" : knell ? " (waiting for their character here)" : " (their character is not here)");
            }
            if (knell) {
                knell_anim_came(from, m.anim, m.loop);  // once their character here can take it
                break;
            }
            g_knell_waits.erase(from);  // a bonfire animation of theirs comes after it
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
        case kSentHome: {
            if (from != g_host || g_role != Role::Guest || !g_invade) return;
            if (world::local_dead()) {
                // Fallen and spectating another invader: the death goes ahead and takes us home, and the invasion has
                // no outcome (no "failed", no Humanity for the co-op team).
                g_invade_lost = true;
                spectate::trip_release();
                say(kMsgDarksignInvader);
                LOG_INFO("[coop] The host's Darksign while we were down: our death goes ahead and takes us home");
                break;
            }
            guest_go_home(kMsgDarksignInvader, "the host's Darksign sent us home");
            break;
        }
        case kDropped: {
            if (from != g_host || g_role != Role::Guest || (g_step != Step::InWorld && g_step != Step::Loading)) return;
            guest_go_home(kMsgHostLost, "the host dropped us after our connection went quiet");
            break;
        }
        case kPong: {
            Simple m;
            if (!as(&m) || m.value == 0) return;
            PingOut& out = g_pings[m.value % kPingSlots];
            if (out.seq != m.value || out.to != from) return;  // not ours, or answered already
            // Timed with the performance counter: GetTickCount jumps by about 15 ms, so a fast reply read as 0.
            LARGE_INTEGER now_q{}, freq{};
            QueryPerformanceCounter(&now_q);
            QueryPerformanceFrequency(&freq);
            const int64_t delta = now_q.QuadPart - out.qpc;
            out = PingOut{};
            if (delta <= 0 || freq.QuadPart <= 0) return;
            const int64_t ms64 = (delta * 1000) / freq.QuadPart;
            if (ms64 >= 10000) return;
            const int ms = ms64 < 1 ? 1 : static_cast<int>(ms64);
            if (g_role == Role::Guest && from == g_host) g_ping_ms = ms;
            for (auto& p : g_peers) {
                if (p.id != from) continue;
                p.rtt_ms = ms;
                p.rtt_at = now();
                if (p.rtt_min < 0 || ms < p.rtt_min) p.rtt_min = ms;
                if (ms > p.rtt_max) p.rtt_max = ms;
            }
            break;
        }
        // Group invasions (1.6.0).
        case kGroupAsk: {
            GroupAskMsg m{};
            if (!as(&m)) return;
            group_on_ask(from, m);
            break;
        }
        case kGroupAnswer: {
            GroupAnswerMsg m{};
            if (!as(&m)) return;
            group_on_answer(from, m);
            break;
        }
        case kKnellSpy: {
            KnellSpyMsg m{};
            if (!as(&m) || g_invade) return;
            // The invaded host is on Step::Hosting (Step::InWorld is a guest's step): it shows the notice when it is
            // the one watched, or passes it to the watched guest.
            if (g_role == Role::Host && g_step == Step::Hosting) {
                const PeerInfo* by = peer(from);
                if (!by || !by->invader || !admitted(from)) return;
                if (m.target == steam::self()) {
                    knell_spied(from);
                } else if (const PeerInfo* t = peer(m.target); t && !t->invader && admitted(m.target)) {
                    m.watcher = from;
                    send_to(m.target, m);
                }
            } else if (g_role == Role::Guest && g_step == Step::InWorld && from == g_host && m.target == steam::self()) {
                knell_spied(m.watcher);
            }
            break;
        }
        case kGroupComing: {
            Simple m{};
            if (!as(&m) || g_role != Role::Guest || g_invade || from != g_host || g_step != Step::InWorld) return;
            const int n = static_cast<int>(std::min<uint32_t>(m.value, 255));
            LOG_INFO("[coop] Our host warns: a group of %d invaders is coming in", n);
            if (n >= 2) say(fmt(kMsgGroupComing, std::to_wstring(n)));
            break;
        }
        case kGroupSearch: {
            GroupSearchMsg m{};
            if (!as(&m) || g_role != Role::Guest || g_invade || from != g_host || g_step != Step::InWorld) return;
            const PeerInfo* p = peer(from);
            say(fmt(m.searching ? kMsgGroupSearchMate : kMsgGroupNoneMate, shown_name(*p)));
            LOG_INFO("[coop] Our host's search for a world our group of %u can invade %s", m.size,
                     m.searching ? "began" : "found none");
            break;
        }
        case kGroupGo: {
            GroupGoMsg m{};
            if (!as(&m)) return;
            group_on_go(from, m);
            break;
        }
        case kGroupOff: {
            if (g_group.phase == GroupPhase::None || g_group.lead || from != g_group.leader) return;
            g_group.heard_at = now();
            group_on_off();
            break;
        }
        case kGroupBack: {
            GroupBackMsg m{};
            if (!as(&m) || g_group.phase == GroupPhase::None || g_group.lead || from != g_group.leader) return;
            g_group.heard_at = now();  // lobby 0: the host is still out, and we wait on
            if (!m.lobby) return;
            if (g_group.back != m.lobby) {
                LOG_INFO("[coop] Group invasion: %s's world is open again (lobby %016llx)",
                         steam::persona(from).c_str(), static_cast<unsigned long long>(m.lobby));
            }
            g_group.back = m.lobby;
            break;
        }
        case kGroupHere: {
            if (!g_group.lead || g_group.phase == GroupPhase::None || !in_group(from)) return;
            g_group.member_heard[from] = now();
            break;
        }
        default:
            break;
    }
}

void send_rest() {
    if (g_role == Role::Host) bloody_reset("we rested");
    if (g_role == Role::None || !g_lobby) return;
    static uint32_t n = 0;
    g_rest_id = static_cast<uint32_t>(GetTickCount64()) * 2654435761u ^ static_cast<uint32_t>(steam::self()) ^ ++n;
    if (!g_rest_id) g_rest_id = 1;
    Simple r{};
    fill_header(&r, kRest);
    r.value = g_rest_id;
    if (g_role == Role::Host) {
        broadcast(r);
    } else if (g_host && g_step == Step::InWorld && !g_invade) {
        send_to(g_host, r);
    }
}

void send_death() {
    g_respawn = kRespawnNone;
    if (g_role == Role::Host && !g_invade) bloody_add("we");
    if (g_role == Role::None || !g_lobby) return;
    const bool undecided = invasion_undecided();
    const bool team_waits = undecided && team_invasion_host() && invader_teammate_standing();
    const bool lost_invasion = undecided && !team_waits;
    if (lost_invasion) {
        g_invade_lost = true;
        LOG_INFO("[coop] We fell while invading: invasion failed");
        say(kMsgBreakInFailed);
    } else if (team_waits) {
        g_team_fell = true;
        LOG_INFO("[coop] We fell while invading; another invader stands: the invasion is not decided yet");
    }
    // The death debuff (Remastered Seamless Co-op): a Crimson Blossom per death in a session, up to five. It is given
    // when the respawn warp goes and the reload keeps the world (world::keep_dead_on_reload).
    PlayerEvent e = player_event(kDead, steam::self(), g_invade, utf8(own_name()));
    e.lost = lost_invasion ? 1 : 0;
    if (g_role == Role::Host) {
        broadcast(e);
    } else if (g_host) {
        send_to(g_host, e);
    }
}

void send_bonfire_warp(int bonfire) {
    if (g_role == Role::Guest) {
        if (g_invade || g_step != Step::InWorld || !g_host) return;
        TravelMsg t{};
        fill_header(&t, kTravel);
        t.bonfire = bonfire;
        t.spot = game::flag(717) && !game::flag(710) ? 1 : 0;
        send_to(g_host, t);
        g_travel_asked = now();
        return;
    }
    if (g_role != Role::Host) return;
    if (g_travel_bonfire >= 0) {
        LOG_INFO("[coop] Our own warp goes instead of %s's travel to bonfire %d", steam::persona(g_travel_from).c_str(),
                 g_travel_bonfire);
        g_travel_bonfire = -1;  // they are taken along to ours
    }
    if (g_homeward_at > 0 && g_homeward_by) {
        LOG_INFO("[coop] Our own warp goes instead of %s's %s", steam::persona(g_homeward_by).c_str(),
                 homeward_name(g_homeward_kind));
        g_homeward_at = 0;  // everyone is taken along to ours, at no cost
        g_homeward_by = 0;
        g_homeward_bumped = 0;
        g_homeward_kind = kHomeBone;
    }
    g_travel_started = now();
    BonfireMsg m{};
    fill_header(&m, kBonfireWarp);
    m.bonfire = bonfire;
    broadcast(m);
}

// The host: our bonfires' kindling, to one co-op guest (to = 0: to everyone; invaders' games leave it).
void send_bonfires(uint64_t to) {
    const std::vector<uint8_t> pts = world::bonfire_points();
    std::vector<uint8_t> msg(sizeof(Header) + pts.size());
    Header h{kMagic, kBonfires};
    std::memcpy(msg.data(), &h, sizeof(h));
    if (!pts.empty()) std::memcpy(msg.data() + sizeof(h), pts.data(), pts.size());
    if (to) {
        steam::send(to, msg.data(), static_cast<uint32_t>(msg.size()), true);
    } else {
        broadcast_raw(msg.data(), static_cast<uint32_t>(msg.size()), true);
    }
}

// A co-op guest: our Kindle in the host's world, for the host's game to add.
void send_bonfire_kindle(int bonfire) {
    if (g_role != Role::Guest || !g_host || g_invade || g_step != Step::InWorld) return;
    BonfireMsg m{};
    fill_header(&m, kBonfireKindle);
    m.bonfire = bonfire;
    send_to(g_host, m);
}

void send_bonfire_lit(int bonfire) {
    BonfireMsg m{};
    fill_header(&m, kBonfireLit);
    m.bonfire = bonfire;
    if (g_role == Role::Host) {
        broadcast_coop(m);
    } else if (g_host && g_step == Step::InWorld && !g_invade) {
        send_to(g_host, m);
    }
}

void send_anim_done(int entity, int anim) {
    if (g_role == Role::None || !g_lobby) return;
    AnimDoneMsg m{};
    fill_header(&m, kAnimDone);
    m.entity = entity;
    m.anim = anim;
    if (g_role == Role::Host) {
        broadcast(m);
    } else if (g_host && g_step == Step::InWorld && !g_invade) {
        send_to(g_host, m);
    }
}

void send_event_lead(uint32_t handle, int entity, int anim, bool start) {
    if (g_role == Role::None || !g_lobby) return;
    EventLeadMsg m{};
    fill_header(&m, kEventLead);
    m.handle = handle;
    m.entity = entity;
    m.anim = anim;
    m.start = start ? 1 : 0;
    m.leader = steam::self();
    if (g_role == Role::Host) {
        broadcast(m);
    } else if (g_host && g_step == Step::InWorld && !g_invade) {
        send_to(g_host, m);
    }
}

// ---------------------------------------------------------------- shared drops (itemshare.cpp)
// A drop of ours, or one the host passes on (relayed in on_message): through the host, as the other world news.
void send_item_drop(const itemshare::Drop& d) {
    if (g_role == Role::None || !g_lobby || g_invade) return;
    ItemDropMsg m{};
    fill_header(&m, kItemDrop);
    m.origin = d.origin;
    m.seq = d.seq;
    m.map = d.map;
    std::memcpy(m.entry, d.entry, sizeof(m.entry));
    if (g_role == Role::Host) {
        broadcast(m);
    } else if (g_host && g_step == Step::InWorld) {
        send_to(g_host, m);
    }
}

void send_item_claim(uint64_t origin, uint32_t seq) {
    if (g_role != Role::Guest || !g_host || g_step != Step::InWorld || g_invade) return;
    ItemKeyMsg m{};
    fill_header(&m, kItemClaim);
    m.origin = origin;
    m.seq = seq;
    send_to(g_host, m);
}

void send_item_taken(uint64_t origin, uint32_t seq, uint64_t taker) {
    if (g_role != Role::Host || !g_lobby) return;
    ItemTakenMsg m{};
    fill_header(&m, kItemTaken);
    m.origin = origin;
    m.seq = seq;
    m.taker = taker;
    broadcast(m);
}

// The host: what still lies on the floor, to a co-op guest who arrived.
void send_live_drops(uint64_t to) {
    for (const itemshare::Drop& d : itemshare::live()) {
        ItemDropMsg m{};
        fill_header(&m, kItemDrop);
        m.origin = d.origin;
        m.seq = d.seq;
        m.map = d.map;
        std::memcpy(m.entry, d.entry, sizeof(m.entry));
        send_to(to, m);
    }
}

// Whether we share drops now: a host in its own session, or a co-op guest in the world of a host whose lobby says it
// shares them (ptdesc_items; a host before 1.4.0, or with ShareDroppedItems off, would never answer a request).
void itemshare_tick() {
    static double asked = 0;
    static bool host_shares = false;
    if (g_role == Role::Guest && g_lobby && now() - asked >= 2.0) {
        asked = now();
        host_shares = steam::lobby_data(g_lobby, "ptdesc_items") == "1";
    }
    const bool host = g_role == Role::Host && g_lobby && g_cfg.share_items;
    const bool guest = g_role == Role::Guest && g_lobby && g_step == Step::InWorld && !g_invade && host_shares;
    itemshare::set_session(host || guest, host, steam::self());
    itemshare::tick();
}

// An NPC's line in our game, for the players near that NPC in theirs (talk.cpp). Invaders hear them too.
void send_npc_talk(int talk, int line, int row, int a2, int a3, talk::Kind kind) {
    if (g_role == Role::None || !g_lobby) return;
    NpcTalkMsg m{};
    fill_header(&m, kNpcTalk);
    m.talk = talk;
    m.line = line;
    m.row = row;
    m.a2 = a2;
    m.a3 = a3;
    m.kind = static_cast<uint8_t>(kind);
    if (g_role == Role::Host) {
        broadcast(m);
    } else if (g_host && g_step == Step::InWorld) {
        send_to(g_host, m);
    }
}

// Our game paid its player a boss's souls (rules.cpp hk_clear_bonus): the amount, for a player whose game did not
// run the boss's defeat (world.cpp). Invaders take no boss souls and tell nobody.
void send_boss_souls(int souls, int flag) {
    if (g_role == Role::None || !g_lobby || g_invade || souls <= 0) return;
    BossSoulsMsg m{};
    fill_header(&m, kBossSouls);
    m.souls = static_cast<uint32_t>(souls);
    m.flag = flag;
    if (g_role == Role::Host) {
        broadcast_coop(m);
    } else if (g_host && g_step == Step::InWorld) {
        send_to(g_host, m);
    }
}

void queue_flag(int id, bool on) {
    if (g_role == Role::None || !g_lobby) return;
    if (g_role == Role::Guest && (g_step != Step::InWorld || g_invade)) return;
    if (g_role == Role::Guest && g_copy_hold) {
        // Judged on a copy of the host's world that is not the host's yet: not the host world's progress, and not
        // set again once the host's map flags came (on_world_flags' "flags of ours", which this keeps empty).
        if (++g_held_flags <= 20) LOG_INFO("[coop] flag %d %s: not sent (our copy of the host's world is not the host's yet)", id, on ? "on" : "off");
        return;
    }
    if (g_flag_queue.size() < 4096) g_flag_queue.push_back({id, on});
    static int lines = 0;
    if (++lines <= 300) LOG_INFO("[coop] flag %d %s: sent", id, on ? "on" : "off");
}

// Guest: our runs and flags held back while our copy of the host's world is not the host's (g_copy_settles_at).
void copy_hold_tick() {
    const bool joining = g_role == Role::Guest && !g_invade &&
                         (g_step == Step::Joining || g_step == Step::Loading || g_step == Step::InWorld);
    const bool settled = g_step == Step::InWorld && g_copy_settles_at > 0 && now() >= g_copy_settles_at;
    const bool hold = joining && !settled;
    if (!joining) g_copy_settles_at = 0;
    if (hold == g_copy_hold) return;
    g_copy_hold = hold;
    rules::hold_event_runs(hold);
    const uint32_t runs = rules::take_held_runs();
    if (hold) {
        g_held_flags = 0;
        LOG_INFO("[coop] Our game's event runs and flags are held back until our copy of the host's world is the "
                 "host's");
    } else {
        LOG_INFO("[coop] Our copy of the host's world is the host's%s: our game's runs and flags go out again (held "
                 "back: %u run(s), %d flag change(s))", joining ? "" : " no longer needed (we left)", runs,
                 g_held_flags);
    }
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

// The player with this Steam id (a session member's, party::Remote::steam_id), however long ago heard from: who an
// id belongs to does not go stale. Null for 0 or an id none of our players has.
const PeerInfo* peer_with_id(uint64_t id) {
    if (!id) return nullptr;
    for (const auto& p : g_peers) {
        if (p.id == id) return &p;
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

// The one player a session member record's name (a Steam name, cut to 16 characters) can be; null when none or more
// than one player has that name.
const PeerInfo* peer_by_member_record(const std::wstring& name) {
    if (name.empty()) return nullptr;
    const PeerInfo* found = nullptr;
    for (const auto& p : g_peers) {
        if (now() - p.last_seen > 10.0) continue;
        const std::wstring steam_name = text::widen(p.name);
        if (!(name == steam_name || (name.size() >= 16 && steam_name.compare(0, name.size(), name) == 0))) continue;
        if (found) return nullptr;
        found = &p;
    }
    return found;
}

// A player counts as back once they have reported standing for kPeerAliveFor, and as up once they have reported no
// event animation for kPeerEventFreeFor (1.5.0; until then 2 s and 1 s, over a report once a second). Their report
// now goes out as soon as either changes (send_info_if_changed), so these are margins over the network only. The
// rise after a respawn (6950) starts up to 0.6 s after the player stands (2026-11-03 test): kPeerAliveFor stays
// above that, so the rise is in their report before they count as back, and their character is never made during it
// (party.cpp: a character made then could be walked through, 2026-10-19).
constexpr double kPeerAliveFor = 1.0;
constexpr double kPeerEventFreeFor = 0.3;

// party::tick asks with a member record's character, name and Steam id, which the game pairs itself
// (update_remote_players): a Steam id one of our players has is the answer; else the record's name and the rest.
party::PeerState peer_state(uint32_t chr, const std::wstring& name, uint64_t steam_id) {
    party::PeerState s;
    const PeerInfo* p = peer_with_id(steam_id);
    if (!p) p = peer_by_member_record(name);
    if (!p && chr) p = peer_by_chr(chr);
    if (!p) p = peer_by_char_name(name);
    if (!p) p = peer_by_member_name(name);
    if (!p) return s;
    s.known = true;
    s.id = p->id;
    s.invader = p->invader;
    s.alive = !p->dead && p->alive_since > 0 && now() - p->alive_since >= kPeerAliveFor && now() - p->last_seen <= 5.0;
    s.dead = p->dead;
    s.deaths = p->deaths;
    s.death_events = p->death_events;
    s.map = p->map;
    s.up_for = !p->dead && p->alive_since > 0 ? now() - p->alive_since : 0;
    s.event_anim = p->event_anim;
    s.away = p->event_anim == kReportLoading || p->event_anim == kReportCutscene;
    // Up: out of every event animation for a second (sitting at a bonfire aside, which is shown on the character),
    // or, for a player who does not report theirs, standing for 6 s (the rise after a respawn takes about 3 s).
    if (p->event_anim < 0) {
        s.settled = s.alive && s.up_for >= 6.0;
    } else if (p->event_anim == 0) {
        s.settled = s.alive && p->event_free_since > 0 && now() - p->event_free_since >= kPeerEventFreeFor;
    } else {
        s.settled = s.alive && p->ceremony != 0;
    }
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
    // Every other player's character in our world (WorldChrMan's player slots), then who it belongs to. First the
    // game's own pairing: the character the game made for a session member (party::remotes: 0xE3D930, by the member
    // id on the character) and that member's Steam id (the member id's SteamPlayerData, party.cpp), which is the id
    // our messages come from: exact, whatever the names. Where the id cannot be read, the member's record (its slot's
    // player data, which holds the member's Steam name, cut to 16 characters; a name two players share names no one).
    // Then, for a character neither names: a match made earlier, the record's name
    // as a character name, the position each player reports every second, and elimination (one character and one
    // player left). Until 1.4.0 the records were compared with character names only, and a match made earlier went
    // first, so after a load every character was matched by position, and five players who came back at one bonfire
    // were paired up wrongly in three of the five games (2026-10-31, after a reload for a lost boss fight). The host's
    // character for 6700k carried spambrion's name: when 6700k fell, the host's game took the dead character for a
    // living spambrion's, rebuilt it while 6700k still lay dead and spectated, and with no character for 6700k the
    // host handed 6700k's game no more enemies, so it kept the Asylum Demon it moved while three other games took it
    // in turn. Two players who fell later followed a character named for the last one standing that was another
    // fallen player's: "waiting for the fight to be decided" with no one to watch.
    struct Match {
        uint32_t chr;
        const PeerInfo* peer;
        const char* how;
        uint64_t id;  // the Steam id of the member the game made this character for, when one of our players has it
    };
    std::vector<Match> chrs;
    for (uint32_t chr : game::remote_player_chrs()) chrs.push_back({chr, nullptr, "none", 0});
    const std::vector<party::Remote> members = party::remotes();
    // The ids the game gave its members that are our players' too. Those players are matched by them and nothing
    // else: every guess below is only for a character with no such id, and only of a player whose id names no member.
    // An id none of our players has (a game that holds another kind of id there) is no help, and the names go on.
    std::set<uint64_t> claimed;
    for (const auto& r : members) {
        if (peer_with_id(r.steam_id)) claimed.insert(r.steam_id);
    }
    auto taken = [&](const PeerInfo* p) {
        for (const auto& m : chrs) {
            if (m.peer == p) return true;
        }
        return false;
    };
    auto open = [&](const Match& m, const PeerInfo* p) {
        return p && !m.peer && !m.id && !claimed.count(p->id) && !taken(p);
    };
    for (auto& m : chrs) {
        for (const auto& r : members) {
            if (!r.chr || r.chr != m.chr || !claimed.count(r.steam_id)) continue;
            m.id = r.steam_id;
            m.peer = peer_with_id(r.steam_id);
            m.how = "steam id";
        }
    }
    for (auto& m : chrs) {
        for (const auto& r : members) {
            if (!r.chr || r.chr != m.chr) continue;
            const PeerInfo* p = peer_by_member_record(r.name);
            if (open(m, p)) {
                m.peer = p;
                m.how = "member";
            }
        }
    }
    // A match made earlier stays (the position match is only good to a few metres and flip-flopped).
    for (auto& m : chrs) {
        if (const PeerInfo* p = peer_by_chr(m.chr); open(m, p)) {
            m.peer = p;
            m.how = "kept";
        }
    }
    for (auto& m : chrs) {
        for (const auto& r : members) {
            if (r.chr != m.chr) continue;
            if (const PeerInfo* p = peer_by_char_name(r.name); open(m, p)) {
                m.peer = p;
                m.how = "name";
            }
        }
    }
    for (auto& m : chrs) {
        if (m.peer || m.id) continue;
        game::Vec3 pos{};
        if (!game::chr_position(m.chr, &pos)) continue;
        const PeerInfo* best = nullptr;
        float best_d = 12.0f * 12.0f;  // a second's run, and some
        float next_d = 3.4e38f;        // the second nearest player (any distance)
        for (const auto& p : g_peers) {
            if (now() - p.last_seen > 5.0 || !open(m, &p)) continue;
            const float dx = pos.x - p.pos[0], dy = pos.y - p.pos[1], dz = pos.z - p.pos[2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < best_d) {
                // Only a player takes second place: the 12 m radius is no one (it refused a lone player 9 to 12 m off).
                if (best) next_d = best_d;
                best_d = d;
                best = &p;
            } else if (d < next_d) {
                next_d = d;
            }
        }
        // Two players a few metres apart (a bonfire after a load) cannot be told apart by the second-old positions:
        // no match then, until one walks off or another way finds it.
        if (best && std::sqrt(next_d) - std::sqrt(best_d) < 3.0f) best = nullptr;
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
            if (!m.peer && !m.id) {
                ++open_chrs;
                last_open = &m;
            }
        }
        for (const auto& p : g_peers) {
            if (now() - p.last_seen <= 10.0 && !taken(&p) && !claimed.count(p.id)) {
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
    g_member_names.clear();
    for (const auto& r : members) {
        const auto it = r.chr ? matched.find(r.chr) : matched.end();
        if (it != matched.end() && !r.name.empty()) g_member_names.emplace_back(r.name, it->second);
    }
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
            // Whose Steam id the member's is (not the id itself: logs get shared).
            const PeerInfo* by_id = peer_with_id(r.steam_id);
            names += !r.steam_id ? " (no Steam id)" : by_id ? " (Steam id of " + by_id->name + ")" : " (Steam id of no one here)";
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

// The Demon Ruins lift moves for one game at a time (rules::set_lift_driver): of the players standing in its map, the
// host's, else a co-op guest's (the lowest Steam id), else an invader's. Every game ranks the same players from the
// maps they report (kInfo, every second), so the games agree within a second of anyone coming or going. A player alone
// in that map is the one, also a guest whose host is elsewhere.
void lift_driver_tick() {
    static double last = 0;
    if (now() - last < 0.25) return;
    last = now();
    const uint32_t map = rules::driven_lift_map();
    const uint64_t self = steam::self();
    bool someone = false;
    uint64_t best = 0;
    int best_rank = 0;
    auto consider = [&](uint64_t id, int rank) {
        if (!someone || rank < best_rank || (rank == best_rank && id < best)) {
            someone = true;
            best_rank = rank;
            best = id;
        }
    };
    if (g_role != Role::None && (g_step == Step::Hosting || g_step == Step::InWorld) && !game::loading()) {
        const int here = game::map_id();
        if (here != -1 && (static_cast<uint32_t>(here) & 0xFFFF0000) == map) {
            consider(self, g_role == Role::Host ? 0 : g_invade ? 2 : 1);
        }
        const std::vector<party::Remote> members = party::remotes();
        for (const auto& p : g_peers) {
            if (p.id == self || p.map == -1 || now() - p.last_seen > 5.0) continue;
            if ((static_cast<uint32_t>(p.map) & 0xFFFF0000) != map) continue;
            if (!coop_member(p, members) && !invader_teammate(p, members)) continue;
            consider(p.id, g_role == Role::Guest && p.id == g_host ? 0 : p.invader ? 2 : 1);
        }
    }
    const bool ours = someone && best == self;
    std::string who;
    if (someone && !ours) {
        for (const auto& p : g_peers) {
            if (p.id == best) who = p.name;
        }
    }
    rules::set_lift_driver(someone, ours, someone && !ours ? chr_of_peer(best) : 0, who);
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
    if (in_cutscene) g_cut_busy_at = now();
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
        if (g_cut_end_told) {
            LOG_INFO("[coop] Already in map %08X (our cutscene's own move, %.1f s after it ended): no need to follow",
                     map, now() - g_cut_busy_at);
        } else {
            LOG_INFO("[coop] Already in map %08X: no need to follow", map);
        }
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
    } else if (g_cut_map == map && now() - g_cut_busy_at < kCutMoveGrace) {
        if (!g_cut_end_told) {
            g_cut_end_told = true;
            LOG_INFO("[coop] Our cutscene into map %08X is over: its own move has %.0f s before we follow", map,
                     kCutMoveGrace);
        }
        return;
    }
    g_cut_follow_map = -1;
    if (g_cut_map == map) {
        LOG_INFO("[coop] Our own cutscene into map %08X did not bring us there (%.1f s after it ended): following "
                 "instead", map, now() - g_cut_busy_at);
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
    world::set_boss_souls_listener(&send_boss_souls);
    rules::set_bonfire_warp_listener(&send_bonfire_warp);
    rules::set_bonfire_lit_listener(&send_bonfire_lit);
    rules::set_bonfire_kindle_listener(&send_bonfire_kindle);
    rules::set_anim_done_listener(&send_anim_done);
    rules::set_event_lead_listener(&send_event_lead);
    talk::set_listener(&send_npc_talk);
    itemshare::set_out(&send_item_drop, &send_item_claim, &send_item_taken);
    rules::set_cutscene_listener(&on_cutscene);
    rules::set_separation_listener([](bool host) { g_separation = host ? 2 : 1; });
    steam::on_lobby_member([](uint64_t lobby, uint64_t user, bool entered) {
        if (lobby != g_lobby) return;
        if (entered) {
            steam::accept(user);  // nothing sent: see group_on_ask
            if (g_role == Role::Host) LOG_INFO("[coop] %s entered the lobby", steam::persona(user).c_str());
            return;
        }
        if (g_role == Role::Guest && user == g_host) {
            if (g_step == Step::Hello && try_next_world("the host left its lobby")) return;
            guest_go_home(kMsgReturnHome, "the host left the lobby");
        } else if (g_role == Role::Host) {
            const bool member = admitted(user);
            // Our group's guests going home before it invades (1.6.0): they leave with the group, not on their own.
            const bool group = g_group.lead && g_group.phase == GroupPhase::Leaving && in_group(user);
            for (const auto& p : g_peers) {
                if (p.id != user || group) continue;
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
            // A dropped player stays marked: woken from sleep, they still talk to us as if in the session. A group's
            // player whose place we hold (its host, done asking) comes back to knock: their session stays accepted.
            if (is_lost(user) || holds_place(user)) {
                steam::accept(user);
            } else {
                steam::close(user);
            }
        }
    });
    // A session request from a member of our lobby (or a group's player whose place we hold) is let in when it comes,
    // also before Steam's word that they entered (on_lobby_member, which accepts them too).
    steam::set_accept_check([](uint64_t peer) {
        if (g_role == Role::Host && holds_place(peer)) return true;
        if (!g_lobby) return false;
        const std::vector<uint64_t> members = steam::lobby_members(g_lobby);
        return std::find(members.begin(), members.end(), peer) != members.end();
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
    g_bloody_on = cfg.bloody_mary;
    g_cfg.max_players = std::max(2, std::min(kMaxSessionPlayers, g_cfg.max_players));
    g_cfg.net_rate = std::max(netrate::kVanilla, std::min(netrate::kMax, g_cfg.net_rate));
    g_cfg.invasion_cooldown_s = std::max(0, std::min(10 * 60, g_cfg.invasion_cooldown_s));
    g_cfg.extra_invaders = std::max(1, std::min(kMaxSessionPlayers, g_cfg.extra_invaders));
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
    reward_tick();
    share_dead_lists();
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
    group_tick();
    text::set_seamless_active(g_role != Role::None);
    if (g_blossoms_home && g_role == Role::None && game::in_game() && !game::loading() && game::own_world() &&
        game::player()) {
        g_blossoms_home = false;
        world::session_over_blossoms("back home after the session");
    }
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
    // Back home from an invasion: the Estus charges set aside on arrival come back on top of what is left.
    if (g_estus_kept > 0 && g_role == Role::None && game::in_game() && !game::loading() && game::own_world()) {
        const int left = items::estus_charges();
        const int kept = g_estus_kept;
        g_estus_kept = 0;
        if (left < 0) {
            LOG_INFO("[coop] Estus Flask: no flask to give the %d set-aside charges back to", kept);
        } else {
            const bool ok = items::set_estus_charges(left + kept);
            LOG_INFO("[coop] Estus Flask: %d set-aside charges back after the invasion (%d left) -> %d%s", kept, left,
                     items::estus_charges(), ok ? "" : " (not taken)");
        }
    }
    g_invader_bars = g_role == Role::Guest && g_invade && g_step == Step::InWorld ? 1 : 0;
    text::tick();
    // Characters of the other players: rebuilt after loads and after a death. An invader's game too: a co-op
    // guest who fell and came back stayed an invisible, untouchable dead copy there with only their health bar
    // showing (2026-10-16 invasions; the game never sees a player come back within one session).
    party::tick(g_step == Step::Hosting || g_step == Step::InWorld, &peer_state);
    // NPC phantoms of the host's party: ours turned into the same phantoms (also after our own loads).
    summons::tick(g_role == Role::Guest && g_step == Step::InWorld);
    if (g_role == Role::Guest && !g_invade && g_step == Step::InWorld) guest_npc_tick();
    if (g_role != Role::Host) {
        g_guest_npcs.clear();
        summons::set_guest_summons({});
    }
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
                s.invader = p.invader;
                spots.push_back(s);
            }
        }
        rules::set_peer_spots(spots);
    }
    lift_driver_tick();
    if (g_role != Role::None && g_anim_log) anim_sync_log();
    // Motion sync: our gait and event animations out; the others' gaits onto their characters here.
    {
        const int gait = motion::local_gait();
        const std::vector<motion::Anim> anims = motion::take_local_anims();
        // Our gait goes to every player in the lobby, invaders and the players they invade included (1.4.0). Until
        // then invasions were left out with the rest of the world rules ("invaders keep the game's own rules", round
        // 5), and their characters walked, ran and sprinted by the receiving game's guess from their speed: a walk
        // showed as a run, a roll as a sprint, and an uneven link switched between them. The gait is only the
        // animation the copy plays; positions, attacks and rolls come as before.
        if (g_role != Role::None && g_lobby && (gait != g_sent_gait || now() - g_last_gait > 0.5)) {
            g_sent_gait = gait;
            g_last_gait = now();
            GaitMsg m{};
            fill_header(&m, kGait);
            m.gait = static_cast<int8_t>(gait);
            broadcast(m, false);
        }
        if (g_role != Role::None && g_lobby) {
            for (const motion::Anim& a : anims) {
                // An invader's game sends the Death Knell's only (its crouch, the rings, the arrival), not the
                // bonfire ceremonies (1.5.0).
                const bool knell_anim = a.kind == motion::kHold || a.kind == motion::kRelease ||
                                        (a.kind == 0 && !motion::synced_anim(a.anim) && motion::knell_arrival_anim(a.anim));
                if (g_invade && !knell_anim) continue;
                AnimMsg m{};
                fill_header(&m, kAnim);
                m.kind = a.kind;
                m.anim = a.anim;
                m.loop = a.loop;
                broadcast(m);
                static int lines = 0;
                if (++lines <= 200) {
                    if (a.kind == motion::kHold || a.kind == motion::kRelease) {
                        LOG_INFO("[coop] Our knell crouch %s: told the others", a.kind == motion::kHold ? "held" : "let go");
                    } else {
                        LOG_INFO("[coop] Our %s animation %s %d%s: told the others",
                                 a.kind == 0 && !motion::synced_anim(a.anim) ? "knell" : "bonfire",
                                 a.kind == 0 ? "start" : "stop", a.anim, a.loop ? " (loop)" : "");
                    }
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
                    if (p.id != id) continue;  // (an invader's too: see the gait above)
                    const bool fresh = p.gait >= 0 && now() - p.gait_at < 1.5;
                    motion::set_remote_gait(chr, fresh ? static_cast<motion::Gait>(p.gait) : motion::kUnknown);
                }
            }
        }
    }
    reconcile_ceremonies();
    signs_tick();
    near_tick();
    remote_characters_upkeep();
    knell_anims_tick();
    player_net_watch();
    stray_copy_watch();
    itemshare_tick();
    // The host: our bonfires' kindling to the guests whenever it changes (a light, a Kindle, a guest's Kindle).
    if (world::bonfires_changed() && g_role == Role::Host && g_lobby) send_bonfires(0);
    rules::tick();
    {
        const auto& st = rules::state();
        world::set_session(st.active, st.invader, st.invader_present);
        int coop = 1;
        if (g_role == Role::Host || (g_role == Role::Guest && g_step == Step::InWorld)) coop = g_host_coop_players;
        world::set_players(coop);
        bool vicinity = false;
        if (g_role == Role::Host) vicinity = host_vicinity();
        if (g_role == Role::Guest && g_step == Step::InWorld) vicinity = g_host_vicinity;
        world::set_vicinity(vicinity);
        // The other co-op players, five times a second (the list asks the game for each member's character).
        static double team_at = 0;
        if (now() - team_at >= 0.2) {
            team_at = now();
            std::vector<spectate::Teammate> team;
            std::vector<spectate::Teammate> targets;  // we invade: the co-op players, for the Death Knell's view
            bool all_dead = true;
            const PeerInfo* boss_mate = nullptr;  // a living teammate with a boss gauge up (rules: bonfires wait)
            if (g_invade && g_step == Step::InWorld && !game::loading()) {
                // We invade: our teammates are the other invaders of this world.
                const std::vector<party::Remote> members = party::remotes();
                for (const auto& p : g_peers) {
                    if (!invader_teammate(p, members)) continue;
                    spectate::Teammate t;
                    t.id = p.id;
                    t.name = shown_name(p);
                    t.chr = chr_of_peer(p.id);
                    t.alive = !p.dead;
                    t.map = p.map;
                    std::copy(std::begin(p.pos), std::end(p.pos), t.pos);
                    t.pos_at = p.pos_at;
                    t.boss_fight = p.boss_fight && now() - p.last_seen <= 5.0;
                    t.cutscene = p.event_anim == kReportCutscene && now() - p.last_seen <= 5.0;
                    t.loading = p.event_anim == kReportLoading && now() - p.last_seen <= 5.0;
                    if (p.floor_at > 0 && now() - p.floor_at <= 3.0) {
                        t.floor_map = p.floor_map;
                        t.floor_index = p.floor_index;
                    }
                    team.push_back(t);
                }
                for (const auto& p : g_peers) {
                    if (!coop_member(p, members)) continue;
                    spectate::Teammate t;
                    t.id = p.id;
                    t.name = shown_name(p);
                    t.chr = chr_of_peer(p.id);
                    t.alive = !p.dead;
                    t.map = p.map;
                    std::copy(std::begin(p.pos), std::end(p.pos), t.pos);
                    t.pos_at = p.pos_at;
                    t.boss_fight = p.boss_fight && now() - p.last_seen <= 5.0;
                    t.cutscene = p.event_anim == kReportCutscene && now() - p.last_seen <= 5.0;
                    t.loading = p.event_anim == kReportLoading && now() - p.last_seen <= 5.0;
                    if (p.floor_at > 0 && now() - p.floor_at <= 3.0) {
                        t.floor_map = p.floor_map;
                        t.floor_index = p.floor_index;
                    }
                    targets.push_back(t);
                }
                all_dead = false;  // the co-op team's wipe rules are not ours
            } else if (g_role != Role::None && !g_invade && !game::loading()) {
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
                    t.map = p.map;
                    std::copy(std::begin(p.pos), std::end(p.pos), t.pos);
                    t.pos_at = p.pos_at;
                    t.boss_fight = p.boss_fight && now() - p.last_seen <= 5.0;
                    t.cutscene = p.event_anim == kReportCutscene && now() - p.last_seen <= 5.0;
                    t.loading = p.event_anim == kReportLoading && now() - p.last_seen <= 5.0;
                    if (p.floor_at > 0 && now() - p.floor_at <= 3.0) {
                        t.floor_map = p.floor_map;
                        t.floor_index = p.floor_index;
                    }
                    team.push_back(t);
                }
            }
            spectate::set_teammates(team);
            spectate::set_targets(targets);
            world::set_everyone_dead(!team.empty() && all_dead);
            // A team wipe takes every Bloody Mary away (the host's count; its guests follow).
            if (g_role == Role::Host && g_bloody > 0 && world::local_dead() && (team.empty() || all_dead)) {
                bloody_reset("the whole team fell");
            }
            int bloody = 0;
            if (g_role == Role::Host) bloody = g_bloody;
            if (g_role == Role::Guest && !g_invade && g_step == Step::InWorld) bloody = g_host_bloody;
            world::set_bloody(bloody);
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
                if (coop_team_down()) {
                    invasion_won();  // also a last guest who left instead of falling
                } else if (g_team_fell && !invader_teammate_standing()) {
                    invasion_team_lost();
                }
            }
        }
        rules::state().team_invasion = invasion_undecided() && team_invasion_host() && invader_teammate_standing();
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
    copy_hold_tick();
    if (!g_flag_queue.empty()) flush_flags();
    if (g_role != Role::None && g_lobby) {
        const bool changed = now() - g_last_info > 0.1 && info_changed();
        const bool floor = now() - g_last_info > kFloorReportGap && floor_changed();
        if (changed || floor || now() - g_last_info > 1.0 || (g_info_again_at > 0 && now() >= g_info_again_at)) {
            g_info_again_at = changed ? now() + 0.15 : 0;
            g_last_info = now();
            send_info();
        }
        if (now() - g_last_ping >= kPingEvery) {
            g_last_ping = now();
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            const uint64_t me = steam::self();
            for (uint64_t to : steam::lobby_members(g_lobby)) {
                // (A host answers only the players it let in, and so times only those.)
                if (to == me || is_lost(to) || (g_role == Role::Host && !admitted(to))) continue;
                if (++g_ping_seq == 0) ++g_ping_seq;  // 0 is no ping
                g_pings[g_ping_seq % kPingSlots] = PingOut{g_ping_seq, to, qpc.QuadPart};
                Simple p{};
                fill_header(&p, kPing);
                p.value = g_ping_seq;
                send_to(to, p, false);
            }
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
    // Hosting (1.6.0): the whole party invades another world together, or the orb calls our search off.
    if (g_role == Role::Host) {
        group_invade();
        return;
    }
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

namespace {

// Every co-op guest in our lobby knows free for all. An older guest's game would take it for no rules at all: its
// player could neither hurt nor lock on to the others, while their games would let them hurt that player.
bool guests_know_free_for_all() {
    if (!g_lobby) return true;
    const uint64_t me = steam::self();
    for (uint64_t m : steam::lobby_members(g_lobby)) {
        if (m == me) continue;
        const PeerInfo* p = peer(m);
        if (p->invader) continue;
        if (compare_versions(p->mod, kFreeForAllVersion) < 0) {
            LOG_INFO("[coop] Free for all left out: %s has Prepare to Co-op %s", p->name.c_str(),
                     p->mod.empty() ? "1.0.0" : p->mod.c_str());
            return false;
        }
    }
    return true;
}

}  // namespace

// The Ominous Tome: no rules, allies hostile (friendly fire without lock-on), the host against the guests, free for
// all (1.4.0: friendly fire with lock-on on everyone), and round again.
void cycle_rules() {
    if (!hosting()) return;
    auto& st = rules::state();
    st.world_rules = (st.world_rules + 1) % 4;
    if (st.world_rules == 3 && !guests_know_free_for_all()) {
        say(kMsgRulesNeedsNew);
        st.world_rules = 0;
    }
    LOG_INFO("[coop] World rules now %d (0 none, 1 allies hostile, 2 teams, 3 free for all)", st.world_rules);
    say(rules_text(st.world_rules));
    g_last_state = 0;  // tell the guests now
}

void taunt() {
    if (!hosting()) return;
    rules::state().taunt = true;
    steam::set_lobby_data(g_lobby, "ptdesc_fingers", "1");  // not ptdesc_taunt: see kFingersWeight
    // No InvasionCooldown while the fingers are in effect: one that is running ends now (invasion_over starts none).
    const bool cooling = now() < g_invade_open_at;
    g_invade_open_at = 0;
    publish_cooldown();
    LOG_INFO("[coop] Invaders are taunted into this world; no invasion cooldown until the session ends%s",
             cooling ? " (the one running is lifted)" : "");
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

namespace {
// Host: a Homeward trip of the party's, used by us (`by` 0) or by a co-op guest (kHomewardAsk).
bool start_homeward(HomewardKind kind, uint64_t by) {
    uint32_t why = trip_slot_busy() ? kTravelBusy : trip_refusal(kind == kHomeDarksign ? kTripDarksign : kTripBone);
    if (!why && kind == kHomeBone && bone_missing()) why = kTravelNoBone;
    if (why) {
        if (by) {
            LOG_INFO("[coop] %s used a %s: refused (%u)", steam::persona(by).c_str(), homeward_name(kind), why);
            answer_travel(by, why);
        } else {
            homeward_refused(why, kind);
        }
        return false;
    }
    if (by) LOG_INFO("[coop] %s used a %s: the party's trip home", steam::persona(by).c_str(), homeward_name(kind));
    // The trip takes the travel slot now: a guest's travel asked for in the same tick (the messages are read
    // before host_tick) would warp us a second time. A guest's travel waiting for our menu comes along to ours.
    g_homeward_bumped = 0;
    if (g_travel_bonfire >= 0) {
        LOG_INFO("[coop] The %s goes instead of %s's travel to bonfire %d", homeward_name(kind),
                 steam::persona(g_travel_from).c_str(), g_travel_bonfire);
        g_homeward_bumped = g_travel_from;
        g_travel_bonfire = -1;
    }
    g_homeward_at = now();
    g_homeward_kind = kind;
    g_homeward_by = by;
    g_homeward_told = 0;
    g_travel_started = now();
    return true;
}

// Guest: our Homeward ask is open. The host's game answers it at once, or starts the trip within 2 s (and gives up
// then if its own game is not ready); an ask nobody answers stays open for kTravelWait (5 s), one the host said waits
// for its menu for kTravelMenu + kTravelWait.
bool homeward_asked() {
    if (g_homeward_asked <= 0) return false;
    return now() - g_homeward_asked < (g_homeward_waits ? kTravelMenu + kTravelWait : kTravelWait);
}
}  // namespace

bool homeward_open() {
    if (hosting()) return true;
    return g_role == Role::Guest && g_step == Step::InWorld && !g_invade && g_host_homeward;
}

bool homeward(HomewardKind kind) {
    if (hosting()) return start_homeward(kind, 0);
    if (!homeward_open() || !g_host) return false;
    // One ask at a time: a use while the host's game still has ours is the same trip. Its answer (other than "waiting")
    // or the trip itself closes the ask, so a refused one can be tried again at once.
    if (homeward_asked()) {
        if (g_homeward_waits) say(fmt(kMsgTravelWaiting, shown_name(*peer(g_host))));
        return true;
    }
    g_homeward_asked = now();
    g_homeward_waits = false;
    Simple m{};
    fill_header(&m, kHomewardAsk);
    m.value = kind;
    send_to(g_host, m);
    LOG_INFO("[coop] Our %s: the host's game is asked to take everyone to its last bonfire", homeward_name(kind));
    return true;
}

uint64_t world_owner() { return game::own_world() ? steam::self() : g_host; }

bool in_session() {
    if (g_role == Role::Host) return g_step == Step::Hosting || g_step == Step::Closing;
    if (g_role == Role::Guest) {
        return g_step == Step::Hello || g_step == Step::Joining || g_step == Step::Loading || g_step == Step::InWorld;
    }
    return false;
}

bool hosting() { return g_role == Role::Host && g_step == Step::Hosting && g_lobby != 0; }

bool group_invade_open() { return hosting() && !rules::state().invader_present; }

std::vector<SummonSign> summon_signs() {
    std::vector<SummonSign> out;
    if (!coop_in_world() && !invader_in_world()) return out;
    for (const auto& [owner, sign] : g_signs) {
        const PeerInfo* p = nullptr;
        for (const auto& q : g_peers) p = q.id == owner ? &q : p;
        if (!p || p->invader) continue;  // not one of our co-op players (any more)
        SummonSign o;
        o.owner = owner;
        o.name = shown_name(*p);
        o.map = sign.map;
        std::copy(std::begin(sign.pos), std::end(sign.pos), o.pos);
        o.angle = sign.angle;
        o.floor_map = sign.floor_map;
        o.floor_index = sign.floor_index;
        out.push_back(o);
    }
    return out;
}

void set_knell_sign(uint64_t owner) {
    if (owner == g_knell_sign) return;
    g_knell_sign = owner;
    g_sign_fx_now = true;  // sign_fx_tick redraws it (and the one before) on the next frame
}

const char* sign_refusal() {
    if (!coop_in_world() || !g_lobby) return "Only in a co-operative session";
    if (!game::player() || game::loading()) return "Not while loading";
    if (world::local_dead()) return "Not while fallen";
    if (world::boss_fight()) return "Not in a boss fight";
    if (rules::cutscene_playing() || rules::cutscene_loading()) return "Not during a cutscene";
    return nullptr;
}

const char* place_summon_sign() {
    if (const char* why = sign_refusal()) return why;
    game::Vec3 at{};
    float angle = 0;
    if (!game::position(&at, &angle) || !std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) {
        return "Not here";
    }
    const int world = rules::world_map();
    const int map = static_cast<int>(static_cast<uint32_t>(world != -1 ? world : game::map_id()) & 0xFFFF0000u);
    const bool moved = g_my_sign.map != -1;
    g_my_sign.serial = static_cast<uint32_t>(GetTickCount64() & 0xFFFFFFFF) | 1;
    g_my_sign.map = map;
    g_my_sign.pos[0] = at.x;
    g_my_sign.pos[1] = at.y;
    g_my_sign.pos[2] = at.z;
    g_my_sign.angle = angle;
    // The floor we stand on, for the others' view of the sign (SignFloorMsg).
    uint32_t hit = 0;
    g_my_sign.floor_map = -1;
    g_my_sign.floor_index = -1;
    if (read32(game::player() + 0x29C, &hit) && hit) rules::floor_id(hit, &g_my_sign.floor_map, &g_my_sign.floor_index);
    send_sign();
    LOG_INFO("[coop] Our summon sign %s (%.1f, %.1f, %.1f) in map %08X (on collision %d of map %08X)",
             moved ? "moved to" : "placed at", at.x, at.y, at.z, static_cast<uint32_t>(map), g_my_sign.floor_index,
             static_cast<uint32_t>(g_my_sign.floor_map));
    return nullptr;
}

KnellMary knell_bloody_mary(bool sign) {
    const char* what = sign ? "rang the Death Knell to a summon sign" : "rang the Death Knell";
    if (g_role == Role::Host) return bloody_add("we", what, true) ? KnellMary::Added : KnellMary::None;
    if (g_role == Role::Guest && !g_invade && g_host) {
        KnellPriceMsg m{};
        fill_header(&m, kKnellPrice);
        m.sign = sign ? 1 : 0;
        send_to(g_host, m);
        g_knell_mary_at = now();
        g_knell_mary_from = g_host_bloody;
        LOG_INFO("[coop] Death Knell: the host's game is told of our price (a Bloody Mary for the session)");
        return KnellMary::Asked;
    }
    return KnellMary::None;
}

int near_count(uint32_t handle) {
    if (handle > 0xFFFF || g_near.empty()) return 0;
    const uint16_t h = static_cast<uint16_t>(handle);
    const double t = now();
    int n = 0;
    for (const auto& [id, l] : g_near) {
        if (t - l.heard > kNearLapse) continue;
        bool coop = false;
        for (const auto& p : g_peers) {
            if (p.id == id) coop = !p.invader && t - p.last_seen <= 10.0;
        }
        if (coop && std::binary_search(l.handles.begin(), l.handles.end(), h)) ++n;
    }
    return n;
}

int knell_bloody_mary_answer() {
    if (g_knell_mary_at <= 0) return -1;
    if (g_host_bloody > g_knell_mary_from) {
        g_knell_mary_at = 0;
        return 1;
    }
    if (now() - g_knell_mary_at < kKnellMaryWait && g_role == Role::Guest) return 0;
    g_knell_mary_at = 0;
    return -1;
}

bool trip_slot_busy() { return now() - g_travel_started < kTravelGap || g_homeward_at > 0 || homeward_asked(); }

// A guest or an invader takes the host's rate once it stands in the host's world: during the join its game sends the
// game's own six a second, as every older build did (the joins that fell through in the 2026-10-25 test were all at
// thirty; see retry_join).
int net_rate() {
    if (g_role == Role::Host) return g_cfg.net_rate;
    if (g_role == Role::Guest && g_step == Step::InWorld && g_host_net_rate > 0) return g_host_net_rate;
    return netrate::kVanilla;
}

bool seal_boss_fogs() {
    if (g_role == Role::Host) return g_cfg.seal_boss_fogs;
    return g_role == Role::Guest && g_host_seal_fogs;
}

bool guest() { return g_role == Role::Guest; }
bool coop_guest_in_world() {
    return g_role == Role::Guest && g_step == Step::InWorld && !g_invade && !game::own_world();
}
bool active() { return g_role != Role::None; }
bool idle() { return g_role == Role::None; }
bool invader() { return g_role == Role::Guest && g_invade; }

void knell_watching(uint64_t target) {
    if (!g_invade || g_step != Step::InWorld || !g_host || !target) return;
    static std::map<uint64_t, double> sent;
    const auto it = sent.find(target);
    if (it != sent.end() && now() - it->second < kKnellSpyEvery) return;
    sent[target] = now();
    KnellSpyMsg m{};
    fill_header(&m, kKnellSpy);
    m.target = target;
    m.watcher = steam::self();
    send_to(g_host, m);
    LOG_INFO("[coop] Death Knell: watching %s, who is told", steam::persona(target).c_str());
}
const std::vector<PeerInfo>& peers() { return g_peers; }

int remote_player_down(uint32_t chr, std::string* name) {
    const PeerInfo* p = chr && g_role != Role::None ? peer_by_chr(chr) : nullptr;
    if (!p) return -1;
    if (name) *name = p->name;
    return p->dead ? 1 : 0;
}

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
    // Only a rest in the host's world: a rest at home while the join loads is our own world's (2026-11-01: a guest who
    // sat down at the Undead Asylum's bonfire as the join went through had that bonfire as their respawn in the
    // host's world, and their next death took them to the host's Asylum).
    if (g_step != Step::InWorld || game::own_world()) {
        LOG_INFO("[coop] Rest at object %d outside the host's world (our own, while joining): the respawn there is "
                 "unchanged", object_id);
        return;
    }
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
    if (g_role == Role::None || g_invade || !g_lobby) return false;
    const PeerInfo* mate = resetting_mate(kResetWaiting);
    if (mate && (g_role == Role::Host || g_step == Step::InWorld)) {
        respawn_signal(kRespawnWaiting);
        g_respawn_waiting = true;
        g_reset_hold = true;
        g_respawn_since = now();
        LOG_INFO("[coop] %s's world reloads after the lost boss fight: our respawn waits for it, so that every game "
                 "reloads at once", utf8(shown_name(*mate)).c_str());
        say(kMsgTeamRespawn);
        return true;
    }
    if (!team_wiped()) return false;
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
    if (g_reset_hold) {
        const char* why = nullptr;
        if (g_role == Role::None || !g_lobby) {
            why = "the session ended";
        } else if (!resetting_mate(kResetWaiting)) {
            why = "no teammate's reload is pending any more";
        }
        if (!why) return false;
        g_respawn_waiting = false;
        g_reset_hold = false;
        LOG_INFO("[coop] Lost boss fight: our world reloads after %.1f s (%s)", waited, why);
        return true;
    }
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

void report_reset_phase() {
    if (g_role == Role::None || !g_lobby || g_invade) return;
    ResetPhase m{};
    fill_header(&m, kResetPhase);
    m.rest = g_rest_id;
    m.phase = g_reset;
    broadcast(m);
}

void set_reset_phase(uint8_t phase) {
    if (g_reset == phase) return;
    g_reset = phase;
    report_reset_phase();  // at once: the others' reloads or warps may be waiting on it
}

bool resets_ready() {
    const std::vector<party::Remote> members = party::remotes();
    for (const auto& p : g_peers) {
        if (coop_member(p, members) && !p.dead && p.reset == kResetWaiting) return false;
    }
    return true;
}

std::string reset_phase_missing() {
    const std::vector<party::Remote> members = party::remotes();
    for (const auto& p : g_peers) {
        if (coop_member(p, members) && p.team_respawn && p.reset_rest != g_rest_id) return utf8(shown_name(p));
    }
    return {};
}

bool invader_team_fell() { return invaders_all_fell(); }

bool wipe_reload(double fell_at) {
    const std::vector<party::Remote> members = party::remotes();
    int n = 0;
    for (const auto& p : g_peers) {
        if (!coop_member(p, members)) continue;
        ++n;
        if (p.dead && p.respawn == kRespawnWent) {
            if (p.went_kept) return false;  // their reload kept the world: ours keeps it too
            continue;                       // their reload started it over: part of the wipe
        }
        if (p.dead) continue;  // down, not gone yet
        if (p.went_over_at > 0 && p.went_over_at >= fell_at) continue;  // up again from the wipe's reload
        return false;  // standing
    }
    return n > 0;
}

bool others_wait_on_reset() {
    const std::vector<party::Remote> members = party::remotes();
    for (const auto& p : g_peers) {
        if (!coop_member(p, members)) continue;
        if (p.dead ? p.team_respawn && p.respawn == kRespawnWaiting : p.reset == kResetReady) return true;
    }
    return false;
}

// A lost boss fight's reload is pending: ours, or a living co-op teammate's.
bool reload_hold() {
    if (g_role == Role::None || g_invade) return false;
    if (world::reset_pending()) return true;
    const std::vector<party::Remote> members = party::remotes();
    for (const auto& p : g_peers) {
        if (coop_member(p, members) && !p.dead && (p.reset == kResetWaiting || p.reset == kResetReady)) return true;
    }
    return false;
}

bool darksign_held() { return reload_hold(); }

int bone_hold() {
    if (g_role == Role::None || g_invade) return kHoldNone;
    const auto& st = rules::state();
    if (st.invader_present) return kHoldInvader;
    if (reload_hold()) return kHoldReload;
    // In a boss fight (ours, or a standing teammate's anywhere) the bone takes everyone out of it, a teammate who
    // fell in it too: their held death goes ahead once the fight is gone, and they follow once back on their feet.
    if (world::boss_fight() || st.teammate_boss_fight) return kHoldNone;
    const std::vector<party::Remote> members = party::remotes();
    for (const auto& p : g_peers) {
        if (coop_member(p, members) && p.dead) return kHoldMateDown;
    }
    return kHoldNone;
}

int travel_hold() {
    if (g_role == Role::None || g_invade) return kHoldNone;
    const auto& st = rules::state();
    if (world::boss_fight() || st.teammate_boss_fight) return kHoldBossFight;
    if (st.invader_present) return kHoldInvader;
    if (world::reset_pending()) return kHoldReload;
    const std::vector<party::Remote> members = party::remotes();
    int hold = kHoldNone;
    for (const auto& p : g_peers) {
        if (!coop_member(p, members)) continue;
        if (!p.dead && (p.reset == kResetWaiting || p.reset == kResetReady)) return kHoldReload;
        if (p.dead) hold = kHoldMateDown;
    }
    return hold;
}

const wchar_t* travel_hold_text(int hold) {
    switch (hold) {
        case kHoldBossFight: return L"No travel while a boss fight is on";
        case kHoldInvader: return L"No travel while an invader is in the world";
        case kHoldReload: return L"No travel while the world starts over after a lost boss fight";
        default: return L"No travel while a teammate is down";
    }
}

bool fallen_at_respawn() {
    const std::vector<party::Remote> members = party::remotes();
    for (const auto& p : g_peers) {
        if (coop_member(p, members) && p.team_respawn && p.dead && p.respawn == kRespawnNone) return false;
    }
    return true;
}

void note_respawn_went(bool kept) {
    if (g_role == Role::None || g_invade || !g_lobby || g_respawn == kRespawnWent) return;
    if (g_role == Role::Guest && g_step != Step::InWorld) return;
    // A reload that starts the world over (a team wipe; the host alone) sends every NPC phantom home, before the
    // load, the game's own way: every other game gets packet 49, and our emptied list goes out at once (we send
    // none while loading; one the others still held would bring the phantoms back there, also onto a fresh copy
    // after their own reload). Any other death's reload keeps them summoned.
    if (g_role == Role::Host && !kept) {
        summons::dismiss_all("Team wipe");
        publish_npc_list(true);
    }
    g_respawn_kept = kept;
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
wchar_t g_nameplate[kMaxSessionPlayers][3][96] = {};
int g_nameplate_n[kMaxSessionPlayers] = {};

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

// The character of a floating bar's slot record (MenuMan+0x8F4 + 0x20 * slot, filled by 0x1012C30 from
// PartyMemberInfo entry `slot`), or 0.
uint32_t chr_of_bar(uint32_t rec) {
    uint32_t mm = 0, gm = game::game_man(), party = 0, handle = 0xFFFFFFFF;
    if (!read32(0x013786D0, &mm) || !mm || rec < mm + 0x8F4) return 0;  // MenuMan
    const uint32_t slot = (rec - (mm + 0x8F4)) / 0x20;
    // With more players the bars are fed from every other player's character (players.cpp), not party entry `slot`.
    if (players::bars_fed()) return slot < 7 ? players::bar_chr(static_cast<int>(slot)) : 0;
    if (slot > 3 || !gm || !read32(gm + game::kGmParty, &party) || party < 0x10000 ||
        !read32(party + 0x18 + slot * 0x14, &handle)) {
        return 0;
    }
    return rules::chr_of_handle(handle);
}

// The smoothing our game draws a character with (netrate, DynamicSmoothing), -1 none: its network manipulator.
int chr_smoothing(uint32_t chr) {
    uint32_t manip = 0;
    return chr && read32(chr + 0x2C, &manip) ? netrate::smoothing_ms(manip) : -1;
}

// The round trip shown for player `p`: our own to them, when one came in the last 3 s; else (a player whose answers do
// not come) a guest's own to its host, or the one `p` reports, theirs to their host.
int shown_ping(const PeerInfo& p) {
    if (p.rtt_ms > 0 && now() - p.rtt_at < 3.0) return p.rtt_ms;
    if (g_role == Role::Guest && p.id == g_host) return g_ping_ms;
    return p.ping_ms;
}

// The text for player `p`'s floating bar; `mine` picks its buffers, `chr` is their character here (0 unknown).
const wchar_t* plate_text(const PeerInfo& p, int mine, uint32_t chr) {
    const int mode = g_cfg.overhead_display;
    const std::wstring shown_as = shown_name(p).substr(0, 16);
    const wchar_t* who = shown_as.c_str();
    const int ping = shown_ping(p);
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
        case 4: swprintf_s(out, kCap, L"(%ls, SL: %d, %ls)", who, p.soul_level, ms); break;
        default: {
            // The smoothing ("lerp", as the shooters' net graphs call their interpolation delay).
            const int lerp = chr_smoothing(chr);
            if (lerp >= 0) {
                swprintf_s(out, kCap, L"(%ls, SL: %d, %ls, lerp %dms)", who, p.soul_level, ms, lerp);
            } else {
                swprintf_s(out, kCap, L"(%ls, SL: %d, %ls)", who, p.soul_level, ms);
            }
            break;
        }
    }
    return out;
}

// The bar's current name, or null when this bar is not a companion we know. The game writes the player's Steam
// name there (its member record, member[n] in the "Character names" log line); the bar shows the character's name.
// A record can hold no name: an invader saw a 1.2.0 guest's bar in the Kiln of the First Flame with no name, soul
// level or ping (a Steam name of 22 characters, member[1]='', 2026-10-29). A bar whose name names no one is known by
// its slot (`rec`, the caller's ebp): the slot's character, matched to its player as the world rules match them.
const wchar_t* __stdcall nameplate_for(const wchar_t* shown, uint32_t rec) {
    if (g_role == Role::None) return nullptr;
    // Which of the HUD's seven bars draw (for sessions of more than four: players.cpp feeds all seven).
    static uint32_t drawn = 0;
    uint32_t mm = 0;
    if (read32(0x013786D0, &mm) && mm && rec >= mm + 0x8F4 && rec < mm + 0x8F4 + 7 * 0x20) {
        const uint32_t bit = 1u << ((rec - (mm + 0x8F4)) / 0x20);
        if (!(drawn & bit)) {
            drawn |= bit;
            LOG_INFO("[coop] Floating health bar %u draws (bars seen so far: %02X)", (rec - (mm + 0x8F4)) / 0x20, drawn);
        }
    }
    wchar_t name[24] = {};
    const bool named = read_shown_name(shown, name, 24) && wcslen(name) <= 16;
    auto member_is = [&](uint64_t id) {
        for (const auto& [member_name, who] : g_member_names) {
            if (who == id && member_name == name) return true;
        }
        return false;
    };
    auto by_name = [&](const PeerInfo& p) {
        return named && (utf8_is(name, p.name) || (!p.char_name.empty() && wcscmp(name, p.char_name.c_str()) == 0) ||
                         member_is(p.id));
    };
    // The players heard from lately, in order: the index picks the buffers.
    const PeerInfo* live[kMaxSessionPlayers] = {};
    int n = 0;
    for (const auto& p : g_peers) {
        if (n >= kMaxSessionPlayers) break;
        if (now() - p.last_seen <= 10.0) live[n++] = &p;
    }
    const uint32_t bar_chr = chr_of_bar(rec);
    for (int i = 0; i < n; ++i) {
        if (by_name(*live[i])) return plate_text(*live[i], i, bar_chr);
    }
    const auto it = g_chr_peer.find(bar_chr);
    if (it == g_chr_peer.end()) return nullptr;
    for (int i = 0; i < n; ++i) {
        if (live[i]->id == it->second) return plate_text(*live[i], i, bar_chr);
    }
    return nullptr;
}

// ecx is the name widget. The wide string is the one stack argument (the callee pops it); ebp is the bar's slot
// record (0xFA8BA5: mov eax,[ebp+10h]).
__declspec(naked) void hk_floating_name() {
    __asm {
        push ecx
        push edx
        push ebp
        push dword ptr [esp + 16]
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

// An invader's floating bar (with its name, soul level and ping): vanilla hides it from the host and the guests (the
// slot's +0x1C, set for a remote dark spirit, tested at 0xFA89E8). Everyone sees everyone's bar now, invaders
// included. A player under Chameleon has none on anyone's screen: their own report (InfoMore4.disguised) or the
// effect on their character here says so. Slot i is the party's entry i (0x1012C30 fills it from PartyMemberInfo).
constexpr uint32_t kBarDarkTest = 0x00FA89E8;
uint32_t g_bar_dark_on = 0x00FA8A00;    // the bar goes on to the game's other tests
uint32_t g_bar_dark_off = 0x00FA8AC0;   // no bar (stores bl = 0)
int g_bar_answer = 0;

// 0 the game's own answer, 1 shown, 2 hidden.
int __stdcall bar_answer(uint32_t rec) {
    // During a load a bar's character may already be freed (its effect list read faulted, 2026-10-31).
    if (g_role == Role::None || game::loading()) return 0;
    const uint32_t chr = chr_of_bar(rec);
    if (!chr) return 0;
    const auto it = g_chr_peer.find(chr);
    if (it != g_chr_peer.end()) {
        for (const auto& p : g_peers) {
            if (p.id == it->second && p.disguised) return 2;
        }
    }
    // A character just made holds anything where its effect list goes until its first updates: each frame's look
    // into it faulted (first-chance, handled) hundreds of times as players came back after a load (2026-11-01, every
    // game, XINPUT1_3+0x1F36D from find_effect). Its effects are read from its second second on.
    // (No bars are asked for during a load: a gap in the calls starts the list over, as a load frees every character.)
    static std::map<uint32_t, double> first_seen;
    static double last_call = 0;
    if (now() - last_call > 0.5 || first_seen.size() > 64) first_seen.clear();
    last_call = now();
    const auto seen = first_seen.emplace(chr, now()).first;
    if (now() - seen->second < 1.0) return 0;
    if (world::has_effect(chr, kChameleonEffect)) return 2;
    uint8_t dark = 0;
    return read8(rec + 0x1C, &dark) && dark ? 1 : 0;
}

__declspec(naked) void hk_bar_dark() {
    __asm {
        mov bl, 1
        mov byte ptr [esp + 13h], 0
        mov byte ptr [esp + 14h], bl
        pushad
        push ebp
        call bar_answer
        mov dword ptr [g_bar_answer], eax
        popad
        cmp dword ptr [g_bar_answer], 2
        je off
        cmp dword ptr [g_bar_answer], 1
        je on
        cmp byte ptr [ebp + 1Ch], 0
        je on
    off:
        xor bl, bl
        jmp dword ptr [g_bar_dark_off]
    on:
        jmp dword ptr [g_bar_dark_on]
    }
}

bool install_invader_bars() {
    // cmp byte ptr [ebp+1Ch],0 ; mov bl,1 ; mov byte ptr [esp+13h],0 ; mov [esp+14h],bl ; je +7 ; xor bl,bl ; jmp
    static const uint8_t kDark[19] = {0x80, 0x7D, 0x1C, 0x00, 0xB3, 0x01, 0xC6, 0x44, 0x24, 0x13,
                                      0x00, 0x88, 0x5C, 0x24, 0x14, 0x74, 0x07, 0x32, 0xDB};
    if (bytes_match(kBarDarkTest, kDark, sizeof(kDark))) {
        uint8_t to[5] = {0xE9, 0, 0, 0, 0};
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_bar_dark) - (kBarDarkTest + 5));
        std::memcpy(to + 1, &rel, 4);
        if (patch(kBarDarkTest, kDark, to, sizeof(to))) {
            LOG_INFO("[coop] The host and guests see invaders' health bars; Chameleon hides anyone's");
        }
    } else {
        LOG_ERROR("[coop] Invaders' health bars stay hidden from the host and guests (code does not match this build)");
    }
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
        if (st.world_rules == 3) rules_line += L"Free for all  ";
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
        line += detail(p.soul_level, p.deaths, shown_ping(p));
        out.push_back(line);
    }
    return out;
}

}  // namespace coop::session
