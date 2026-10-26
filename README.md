## If my mod brought you some happiness, and you have the means, you're welcome to donate at [buymeacoffee.com/zofrea](https://buymeacoffee.com/zofrea). Thank you and enjoy the mod!

# Prepare to Co-op Edition

Play all of **Dark Souls: Prepare to Die Edition** together with up to three friends, from the Undead Asylum to the Kiln, without the limits of the game's own co-op. Guests keep their own body and progress, deaths respawn at a bonfire without bringing back the enemies the team has killed, a player who falls during a boss fight or an invasion spectates the others until the team wins or wipes (a wipe respawns everyone together in a world that starts over), boss kills and area changes no longer end the session, multiplayer fog walls are gone, a bonfire rest resets everyone's world (never in the middle of a teammate's boss fight), progress made together carries home, and enemies scale with the number of players. Evil spirits can invade a session too.

It is inspired by Yui's **Seamless Co-op** for Dark Souls Remastered. Nothing of that mod is used: its behaviour was studied and written again for Prepare to Die Edition's engine, with the same items, rules and session behaviour wherever the engine allows.

**Prepare to Co-op includes the DS1 FPS Unlock.** Its `xinput1_3.dll` is the [DS1 FPS Unlock](https://github.com/zofrea1/DS1-FPS-Unlock-PTDE-Remastered) for Prepare to Die Edition plus the co-op, built into one file, and its `PTDE-FPS-Unlock.ini` holds the settings of both. If you have Prepare to Co-op, you have the FPS Unlock: do not install both. The co-op works with the FPS unlock on or off (`FPSUnlock`).

| Release | Includes |
| --- | --- |
| Prepare to Co-op Edition 1.3.0 | DS1 PTDE FPS Unlock 1.9.0 |
| Prepare to Co-op Edition 1.2.0 | DS1 PTDE FPS Unlock 1.8.0 |
| Prepare to Co-op Edition 1.1.0 | DS1 PTDE FPS Unlock 1.8.0 |
| Prepare to Co-op Edition 1.0.0 | DS1 PTDE FPS Unlock 1.7.0 |

## What's new in 1.2.0

- **Anyone can travel.** A guest's bonfire warp takes everyone along, to any bonfire the host's own warp could reach.
- **Your keys come with you** into the host's world, when the host has them too.
- **Keep your seat.** A player already sitting at a bonfire when a teammate's boss fight starts can still level up, attune, use the box, kindle or reverse hollowing.
- **The Darksign** takes everyone home like the Homeward Bone, at the cost of the host's souls and humanity.
- **YOUR TEAM DIED** after a team wipe, and **invaders can drink Estus** (up to 5 charges per invasion).
- **Fixes**: guests no longer fall through the world after following a warp, and another player's character no longer comes back intangible and without attacks after a respawn.

The full list is in the [1.2.0 release notes](release-notes/v1.2.0.md). 1.1.0, a major update, made the session one shared world with teamwork rules: boss fights and invasions won or lost together, deaths that keep the world, Crimson Blossoms ([1.1.0 release notes](release-notes/v1.1.0.md)).

## Requirements

- Dark Souls: Prepare to Die Edition, the current Steam build (the same check as the FPS Unlock's).
- Steam running. While Prepare to Co-op is on, the game runs as Steam's Spacewar (app 480): Prepare to Die Edition's own Steam lobbies were switched off in 2022, and Spacewar is the long-standing community route for its multiplayer. Steam shows you as playing Spacewar.
- Everyone in a session uses the same `Password`. Prepare to Co-op 1.0.0, 1.1.0 and 1.2.0 play together, but the team rules (spectating, wipes, the rest lock, deaths that keep the world) need everyone on 1.1.0 or later, and guest travel and carried keys need the host on 1.2.0.
- Works with DSfix (set `unlockFPS 0` in `DSfix.ini` if the FPS unlock is on).

## Install

1. From the zip on the [Releases](https://github.com/zofrea1/DS1-Prepare-to-Coop-PTDE/releases) page, copy `xinput1_3.dll` and `PTDE-FPS-Unlock.ini` next to `DARKSOULS.exe` (`dxvk_d3d9.dll` too, only for the FPS Unlock's experimental DXVK). If the FPS Unlock or an older Prepare to Co-op is already installed, these replace its files; set your settings again in the new INI, which has all of them.
2. In `PTDE-FPS-Unlock.ini`, set the same `Password` as your friends. `PrepareToCoop = true` is already set.
3. Steam Deck and Linux: add the launch option `WINEDLLOVERRIDES="xinput1_3=n,b" %command%` so Proton loads the DLL (the same as for the FPS Unlock).
4. Your save: with Prepare to Co-op on, the game uses `DRAKS0005.co2` (`SaveFileExtension`) next to `DRAKS0005.sl2` in `Documents\NBGI\DarkSouls`. To play an existing character, copy `DRAKS0005.sl2` to `DRAKS0005.co2` before starting. Back up your saves.

The title screen reads PREPARE TO CO-OP EDITION while the co-op is on.

## How to play

Rest at any bonfire: each rest gives you every co-op item you do not carry.

| Item | Use |
| --- | --- |
| Blessed Eye Orb | Host a session. Players with your password can join at any time, wherever you are. |
| Crystal Eye Orb | Join the session of a player with your password. You arrive at the host's last bonfire, in your own body. |
| Chaos Eye Orb | Invade a session that allows invaders, as an evil spirit. Defeat every co-op player for a Humanity each; fall, and each of them earns one. |
| Abyssal Eye Orb | A guest returns home; the host disbands the session. |
| Ominous Tome | Host: change the world rules (allies cannot harm each other, allies can harm each other, the host against the guests). |
| Dried Fingers | Host: taunt invaders into your world: an invader looking for a world is 50% more likely to pick yours. |
| Cursed Pendant | Curse your own world (pure black World Tendency), alone or as host: black phantoms appear in every area that has them and stay until you use the pendant again, and the NPC invaders (Kirk, Leeroy, Mildred, Jeremiah) come even while you are hollow, whatever your covenant. The curse lasts through sessions; it is not saved, so after restarting the game use the pendant again. |
| Crimson Blossom | Not used: one blooms with each death in a session (up to five), each lowering maximum HP by 10% (`DeathDebuffs`). They wither when anyone in the session rests at a bonfire, and the health they took comes back with the bar; a team wipe withers them too. |
| Parchment of Deliverance | Pay a soul level's worth of souls to revive certain NPCs lost while visiting other worlds. |

The items are used like any other item. Keyboard shortcuts do the same (`CoopHotkeys`): **Ctrl+F1** host, **Ctrl+F2** join, **Ctrl+F3** leave, **Ctrl+F4** invade. Another player's floating health bar shows their character's name with their soul level and ping (`OverheadPlayerDisplay`): everyone sees everyone's, invaders included, except a player under Chameleon, who has none. Joins, departures, deaths and rule changes are told at the bottom of the screen, by character name. If a player's connection drops (a crash, a lost network, a Steam Deck put to sleep), the others are told within seconds and that player is sent home.

Prepare to Co-op 1.0.0, 1.1.0 and 1.2.0 use the same co-op protocol, so their players join and invade each other (each version's new features work between players who have it). When a later version has to change the protocol, players who cannot play together are told so instead of "No sessions found": three times at the bottom of the screen, with the versions and who needs to update. A host is told the same when such a player tries to join (from 1.1.0 on; Prepare to Co-op 1.0.0 itself only says "Failed to connect").

## Playing together

**Guests and the host's world.** Guests keep their own body (human or hollow), level, items and progress. They pick up items, get their own enemy drops and boss rewards, rest at bonfires, level up and talk to NPCs, who are real characters for them, not wandering ghosts. Defeating a boss or crossing into another area no longer ends the session, joining is never locked, and players roam the host's world freely: the white fog walls that kept phantoms in the host's area are gone (boss fog stays). Every player's game runs the world's scripted events (`GuestWorldEvents`), so a guest exploring alone meets the Hellkite on the Undead Burg bridge, starts boss fights, rides lifts and gets the event rewards.

**Travel.** Anyone can warp by bonfire, and everyone travels together to the same bonfire. A guest can warp to the bonfires the host's own warp could reach: the host owns the Lordvessel and has rested there. A player with a menu open follows once it is closed, and a player who has fallen follows once back on their feet. Nobody travels while a teammate who still stands fights a boss, a teammate is down, an invader is in the world, or the world is starting over after a lost boss fight. When the host uses a Homeward Bone, the host goes to their last bonfire and every guest follows a moment later; it takes one Homeward Bone from every co-op player, so it can be used only when everyone holds one, and as in single player it works in a boss fight, taking everyone out of it. The host's Homeward miracle does the same whenever the bone could be used, and costs nobody a bone. The Darksign does the same, at the cost of everyone's souls and humanity, and it can always be used: in a boss fight, while a teammate is down, and during an invasion, where it sends the invaders home first. A guest who has fallen in a boss fight respawns at once and then follows. A guest on Prepare to Co-op 1.2.0 or older neither holds the bone back nor pays one, and loses no souls or humanity to the Darksign. None of these is a rest: the enemies stay as they were, as in single player. Guests cannot use Homeward Bones, the Darksign or the Homeward miracle.

**Bonfires.** Lighting a bonfire for the first time lights it for every player. When anyone rests, every player's world resets (enemies respawn), and a teammate who fell and is spectating respawns. Nobody can rest while a teammate who still stands fights a boss (the bonfire is out of use, as with enemies nearby), so a rest never resets a fight in progress. A player who sat down before the fight began stays seated and can still level up, attune, use the Bottomless Box, kindle or reverse hollowing; only the warp waits for the fight. The others see you sit down, rest, kindle, reverse hollowing and stand up (`BonfireAnimations`).

**Deaths.** You respawn at your last bonfire in the host's world (the host's, until you rest at one yourself). The session goes on, the host's death included, and every death is announced. A death keeps the world as it is: the enemies the team has killed since the last rest stay dead in your reload, as they are for everyone else, host or guest. Each death in a session also costs a Crimson Blossom (see the items above): the price of a death that keeps the world. Whatever brings the enemies back takes the Blossoms away: anyone's rest withers everyone's (no need for each player to rest), and so does a team wipe or a death with no teammate in the world. A guest leaves their bloodstain where they fell, in the host's world, and finds the same stain at the same place back in their own.

**Boss fights and invasions are fought as one team.** Fall during a boss fight (yours, or a teammate's anywhere in the world) or while an invader is in the world, with another co-op player still standing, and you do not die yet: the camera follows a living teammate in full colour, turned with your own stick (D-pad left/right or the arrow keys switch between teammates). Your game draws the world around that teammate and updates their character every frame, so a teammate far away is seen in full detail and moves smoothly. While you spectate, your game never takes an enemy over from a standing player. Your death ("YOU DIED", the lost souls, the respawn) goes ahead once the fight is decided:

- **The team wins** (the boss is gone and no invader is left): the fallen respawn at their bonfires, and the boss stays dead.
- **Everyone falls** (a team wipe): the death text says "YOUR TEAM DIED", each respawn waits after it until every player's death has got there, then every game reloads at once into a world that starts over, the boss and the enemies included, as after a death in single player. Every other death keeps "YOU DIED".
- **Someone rests at a bonfire**: the spectators respawn at once. If the boss was still awake in a game (a lost fight), that game reloads at its bonfire together with the fallen, so the boss starts over for everyone. The reload waits until the player who rested leaves the bonfire menu, and that player is told that the others are waiting.
- **Ten minutes** pass, as a last resort.

**Invasions.** With `AllowInvaders`, others can invade as evil spirits. An invader arrives at the game's own invasion points (the Undead Asylum and Firelink Shrine included), the multiplayer walls come down for them, and invaders, the host and the guests all see each other's health bars (Chameleon hides the caster's). While an invader is in the world, bonfires are out of use (and, with `SealBossFogsDuringInvasions`, boss fog stays shut for the host and the guests), and the Abyssal Eye Orb, Homeward Bone, Humanity, Twin Humanities, Divine Blessing, Elizabeth's Mushroom and Fire Keeper Souls cannot be used. The host's Darksign still can: the invaders are sent home (nobody wins), and the host and every guest go to the host's last bonfire, each losing all souls and humanity. Nobody joins or invades while anyone in the world is in a boss fight, and after an invasion ends the world takes no other invader for `InvasionCooldown` minutes. An invader who cannot get into any world is told why: a world on cooldown (with how long until it opens), a boss fight, or a full world. The invader wins only when every co-op player is down; every invasion has exactly one outcome. Invaders in the same world fight as one team: an invader who falls while another still stands spectates them, and they all win or lose together (the fallen stay meanwhile, so no new invader takes their place). Invaders can drink from the Estus Flask, up to 5 charges per invasion: any more are set aside when they arrive and come back once they are home. With `InvasionRewards`, the winning side earns Humanity: the invader one per co-op player defeated (once home), or the host and every guest one each for every invader who falls, spectators included.

**Progress.** Event flags changed in the session's world are shared with everyone, and a guest's flags are written to their own world too: progress made together carries home, and key items picked up in another world always reach your own save. In another world your own key items stay at home (the game's own rule), so you cannot open what the host's world has not opened yet; you do carry the keys you have that the host has too, and a key the host gets while you are there joins you at once if you have it at home. An NPC's fate (Oscar's, for one) follows the host's world and carries home with you. A disaster of the host's world (the black phantoms of black World Tendency or the Cursed Pendant) stays there.

**World rules.** The Ominous Tome makes co-op allies hostile to each other (everyone fights everyone) or splits them into teams (the host against the guests); invaders are always hostile. The Cursed Pendant makes a world pure black World Tendency until it is used again, and Dried Fingers draws invaders to it.

**Enemies.** Every enemy gets more health, damage and poise per extra co-op player (`EnemyHealthScaling` ... `BossPostureScaling`); bosses (every enemy with a boss health bar) use their own row, and the game's own extra boss health for co-op is left out. Each game scales its own copy of an enemy, so health bars drain correctly with extra players. Each enemy is moved by one game, normally the one whose player is nearest and whose copy of it is awake; a game far away, a sleeping copy, and a player who fell defer to a standing player nearby.

**Light.** Another player's Skull Lantern, Cast Light or Sunlight Maggot also lights your game.

**Finding a world.** If a world cannot take you (full, busy, mid-boss, gone), the next one found is tried. A host on the title screen is hidden from searches. A guest cannot join a host who is dead or in a boss fight; they are told to try again when the fight is over.

[docs/PREPARE-TO-COOP.md](docs/PREPARE-TO-COOP.md) describes what changes in a session in detail, how it works and how it differs from the Remastered mod.

## Settings

In the `[PrepareToCoop]` section of `PTDE-FPS-Unlock.ini` (the FPS Unlock's settings follow it in the same file; see [its README](https://github.com/zofrea1/DS1-FPS-Unlock-PTDE-Remastered#readme)).

| Key | Default | What it does |
| --- | --- | --- |
| `PrepareToCoop` | `true` | The co-op on or off. Off, the DLL is the FPS Unlock alone and the game is as usual (Steam app, save file, title screen). |
| `Password` | empty | Players with the same password find each other. Avoid `#` (it starts a comment). |
| `AllowInvaders` | `true` | Others may invade your session as evil spirits. |
| `InvasionRewards` | `true` | The side that wins an invasion gets Humanity: the invader one per co-op player defeated (once home), or the host and every guest one each when the invader falls. Both sides need 1.1.0 or later. |
| `InvasionCooldown` | `3` | After an invasion of your world ends (every invader has won, fallen, left or been sent home), no other invader can come in for this many minutes: 0 (no cooldown) to 10, fractions allowed (1.5 = 90 seconds). An invader's search passes over a world on cooldown and tries the others; when none can take them, they are told how long until the first one opens. The host's setting decides for its world. Invaders of 1.2.0 or older are just turned away. |
| `SealBossFogsDuringInvasions` | `false` | `true`: while an invader is in your world, boss fog stays shut for you and your guests, so a fight with an invader cannot be ended by walking into a boss. `false`: the fogs work as in the unmodded game: you can go in, the invader cannot follow, and your entry sends the invader home. The host's setting decides for its world. |
| `DeathDebuffs` | `true` | Each death in a session adds a Crimson Blossom (up to five), each lowering maximum HP by 10% until anyone in the session rests at a bonfire (the health they took comes back) or the whole team falls. On by default since 1.1.0 (1.0.0's INI has it off). |
| `GuestWorldEvents` | `true` | Guests run the world's scripted events in their own game, as the host does (bosses, lifts, cutscenes, the Hellkite). |
| `BonfireAnimations` | `true` | The others see you sit, rest and stand at a bonfire. |
| `OverheadPlayerDisplay` | `4` | Another player's health bar: their character's name, with 0 nothing more, 1 ping, 2 soul level, 3 deaths, 4 soul level and ping. |
| `NetworkTickRate` | `30` | How many times a second every game of your session sends where its player is (and the enemies it moves): 6, the game's own, up to 30. Higher draws the others closer to where they really are, for a little more upload (about 2 KB a second to each other player at 30). On an uneven connection raise `NetworkSmoothing` rather than lowering this. The host's setting is used by the whole session (players of 1.2.0 or older still send 6). |
| `NetworkSmoothing` | `32` | Above 6 updates a second, how many ms other players' characters (and enemies other games move) stay behind their updates, as a margin for late ones: they are drawn about one update interval plus this behind, plus the ping (about 65 ms at 30 a second, 99 ms at 15; 6 a second stays the game's 200 ms). Each player's own, 0 to 200; raise it if others move in jerks. |
| `EnemyHealthScaling`, `EnemyDamageScaling`, `EnemyPostureScaling` | `40`, `0`, `25` | Enemy scaling per extra player, in percent. |
| `BossHealthScaling`, `BossDamageScaling`, `BossPostureScaling` | `115`, `0`, `40` | The same for bosses. |
| `SaveFileExtension` | `co2` | The save file while the co-op is on (vanilla: `sl2`). |
| `CoopHotkeys` | `true` | The Ctrl+F1..F4 shortcuts. |
| `CheckForUpdates` | `true` | At each start, look on the GitHub Releases page for a newer Prepare to Co-op, and say so on the title screen when there is one (until a character is loaded). One small request to api.github.com; `false` turns it off. |

## Notes and known limits

- Up to four players, the most Prepare to Die Edition's engine keeps in one world.
- **Online play: no guarantees, use at your own risk.** The mod changes the game in memory while it runs and runs the game under Spacewar. No responsibility is accepted for bans, lost progress or anything else that results from using it.
- Some cutscenes that move a player into another map have only been checked for the common ones (the Undead Asylum's crow, the Painted World).
- An NPC's death line can show only in the game of the player who killed them.
- An enemy killed in the last moment of a fallen player's load can come back in that player's game.
- Spectating a teammate far away: their character and their surroundings are drawn in full, but the world more than about 25 to 30 m from them can be missing.
- While spectating, lock-on only reaches enemies near where you fell, not near the teammate you watch.
- The first invasion of a world after an earlier invasion of it can fall through while connecting. The mod now knocks once more by itself ("The connection fell through. Trying again..."); in testing, a second try always got in.

## Logs and bug reports

Everything goes to the FPS Unlock's log, `PTDE-FPS-Unlock.log` next to the DLL; the co-op's lines start with `[coop]`. A crash writes `PTDE-FPS-Unlock-crash.dmp`. Send the logs of every player in the session, and any dump, with a bug report.

## Build

Visual Studio 2022 (MSVC). The FPS Unlock is a git submodule (`fps-unlock`, pinned to the release it ships with):

```
git clone --recursive https://github.com/zofrea1/DS1-Prepare-to-Coop-PTDE.git
cd DS1-Prepare-to-Coop-PTDE
build.bat
```

`build.bat` compiles the FPS Unlock's sources (`fps-unlock\src\ptde\sources.txt`) together with this repository's (`src\sources.txt`) into `build\xinput1_3.dll`, writes `build\PTDE-FPS-Unlock.ini` (`PrepareToCoop.ini` followed by the FPS Unlock's INI) and fetches DXVK through the FPS Unlock's script. The co-op plugs into the FPS Unlock's start-up through `fps-unlock\src\ptde\extension.h`; `src\extension.cpp` replaces its `extension_none.cpp`. `package_release.bat` makes the release zip. To move to a newer FPS Unlock, check out its release in `fps-unlock`, rebuild and commit the submodule.

`tools\build_title_movie.bat` remakes the title movie from the game's own and `assets\PrepareToCoop-Title.png`. It also needs the game's original title texture (the 512 x 512 `Title` texture in `menu/english/menu_local.tpf`, saved as a PNG) as `assets\Title-original-PTDE.png`; that file is the game's own and is not in the repository. `tools\embed_title.py` remakes `src\title_art.inc` from the PNG.

## Credits

- Yui, for Seamless Co-op for Dark Souls Remastered, the mod this one follows.
- The [DS1 FPS Unlock](https://github.com/zofrea1/DS1-FPS-Unlock-PTDE-Remastered) (MIT license), included; its own acknowledgements are in its README.
- [DXVK](https://github.com/doitsujin/dxvk) by Philip Rebohle and contributors (zlib/libpng license), included unmodified as `dxvk_d3d9.dll` for the FPS Unlock's optional DXVK.

## License

Prepare to Co-op Edition is free software under the GNU General Public License, version 3 or later ([LICENSE](LICENSE)), with an additional permission that allows it to be combined with the game and its libraries (see [NOTICE.md](NOTICE.md)). Anyone who distributes it, or a mod that includes it, must make the complete source of what they distribute available under the same license.

The FPS Unlock in `fps-unlock` keeps its own MIT license. The title logo and movie in `assets` and `src/title_art.inc` are derived from the game's own title screen, and the YOUR TEAM DIED lettering (`assets/TeamDied.png`, `src/coop/team_died_art.inc`) from the game's own letters (© Bandai Namco Entertainment, FromSoftware); they are not covered by either license and are included only for use with the game.
