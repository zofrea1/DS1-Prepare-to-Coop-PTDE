#pragma once

// World rules of seamless co-op that are not about the session itself: bonfire rests (the world resets for
// every player), limbo (a death during a boss fight or an invasion waits for the fight to end), enemy scaling
// per player and the optional death debuff (Crimson Blossoms). The SpEffects these use are Remastered
// Seamless Co-op's extra SpEffectParam rows, served by the SpEffect param getter.

#include <cstdint>
#include <vector>

namespace coop::world {

struct Config {
    bool death_debuffs = false;
    int enemy_health = 35, enemy_damage = 0, enemy_posture = 15;
    int boss_health = 100, boss_damage = 0, boss_posture = 20;
};

bool install();
void configure(const Config& cfg);
void tick();

// Session state the rules depend on.
void set_session(bool active, bool invader, bool invader_present);
void set_players(int coop_players);       // players sharing the host's world, invaders not counted
void set_everyone_dead(bool all_dead);    // every other player is dead too
// A local rest at a bonfire: `before` runs before the game's own rest (items), `fn` after it (tell the others).
void set_rest_handler(void (*fn)());
void set_before_rest_handler(void (*fn)());
// The local player died (in a session).
void set_death_handler(void (*fn)());
// How many Crimson Blossoms the player carries (the death debuff's strength).
void set_blossom_counter(int (*fn)());
// A bonfire rest: take off every death-debuff effect immediately. The inventory clear is separate.
void clear_death_debuff();
// Another player rested: reset this world (enemies respawn), clear rest-cured effects, end limbo.
void remote_rest();

bool boss_fight();
bool local_dead();
bool in_limbo();
// Shared light: the light effects the local player carries (bit 0 Skull Lantern, 1 Cast Light, 2 Sunlight
// Maggot), and applying the other players' lights to their characters here.
struct RemoteLight {
    float pos[3];
    int mask;
};
int local_lights();
void share_lights(const std::vector<RemoteLight>& peers);

// Warps that must not wait in limbo (going home, following the host).
void warp_now(int map, int point);
void bypass_limbo(bool on);

}  // namespace coop::world
