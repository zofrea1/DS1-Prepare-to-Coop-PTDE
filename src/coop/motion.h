// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// How the other players' characters move and animate in our world, beyond what the game's own player samples
// carry. Remastered Seamless Co-op relies on Remastered, whose samples carry the sender's gait; PTDE's do not.
//
//  * Gait: PTDE picks a remote character's walk, run or sprint from its estimated speed (0xE1E6A0: stand, walk
//    from 0.01 m/s, run from 2, sprint from 6). Remastered sends the sender's own gait with every sample and only
//    falls back to the speed (sprint from 4.5). Each player sends its gait here; the receiving game's choice is
//    replaced by it while the character is moving, and the fallback's sprint threshold is Remastered's.
//  * Bonfire ceremonies: sitting down, the sitting loop, standing up, kindling, Reverse Hollowing and the rest
//    are event animations (ChrIns 0xE69590), which the player samples do not carry: the other players saw the
//    character stand still. Remastered Seamless Co-op watches that function on the local player and, for its
//    list of nineteen bonfire animations, has every other game call it on that player's character. The port
//    does the same, and also passes on the end of an event animation (0xE69470, the event script's
//    StopLoopAnimation and the bonfire menu's own stops). PTDE's script stops the sitting loop before it plays
//    the standing up; a character whose loop was never stopped kept that loop's slot for good, which left it
//    without collision and without its normal animations. Each player also reports the loop it is in, so a
//    character that missed a message is put right within a second or two.

#include <cstdint>
#include <vector>

namespace coop::motion {

enum Gait : int8_t { kUnknown = -1, kStand = 0, kWalk = 1, kRun = 2, kSprint = 3 };

struct Anim {
    uint8_t kind;  // 0 an event animation starts, 1 the event animation is stopped
    int32_t anim;
    uint8_t loop;
};

bool install();
// Bonfire event animations are passed on to the other players and shown here (default on).
void set_bonfire_animations(bool on);

// Our own gait this frame (game thread, every frame).
Gait local_gait();
// Event animations the local player started or stopped since the last call.
std::vector<Anim> take_local_anims();
// The synced event animation the local player is looping right now (the bonfire sit), 0 none.
int local_loop_anim();

// The gait another player reports, for their character in our world (kUnknown: the game's own estimate).
void set_remote_gait(uint32_t chr, Gait gait);
void clear_remote_gaits();
// Starts or stops another player's event animation on their character here. False when nothing was done
// (no such character, it is dead, or bonfire animations are switched off).
bool play_remote_anim(uint32_t chr, const Anim& a);
// The synced event animation that character is looping in our game, 0 none.
int remote_loop_anim(uint32_t chr);
// The synced event animation that character still holds a slot for (looping or not), 0 none. The game lets
// go of a finished one by itself (0xE6B180, every frame); one that stays long after its player moved on
// was missed.
int remote_open_anim(uint32_t chr);
// Any event animation the character holds a slot for (a bonfire ceremony, a door, a lever, the fog gate, ...): its
// id, -1 held with no id known, 0 none. While one is held the character has no collision and takes no other
// animation (see the header comment).
int held_event_anim(uint32_t chr);
// True for the animations that are passed on (Remastered Seamless Co-op's list).
bool synced_anim(int anim);

}  // namespace coop::motion
