#pragma once

// The seamless co-op items (goods 389000-389008): extra EquipParamGoods rows served by the goods param
// getter, their texts through the FMG hook, and their use caught in PlayerIns::UseGoods.

#include <cstdint>

namespace coop::items {

enum Id : int32_t {
    kHost = 389000,      // open a session
    kJoin = 389001,      // join a session with your password
    kInvade = 389002,    // invade a random session
    kLeave = 389003,     // leave or close the session
    kRules = 389004,     // host: allow or forbid invaders
    kTaunt = 389005,     // host: call invaders in
    kMisfortune = 389006,  // host: the world's enemies grow stronger until a rest
    kBlossom = 389007,   // curse: less max HP, more souls; wilts at a bonfire
    kClemency = 389008,  // restores certain NPCs to life
    kFirst = kHost,
    kLast = kClemency,
};

bool install();
// Gives the missing items (own world, at most once per item per call).
void grant_missing();
int count(int32_t goods_id);

// The handler for a used co-op item (set by the coop module).
void set_use_handler(void (*fn)(int32_t goods_id));
// Items whose use is refused (e.g. Homeward Bone while an invader is in the world): return true to block.
void set_block_filter(bool (*fn)(int32_t goods_id));

}  // namespace coop::items
