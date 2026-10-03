#pragma once

// The seamless co-op items (goods 389000-389008), as in Remastered Seamless Co-op: extra EquipParamGoods rows
// served by the goods param getter, their texts through the FMG hook, whether they can be used decided in the
// game's own item check (0xEADCA0), and their effect caught in PlayerIns::UseGoods. They behave like any other
// item: equipped to a quick slot, used with the item animation, refused (greyed out) when not usable.

#include <cstdint>

namespace coop::items {

enum Id : int32_t {
    kHost = 389000,       // Blessed Eye Orb: create a session
    kJoin = 389001,       // Crystal Eye Orb: join a session
    kInvade = 389002,     // Chaos Eye Orb: invade a session
    kLeave = 389003,      // Abyssal Eye Orb: leave (guest) or disband (host)
    kRules = 389004,      // Ominous Tome: host changes the world rules
    kTaunt = 389005,      // Dried Fingers: host taunts invaders into joining
    kInfection = 389006,  // Cursed Pendant: host curses their own world
    kBlossom = 389007,    // Crimson Blossom: death debuff counter (key item, never used)
    kParchment = 389008,  // Parchment of Deliverance: revive certain NPCs
    kFirst = kHost,
    kLast = kParchment,
};

// The Parchment's yes/no dialog (event text FMG id), answered with the soul cost.
constexpr uint32_t kParchmentDialog = 93129700;

bool install();

// Inventory (the local player's).
int count(int32_t goods_id);
void give(int32_t goods_id, int n);
void remove_all(int32_t goods_id);
// A bonfire rest: every co-op item except the Crimson Blossom that is not in the inventory (one each).
void grant_missing();

// Decides use for the local player: -1 the game's own rules, 0 refused, 1 allowed, 2 allowed if the game's
// own rules allow it too.
void set_can_use(int (*fn)(int32_t goods_id));
// Runs a used item's effect for the local player. Return true to skip the game's own effect.
void set_use_handler(bool (*fn)(int32_t goods_id));
// Text of the Parchment dialog.
void set_dialog_text(const wchar_t* (*fn)());

}  // namespace coop::items
