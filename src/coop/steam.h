#pragma once

// Steam for the seamless co-op module, through PTDE's own steam_api.dll (SteamMatchMaking009,
// SteamNetworking005). The module keeps its own invisible lobby for finding the session and talks to the
// other players on a private peer-to-peer channel; the game's session uses its own lobby and channels 0/1.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace coop::steam {

constexpr int kChannel = 9;  // the game reads channels 0 and 1

// Before the game initialises Steam (DllMain): run as `app_id`, and get a per-frame tick on the game thread
// (right after the game's SteamAPI_RunCallbacks). A non-empty `game_tag` limits the game's own lobbies
// (sessions, signs, invasions, messages) to players with the same tag.
bool install_early(uint32_t app_id, void (*tick)(), const std::string& game_tag);
// After SteamAPI_Init: interfaces and callbacks. Safe to call every frame; true once ready.
bool ready();
uint32_t app_id();
uint64_t self();
std::string persona(uint64_t id);

// Lobbies. Results arrive on the game thread from inside SteamAPI_RunCallbacks.
using LobbyResult = std::function<void(bool ok, uint64_t lobby)>;
void create_lobby(int max_members, LobbyResult done);
void find_lobbies(const std::vector<std::pair<std::string, std::string>>& equal, int max_results,
                  std::function<void(std::vector<uint64_t>)> done);
void join_lobby(uint64_t lobby, LobbyResult done);
void leave_lobby(uint64_t lobby);
bool set_lobby_data(uint64_t lobby, const char* key, const char* value);
std::string lobby_data(uint64_t lobby, const char* key);
void set_lobby_joinable(uint64_t lobby, bool joinable);
uint64_t lobby_owner(uint64_t lobby);
std::vector<uint64_t> lobby_members(uint64_t lobby);

// Called when someone enters (true) or leaves (false) a lobby we are in.
void on_lobby_member(std::function<void(uint64_t lobby, uint64_t user, bool entered)> fn);

// Peer to peer on kChannel.
bool send(uint64_t to, const void* data, uint32_t size, bool reliable);
void poll(const std::function<void(uint64_t from, const uint8_t* data, uint32_t size)>& fn);
void accept(uint64_t peer);  // let `peer` open a P2P session with us
void close(uint64_t peer);
void on_p2p_failed(std::function<void(uint64_t peer, int error)> fn);

}  // namespace coop::steam
