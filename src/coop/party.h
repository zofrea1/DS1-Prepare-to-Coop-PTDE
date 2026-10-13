#pragma once

// The other players' characters in our world. PTDE creates a session member's character once, when the
// member joins: the member goes on the session's waiting list (SessMan+0x118) and the in-game step (0xEE2B10)
// builds the character as soon as the member's player data is complete. A map load (death, bonfire travel)
// throws every character away and nobody is put on the list again, so after a death the players no longer
// see each other. This module puts members whose character is missing back on that list, and removes and
// rebuilds the character of a player who died and came back (the old one stays dead otherwise).

#include <cstdint>
#include <string>
#include <vector>

namespace coop::party {

struct Remote {
    int slot = -1;          // the member's slot in the remote player data (0..2)
    uint32_t chr = 0;       // their PlayerIns in our world (0 = none)
    std::wstring name;      // their character name
    bool data_ready = false;
};

// Session members that have a slot, with their character in our world.
std::vector<Remote> remotes();
// Our own character name (PlayerGameData+0xA0).
std::wstring local_name();

// What the session knows about a player, by character name.
struct PeerState {
    bool known = false;
    bool invader = false;
    bool alive = false;     // reports being alive and has done so for a moment
    int deaths = -1;        // their death count as they report it (-1 unknown)
    int map = -1;           // the map they report being in (-1 unknown)
    float pos[3] = {};      // where they report standing (once a second)
    bool has_pos = false;
};

// Every frame on the game thread while a seamless session is established. `peer` answers for a character (and
// its character name) which session player it belongs to.
void tick(bool active, PeerState (*peer)(uint32_t chr, const std::wstring& name));

}  // namespace coop::party
