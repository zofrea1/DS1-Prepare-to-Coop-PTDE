# Seamless Co-op for Prepare to Die Edition

Play all of Dark Souls together with up to three friends, from the Undead Asylum to the Kiln, without the
limits of the game's own co-op. Built into the PTDE FPS Unlock mod behind `[SeamlessCoop] SeamlessCoop` in
`PTDE-FPS-Unlock.ini` (off by default) and independent of the FPS unlock.

This follows the design of LukeYui's **Dark Souls Remastered Seamless Co-op** (`ds1sc.dll`), which only
exists for Remastered. Nothing of that mod is used: its behaviour was studied by reverse engineering and the
same ideas were written again for PTDE's engine.

**Status:** complete and building; **not yet tried in the game** (it needs two PCs and two Steam accounts).

## How to play

1. Everyone sets the same `Password` and `SeamlessCoop = true`. Keep the other settings the same too (the
   host's world rules apply, but enemy scaling is worked out on each PC).
2. Start the game. Your character now has nine new items (shown under Items), given in your own world:

   | Item | Use |
   | --- | --- |
   | Seamless Host Orb | Open your world. Players with your password can join at any time, wherever you are. |
   | Seamless Join Orb | Join the world of a player with your password. You arrive next to the host. |
   | Seamless Invasion Orb | Invade a random seamless world that allows invaders, as an evil spirit. |
   | Seamless Return Orb | A guest goes home; the host closes the session and everyone goes home. |
   | Tome of World Rules | Host: allow or forbid invaders for this session. |
   | Withered Finger | Host: invaders searching for a world find yours first. |
   | Pendant of Misfortune | Host: enemies fight as if one more player were present, until a bonfire rest. |
   | Crimson Blossom | Lower maximum HP (85%), 25% more souls, until your next rest. |
   | Writ of Clemency | Repairs certain NPC flags that can go wrong while visiting other worlds. |

   Keyboard shortcuts do the same (`CoopHotkeys`): **Ctrl+F1** host, **Ctrl+F2** join, **Ctrl+F3** leave,
   **Ctrl+F4** invade.
3. A panel at the top right lists the players in the session, with ping, soul level or deaths
   (`OverheadPlayerDisplay`).

**Your save.** With seamless co-op on, the game uses `DRAKS0005.co2` (`SaveFileExtension`) next to
`DRAKS0005.sl2` in `Documents\NBGI\DarkSouls`. To play an existing character, copy `DRAKS0005.sl2` to
`DRAKS0005.co2` before starting. Vanilla play keeps using `.sl2`.

**Steam.** The game runs as Steam's Spacewar (app 480) while seamless co-op is on. PTDE's own app (211420)
has had its Steam lobbies switched off since 2022, and Spacewar is the long-standing community route for PTDE
multiplayer. The game's own online features (messages, bloodstains, signs, vanilla invasions) still work,
but only between players with the same password.

## What changes in a session

| Rule | Seamless co-op |
| --- | --- |
| Guests | Keep their own body (human or hollow), level, items and progress. They pick up items, get their own enemy drops and boss rewards, rest at bonfires, level up and talk to NPCs. |
| Deaths | You respawn at the last bonfire (the host's, until you rest at your own in the host's world). The session goes on, the host's death included. |
| Bosses and areas | Defeating a boss or crossing into another area no longer ends the session. Joining is never locked. |
| Fog walls | The white walls that keep phantoms in the host's area are gone. |
| Host travel | When the host warps (bonfire travel, Homeward Bone, death), guests follow to the host's bonfire. |
| Bonfire rest | When anyone rests, every player's world resets (enemies respawn). |
| Progress | Event flags changed in the session's world are shared with everyone, and a guest's flags are written to their own world too: progress made together carries home. |
| Limbo | Dying during a boss fight or while an invader is present holds your respawn until the fight ends, everyone has fallen, or someone rests. |
| Invasions | With `AllowInvaders`, others can invade as evil spirits. While an invader is in the world, bonfires are out of use and Homeward Bone and Darksign do nothing. Invaders cannot enter the Undead Asylum or Firelink Shrine. |
| Enemy scaling | Every enemy gets more health, damage and poise per extra co-op player (`EnemyHealthScaling` ... `BossPostureScaling`, percent per player). |
| Shared light | Another player's Skull Lantern, Cast Light or Sunlight Maggot also lights your game. |
| Death debuff | With `DeathDebuffs`, each death lowers maximum HP by 10% (down to half) until you rest. |

## How it works

### Finding and joining

The mod keeps an invisible Steam lobby (`ISteamMatchmaking`, `SteamMatchMaking009`) tagged with a hash of the
password, and talks to the other players on a private peer-to-peer channel (`ISteamNetworking`, channel 9;
the game itself uses channels 0 and 1). Everything else is the game's own multiplayer: a guest joins through
the game's normal session and summon code, so the world, enemies and players synchronise exactly as in
vanilla co-op.

1. The host's game opens a session the way touching a white sign does (`CreateSession`, `0xD2D4F0`). The
   room's empty-room timer and lock are kept off.
2. A guest finds the lobby, asks the host (`Hello`), and the host's game builds the same invitation it sends a
   summoned phantom (join type, flag and the session's `NRSessionSearchResult`, code at `0xDB44EE`).
3. The guest deserialises it and joins with the game's own guest code path (`JoinSession`, `0xD2D320`, as at
   `0xDB47A4`), with the host's position as the spawn point. The game then loads the guest into the host's
   world ("other world": separate event-flag store, own world saved for the return).
4. On arrival the game would turn the guest into a white phantom (`0xEA15B0`); the guest keeps their own
   chr type instead.

### Keeping the session

`FrpgSessionManagerImp`'s leave (`0xD2B460`) is called by many rules. The mod checks who calls it: the rule
leaves are skipped (boss kills via Lua `LeaveSession`, `WarpNextStageKick` from someone else's world, the end
of the in-game step on every load, the idle timer, the empty-room timeout, the host walking into another map
block, a load with fewer than two members, event restart warps), the party reset after three of them too
(`0xEA1CB0`); network failures, invasions and an intended leave still run. Lua `LockSession` does nothing.
The host's death would broadcast `LuaCallStart(4000, 1)`, which sends every guest home; it is not sent (an
invader gets the victory from the mod instead). A guest's later loads (death, following the host) are marked
as loads into someone else's world while `MoveMapStep` (`0xEFC470`) decides which world to load.

### Rules for guests

| What | Where |
| --- | --- |
| Enemy and object drops, scripted reward items | inlined guest checks in `0xDC80A0`, `0xDC8370`, `0xDC9170` |
| Bonfires and NPC talk | `HasPartyMembers` (`0xD2A4B0`) at the bonfire object (`0xD58AE4`) and talk script function 0x34 (`0xD4ABFB`) |
| Multiplayer walls | Lua `GetMultiWallNum` (`0xD5DC40`) answers 0 |
| Progress | the event-flag setter (`0xD38CB0`): changes are shared, and a guest's are also written to their own world's store |

### World rules

- **Rest:** Lua `Util_RequestRegene` (`0xD5D820`, the bonfire rest event) tells the others, who reset their
  world the way a rest does (`WorldChrMan+0x2CC`).
- **Limbo:** `WarpNextStageKick` (`0xD62930`), which performs the respawn warp, waits while you are dead,
  others are alive and a boss gauge (`MenuMan+0xF64/+0xF78`) or an invader is present.
- **Scaling, debuff, blossom:** extra `SpEffectParam` rows (ids 389100-389130, built from the vanilla
  multiplayer "doping" effect 7500) served by the SpEffect param getter (`0xD1B650`) and applied with
  `ChrIns::ApplySpEffect` (vtable slot 125). Scaling is applied to every `EnemyIns`; bosses are the
  characters with a boss gauge.
- **Items:** extra `EquipParamGoods` rows 389000-389008 from the goods param getter (`0xD1E3A0`), names and
  descriptions through the game's FMG lookup (`0xD175E0`), use caught in `PlayerIns::UseGoods` (vtable slot
  128). Messages use the game's information list (`0xD4C090`/`0xD4C190`).
- **Save file:** the save manager's extension (`0x733FC0`).

## Differences from the Remastered mod

- **Four players, not six:** PTDE's engine keeps three remote players in a world.
- **Spectating in limbo** is not implemented: the camera stays where you fell.
- **Tome of World Rules** switches invasions on and off; the Remastered tome changes player-versus-player
  rules among allies.
- **Pendant of Misfortune** and **Crimson Blossom** have simple effects of our own design; their Remastered
  behaviour could not be fully established.
- **NPC dialogue** is not mirrored as speech: the progress it makes (event flags) is shared.
- The player panel is a list at the top right rather than text above each player.
- The Remastered mod's GitHub ban list is not used.

## Troubleshooting

`PTDE-FPS-Unlock.log` has `[coop]` lines for every step (Steam ready, session state, joins, kept sessions).
If any hook does not match the executable (it expects the 2015 Steam build), seamless co-op turns itself off
and says which.
