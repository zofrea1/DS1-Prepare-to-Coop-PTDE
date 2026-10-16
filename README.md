## If my mod brought you some happiness, and you have the means, you're welcome to donate at [buymeacoffee.com/zofrea](https://buymeacoffee.com/zofrea). Thank you and enjoy the mod!

# Prepare to Co-op Edition

Play all of **Dark Souls: Prepare to Die Edition** together with up to three friends, from the Undead Asylum to the Kiln, without the limits of the game's own co-op. Guests keep their own body and progress, deaths respawn at a bonfire, boss kills and area changes no longer end the session, multiplayer fog walls are gone, a bonfire rest resets everyone's world, progress made together carries home, and enemies scale with the number of players. Evil spirits can invade a session too.

It is inspired by Yui's **Seamless Co-op** for Dark Souls Remastered. Nothing of that mod is used: its behaviour was studied and written again for Prepare to Die Edition's engine, with the same items, rules and session behaviour wherever the engine allows.

**Prepare to Co-op includes the DS1 FPS Unlock.** Its `xinput1_3.dll` is the [DS1 FPS Unlock](https://github.com/zofrea1/DS1-FPS-Unlock-PTDE-Remastered) for Prepare to Die Edition plus the co-op, built into one file, and its `PTDE-FPS-Unlock.ini` holds the settings of both. If you have Prepare to Co-op, you have the FPS Unlock: do not install both. The co-op works with the FPS unlock on or off (`FPSUnlock`).

| Release | Includes |
| --- | --- |
| Prepare to Co-op Edition 1.0.0 | DS1 PTDE FPS Unlock 1.7.0 |

## Requirements

- Dark Souls: Prepare to Die Edition, the current Steam build (the same check as the FPS Unlock's).
- Steam running. While Prepare to Co-op is on, the game runs as Steam's Spacewar (app 480): Prepare to Die Edition's own Steam lobbies were switched off in 2022, and Spacewar is the long-standing community route for its multiplayer. Steam shows you as playing Spacewar.
- Everyone in a session uses the same version of Prepare to Co-op and the same `Password`.
- Works with DSfix (set `unlockFPS 0` in `DSfix.ini` if the FPS unlock is on).

## Install

1. From the zip on the [Releases](https://github.com/zofrea1/DS1-Prepare-to-Coop-PTDE/releases) page, copy `xinput1_3.dll` and `PTDE-FPS-Unlock.ini` next to `DARKSOULS.exe` (`dxvk_d3d9.dll` too, only for the FPS Unlock's experimental DXVK). If the FPS Unlock is already installed, these replace its files; set your FPS Unlock settings again in the new INI, which has all of them.
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
| Chaos Eye Orb | Invade a session that allows invaders, as an evil spirit. |
| Abyssal Eye Orb | A guest returns home; the host disbands the session. |
| Ominous Tome | Host: change the world rules (allies cannot harm each other, allies can harm each other, the host against the guests). |
| Dried Fingers | Host: taunt invaders into your world. |
| Cursed Pendant | Host: curse your own world (pure black World Tendency). Use it again to lift the curse; the curse also ends when the session does. |
| Parchment of Deliverance | Pay a soul level's worth of souls to revive certain NPCs lost while visiting other worlds. |

The items are used like any other item. Keyboard shortcuts do the same (`CoopHotkeys`): **Ctrl+F1** host, **Ctrl+F2** join, **Ctrl+F3** leave, **Ctrl+F4** invade. Another player's floating health bar shows their soul level and ping (`OverheadPlayerDisplay`), and joins, departures, deaths and rule changes are told at the bottom of the screen. If a player's connection drops (a crash, a lost network, a Steam Deck put to sleep), the others are told within seconds and that player is sent home.

Everyone in a session needs the same version of Prepare to Co-op. When the only session found has another version, the game says so instead of "No sessions found": three times at the bottom of the screen, with the versions and who needs to update. A host is told the same when a player of another version tries to join (from 1.1.0 on; Prepare to Co-op 1.0.0 itself only says "Failed to connect").

[docs/PREPARE-TO-COOP.md](docs/PREPARE-TO-COOP.md) describes what changes in a session, how it works and how it differs from the Remastered mod.

## Settings

In the `[PrepareToCoop]` section of `PTDE-FPS-Unlock.ini` (the FPS Unlock's settings follow it in the same file; see [its README](https://github.com/zofrea1/DS1-FPS-Unlock-PTDE-Remastered#readme)).

| Key | Default | What it does |
| --- | --- | --- |
| `PrepareToCoop` | `true` | The co-op on or off. Off, the DLL is the FPS Unlock alone and the game is as usual (Steam app, save file, title screen). |
| `Password` | empty | Players with the same password find each other. Avoid `#` (it starts a comment). |
| `AllowInvaders` | `true` | Others may invade your session as evil spirits. |
| `DeathDebuffs` | `false` | Each death in a session lowers maximum HP by 10% (up to five), until the next bonfire rest. |
| `GuestWorldEvents` | `true` | Guests run the world's scripted events in their own game, as the host does (bosses, lifts, cutscenes, the Hellkite). |
| `BonfireAnimations` | `true` | The others see you sit, rest and stand at a bonfire. |
| `OverheadPlayerDisplay` | `4` | Another player's health bar: 0 name only, 1 ping, 2 soul level, 3 deaths, 4 soul level and ping. |
| `EnemyHealthScaling`, `EnemyDamageScaling`, `EnemyPostureScaling` | `35`, `0`, `15` | Enemy scaling per extra player, in percent. |
| `BossHealthScaling`, `BossDamageScaling`, `BossPostureScaling` | `100`, `0`, `20` | The same for bosses. |
| `SaveFileExtension` | `co2` | The save file while the co-op is on (vanilla: `sl2`). |
| `CoopHotkeys` | `true` | The Ctrl+F1..F4 shortcuts. |

## Notes and known limits

- Up to four players, the most Prepare to Die Edition's engine keeps in one world.
- **Online play: no guarantees, use at your own risk.** The mod changes the game in memory while it runs and runs the game under Spacewar. No responsibility is accepted for bans, lost progress or anything else that results from using it.
- Some cutscenes that move a player into another map have only been checked for the common ones (the Undead Asylum's crow, the Painted World).
- An NPC's death line can show only in the game of the player who killed them.

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

The FPS Unlock in `fps-unlock` keeps its own MIT license. The title logo and movie in `assets` and `src/title_art.inc` are derived from the game's own title screen (© Bandai Namco Entertainment, FromSoftware) and are not covered by either license; they are included only for use with the game.
