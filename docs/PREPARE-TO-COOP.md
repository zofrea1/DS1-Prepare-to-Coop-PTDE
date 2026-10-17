# Prepare to Co-op Edition

Play all of Dark Souls together with up to three friends, from the Undead Asylum to the Kiln, without the
limits of the game's own co-op. A mod for Dark Souls: Prepare to Die Edition built on the DS1 FPS Unlock,
which it includes: its `xinput1_3.dll` is the FPS Unlock plus the co-op, switched on by
`[PrepareToCoop] PrepareToCoop` in `PTDE-FPS-Unlock.ini`. The co-op works with the FPS unlock on or off.

It is inspired by Yui's **Seamless Co-op** mod for Dark Souls Remastered (`ds1sc.dll`, called ds1sc
below), which only exists for Remastered. Nothing of that mod is used: its behaviour was studied by reverse engineering and written again
for PTDE's engine. The goal of this first stage is a faithful port: the same items, the same rules, the same
session behaviour. Where PTDE's engine sets a hard limit (four players instead of six), the port keeps the
engine's limit.

**Status:** release candidate for 1.0.0. Tested with up to three players on two PCs and a Steam Deck:
joining, invading, dying, resting, bosses, NPCs, area changes, cutscene travel, leaving and lost connections.

## How to play

1. Everyone sets the same `Password` and `PrepareToCoop = true`. Keep the other settings the same too (the
   host's world rules apply, but enemy scaling is worked out on each PC).
2. Rest at any bonfire. Each rest gives every co-op item you do not carry (as ds1sc
   does), so an existing character copied to the co-op save gets them at its first rest.

   | Item | Use | Usable |
   | --- | --- | --- |
   | Blessed Eye Orb | Create a co-operative session. Players with your password can join at any time, wherever you are. | outside a session |
   | Crystal Eye Orb | Join the session of a player with your password. You arrive at the host's last bonfire. | outside a session |
   | Chaos Eye Orb | Invade a co-operative session that allows invaders, as an evil spirit. | outside a session |
   | Abyssal Eye Orb | A guest returns home; the host disbands the session. | in a session, no invader present |
   | Ominous Tome | Host: change the world rules (none, allies hostile, allies in teams; each use moves on). | host |
   | Dried Fingers | Host: taunt invaders; an invader searching for a world is 50% more likely to pick yours. | host |
   | Cursed Pendant | Curse your own world: it counts as pure black World Tendency until you use the pendant again. | your own world: alone or as host |
   | Crimson Blossom | Not used: with `DeathDebuffs`, one blooms with each death in a session (up to five), each lowering maximum HP by 10%. They wither at a bonfire. | never |
   | Parchment of Deliverance | Pay a soul level's worth of souls to revive certain NPCs lost while visiting other worlds, then return to your last bonfire. Asks first. | outside a session |

   The items are used like any other item (quick slot, the item animation); when one cannot be used it is
   refused the way the game refuses any item. Keyboard shortcuts do the same (`CoopHotkeys`): **Ctrl+F1**
   host, **Ctrl+F2** join, **Ctrl+F3** leave, **Ctrl+F4** invade. Each press is written to the log.
3. Another player's floating health bar shows their character's name with their soul level and ping, or
   ping, soul level or deaths alone (`OverheadPlayerDisplay`). Joins, departures, deaths and changes to the
   world rules are told at the bottom of the screen, with character names (the game's own records of other
   players hold their Steam names).

**Your save.** With Prepare to Co-op on, the game uses `DRAKS0005.co2` (`SaveFileExtension`) next to
`DRAKS0005.sl2` in `Documents\NBGI\DarkSouls`. To play an existing character, copy `DRAKS0005.sl2` to
`DRAKS0005.co2` before starting. Vanilla play keeps using `.sl2`.

**Steam.** The game runs as Steam's Spacewar (app 480) while Prepare to Co-op is on. PTDE's own app (211420)
has had its Steam lobbies switched off since 2022, and Spacewar is the long-standing community route for PTDE
multiplayer. The game's own online features (messages, bloodstains, signs, vanilla invasions, the ghosts of
other players) still work, but only between players with the same password. ds1sc has no
such features (Remastered's online play needs FromSoftware's servers, which that mod replaces).

**Title screen.** While Prepare to Co-op is on, the title screen reads PREPARE TO CO-OP EDITION instead of
PREPARE TO DIE EDITION. The logo is the game's 512 x 512 `Title` texture (`menu/english/menu_local.tpf`) with new
edition lines, built into the mod from `assets/PrepareToCoop-Title.png` and swapped in as the game creates the
texture (`D3DXCreateTextureFromFileInMemory(Ex)`). To try other art, save it as `PrepareToCoop-Title.png` (or
`.dds`) next to `DARKSOULS.exe`: a 512 x 512 RGBA PNG on solid black. The game cuts "DARK SOULS" from three
pieces of it, (0,0)-(512,170), (0,170)-(512,340) and (0,340)-(155,510); leave those as they are. The edition
text lives in (155,340)-(512,510), and the original's lines fill (178,343)-(505,433), the safest fit. The
original text is about RGB 222, 203, 189. After changing the built-in PNG, run
`tools/embed_title.py` and rebuild.

Before the logo, the title screen plays `movWW/frpg_title.wmv`, which zooms the logo in and fades the edition line
in under it, ending on the texture's picture. The mod plays `PrepareToCoop-Title.wmv` in its place: the same movie
with the new edition line. Only the second line is repainted, moved, blurred and faded frame by frame the way the
original line is, and the movie ends on exactly the picture the menu then draws. The built-in copy
(`assets/PrepareToCoop-Title.wmv`) is written to the temp folder at start-up; a `PrepareToCoop-Title.wmv`
next to `DARKSOULS.exe` is used instead if there is one. `tools/build_title_movie.bat "<game>\DATA\movWW\frpg_title.wmv"`
makes it again from `assets/PrepareToCoop-Title.png` (Python with numpy, scipy, Pillow and imageio-ffmpeg);
it is encoded as WMV9 with Windows' own encoder, like the original, then built in by `build.bat`.

## What changes in a session

| Rule | Prepare to Co-op (as in ds1sc) |
| --- | --- |
| Guests | Keep their own body (human or hollow), level, items and progress. They pick up items, get their own enemy drops and boss rewards, rest at bonfires, level up and talk to NPCs. |
| Joining | A guest arrives at the host's last bonfire (where the host last rested). |
| Deaths | You respawn at your last bonfire in the host's world (the host's, until you rest at one yourself). The session goes on, the host's death included. Every death is announced. After any load (a death, bonfire travel) each player's game builds the other players' characters again, and a player who died is rebuilt for the others as soon as they are back on their feet. |
| Messages | Joins, departures, deaths and the like appear in the box at the bottom of the screen that the game uses for "Phantom X summoned", and fade by themselves. The game's own summon texts and its "phantom returned home" / "sent home" boxes, which wait for a button, are not shown during a session. |
| Bosses and areas | Defeating a boss or crossing into another area no longer ends the session. Joining is never locked. Players roam the host's world freely. |
| Fog walls | The white walls that keep phantoms in the host's area are gone (boss fog stays). |
| World events | Every player's game runs the world's scripted events, not only the host's (`GuestWorldEvents`): a guest exploring alone meets the Hellkite on the Undead Burg bridge, starts boss fights, rides lifts and gets the event rewards. Progress (event flags) is shared. |
| Host travel | When the host warps by bonfire, the guests follow to the same bonfire. A guest's own bonfire warp does nothing. When the host uses a Homeward Bone, the host goes to their last bonfire and every guest follows a moment later ("The power of the homeward bone consumes you..."). Guests cannot use Homeward Bones. |
| Bonfires | Lighting a bonfire for the first time lights it for every player. When anyone rests, every player's world resets (enemies respawn). The others see you sit down, rest, kindle, reverse hollowing and stand up (`BonfireAnimations`). |
| Bloodstains | A guest who dies leaves their bloodstain where they fell, in the host's world, and finds the same stain at the same place when they are back in their own. |
| Progress | Event flags changed in the session's world are shared with everyone, and a guest's flags are written to their own world too: progress made together carries home. A disaster of the host's world (the black phantoms of black World Tendency or the Cursed Pendant) stays there. |
| World rules | The Ominous Tome makes co-op allies hostile to each other (everyone fights everyone) or splits them into teams (the host against the guests); invaders are always hostile. The Cursed Pendant makes the host's world pure black World Tendency (a player can also curse their own world without a session), and lifts the curse when used again: every map's disaster flag is cleared and the black phantoms of the current map die, as when the game's own events end a disaster. Each area with black phantoms starts its own disaster when a player enters it; the game checks every 600 s whether the world is still black and ends the disaster only if not, so in a cursed world the phantoms stay. The curse lasts through sessions but is not saved: after the game is restarted, or another character is loaded, the world is no longer cursed and disasters left from the curse end; otherwise white tendency events never happen and black ones only in a cursed world (with Prepare to Co-op on, world tendency is replaced in every world, as in ds1sc). |
| Spectating | Bosses and invaders are fought as one team. Falling during a boss fight or while an invader is in the world, with another co-op player (host or guest) still standing, you do not die yet: after your fall the camera follows a living teammate (D-pad left/right or the arrow keys switch between them). Your death ("YOU DIED", the lost souls, the respawn) goes ahead once the fight is decided: the boss is gone and no invader is left, every co-op player has fallen, someone rests at a bonfire, or after ten minutes. |
| Invasions | With `AllowInvaders`, others can invade as evil spirits. While an invader is in the world, bonfires are out of use, the Abyssal Eye Orb, Homeward Bone, Humanity, Twin Humanities, Divine Blessing, Elizabeth's Mushroom and Fire Keeper Souls cannot be used. Boss fogs stay shut for the host and the guests while an invader is in the world, and no invader joins while anyone in the world is in a boss fight (the invader tries another world; such a world is also hidden from invaders' searches). An invader arrives at the game's own black invasion point for the play region the host stands in, else at the nearest one within 50 m, else on the host (Firelink Shrine and the Undead Asylum have none; invasions are allowed there). The invader wins when every co-op player has fallen (the host's death alone does not end an invasion while a guest stands; with a 1.0.0 invader it still does); when the invader dies the invasion fails. |
| Enemy scaling | Every enemy gets more health, damage and poise per extra co-op player (`EnemyHealthScaling` ... `BossPostureScaling`, percent per player); bosses are the characters on team 7. |
| Shared light | Another player's Skull Lantern, Cast Light or Sunlight Maggot also lights your game. |

## How it works

### The base: the game's own session (the same base as ds1sc)

Reverse engineering `ds1sc.dll` shows that ds1sc does not replace the game's multiplayer
with its own: it builds the guest's presence out of Remastered's own session code. It creates the game's own
connection objects for each peer and feeds the game's session manager with synthetic session events; the
game's own netcode (player positions, enemies, objects, damage, event synchronisation) then runs as usual. It
replaces only the transport underneath (packet send, receive and pop by type), with Steam's
`ISteamNetworkingMessages`, because Remastered's sessions are brokered by FromSoftware's servers. A guest loads
the host's world as "someone else's world", exactly like a summoned phantom: its replacement of the game's
"own world" setter (`GameMan+0xB7F`, the twin of PTDE's `GameMan+0xB4F`) answers "someone else's" for guests,
and the event flags of that world are the host's.

PTDE's sessions are pure Steam lobbies and Steam peer-to-peer, which still work under Spacewar, so the port
needs no transport of its own for the game's traffic: the guest joins through the game's own session and
summon code and the game's own netcode runs over the game's own Steam channels. The mod's own messages (the
equivalent of ds1sc's packets `0x0B`-`0x1F`) travel on a private channel.

So the base is the same in both: the game's session, the guest in the host's world, the host's game in charge
of the world, and the mod's rules and synchronisation layered on top. Every system ds1sc
synchronises sits on this layer and can be ported:

| ds1sc feature | How it works there | PTDE port |
| --- | --- | --- |
| Enemy sync | the game's own session netcode | the game's own session netcode |
| World state (objects, doors, levers) | the game's own netcode plus event flags | the same |
| Event flags / progress | packets `0x0C` (guest to host) and `0x0B` (host to all) | flags relay (guest to host to all) |
| Activated bonfires | `Util_RequestLevelUpFirst` mirrored (`0x16`) | the same function mirrored |
| Bonfire rests | world reset mirrored (`0x14`/`0x15`) | the same |
| Host bonfire warp / Homeward Bone | `WarpNextStage_Bonfire` mirrored (`0x17`), host bonfire (`0x18`) | the same |
| Fog walls | multiplayer walls removed | the same |
| NPC quest lines | event flags shared; NPC flag repair (Parchment) | the same |
| Items from bosses and NPCs | each player gets their own (guest checks lifted), flags shared | the same |
| World rules, tendency, teams | own hooks on the team and tendency functions | the same functions in PTDE; teams written on the characters (PTDE inlines the team lookup) |
| World events for every player | event condition `EcTestMultiplayState`: "host" passes, "client" fails, two event lists | the same function in PTDE |
| Players after a load or a death | (part of its session emulation) | members put back on the game's character waiting list; dead copies rebuilt (in an invader's game too) |
| Spectating | the death event's dispatch held while a teammate lives (`0x1404A7740`), the HUD gauges retargeted | the same dispatch (`0xD66130` from the Lua HP test `0xD53B70`); the camera retargeted instead of the HUD |
| Six players | Remastered's six-player sessions plus patches for its character and object lists | **not possible without rebuilding PTDE's session and world limits (three remote players)** |

The one hard limit found is the player count. Everything else is a matter of porting more of the mod's rules,
not of changing the base.

### Finding and joining

The mod keeps an invisible Steam lobby (`ISteamMatchmaking`, `SteamMatchMaking009`) tagged with a hash of the
password, and talks to the other players on a private peer-to-peer channel (`ISteamNetworking`, channel 9;
the game itself uses channels 0 and 1).

1. The host's game opens a session the way touching a white sign does (`CreateSession`, `0xD2D4F0`). The
   room's empty-room timer and lock are kept off.
2. A guest finds the lobby, asks the host (`Hello`), and the host's game builds the same invitation it sends a
   summoned phantom (join type, flag and the session's `NRSessionSearchResult`, code at `0xDB44EE`).
3. The guest deserialises it and joins with the game's own guest code path (`JoinSession`, `0xD2D320`, as at
   `0xDB47A4`). The arrival point is where the host last rested at a bonfire. The game then loads the guest
   into the host's world ("other world": separate event-flag store, own world saved for the return).
4. On arrival the game would turn the guest into a white phantom (`0xEA15B0`); the guest keeps their own
   chr type instead.

Players need the same co-op protocol: 1.0.0 and 1.1.0 both speak 8. Lobbies and hellos also carry the Prepare
to Co-op version, which a later release can compare; when the protocol changes, both sides are told who needs
to update.

The Steam lobby has room for 16 members, while the game keeps four players (the host and three others, guests
and invaders together), which the host enforces when it answers. Players of other games that use Spacewar, or
knocks that never get through, can sit in a lobby but cannot fill it. Steam lobbies cannot remove a member.

An invader picks at random among the worlds that allow invaders. Dried Fingers make a world 1.5 times as likely
to be picked. Hosts from 1.1.0 on publish them as `ptdesc_fingers`: 1.0.0 invaders go to a `ptdesc_taunt` world
before any other, so with few players one host would draw all of them.

When the world picked cannot take the player (full, invaders refused there, busy for too long, gone, or no answer),
the next world the search found is tried, up to five worlds, with "Trying another world..." on screen. A host who
has had no character for 10 s (the title screen) is hidden from searches until back in the world: Steam does not
list a lobby that cannot be joined, for any version of the mod.

### Keeping the session

`FrpgSessionManagerImp`'s leave (`0xD2B460`) is called by many rules. The mod checks who calls it: the rule
leaves are skipped (boss kills via Lua `LeaveSession`, `WarpNextStageKick` from someone else's world, the end
of the in-game step on every load, the idle timer, the empty-room timeout, the host walking into another map
block, a load with fewer than two members, event restart warps), the party reset after three of them too
(`0xEA1CB0`); network failures, invasions and an intended leave still run. Lua `LockSession` does nothing.
The host's death would broadcast `LuaCallStart(4000, 1)`, which sends every guest home; it is not sent (an
invader gets the victory from the mod instead, once every co-op player has fallen). A guest's later loads (death, following the host) are marked
as loads into someone else's world while `MoveMapStep` (`0xEFC470`) decides which world to load.

### Spectating

The global event script registers `OnCharacterDead(99999, LOCAL_PLAYER, 'OnEvent_4000', everytime)`: the
Lua HP test (`0xD53B70`) fires it every frame while the local player is dead, through one call to the event
dispatch (`0xD66130`, stdcall kind/event/entity/extra with `esi` = the event system; kind `0xE` = dead), and
`OnEvent_4000` ignores the repeats with flag 4000. Remastered Seamless Co-op holds the same dispatch
(`0x1404A7740` from `0x1404CFAF0`) while a teammate lives during a boss fight (a boss gauge shown) or an
invasion, and lets it through once nobody is left; the game's own death sequence ("YOU DIED", the bloodstain,
the warp) then runs as usual. Prepare to Co-op does the same at `0xD53CF0` and also releases it when the fight
is over, someone rests, the session ends or after ten minutes. Where Remastered points the HUD's HP and stamina
gauges at the spectated player (a fake `GameDataMan` for the gauge code), PTDE's camera is retargeted: the
in-game step (`0xEFD2F0`) hands ChrCam's update (`0xF14320`) the debug "player" (`[0x137D648]+0xEC`, read by
the menus and the pad too, so not usable) or else the local player (`0xEFDB1F`); the teammate's character is
handed over there instead, and the death camera's flag (`WorldChrMan+0x40`, set at `0xE8332E` when the local
player dies) reads clear for that character only (`0xF1457A`). A boss gauge is EMEVD's (`2003[11]`): the
Asylum Demon's goes off when the boss dies or when the local player leaves the arena, and a dead body stays.

### Invasions: arrival, boss fogs

The maps' MSBs hold one black invasion point per play region: a region named `黒侵入ポイント` with entity id
map base + 4000 + the play region's number (the Undead Burg's play regions 101000 ... 101210, the collisions'
`PlayRegionID`, have points 1014000 ... 1014210; seventeen maps checked, the range holds nothing else; Firelink
Shrine, the Undead Asylum and Ash Lake have none). The game keeps the player's play region at `PlayerIns+0x284`
and turns it into the point's id the same way (`0xD6BB50`: id / 1000 * 9000 + 4000 + id). The host picks the
point of its own play region from the loaded maps' regions (`WorldPointManImp`, `[0x137D6E8]`, as `0xD6C490`
looks one up), else the nearest within 50 m, else its own position, and sends it in its answer as the invader's
arrival (the game's summon position, `GameMan+0xA80`/`+0xAB0`, map `+0xAA4`).

A boss fog's entry is an EMEVD "Action Button - Boss Room" (`3[19]`). Its test (`EcTestActionButton`, `0xBBD7E0`)
greys the button out for a Gravelord or Path of the Dragon phantom in the party (invade types 7 and 10) or state
effect 185; any other invader leaves it open, cannot follow (the fog takes living players and white phantoms),
and is sent home by the host's entry (`2009[06]` Issue Boss Room Entry Notification: Lua 4068 `ReportBossArea`
-> `Failed_BossAreaMission` on the invader, `0xEA2C80` drops black phantoms from the party). Prepare to Co-op adds
"an invader is in the world" to those reasons at `0xBBD86C`. Each player reports whether a boss gauge is shown in
their game (an addition behind the status message that older versions ignore); while one is, the host answers
invaders "refused" and its lobby's `ptdesc_invade` reads `0`.

### When a player leaves

When a member's character is taken out of the party (`0xEA2A10`), the game looks at that member's summon type
(`0xD79380`, remote player data `+0x110`). Type 0 is the master of the world: the game shows "Disconnected
from master of world", requests a save and arms the event script's `OnLeave_Limit`, which five seconds later
sets the sign warp and sends the player back to where they were summoned from. A co-op guest keeps their own
body and has no summon type, so a host took every departing guest for the master and was warped to a summon
point that never existed: the title screen, and a save without a floor under it. The return home is no longer
armed in one's own world (nobody else is the master there) nor for a co-op guest (the session takes them
home), and `SetSosSignWarp` (`0xD5E070`) is refused in one's own world.

The host's Abyssal Eye Orb disbands the way the game ends a session after the Black Separation Crystal: the
guests are told and leave, the game's own member-left code takes their characters out of the party, and when
the host is the last member the game's own leave (`0xD2B460`) closes the session.

A player who drops off without leaving (a crash, a lost network, a Steam Deck put to sleep) is noticed by the
silence: every player sends its status each second. With nothing heard for 10 s after Steam reports the link
lost, or for 60 s in any case, the host shows "{name} lost the connection", tells the other players and stops
sending to them; a guest or invader who stops hearing the host goes home ("Lost the connection to the host").
The game itself takes the lost player's character out when Steam gives up on the link. A guest or invader who
comes back from more than a minute of sleep goes home at once (measured by the wall clock: under Proton the
game's tick count stops while a Steam Deck sleeps); after a shorter one, the host answers anything they send
with "you were dropped" and they go home. The host answers pings only from players in the session, and a pong
alone does not count as hearing from the host. Time in which a game was not running at all is not counted as
anyone's silence.

When a player leaves the lobby without a Bye (an invader the game sends home after their death), the host tells
the guests as well. The Ominous Tome, Dried Fingers and Cursed Pendant say what they did, to the host and to the
guests.

### A guest's bonfire menu

The bonfire talk (`t<map>0<nn>.esd`, the same script at every bonfire) builds its menu with
`AddTalkListData(row, text, flag)` (talk command 1:19, `0xFCF69A`): a row is only added when its flag is on.
Level Up, Kindle and Reverse Hollowing need `11810000` (set when the player leaves the Undead Asylum),
Reinforce Weapon, Reinforce Armor and Repair need `250`, `251` and `252` (the smithboxes and the Repair Box),
Attune Magic `719` and the Bottomless Box `258`; kindling past the second level asks for `257` (Rite of
Kindling) through the talk script's GetEventFlag (function 15, `0xD495EC`). These flags describe the
character, but a guest's active flag store is the host's world, so the rows were missing or followed what the
host owned. When the talk script reads one of them (return addresses `0xFCF7D6` and `0xD4966C` of the flag
getter `0xD38C40`), a guest gets the value from their own world. Nothing else reads it from there: the world's
own events in the host's world (the Asylum's, for example) keep the host's state.

Kindling and Reverse Hollowing signal through flags: the menu sets `760` while kindling and `762`/`764` during
Reverse Hollowing, and common event 763 plays the Reverse Hollowing ceremony and then the sitting loop on the
local player whenever `762` is set. Shared progress had sent those to the other players, so a guest's Reverse
Hollowing sat the host down wherever they were. Flags that describe one player are no longer shared: `250` to
`258` (owned boxes, Rite of Kindling), `702`, `706`, `710` (Lordvessel: bonfire warp), `715`, `717`, `719`, `740`
(class), and `750` to `765` except `756` (curse and parasite, Oswald's absolution and curse removal, status
messages, the bonfire ceremonies). Before, a guest's Lordvessel gave the host bonfire warp, and a host's
Bottomless Box was written into the guest's own world.

A guest keeps the chr type they joined with, and every arrival in the host's world puts it back. Reverse
Hollowing in the host's world (hollow to human) now updates that type, so the next death or bonfire warp, and
the trip home, keep the guest human.

### Bloodstains

The death penalty (`0xD78B10`, the event script's `UpDateBloodMark`) records the stain at the start of the
player's death replay (`PlayerIns+0x5E0`) in their own world, and at the place they were summoned from
(`GameMan+0xAE0`) in someone else's. A co-op guest was never summoned from anywhere. As in ds1sc, that test answers "own world" for a co-op guest, so the stain is where they fell. Creating the marker
(`0xDB6010`) and picking it up (`FrpgEventConditionSelfBlood`, `0xD5B330`) do not depend on whose world it is.

### Bonfire animations

Sitting, the sitting loop, standing up, kindling and the bonfire menu's ceremonies are event animations
(`0xE69590` starts one on a character, `0xE69470` stops it), which the player samples do not carry (they
carry the character's state machine ids). ds1sc watches the start function on the local
player and, for nineteen bonfire animations, has every other game call it on that player's character. The
port does the same and passes on the stops as well: PTDE's event script stops the sitting loop before it
plays the standing up, and the bonfire menu stops and starts animations of its own. Each player also reports
the loop they are in once a second, so a character that missed a message (it was not built yet, or its player
was already sitting) is put right.

### Messages

The mod's lines go to the action box (`MenuMan+0xEC0`, two channels of three lines, queued by `0x515B80` and
shown with `MenuMan+0xF4`, as Lua `RecallMenuEvent` does). `RecallMenuEvent`'s body (`0xD74950`, also used by
the game's own summon notices in `0xEA2510`) drops ds1sc's list of summon texts (140013,
140014, 140023, 140024, 140030, 140031, 140033, 140034, 140153, 140154, 140192) during a session. The game's
information box (`0xD4C090` opens it, `0xD4C190`/`0xD4C290` add a line) is a modal list; during a session (and
for a minute after) it opens only when its first line arrives, and the notices about phantoms going home
(20000415/416/420/421/425/435/450, 20001001/1003/1004/1020) are dropped with their box.

### Other players' characters

PTDE builds a session member's character once: the member goes on the session's waiting list
(`SessMan+0x118`, pushed by `0xD2AFD0`) and the in-game step (`0xEE2B10` via `0xD2A360`) builds the character
when the member's player data is complete (`GameDataMan+0x10[slot] == 0x1F`). A map load throws every
character away and nobody goes on the list again, which is why players lost sight of each other after a
death. Every second the mod looks up each member's character (`0xE3D930`; members and slots in the list at
`SessMan+0x12C`, character name in the remote player data at `+0xA0`): a missing one is put back on the waiting
list, and a dead one whose player reports being alive again is removed the way the member-left handler does
(`0xEA2A10`, `0xE3E600`) and built again. The player messages carry the character name to match players to
characters.

### World events

Map events test the session through `EcTestMultiplayState::Test` (`0xBB3130`; the SKIP/END IF instructions call
it directly from `0xBB3200`). The game's co-op ends most events on clients, so only the host's game ran them.
As in ds1sc, during a session "host" passes and "client" fails for every event, so each
game runs the world's events; events 11005844, 11015846, ... 11810150 (the Asylum crow) fail every
multiplayer test, and 11020301, 11200005, 11200006, 11305000, 11510230, 11600200 act as in single player. The
event id is `FrpgEventIns+0x28` (constructor `0xBC4150`). `GuestWorldEvents = false` restores the game's rule.

### Items

| What | Where |
| --- | --- |
| Rows | extra `EquipParamGoods` rows 389000-389008 from the goods param getter (`0xD1E3A0`): ds1sc's rows field for field (refId -100..-93, icons, the eye orb animation, the Parchment's yes/no dialog), with the flag bytes translated (Remastered inserted `enable_pvp` into the bit fields) |
| Can it be used? | the game's item check (`0xEADCA0`, cdecl) answered by the session rules for these ids, Homeward Bone, Black Eye Orb, Humanity, Twin Humanities, Divine Blessing, Elizabeth's Mushroom and Fire Keeper Souls |
| Effect | `PlayerIns::UseGoods` (vtable slot 128) |
| Names | the game's FMG lookup (`0xD175E0`); the Parchment dialog (event text 93129700) shows the soul cost |
| Given / removed | Lua `AddInventoryItem` (`0xD664C0`) at each rest; `RemoveInventoryEquip`'s core (`0xD6D670`) for the Crimson Blossoms |

### Rules for guests

| What | Where |
| --- | --- |
| Enemy and object drops, scripted reward items | inlined guest checks in `0xDC80A0`, `0xDC8370`, `0xDC9170` |
| Bonfires and NPC talk | `HasPartyMembers` (`0xD2A4B0`) at the bonfire object (`0xD58AE4`) and talk script function 0x34 (`0xD4ABFB`) |
| Multiplayer walls | Lua `GetMultiWallNum` (`0xD5DC40`) answers 0 in every co-op role: `OnEvent_4034` (`OnSessionJustIn`) raises the walls on the host's side (`IsClient()` false), which every player then meets |
| Progress | the event-flag setter (`0xD38CB0`): changes are shared, and a guest's are also written to their own world's store, except the maps' disaster flags |

### World rules

- **Rest:** Lua `Util_RequestRegene` (`0xD5D820`, the bonfire rest event) first gives the items and withers the
  blossoms, then tells the others, who reset their world the way a rest does (`WorldChrMan+0x2CC`).
- **Bonfires:** `WarpNextStage_Bonfire` (`0xD62CA0`) and `Util_RequestLevelUpFirst` (`0xD5D850`) are mirrored.
- **Teams:** who may hit whom comes from the team relation table (`0x12DF268`, 19 x 19, row attacker,
  column target) and each character's team (`chr+0x74`), mostly read inline, so the mod writes the team on the
  players' characters every frame (and answers the getter `0xE68C80` the same way). ds1sc
  uses 0xF (allies hostile), 0x12 (teams) and 0x10 (invaders); Remastered has a second, more hostile relation
  table in which 0xF is hostile to every team. PTDE has only the first, where 0xF is friendly to players, so the
  port uses PTDE's team 13 (hostile to every team, itself included) for hostile allies and 16 for invaders
  (hostile to players only, as 0x10 is in Remastered). **World tendency:** `EcTestTendency` (vtable `0x11DE5F0`).
- **Limbo:** `WarpNextStageKick` (`0xD62930`), which performs the respawn warp, waits while you are dead,
  others are alive and a boss gauge (`MenuMan+0xF64/+0xF78`) or an invader is present.
- **Scaling and debuff:** ds1sc's `SpEffectParam` rows and ids (enemy 2147418000+, boss
  2147418005+, debuff 2147419000+; the param layout is identical in both games) served by the SpEffect param
  getter (`0xD1B650`) and applied with `ChrIns::ApplySpEffect` (vtable slot 125).
- **Save file:** the save manager's extension (`0x733FC0`).

## Differences from the Remastered mod

- **Four players, not six:** PTDE's engine keeps three remote players in a world.
- **Spectating in limbo** is not implemented yet: the camera stays where you fell.
- **Party HP across areas** (ds1sc shows the HP of players in other areas) and the
  **covenant reward** for a winning invader are not implemented yet.
- **Item descriptions** are written for the port; names, one-line infos and icons are the original's.
- Ghosts, messages and bloodstains of players with the same password still appear (ds1sc has none).
- The Remastered mod's GitHub ban list is not used.
- **Teams** ("allies in teams") put the host against the guests; the Remastered mod's exact team split could
  not be reproduced with PTDE's team table.
- The Remastered mod's character-type condition list (`EcTestChrType`) is not decoded yet.
- **The Cursed Pendant** also works without a session, and its curse lasts until the pendant lifts it (the
  Remastered mod's curse ends with the session).

## Troubleshooting

`PTDE-FPS-Unlock.log` has `[coop]` lines for every step (Steam ready, session state, joins, kept sessions,
each item used, each hotkey press). A `Tick running` line every minute shows the co-op module is alive. If
any hook does not match the executable (it expects the 2015 Steam build), Prepare to Co-op turns itself off and
says which.
