#pragma once

// Prepare to Die Edition (DARKSOULS.exe 1.0.2.0, Steam) addresses and thin wrappers used by the seamless
// co-op module. Every address was found by reverse engineering the PTDE executable; the matching
// Remastered functions are noted where they were the starting point. Functions run on the game's main
// thread only (call them from the co-op tick).

#include <cstdint>
#include <vector>

namespace coop::game {

// Singletons (pointer variables).
constexpr uint32_t kGameManVar = 0x013784A0;       // FrpgGameMan (warp requests, summon data, party info)
constexpr uint32_t kGameDataManVar = 0x01378700;   // GameDataMan (+8 PlayerGameData, +0xC remote players)
constexpr uint32_t kWorldChrManVar = 0x0137D644;   // WorldChrMan (+0x3C local PlayerIns)
constexpr uint32_t kSessManVar = 0x0137D82C;       // FrpgSessionManagerImp
constexpr uint32_t kNetManVar = 0x0137E204;        // FrpgNetManImp
constexpr uint32_t kEventFlagManVar = 0x0137D7D4;  // EventFlagMan (+0x10C active store: +0x14 own world, +0x90 other)
constexpr uint32_t kLuaEventsVar = 0x0137D790;     // Lua event system (+0x1D8 temporary summon parameter)
constexpr uint32_t kWorldObjManVar = 0x0137D5F8;

// FrpgSessionManagerImp fields.
constexpr uint32_t kSessState = 0x90;       // 0 none, 1 creating, 2 create failed, 3 hosting, 4 joining,
                                            // 5 join failed, 6 joined (guest), 7 leaving
constexpr uint32_t kSessMaxMembers = 0x8C;
constexpr uint32_t kSessJoinStep = 0x194;   // guest: 1 joined, 2 host data received, 3 reload into the host's
                                            // world requested, 4 in the host's world
constexpr uint32_t kSessRoomTimer = 0x1BC;  // host: seconds left until an empty room closes (<0 = never)
constexpr uint32_t kSessLocked = 0x1C2;     // LockSession: refuse new members
constexpr uint32_t kSessMembersBegin = 0x100, kSessMembersEnd = 0x104;  // 0x24-byte member records

// FrpgGameMan fields.
constexpr uint32_t kGmWarpRequest = 0x11;    // byte: move to another map/point
constexpr uint32_t kGmWarpMap = 0x14;        // map id (area << 24 | block << 16)
constexpr uint32_t kGmWarpPoint = 0x18;      // warp point entity id, -1 = map default
constexpr uint32_t kGmLastBonfire = 0xB04;   // bonfire entity id of the last rest (-1 none)
constexpr uint32_t kGmUseBonfire = 0xB02;    // byte: spawn at the last bonfire
constexpr uint32_t kGmOwnWorld = 0xB4F;      // byte: 1 own world, 0 someone else's
constexpr uint32_t kGmJoinType = 0xBE4;      // summon type (-1 white, -2 black ...)
constexpr uint32_t kGmJoinFlag = 0xBE8;
constexpr uint32_t kGmParty = 0xBEC;         // PartyMemberInfo*

struct Vec3 {
    float x, y, z;
};

uint32_t game_man();
uint32_t game_data_man();
uint32_t sess_man();
uint32_t event_flag_man();
uint32_t player();             // local PlayerIns, 0 while loading
uint32_t player_game_data();   // GameDataMan+8

int session_state();
int join_step();
void set_join_step(int step);
int member_count();            // session members including us
bool own_world();
int chr_type();                // PlayerIns+0x70: 0 human, 1 white, 2 black, 8 hollow, 12 intruder
int map_id();                  // PlayerIns+0x444
bool position(Vec3* pos, float* angle);
// Another character's position (PlayerIns/ChrIns: +0x28 controller, +0x1C physics, +0x10 position).
bool chr_position(uint32_t chr, Vec3* pos);
// The other players' characters in our world: WorldChrMan's player slots 1.. (+0x24 count, +0x28 array of
// 0x20-byte entries; slot 0 is the local player).
std::vector<uint32_t> remote_player_chrs();
// A character's current animation on layer 0 or 1, packed as the network samples carry it (0xE2C030: anim id in
// the low 16 bits), 0xFFFFFFFF none.
uint32_t anim_state(uint32_t chr, int layer);
// A character's controller (+0x28) manipulators: +0x54 its own (PadManipulator for the local player,
// NetworkManipulator for another player), +0x244 an override that drives it instead while set.
uint32_t manipulator(uint32_t chr);
uint32_t override_manipulator(uint32_t chr);
int last_bonfire();
bool loading();                // no local player (map load in progress)
bool in_game();                // a character is loaded (not title/menu)

// Host: starts a session (state 1 -> 3), like touching a white sign. `map` from map_id(), timeout in s.
bool create_session(int map, float timeout);
// Host: the bytes the game sends a summoned player (join type, flag, NRSessionSearchResult of our session).
bool build_invitation(int join_type, int flag, std::vector<uint8_t>* out);
// Guest: joins the session an invitation describes (the vanilla guest code path), then sets the spawn point.
bool join_from_invitation(const uint8_t* data, uint32_t size, int map, const Vec3& pos, float angle);
// The game's own leave (also used for invasion endings and genuine disconnects).
void leave_session();
// Stop new members and the empty-room countdown.
void keep_room_open();

// Event flags.
bool flag(int id);
void set_flag(int id, bool on);           // active store (whatever world is loaded)
void set_own_world_flag(int id, bool on);  // store of the player's own world, even while away

// Warps the local player to `map` at warp point `point` (a bonfire's entity id), or the map default.
void warp(int map, int point);
// Map id of a bonfire entity id (e.g. 1012961 -> m10_01).
int bonfire_map(int bonfire);

// Runs a global function of the game's event script by name (like the game's own OnIrregularLeaveSession).
bool call_lua(const char* name);

// The local player's soul level, souls held and death count.
int soul_level();
int souls();
void set_souls(int value);
int death_count();

}  // namespace coop::game
