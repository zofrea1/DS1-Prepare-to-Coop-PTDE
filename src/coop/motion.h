#pragma once

// How the other players' characters move and animate in our world, beyond what the game's own player samples
// carry. Remastered Seamless Co-op relies on Remastered, whose samples carry the sender's gait; PTDE's do not.
//
//  * Gait: PTDE picks a remote character's walk, run or sprint from its estimated speed (0xE1E6A0: stand, walk
//    from 0.01 m/s, run from 2, sprint from 6). Remastered sends the sender's own gait with every sample and only
//    falls back to the speed (sprint from 4.5). Each player sends its gait here; the receiving game's choice is
//    replaced by it while the character is moving, and the fallback's sprint threshold is Remastered's.
// Bonfire ceremonies (sitting, standing, kindling, Reverse Hollowing) are not replayed on the other player's
// character. The event cores that play them (0xD71A20 / 0xD719C0) also take that character off the network
// manipulator and clear its collision, so the other player could push the body and its animations stopped.
// play_remote_anim ignores them, including ceremonies an older build still sends.

#include <cstdint>
#include <vector>

namespace coop::motion {

enum Gait : int8_t { kUnknown = -1, kStand = 0, kWalk = 1, kRun = 2, kSprint = 3 };

struct Anim {
    uint8_t kind;  // 0 PlayAnimation core (0xD71A20), 1 ForcePlayAnimation core (0xD719C0)
    int32_t anim;
    int32_t a2;    // the cores' second and third arguments as the event script passed them
    int32_t a3;
};

bool install();

// Our own gait this frame (game thread, every frame).
Gait local_gait();
// Event animations the local player played since the last call.
std::vector<Anim> take_local_anims();

// The gait another player reports, for their character in our world (kUnknown: the game's own estimate).
void set_remote_gait(uint32_t chr, Gait gait);
void clear_remote_gaits();
// Plays another player's event animation on their character here.
void play_remote_anim(uint32_t chr, const Anim& a);

}  // namespace coop::motion
