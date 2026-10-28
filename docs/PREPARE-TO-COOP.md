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
   | Cursed Pendant | Curse your own world: it counts as pure black World Tendency until you use the pendant again, and the NPC invaders come even while you are hollow. | your own world: alone or as host |
   | Crimson Blossom | Not used: one blooms with each death in a session (up to five; none when the team defeats a boss while you are down), each lowering maximum HP by 10% (`DeathDebuffs`, on by default since 1.1.0); it is given when the respawn warp goes and the reload keeps the world. They wither when anyone in the session rests (the health they took comes back), the whole team falls, or the player is in the world with no session for 3 s (at home they lie in the own world's key array, which a visit to another world does not show, so one from a death at home came back after a visit). | never |
   | Parchment of Deliverance | Pay a soul level's worth of souls to revive certain NPCs lost while visiting other worlds, then return to your last bonfire. Asks first. | outside a session |

   The items are used like any other item (quick slot, the item animation); when one cannot be used it is
   refused the way the game refuses any item. Keyboard shortcuts do the same (`CoopHotkeys`): **Ctrl+F1**
   host, **Ctrl+F2** join, **Ctrl+F3** leave, **Ctrl+F4** invade. Each press is written to the log.
3. Another player's floating health bar shows their character's name with their soul level and ping, or
   ping, soul level or deaths alone (`OverheadPlayerDisplay`). Everyone sees everyone's, invaders included (the
   game hides an invader's from the host and guests), except a player under Chameleon (SpEffect 1500), who has none. Joins, departures, deaths and changes to the
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
| Guests | Keep their own body (human or hollow), level, items and progress. They pick up items, get their own enemy drops and boss rewards, rest at bonfires, level up and talk to NPCs. A drop the world gives once (an enemy that never comes back, a boss's rewards) reaches every player of the session: a player whose game did not roll it (loading, in a map their game had not loaded, or down when it died) gets their own roll into the inventory a few seconds later, and in a session such an enemy's floor drop goes into the inventory too (see One-time drops). The host's game decides its NPCs' deaths, and a scripted death waits while another player stands at the NPC (Oscar dies once the guest who talked to him walks away, not in the middle of the talk because the host stands in the hall). |
| Joining | A guest arrives at the host's last bonfire (where the host last rested). |
| Deaths | You respawn at your last bonfire in the host's world (the host's, until you rest at one yourself). The session goes on, the host's death included. A death keeps the world as it is: the enemies killed since the last rest stay dead in your reload, as they are for everyone else, host or guest. They come back at a rest, or for everyone after a team wipe. The enemies the others had hurt keep their wounds in your reload too. Every death is announced. After any load (a death, bonfire travel) each player's game builds the other players' characters again, and a player who died is rebuilt for the others as soon as they are back on their feet. |
| Messages | Joins, departures, deaths and the like appear in the box at the bottom of the screen that the game uses for "Phantom X summoned", and fade by themselves. The game's own summon texts and its "phantom returned home" / "sent home" boxes, which wait for a button, are not shown during a session. |
| Bosses and areas | Defeating a boss or crossing into another area no longer ends the session. Joining is never locked. Players roam the host's world freely. |
| Fog walls | The white walls that keep phantoms in the host's area are gone (boss fog stays). |
| World events | Every player's game runs the world's scripted events, not only the host's (`GuestWorldEvents`): a guest exploring alone meets the Hellkite on the Undead Burg bridge, starts boss fights, rides lifts and gets the event rewards. Progress (event flags) is shared. |
| Travel | Anyone can warp by bonfire, and everyone travels together. The host's warp takes every guest to the same bonfire. A guest's warp asks the host's game, which takes everyone there if the host's own world could: the host owns the Lordvessel and has rested at that bonfire (a guest's warp list shows the host's bonfires); otherwise the guest is told why not. A player with a menu open or in a bonfire animation (sitting, kindling) follows once it is over and is told so meanwhile; a player in a cutscene follows after it, and a player who has fallen follows once back on their feet. A guest's travel waits up to a minute for the host's menu, and the host's own warp replaces a guest's travel that is still waiting. Nobody travels while a boss fight is on (a standing teammate's included), an invader is in the world, a teammate is down (their respawn goes first), or a world is starting over after a lost boss fight. When anyone uses a Homeward Bone, the host or a co-op guest, the host goes to their last bonfire and every guest follows a moment later, as they follow a travel ("The power of the homeward bone consumes you..."); the bone waits for the same things as travel except a boss fight, which it takes everyone out of, as in single player (a teammate who fell in the fight follows once back on their feet), and takes one Homeward Bone from every co-op player when the trip goes, so one can be used only when every co-op player holds one ("Everyone needs a Homeward Bone: {name} has none"; a guest of 1.2.0 or older does not say how many they hold, and neither holds the bone back nor pays). The Homeward miracle does the same as the bone whenever the bone could be used, without a bone from anyone ("The power of Homeward consumes you..."); the game keeps it out of multiplayer, and in a session the host and the co-op guests can cast it. The Darksign does the same as the bone, without a bone, and costs every soul and all humanity, the host's and every guest's, taken when the trip goes ("The Darksign consumes your souls and humanity..."; a guest of 1.2.0 or older loses nothing). It waits for nothing but a world starting over after a lost boss fight: it can be used in a boss fight, while a teammate is down, and while an invader is in the world, whom it sends home first ("The Darksign was used in this world. Returning to your world..."; nobody wins that invasion). A guest whose death waits for a boss fight when the trip comes (spectating) respawns at once and follows once back on their feet. If the host is the one who has fallen, a guest's Darksign, or a guest's bone or miracle in a boss fight, still goes: the host's death goes ahead at once, and its respawn is at the bonfire everyone is taken to. None of them is a rest: the trip is a warp (`WarpNextStageKick`), and the enemies everyone killed stay dead, as after a bone in single player. A co-op guest's Homeward Bone, Darksign or Homeward miracle asks the host's game, which takes everyone to the host's last bonfire as for its own, at the same costs ("{name}'s Homeward Bone: its power consumes you..." on the host's screen), waiting up to a minute for the host's menu ("{name} wants to go home: close the menu to take everyone"), or tells the guest why not (a refused guest can try again at once); a trip whose guest leaves before it goes is called off and costs nobody anything; both need Prepare to Co-op 1.3.0 (with an older host the guest's items stay unusable in a session). (In ds1sc only the host travels; a guest's warp does nothing.) |
| Bonfires | Lighting a bonfire for the first time lights it for every player. When anyone rests, every player's world resets (enemies respawn), and a teammate who fell and is spectating respawns. While a living teammate is in a boss fight nobody can rest (the bonfire is out of use, as with enemies nearby), so the boss is not reset in the middle of their fight. A player who sat down before the fight began stays seated with the bonfire menu (level up, attunement, the Bottomless Box, kindling, Reverse Hollowing): the rest that resets the world is behind them. Only the warp waits for the fight. If everyone in a boss fight fell while others waited outside, the rest that brings them back also reloads the world of every living player whose game had the boss awake, together with the fallen, so the boss starts over for everyone: the one who rested stays at that bonfire, another player in the area goes to their last bonfire. The reload waits until the one who rested leaves the bonfire menu (a load is not started from a menu); while the others wait, that player is told so at the bottom of the screen. The others see you sit down, rest, kindle, reverse hollowing and stand up (`BonfireAnimations`). |
| Key items | Entering another world, your key items stay at home (the game's own rule), so a guest cannot open what the host's world has not opened yet. A guest carries there the key items of their own that the host holds too, and one the host gets while you are there joins you at once if you have it at home (a 1.2.0 host). Only items that open the way and that no event takes back are carried: the keys, the Master Key, the Peculiar Doll, the boxes and the Rite of Kindling (not Embers, Lord Souls or the Lordvessel). Key items gained in the host's world come home with you, and stay with you through deaths and travel there. |
| Bloodstains | A guest who dies leaves their bloodstain where they fell, in the host's world, and finds the same stain at the same place when they are back in their own. |
| Progress | Event flags changed in the session's world are shared with everyone, and a guest's flags are written to their own world too: progress made together carries home. A disaster of the host's world (the black phantoms of black World Tendency or the Cursed Pendant) stays there. |
| World rules | The Ominous Tome makes co-op allies hostile to each other (everyone fights everyone) or splits them into teams (the host against the guests); invaders are always hostile. The Cursed Pendant makes the host's world pure black World Tendency (a player can also curse their own world without a session), and lifts the curse when used again: every map's disaster flag is cleared and the black phantoms of the current map die, as when the game's own events end a disaster. Each area with black phantoms starts its own disaster when a player enters it; the game checks every 600 s whether the world is still black and ends the disaster only if not, so in a cursed world the phantoms stay. The curse lasts through sessions but is not saved: after the game is restarted, or another character is loaded, the world is no longer cursed and disasters left from the curse end; otherwise white tendency events never happen and black ones only in a cursed world (with Prepare to Co-op on, world tendency is replaced in every world, as in ds1sc). In a cursed world the NPC invasions (Kirk, Paladin Leeroy, Maneater Mildred, Xanthous King Jeremiah) also come for a hollow player, whatever their covenant (the game has no covenant condition on them: their only tests are human form and the area's boss still alive); without the curse they need human form, as in the game. |
| Spectating | Bosses and invaders are fought as one team. Falling during a boss fight (yours, or a teammate's anywhere in the world) or while an invader is in the world, with another co-op player (host or guest) still standing, you do not die yet: after your fall the camera follows a living teammate in full colour, turned with your own stick (D-pad left/right or the arrow keys switch between teammates). While you watch, your game draws the world around that teammate and updates their character every frame, so a teammate far away is seen in full detail and moves smoothly. Your death ("YOU DIED", the lost souls, the respawn) goes ahead once the fight is decided: the boss is gone and no invader is left, every co-op player has fallen, someone rests at a bonfire, the host takes everyone home (Homeward Bone, Darksign, Homeward miracle; you follow once back on your feet), or after ten minutes. A boss's rewards come to the fallen too: every boss's common event 970 gives its first reward when its defeat flag turns on and the rest 5 s later, and ends at a load that finds the flag on, so the respawn waits until 9 s after a defeat flag turned on, leaving the last reward's box on screen for a while (12 s at most after a won fight with none yet); a teammate who falls during that wait makes it a team wipe, and the respawn then waits for theirs as below. When every co-op player has fallen (a team wipe), the death text says "YOUR TEAM DIED" (every other death keeps "YOU DIED"), each respawn waits after it until everyone's death has got there, and then every game reloads at once: the boss and the enemies start over for everyone, as after a death in single player. |
| Invasions | With `AllowInvaders`, others can invade as evil spirits. While an invader is in the world, bonfires are out of use, the Abyssal Eye Orb, Homeward Bone, Humanity, Twin Humanities, Divine Blessing, Elizabeth's Mushroom and Fire Keeper Souls cannot be used; the Darksign (the host's or a co-op guest's) can, and sends the invaders home (nobody wins) before everyone goes to the host's bonfire at its usual cost. With `SealBossFogsDuringInvasions` (the host's, off by default; a guest takes it from the host's state, `StateMore2`), boss fogs stay shut for the host and the guests while an invader is in the world; off, they work as in vanilla (the host's entry sends the invader home). Nobody joins while anyone in the world is in a boss fight: an invader tries another world (a 1.3.0 invader's search passes over such a world; older invaders' searches do not list it), a co-op guest is told to come back when the fight is over. After an invasion ends, the world takes no other invader for `InvasionCooldown` minutes (default 3). An invader who finds no world that can take them is told why (a cooldown with the time left, a boss fight, a full world). An invader arrives at the game's own black invasion point for the play region the host stands in, else at the nearest one of the host's map within 30 m across and 6 m up or down, else on the host (Firelink Shrine and the Undead Asylum have none; invasions are allowed there). The invader wins when every co-op player has fallen (the host's death alone does not end an invasion while a guest stands; with a 1.0.0 invader it still does); when the invader dies the invasion fails. Invaders of one world are a team (1.3.0, a host of 1.3.0 or later): an invader who falls while another invader stands spectates them (spectate.cpp, with the other invaders as teammates), its death report says the invasion is not lost (`lost` 0), and the invasion is decided for all of them: every co-op player down (all win, the fallen too; a fallen one's held death then takes them home) or every invader down (each fallen one reports `lost` 2, so the co-op team earns a Humanity per invader). The fallen stay in the session, so no new invader takes their place, and InvasionCooldown starts once the last has gone. A boss that dies while an invader is in the world sends the invader home, as the game does (its boss-clear script), but an invader takes none of the boss's rewards: not the clear bonus souls (`GetClearBonus`, `0xD5DEB0`) nor the event's item awards to every player (2003[36]); an invader who arrived in a 1.2.0 host's world during a guest's Moonlight Butterfly fight was sent home 8 s later with 6500 of its souls (2026-10-26). An invader can drink from the Estus Flask (the game forbids it to invaders): up to 5 charges per invasion, any more are set aside on arrival and come back once home, so an invasion costs only what was drunk. With `InvasionRewards`, the winning side gets Humanity: the invader one per co-op player defeated (once home), or, when the invader falls, the host and every guest one each. |
| Enemy scaling | Every enemy gets more health, damage and poise per extra co-op player (`EnemyHealthScaling` ... `BossPostureScaling`, percent per player); bosses are the characters on team 7 and every boss with a health bar (the Bell Gargoyles are on team 6). The game's own co-op boss buff (2009[04], Lua_MultiDoping) is left out, as Remastered Seamless Co-op leaves out its ApplyMultiDoping: it came on top (the first gargoyle at 999 x 1.8 x 1.5). Enemies far from a player get the same scaled health in that player's game, so a fight ends at the same time in every game. The host's six settings are used by every game in its world (sent behind the state message, `StateMore3`): the host's game decides the enemies' health as a number, so a guest with other settings saw the host's whole Asylum Demon fill 95 % of its bar (x2.05 against x2.15, 2026-10-26). When the scaling changes (a player joins or leaves, the scaling first comes on), an enemy keeps its share of health: the game's own recompute kept the health it had lost in points (a hurt boss whole again when a player left), and the second Bell Gargoyle, which its own effect (SpEffect 5490) brings in at 48 %, came in whole. A boss with a health bar whose fight has not begun in a player's game does not pass that game's "health at most X %" tests (X above 0): a reload's unscaled health, passed between games as a number, read 46 % on a scaled copy and brought the second gargoyle in during a Hellkite fight 200 m away. |
| NPC summons | The host can summon NPC phantoms (Solaire and the other white signs). Every guest, and an invader, sees the same gold phantom, also when they join later or arrive somewhere else in the world, and he fights the invader. He stays when the host closes the world. He does not take one of the four places: four players fit with NPCs on top (the party list holds four, so one of them then has no floating health bar). |
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

From 1.3.0 an invader's search lists every Prepare to Co-op lobby (`ptdesc` = 1) rather than only those with
`ptdesc_invade` = 1, and sorts them itself: a world in a boss fight (`ptdesc_boss` = 1) or on invasion cooldown
(`ptdesc_cooldown`: when it opens, in Steam's server time, `ISteamUtils::GetServerRealTime`, which every game reads
alike) is passed over without a knock; one that takes no invaders at all is left out. The answers count too: a 1.3.0
host answers a 1.3.0 invader `kBossFight` or `kCooldown` (with the seconds left, in a field behind the Welcome that
older games do not read) instead of `kRefused`. When no world can take the player, the line on screen says why:
"That world was invaded recently. Invaders can enter again in 2:15", the boss fight or full world, or for several
worlds a count of each and when the next one on cooldown opens.

`InvasionCooldown` (the host's, minutes, default 3, 0 to 10): when the last invader who arrived (`kArrived`) leaves the
host's world in any way (death, victory, the Darksign, its own leave, a lost connection, the host closing the world),
the host takes no invader for that long. Until then another invader may still join the ones in the world. The cooldown outlives the session, so a host who closes the world and opens
it again is still on cooldown.

A join whose game session ends before the load, with the joiner still at home, knocks once more by itself. In the
2026-10-25 test the first invasion of a world after an earlier invasion of it always fell through 0.5 to 3.5 s into
the join, by the game's own leave, and a second try always got in. That joiner had switched to the host's 30 updates
a second before the load; from 1.3.0 a joiner sends the game's own six a second until it stands in the host's world.
The game's leave of an invader is logged with its caller and the bytes the session has waiting for its members: the
send function leaves the session when a reliable message finds more than 1000 bytes waiting (`0xD2DAD1` ->
`0xD2D2F0`, Lua `OnIrregularLeaveSession`).

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
is over, someone rests, the session ends or after ten minutes. A living teammate's boss fight far away counts too
(their 1.1.0 player report says that a boss gauge is up in their game). In the 2026-10-18 test the host fell 60 m
from the guest's Taurus Demon fight, with no gauge of its own, and reloaded at once. The session host's game
decides every enemy's HP for the others (`0xE40FE0`: it writes each changed HP into WorldChrMan's handle table and
the guests set it), so its fresh Taurus, at full health without the scaling applied yet, went to the guest's game,
where the scaled demon (x2 for two players) dropped to half its health. Where Remastered points the HUD's HP and stamina
gauges at the spectated player (a fake `GameDataMan` for the gauge code), PTDE's camera is retargeted: the
in-game step (`0xEFD2F0`) hands ChrCam's update (`0xF14320`) the debug "player" (`[0x137D648]+0xEC`, read by
the menus and the pad too, so not usable) or else the local player (`0xEFDB1F`); the teammate's character is
handed over there instead, and the death camera's flag (`WorldChrMan+0x40`, set at `0xE8332E` when the local
player dies) reads clear for that character only (`0xF1457A`). The cameras turn by the followed character's controller input (`ChrCtrl+0x244`, else `+0x54`: the vector at
`+0x40`; `0xF04B2C`, `0xF18AFA`, and `0xF1B2AE` writing back), a remote player's being the network's; while a
teammate is followed those reads take the local player's controller, as Remastered's camera hook does. The
picture greys while the local player is dead: the in-game step (`0xEFE74B`) fades FadeMan's channel 6
(`[0x13786B8]+0x130`) to 1 while the player's dead flag is set, and the tone correction (`0xEAE930`) takes that
much saturation away; while a death waits the test reads alive and the channel is cleared. A boss gauge is EMEVD's (`2003[11]`): the
Asylum Demon's goes off when the boss dies or when the local player leaves the arena, and a dead body stays.

**The view around a far teammate.** A game draws the world as from the floor its own player stands on: FieldArea's
update (`0xDE85D0`) takes that floor (`+0x30`, from the local player's `chr+0x29C`) and from it every block's draw
and display groups (`0xDE2C60`: map pieces, objects, characters), the area's light and the connected blocks. A
spectator's body stays where it fell, so a teammate far away was seen in the far area's low-detail models and was
not drawn at all, and the game updated that character less often (its update rate `chr+0xF4`, lower far from the
local player), so it moved in jerks (2026-10-18). While a teammate is followed, the floor they stand on goes into
FieldArea's one-update slot (`+0x34`) every frame, when it is a collision of a block loaded and in use here
(checked every frame: the pointer can outlive its block), and their update rate is fixed at "always", as the event
command "Set Network Update Rate" (2004[34], `0xD6D780`: `chr+0x1FD` bit 0, `+0xF4` = 0) does; both stop when the
view does. The map we are in still follows our own floor.

**A team wipe reloads every game at once.** Each game reloads its own world after a death, so a wipe starts the
world over in every game, but only if the games reload together. With each death going ahead on its own, the
first game to wake in its new world met the boss and the enemies from a game still in the old one: the network
went on following that game for them, and the new world's boss flags went into the old one (the Asylum Demon
already standing on the hall floor, then jumping from there and falling through it). After a wipe the respawn
warp (`WarpNextStageKick`, `0xD62930`, which every death reaches: the bonfire case of the regen switch at
`0xD763E0` for one's own world, `WarpSelfBloodMark` for a guest) waits as limbo did, and limbo's own release when
everyone has fallen waits there too. Each player reports where its respawn is for the death it reports: not at
the warp yet, waiting at the warp, or gone (message 21 on every change, and a byte in the 1.1.0 extension of the
player report every second, through the load as well). A new death starts it over. The host waits for every
fallen guest that announces the feature and is not at its warp yet, up to 15 s; a guest waits for the host's warp
to go, up to 25 s: the host's "went" (kept once seen, as the host's first report after standing up sets its phase
back), or the host standing again since the wait began. Every death's warp says it went, wipe or not, so a guest whose death reaches its warp after the
host's already went follows at once, and the host does not wait for a guest that went before it (one that died
alone). A warp that replaces the waiting one (the trip home) drops the wait. Players from before this never wait
or are waited for. A player who fell counts as dead in their reports until their respawned character exists, so a
wipe lasts through the loads. Another player's character that a load threw away is asked for again only once that player reports standing in the world: asked
for during their load, it came out invisible and could be walked through. Nor is it asked for while they still get
up at their bonfire (event animation 6950, about 3 s after a respawn or a Homeward Bone) or hold any other event
animation (up to 20 s after they stood): asked for then, it could be walked through and showed none of their attacks
for as long as it lived (the host's character on the guest, made 2.3 s after the host stood again, twice). A
character first seen while its player reports being dead or loading is rebuilt once they are back.

Even together, one game's load ends a little before another's, and the other games' enemy positions wait in the
session's queue meanwhile; the first frame after the load takes them all (`0xE57D01`). Net handles are the same in
every load of a map, so a position from another game's last moments of the old world went to the new copy of that
enemy: the Taurus Demon stood where the team had fallen, its AI held off by its event and moved by no game, and its
entrance leap from there took it off the bridge (2026-10-25). After a reload that starts the world over (a team
wipe, a lost boss fight) the positions that come in during the load and for 1.5 s after are dropped, so the enemies
stay where the map makes them until the games send positions from the new world.

**A death keeps the world.** In vanilla a death reloads the world with every enemy back. In a session only the
game of the player who fell reloads, so the enemies the team had killed came back in that game alone: after the
host's death the host's game decided their HP for the others (`0xE40FE0`), and in the guests' games their bodies
got HP again and could be locked on to without moving or taking damage (2026-10-18); after a guest's death they
stood up in the guest's game, and the host's game, where they stayed dead, never sent anything that put them down.
The game already has what a summoned phantom needs: a list of the world's dead characters ("CHR " list, built by
WorldChrMan `0xE3E540`: per character set of a map block a key, the count and a bit per character, dead when its HP
is at most 0 and its param does not leave it out), which the host sends a joining phantom once (`0xD30910`,
packet 15) and a map load applies (`0xE40EC0` from `0xEFCE7A`: GameMan's own-world copy at `+0xBCC` in one's own
world, `GameMan+0xB4F`, else the host's copy at `+0xBC4`; each marked character is killed in its dead pose, `0xE44150`
-> `0xE6B4E0`), and then frees (`0xEB9580`). The phantom's arrival uses the host's copy up, and a death clears the
own world's (`ClearMyWorldState`, right before `WarpNextStageKick` in `SoloPlayDeath_2` and `PartyGhostDeath_2`).
Now, in a session with a co-op teammate, an individual death's respawn warp hands the game a list for the reload
(the game's own setter, `0xEB9910`, so the load frees it with the game's heap): the host's game gets its own list
merged with the guests' (dead in any game is dead), and a guest's the host's list, with its own for the map blocks
the host does not have loaded. While a co-op teammate is down, the host sends a fallen guest its list, and a guest
a fallen host its own, each time it changes (message 22, every 0.5 s at most); a list that comes during the load,
before the load takes it, replaces the one given at the warp, so a kill during the load is kept too. A team wipe
and a rest during the death start the world over as before, and a load that did not take the list drops it. Which
of the two a reload is, is decided when its warp goes, after any wait, and every game must decide the same for the
same deaths: each player's "went" (message 21 and the player report) says whether its reload keeps the world, and a
later reload follows the teammates that went before it. One that kept the world means ours keeps it too (the
second of two deaths that came seconds apart, before either player had seen the other's); one that started it over
is part of the same wipe (in a wipe the host's warp goes first, and the guests' follow it). With none gone yet, it
is a wipe when every other co-op player is down. A player from before this gets no list and brings the world's
dead back in their own game, as before.

The hurt enemies go with the dead (1.3.0). A reload makes every character it brings back whole, so an enemy a
teammate had worn down stood at full health in the fallen player's game alone while the other games went on from
where it was: the Black Knight a guest had hit before dying stood whole for the guest, and died in the host's game
part of the way through the guest's second fight (2026-10-23). While a co-op teammate is down each game also sends
them its hurt enemies (message 28: the net handle `chr+0x78` = block << 11 | place, which the game's own HP and
damage sync use to name a character in every game, with HP and max HP), and the reload that keeps the world puts
each one down to the same share of its max HP once the scaling of its health is in place (the host takes the lowest
share any guest reports). Health only goes down this way, and only within 20 s of the reload.

The scaling of an enemy's health is an effect, and the game works an enemy's max HP out of its effects only while it
updates that enemy: one far from the player of that game (its update rate) kept its unscaled max HP with the effect
on it. HP and damage go between the games as plain numbers (`0xE40FE0` sends the session host's HP, `0xE413B0` the
damage), so that copy took the same hits as a nearby, scaled one and died first: the host at Firelink saw a hollow
of the Burg die at 14/85 HP while the guest's copy stood at 43/114, each down 71. Such a copy now gets the scaled
max HP from the mod (the damage it took kept), 1.5 s after the scaling went on, as the game itself gives it once the
enemy updates again; what the mod wrote is put back when the scaling comes off while the game still has not
updated the enemy.

**A rest after a lost boss fight reloads too.** Only a load puts a boss fight back to its start: the boss, its fog
and its flags. A rest restarts some of the fight's events, but they end at once when the fight has begun (the Asylum
Demon's `11810310` while `11810314` is on, the Taurus Demon's `11015382` while its own flag is on), or put the boss to
sleep again without moving it back (most bosses' `11xx5392`), and the boss-room flag `11xx5393` and the other working
flags are cleared only by a load. In vanilla nobody rests during a fight, and a lost fight always ends in a load. In a
session every game in the map runs the fight's events (the runs are shared), so a teammate who waited outside kept
the fight going in their game after the fighters fell. In the 2026-10-18 test the host rested to bring back the guest
who fell to the Asylum Demon: in the host's game the demon stayed awake on the floor, won the network's bids against
the guest's reloaded game (where it slept on its perch with its AI off) and walked into it without a health bar or
music. Now each game notes the bosses (the 32 with a health bar) whose fight began in it since its last load: an
event switched the boss's AI on (Set Character AI State, `0xD6C3F0`; off puts it back to sleep), an event played an
animation on it (every one in the maps' events belongs to a fight), our own boss gauge shows it, or a standing
teammate reports it on theirs (`boss` in the 1.1.0 player report). The last two came from a second test: the guest
fought the Taurus Demon far from the host, whose copy of the fight's run stopped at "wait for the leap to end" (an
animation does not end on a copy the network moves), so the switch never came there and the host's rest left its
fight going. A rest, ours or another player's, while such a boss still lives reloads that game at its bonfire. It is
decided at the rest, before the rest restarts the events. Every 1.1.0 game reports its reload phase for each rest
(message 23, reliable, with the rest's id, which the rester puts in the rest's message and the host passes on): none,
waiting or ready. The player report is not used for it: it is unreliable, so one from before the rest could come
after it and read "none" for a reload that waits. A reloading game is waiting while its menus are open, until every
teammate's phase for this rest has come (one that never answers is given up after 5 s), or while a fallen player is
not at their respawn warp yet (up to 15 s), then ready. The reloads go once none of them is still waiting, and the fallen's
respawn warps go once none is pending, so every game reloads at once, as after a wipe. A fallen player waits as long
as such a teammate is heard from, with no time limit (leveling at the bonfire can take a while, and a fallen player
who reloaded first would meet the old world's boss); the teammate is told every 8 s that the others are waiting for
them to leave the bonfire (or close the menu). A guest's reload stays in the host's world and starts its copy of the
host's temporary flags over, as a death reload does.

**A death that does not reach its respawn.** A death is the global script's chain `OnEvent_4000` ("YOU DIED") ->
`SoloPlayDeath` -> `SoloPlayDeath_TextWait` -> `SoloPlayDeath_1` -> `SoloPlayDeath_2`, which asks for the respawn
warp and clears flag 4000; `OnEvent_4000` ignores the death while a return to the title is asked for
(`GetReturnState`, `[[0x13784A4]]` > 0) and starts it only while flag 4000 is off. A guest who fell after the host
once never got to the warp: no respawn for the rest of the session, and a rest changed nothing. In the 2026-10-17
evening test every guest death after a death reload stalled with flag 4000 on and no return to the title: a load at
home clears flag 4000, a guest's death reload stays in the host's world and kept it, so the next death never started.
That reload now clears it with the temporary flags, and a death that begins with it still on says so in the log. A
death that has not asked for its warp 15 s after it went ahead (not counting a wait for the fight, limbo, the team or
an open menu such as the indictment menu) first has a return to the title that never happened dropped, then is
finished with `SoloPlayDeath_2`; another player's rest finishes one that has waited over 10 s. A rest heard between
a fall and its death event (about 3 s) counts as well: the death goes ahead instead of starting to wait for the
fight that the rest has already reset.

### Invasions: arrival, boss fogs

The maps' MSBs hold one black invasion point per play region: a region named `黒侵入ポイント` with entity id
map base + 4000 + the play region's number (the Undead Burg's play regions 101000 ... 101210, the collisions'
`PlayRegionID`, have points 1014000 ... 1014210; seventeen maps checked, the range holds nothing else; Firelink
Shrine, the Undead Asylum and Ash Lake have none). The game keeps the player's play region at `PlayerIns+0x284`
and turns it into the point's id the same way (`0xD6BB50`: id / 1000 * 9000 + 4000 + id). The host picks the
point of its own play region from the loaded maps' regions (`WorldPointManImp`, `[0x137D6E8]`, as `0xD6C490`
looks one up), else the nearest point of its own map within 30 m across and 6 m up or down, else its own position,
and sends it in its answer as the invader's arrival (the game's summon position, `GameMan+0xA80`/`+0xAB0`, map
`+0xAA4`). The play region's own point is the game's choice; the nearest point is the mod's, for the places that have
none (Firelink Shrine, the Undead Asylum, Ash Lake, the stretches between play regions). Until 1.3.0 it was any
point within 50 m in a straight line: from Firelink that could be one of the New Londo Ruins below, behind a lift
the host had not used yet.

A boss fog's entry is an EMEVD "Action Button - Boss Room" (`3[19]`). Its test (`EcTestActionButton`, `0xBBD7E0`)
greys the button out for a Gravelord or Path of the Dragon phantom in the party (invade types 7 and 10) or state
effect 185; any other invader leaves it open, cannot follow (the fog takes living players and white phantoms),
and is sent home by the host's entry (`2009[06]` Issue Boss Room Entry Notification: Lua 4068 `ReportBossArea`
-> `Failed_BossAreaMission` on the invader, `0xEA2C80` drops black phantoms from the party). With
`SealBossFogsDuringInvasions` (the host's, off by default; sent behind the state message in `StateMore2`, which earlier
versions do not read) Prepare to Co-op adds "an invader is in the world" to those reasons at `0xBBD86C`. Each player reports whether a boss gauge is shown in
their game (an addition behind the status message that older versions ignore); while one is, the host answers
invaders "refused" (a 1.3.0 invader "boss fight") and its lobby's `ptdesc_invade` reads `0` and `ptdesc_boss` `1`.

**Rewards** (`InvasionRewards`). The game's own rewards never come in a session: the invader's victory script
(`TextEffectEnd_PK_Success`) asks for item lot 16581, which PTDE's `ItemLotParam` does not have, and a host who
killed the invader kept 0 humanity in the 2026-10-17 test. So each game gives its own player Humanity (goods 500,
silently, with a line at the bottom of the screen), once standing in the world. An invasion has one outcome, and the
invader's game decides it: its victory gives it one Humanity per co-op player the host counted ("Defeat {} players"),
handed over once it is home; a death of its that lost the invasion is flagged in its death message (`lost`, a byte
the 1.0.0 message already had and leaves 0 for deaths), and every host and guest that hears it gets one Humanity,
fallen players included. An invader who leaves or drops gives nobody anything. Any death that loses the invasion
counts, a fall included, as the invasion's outcome already did. Only what reaches the inventory is counted and
announced: Humanity that does not fit (a full stack) is tried again every 30 s.

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

`UpDateBloodMark` is the last step of `SoloPlayDeath_2`, after `WarpNextStageKick` and the clearing of flag 4000.
While a respawn waits (a team wipe, the reload after a lost boss fight) the kick returns without a warp, the player is
still dead, and the death starts over every five seconds or so; each pass wrote the stain again with what the first
had left, 0 souls and 0 humanity (2026-10-23: a guest's 2594 souls and 2 humanity from the Taurus Demon fight were
written over three times while it waited, and the stain it picked up held nothing). One stain per death is written
now (the call at `0xD66204`); the next goes after the player has stood up again.

### NPC summons

The host summons an NPC phantom (Solaire and the other white signs) in its own game: the sign's NPC (`SosSignMan`,
`0xDB3D7F`: entity at sign `+0x34`) becomes a phantom (`0xE407F0`: character type `+0x70`, team through `0xE66AB0`),
joins the party (`0xEA2510`, entry type 2), and packet 48 tells the session {character handle, two party values, the
type, the summon effect}; the host sends the same to every member who joins later (`0xEA2900` from the member-join
handler). Each other game looks the handle up (`0xE3E340`) and does the same to its own copy of the NPC. Character
handles are `0x10000000 | block << 14 | place`, the block numbered in the world's block list (WorldChrMan is made once
with room for 27 blocks), so a handle names the same character in every game that has that block loaded. In vanilla
every player stands in the host's map; a co-op guest or an invader who arrives somewhere else does not have the
block, the lookup came back empty, and the packet loop read through it (`0xEE3036`, reading address 0x70): the
guests' crash on joining a host with Solaire summoned (2026-10-23, two dumps). Now:

- packet 48 goes ahead only with a character in our world (the call at `0xEE3023`); packet 49 (an NPC phantom's state,
  `0xEA2DD0`, which looks the character up again) likewise (`0xEE30C0`);
- the host sends which NPC each of its phantoms is (message 27: entity id, handle, type, party values; on a change,
  to each newcomer, and every 3 s while there are any), and every guest's and invader's game turns its own copy into
  the same phantom as soon as that NPC is in its world, also after its own loads (a reload makes a fresh, unsummoned
  copy). An NPC the host no longer lists leaves their party the game's own way (`0xEA2A10`) after 3 s;
- the party's update (`0xEA2FD0`) reads a member's character with no check in states 1, 3 and 4; a guest who walks
  away from the NPC unloads its block, so an entry without a character waits (`0xEA2FFC`) until it is back;
- the session's end keeps the host's NPC phantoms (and its own entry) in the party, as the game is when alone with a
  summoned NPC: the Black Separation Crystal closed the world over Solaire and the mod's settling of the party took
  his entry, so his health bar went and he stayed;
- in one's own world, an NPC entry whose character is no longer a phantom (the host's own reload made it anew) leaves
  the party, as the game's party reset at that load would have done;
- no wall keeps a phantom in its area in a session and the party is not reset at a map-block change, so the NPC can
  follow the host far enough for its own map block to unload: its character is gone until the block streams back,
  as a fresh copy. The summon goes on meanwhile (the host keeps announcing it, it keeps its place and stays through
  the session's end), and the fresh copy becomes the phantom again; a real load (a death, a rest, travel) ends it;
- an NPC phantom does not take one of the game's four places (until the 2026-10-25 build it did): the party list holds
  four entries, so with four players and an NPC one character has none in each game and no floating health bar.

An invader's game turns Solaire into a phantom too, so he is hostile to the invader there and his blows land (before,
the invader saw a plain NPC who swung and never hit). Older hosts send no list: a 1.3.0 guest then follows packet 48
alone, without the crash.

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

### The death text of a team wipe

"YOU DIED" is no string. Lua `SetTextEffect` (`0xD5E430`, queued for the menu by `0xC2A7B0`) shows
`TEXT_TYPE_Dead` (2) or `TEXT_TYPE_GhostDead` (6); their dialogs in `menu.drb`, `F23_YouDied` and
`F23_YouDide_phantom`, draw the sprite `main_text_youdied`: rows 496-596 of the 1024 x 1024 DXT5 atlas `Menu14`
(`menu/english/menu_local.tpf`), the atlas's whole width, onto 1024 x 100 of the 1280 x 720 menu screen. The band is
opaque, dark red letters on black, next to VICTORY ACHIEVED, TARGET DESTROYED and the other text effects.

For a team wipe the band is lettered "YOUR TEAM DIED" while the text shows. `assets/TeamDied.png` is made from the
game's own letters by `tools/build_team_died.py` (it reads the atlas from the game's archives): Y, O, U, D, I and E
are the band's own; R, T and A come from TARGET DESTROYED and M from BLACK PHANTOM DESTROYED (the same typeface,
crisp, set narrower and smaller), scaled across and up as measured on the letters both lines share and given the
band's colour. At the band's size the line is 856 pixels wide (YOU DIED: 475), so it fits without being squeezed.
The game creates the atlas with `D3DXCreateTextureFromFileInMemoryEx` in the managed pool; the hook on that
import (`title.cpp`) recognizes it by size and FNV-1a hash (other languages' atlases are left alone). The death
script's call to `SetTextEffect` asks `world::team_wipe_death` (the same test as the reload's,
`session::wipe_reload`); right before the next frame is presented the band's whole 4 x 4 blocks of levels 0-3 are
written from the PNG (`D3DXLoadSurfaceFromFileInMemory`, which compresses them), the game's blocks having been kept.
Two seconds after the player stands again they are put back, byte for byte.

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

The other players' characters are updated every frame wherever they stand (chr+0x1FD bit 0 fixes the update rate
chr+0xF4 at 0, as the event command "Set Network Update Rate: Always" does for the Hellkite). Otherwise WorldChrMan's
update gives a character off screen a lower rate and one whose state object is inactive none at all; vanilla never
needed more, since a phantom stays in the host's area. Not updated, an event animation played on a copy (the other
player opening a door, pulling a lever, passing a fog gate) never ended, and a copy holding an event animation has no
collision and takes no other animation: after a player went through the Firelink / Undead Burg seam their copy
could be walked through and showed no attacks. Each player also reports the event animation their own character
holds (1.2.0, behind the 1.1.0 report), and a copy still holding one 3 s after its player holds none (30 s for
players who do not report it) is stopped as the game stops its own. The rate is fixed only on a character made 2 s
ago or more: the game sets a new one up in its first frames (its entity not active yet, rate -1).

For the log, each of these characters' state (its map entity's state and flags at `[chr+0xC]+8` and `+0x14`, the
update flags `chr+0x1FD`, `chr+0x1FE`, `chr+0x3C4`, the rate, HP, floor, physics and event animation) is written
when it is first seen, with its player's report (up for how long, which event animation), and again when it
changes; a character we stand inside of (two that collide stay 0.5 m apart) is pointed out with its state.

A player who falls in their own world while joining (before the move into the host's world) has the join called
off: the death goes to their own last bonfire, as any death does.

### Network rate

The game sends its player's character (packet type 1 through the broadcast `0xD2D940`, from the PadManipulator
`0xE1DC60`: position, facing, two animation ids, HP) every 5 frames, six times a second; the FPS Unlock's ghost fix
(`[ebx+0x234]`, reloaded at `0xE1DF7F`) keeps that in real time at any frame rate. The receiver (`0xE1EDC0`) moves the
character toward each update at the speed that covers it in 1.2 times the gap since the one before (`+0x2BC`), and
turns it by a fifth of the angle left every 30 FPS frame (the shared 0.2 at `0x1160764`). Enemies' positions go in
rounds (`0xE56BC0`, packet type 3, 8 a packet, one packet every other frame) started every 0.25 s (`+0x1BC` of the
table at WorldChrMan+0x2B4); their HP from the host every 4th frame (`0xE56790`). The mod's own reports go once a
second.

Between updates: a player's character moves toward the newest update at the speed that covers it in 1.2 times the
gap (the double at `0x11E84F8`, used at `0xE1F3A1`), an enemy's (`0xE21270`, NetAIManipulator, gap `+0x288`) in 1.1
times it (`0x11E8500` at `0xE21663`) and turns by angle over gap; either jumps when the update would need more than
20 m/s (an enemy: its own limit, `+0x80`), the gap is 0, or more than 2 s passed. A player's animations (both layers,
with their time, packed by `0xE2C030`) come in the same update, so another player's roll or swing starts when the
next one arrives. An enemy's actions are messages of their own (`0xE56AB0`, packet type 0x21), sent the frame they
start whatever the rate. The update loop (`0xE1F950`) decodes every update that came for a player in a frame (pop
`0xD30140`, back to `0xE1F980`); the gap is zeroed after each, so the second of two in one frame jumps. Enemies take
only the newest position each frame (`0xE556F0` overwrites, `0xE55360` takes it once).

`NetworkTickRate` (the host's, sent behind the state message, 1.3.0; 6 to 60, default 30) sets the player update's
reload to 30 / rate frames and the enemies' round to 0.25 x 6 / rate s in every game of the session. One update goes a
frame at most, so a game drawing fewer frames than the rate sends every frame; with the FPS Unlock's added reload its
counter then sank a little further each frame and in the end wrapped round (no update for a quarter of an hour after),
so a stub on the send test (`0xE1DDE4`) holds it at minus one reload. A round of enemy positions goes every other frame
at best (a round's first call is skipped when the last round sent the frame before, `+0x1CC`): at 30 a second, 15 to
20 rounds a second at 60 FPS and 15 at 30 FPS. (60 a second was tried before the jump test below was fixed: player animations missed steps at speed. It is allowed
again for testing; 30 stays the default.)
Above the game's own rate the
receiving side changes too (`netrate.cpp`): the pop takes every update that is waiting and keeps the newest (each
holds the whole state); the movement covers an update in at least the gap plus `NetworkSmoothing` (each player's
own, default 32 ms; stubs at `0xE1F3A1` and `0xE21663`), where the game's 20% (10%) of slack is a few ms at thirty
a second and an update a little late left the character standing; and the turn is a 30 FPS frame over the gap plus
the smoothing (stubs at `0xE1F83C` and `0xE1F8CB`, never less than the game's fifth, at most the whole turn). A
character is then drawn about one update interval plus the smoothing behind its player, plus the ping (about 65 ms
at thirty a second, against the game's 200 ms). At six a second everything is the game's own. A guest of 1.2.0 or
older keeps sending six a second.

The jump test divides the distance to the update by the gap since the one before. At the game's own rates that gap is
a sixth of a second (players) or about 0.3 s (enemies, whose limit is their `networkWarpDist`, 16 m/s for nearly
all), so only a copy metres away jumps; at thirty a second a
metre over a thirtieth reads as 30 m/s. A copy a frame or two behind (the smoothing keeps it behind on purpose) jumped
from update to update, and a fast one (the Asylum Demon's leap off its perch, the Taurus Demon's onto the bridge) at
every update: steps thirty times a second on a picture drawn far more often, standing still between them, with a run
left at a walk (2026-10-25 test: worse at sixty than at thirty, on a fast, low-latency connection). Above the game's
own rate the time in the test is at least the game's own gap (stubs at `0xE1F370` and `0xE2162B`), so a copy jumps
only when it is as far off as it would have to be at the game's own rate (players 3.3 m, most enemies 4.8 m).

### Update check

Releases keep the co-op protocol as long as they can, so players of different versions still play together and a
session never tells anyone to update. Instead, at each start (`CheckForUpdates`, on by default) a thread of the mod
asks GitHub's API for the repository's latest release (`api.github.com/repos/zofrea1/DS1-Prepare-to-Coop-PTDE/
releases/latest`, WinHTTP, 5 s timeouts, drafts and pre-releases left out) and compares its tag with this build's
version. A newer one is announced in a box at the top of the title screen, drawn by the mod's Direct3D overlay (a
Windows message box would take the focus from a game in exclusive fullscreen), until a character is loaded. An answer
that comes after that is only logged and shown at the next start: a map load has no character either, and the game's
PlayerGameData lives from start to exit, so a later visit to the title screen cannot be told apart from a load. No
answer (offline, a firewall) shows nothing; the log says what happened.

### World events

Map events test the session through `EcTestMultiplayState::Test` (`0xBB3130`; the SKIP/END IF instructions call
it directly from `0xBB3200`). The game's co-op ends most events on clients, so only the host's game ran them.
As in ds1sc, during a session "host" passes and "client" fails for every event, so each
game runs the world's events; events 11005844, 11015846, ... 11810150 (the Asylum crow) fail every
multiplayer test, and 11020301, 11200005, 11200006, 11305000, 11510230, 11600200 act as in single player. The
event id is `FrpgEventIns+0x28` (constructor `0xBC4150`). `GuestWorldEvents = false` restores the game's rule.

Each game bids for the enemies around it (`0xE6D250`: authority, engagement, nearness to its player) and the
session host's game hands each enemy to the highest bid; the others follow it over the network. The events switch
an enemy's AI on or off in each game by that game's own player (Set Character AI State, `ChrCtrl+0xC0` bit 0), so
the copies can differ: a game whose copy has its AI off bids the least while another player is near the enemy (a
player who fell and spectates counts: their game still runs it), and a game whose player is more than 64 m away
does not bid while another player is nearer. A game whose player fell bids the least but one while a standing
player is near the enemy: the nearness term counts from the fallen player's body, so a guest who fell in the Taurus
Demon's arena outbid the host fighting the demon 9 m away, and the demon walked back to the body until the host came
closer (2026-10-18). A standing player's game then wins, unless its copy sleeps, which still bids the least. Without the first rule the Asylum Demon, its AI off in the guest's
game after a retry, stood idle once the guest's game took it over. The AI-off copy still bids, so an enemy whose
copies all sleep is still some game's to move: at first such a game did not bid at all, and the sleeping demon on
its perch was nobody's until the session host next handed enemies out.

A load at home starts every map's temporary flags (`xxxx5000`-`xxxx5999`: timers, who stands where, which runs of
the map's events are done) over. A guest's death reload stays in the host's world, whose flags the game then left
as they were: the Asylum Demon's "AI on" event (11815392) saw its condition (11815393) still done, passed at the
load ahead of the intro that switches the AI off, and the guest's copy fought the retry with its AI off. At a
guest's death reload the visited world's temporary blocks (group 1, sub-block 5 of every map) are cleared, as the
host's own reload clears them at home.

A scripted animation that moves an enemy (the Asylum Demon's leap from its perch, 9060; the Hellkite's flights)
moves it only in the game that moves the enemy; the others follow over the network. In the 2026-10-18 tests the
demon slept on its perch with its AI off in both games, so neither bid and it was nobody's. When the guest walked
in, the leap began in both games while neither moved the demon; the session host's next hand-out gave it to the
host's game 0.25 s, 0.5 s and, the third time, 2.5 s into the leap. That time the host's game played only the end
of the leap, from the perch, and the demon stayed up there (at height 213; the floor is at 198). The AI-off copies now
bid the least instead (above). Each event animation on a boss or the Hellkite is still followed in the log for 8 s
(which game moves it, where it is, whether the animation still plays, this game's bid).

In a guest's game the host's NPCs (teams 8 and 9) die only when the host's game says so (its HP report or its list
of the dead) while the host is in the same map, also while the host's game is loading and has not reported them
yet: Oscar otherwise died in the guest's game during the host's load and dropped his items under the living Oscar.

The flags below 10000 belong to no map (group 0, area 0, a block per thousand): the bosses killed (2-16), the bells
(812, 813) and every NPC's state (1000-1999). The game's join copy (`0xD30A20` -> `0xD37F30`) gives a summoned
player the host's sub-blocks 0, 1 and 5, but a co-op guest could keep what its visited-world store held already. Its
game then ran the host's world's events on that progress, and the event sync (`0xBC4000`, which moves another game's
copy of an event past a condition that passed) took the runs into the host's world: a guest with both bells rung and
the gargoyles dead arrived at Firelink Shrine, and the host's Lautrec went from his cell (1570) to Firelink (1577) and
on to "gone to Anor Londo" (1578), the state in which he has killed the Firekeeper (2026-10-25). From 1.3.0 the host
sends its sub-blocks 0 and 1 (`kWorldGlobals`) with the welcome and again on arrival; the guest's visited store takes
them, all but the flags that belong to the player (`personal_flag`), which come from the guest's own world. A guest
of 1.2.0 or older ignores the message.

An event that waits for an animation to end (Force Animation Playback with ShouldWaitForCompletion; the wait is
`0xBB9A20`, from `0xBC1CAD` and `0xBC2D2B`) waited for ever on a copy another game moves: the end only comes in the
game that plays it. The Bell Gargoyles' 11015396 brings the second gargoyle in with its animation 7001 and only then
takes its invincibility off and shows its gauge, so in the game of a player far from the fight the second gargoyle
stayed invincible, never died there and its reward (11010750, item lot 53500100) never came; when that player walked
up after the fight the event went on and showed the dead gargoyle's empty gauge, and their report put every game in a
boss fight. `0xBB9A20` makes a condition object (vtable `0x11DD04C`, entity at `+0x1C`, animation at `+0x20`) that
the event adds to its condition group and updates every frame (`vtable+4`); the group's test (`0xBBDA00`) reads bit 0
of `+8`. In a session each such wait on a character gets a copy of that vtable with our update: the game's own check
first, and when the animation ends in the game that moves the character, that game tells the others (message 31,
`kAnimDone`, through the host). In a game whose copy another game moves (an enemy on its NetAIManipulator, or another
player) the wait ends on that word, or after 10 s without one (a player of 1.2.0 or older, or a game that does not run
the event), so what the event does next happens about when it does in the game that plays the animation. Ending the
wait at once let the rest of such an event run ahead of the animation (a flag shared early, an AI switched on early),
and returning no object crashed at `0x528B6F` (Sen's Fortress 11505051, 2026-10-25). The game's own check
(`0xE6CEC0`) can call the wait over before the animation has begun: it counts the request (`[chr+0x10]+4`) only for an
enemy that is engaged (`chr+0xF4`), and the animation's slots come with the character's next update. The Asylum
Demon's drop ended at once in the game that moved it, so its event took off SpEffect 4160 (state 47: no fall damage,
`0xE7FFDC`) at the start of the 28 m drop, and the guest's copy landed 5 % down (2026-10-26). In the game that moves
the character the wait now ends only once the request is no longer that animation (up to 30 s). A gauge whose boss is dead is no fight (`world::boss_fight`, `rules::own_gauge_tick`), and one whose
bosses have all been dead for 10 s is taken down.

When a boss passes to this game (another game moved it: its player fell, or ours came nearer), its AI here had been
idle and had no one to fight: after the host fell to the gargoyles, the guest's game took the second one, which walked
back toward its perch and fought only once the guest hit it. A boss whose fight began here and that passes to this
game while our player stands within 40 m is turned on our player with the event scripts' ForceChangeTarget
(`0xD61E00`, stdcall(entity, 10000)), at most every 10 s per boss.

The bonfires' state is a table beside the flag stores (`[[0x137E204]+0xB48]`, a list at +0x24; an entry holds the
bonfire object at +4, two counts at +8 and +0xA, lit +0xC, state +0xD and the kindle level +0xE). A visit to another
world can leave that world's states in it: after the host invaded a world further on, their own Undead Burg bonfire
gave 15 Estus. While a player is in their own world the table is copied twice a second; back home from another world,
the bonfires whose kindling changed meanwhile get theirs back (logged), and a kindle level that changes at home is logged.

### Items

| What | Where |
| --- | --- |
| Rows | extra `EquipParamGoods` rows 389000-389008 from the goods param getter (`0xD1E3A0`): ds1sc's rows field for field (refId -100..-93, icons, the eye orb animation, the Parchment's yes/no dialog), with the flag bytes translated (Remastered inserted `enable_pvp` into the bit fields) |
| Can it be used? | the game's item check (`0xEADCA0`, cdecl) answered by the session rules for these ids, Homeward Bone, Black Eye Orb, Humanity, Twin Humanities, Divine Blessing, Elizabeth's Mushroom and Fire Keeper Souls |
| Homeward miracle | Magic 5210 puts on effect 580, which becomes 581 after 0.6 s: stateInfo 16, the bone's return to the bonfire (3220). Its `MagicParam` `enable_multi` (row `+0x2B` bit 0, read by the spell check `0xEAD7C0`; row from `0xD1D800`) is switched on for the host and the co-op guests while the bone could be used. The effect update (`0xE51100`) calls `0xD76390` (`WarpNextStageKick`) for the local player when `0xDC2A40` finds a stateInfo 16 effect; in a session that call (`0xE5143C`) starts the party's trip instead (`session::homeward`, `kHomeMiracle`; a guest's asks the host's game), and is dropped for an invader |
| Effect | `PlayerIns::UseGoods` (vtable slot 128) |
| Names | the game's FMG lookup (`0xD175E0`); the Parchment dialog (event text 93129700) shows the soul cost |
| Given / removed | Lua `AddInventoryItem` (`0xD664C0`) at each rest; `RemoveInventoryEquip`'s core (`0xD6D670`) for the Crimson Blossoms |

Key items: `MoveMapStep`'s constructor (`0xEFC470`) switches the inventory's key array with the world. At home it
reads the own array (`+0x120` = `+0x114`, its count at `+0x134` = `&+0x12C`); in someone else's world `0x51C520`
points it at a second array (`+0x11C`, count `+0x130`) and clears that. Keys a guest gained in the host's world went
into the second array and were gone after the trip home. Each one is now added to the own array as well (the own
array switched in for the moment), so the game's own switch brings it home, also after a crash or a quit there;
three seconds after the trip home any that is still missing is given back. The second array is cleared on every load
in another world (`0xEFC6C3`, clear argument 1), a death or a travel there included, so keys gained there are put back
after such a load. With a 1.2.0 host (message 24: the host's key items, its Lordvessel and the warp list's bonfires it
has rested at, sent when they change) a guest also gets the key items of the own array that the host holds, limited
to `items::travel_key` (an Ember, Lord Soul or the Lordvessel handed over there would stay in the own array at home).

**The item-get window.** Items the mod gives (the co-op items at a rest, a death's Crimson Blossom, an invasion's
Humanity, a one-time drop) go into the inventory with `0xC0B700` and are then shown in the game's own item-get window
with its pickup sound: `0xC22830` (eax = the queue `[0x137E174]`, stdcall(category, id, count)), as the game's
awards (`0xDC8CD0`, after `0xDC5480` adds the item with `0xC0B700`) and pickups do. The HUD draws the queue
(`0xC23210`, up to ten lines); no dialog slot is taken. The earlier attempt used the Lua `AddInventoryItem`
(`0xD664C0`), whose information box (`0xD4BFA0`, the generic modal list in the dialog slots at `MenuMan+0x9EC`) has
no other caller in the game: called by the mod it stayed requested and was never drawn, and the action buttons and
Quit Game waited behind it (2026-10-12/13). The rest's gifts and the Blossom wait until the player stands in the
world with no menu or dialog open; a one-time drop shows when it is put in.

### Rules for guests

| What | Where |
| --- | --- |
| Enemy and object drops, scripted reward items | inlined guest checks in `0xDC80A0`, `0xDC8370`, `0xDC9170` |
| One-time drops missed in a session | flag setter hook (`rules::after_set_flag`), awards at `0xBC23C0` and `0xBC1D46`, floor drop at `0xE806BD`, roll `0x47F640` (`drops.cpp`) |
| Bonfires and NPC talk | `HasPartyMembers` (`0xD2A4B0`) at the bonfire object (`0xD58AE4`) and talk script function 0x34 (`0xD4ABFB`) |
| Multiplayer walls | Lua `GetMultiWallNum` (`0xD5DC40`) answers 0 in every co-op role: `OnEvent_4034` (`OnSessionJustIn`) raises the walls on the host's side (`IsClient()` false), which every player then meets |
| Progress | the event-flag setter (`0xD38CB0`): changes are shared, and a guest's are also written to their own world's store, except the maps' disaster flags |

### One-time drops

Some enemies never come back: their map script removes them on every load once their flag is on (Force Character
Death without souls, `0xD66600`, or disabled), and it pays for the death once, with an award into the inventory
(Award Items (Including Clients) 2003[36], Award Item Lot 2003[04]: the Black Knights, crystal lizards, drakes,
the Anor Londo gargoyles) or the enemy's own NpcParam lot (`itemLotId_1`) on the floor where it dies (Titanite
Demons, mimics, Havel, Ricard, Pinwheel's masks, the Stray Demon's slab, the Taurus and Capra Demons' weapons). The
bosses' rewards come from common event 970 when their flag turns on. Each game of a session rolls its own drops from
its own copy of the enemy: the award runs in every game whose copy dies, and the floor drop (`0xE80670`, from the
death animation) only while that game's own player is alive. A player whose game was loading, whose game had not
loaded that map (DS1 streams map blocks: the Burg and Firelink run together, Lost Izalith does not run in a game at
the Burg), or who was down (spectating) got nothing, and the enemy is gone for good in the host's world. A floor
drop also went with the next load, and a session forces loads (a teammate's Homeward Bone, a team wipe).

`drops.cpp` keeps a table of these drops (`drops_table.inc`, generated from every map's script, MSB and the params:
141 rows of {the flag that is on once it is gone, the enemy's entity, its floor lot, the lots its script awards},
common event 970's rows without an enemy). The flags are shared in a session (a guest's go through the host), so
every game sees each such flag turn on, whoever killed the enemy and wherever. When one does (the setter hook,
`rules::after_set_flag`, our own scripts' or another player's applied), the row waits: 12 s for our game's own roll
(the death animation, our copy's death, event 970's second and third lots 5 s after the first), and then for the
enemy to be gone in our game (not loaded, dead, a load since, or 90 s). Our own rolls are seen at the award (the
event running it, `0xBC3AB0`'s instance: id at +0x28, slot at +0x2C, names the row; event 970's awards go to the
boss row armed last) and at the floor drop (`0xE806BD`, the character matched by entity). Whatever our game did not
roll, the mod rolls with the game's own roll (`0x47F640`: the lot and the lots after it, the player's item discovery
from `0xDC9FC0`, entries already taken skipped by their one-time flag; 0x50-byte entries, decoded as `0x4810E0`
does) and puts in the inventory (`0xC0B700`, as the co-op items) with the game's item-get window (see Items),
setting the entries' flags as the award (`0xDC9170`) does, once the player stands in the world alive; the action box
adds "From a kill you missed". In a session a one-time enemy's floor drop goes into the inventory at once, so no
load can take it off the floor before it is picked up. A drop the mod gave is never given twice: our game's late
award or floor drop of it is skipped while its flag is on (a flag found off means a new playthrough), also after the
session. A missed roll is only decided during the session, when the hooks see our game's own rolls: when it ends
(or the player quits to the title), a drop still waiting is ours if its enemy is not loaded in our game (or a load
cut a boss's event 970 short); an enemy still loaded pays as the game does if it dies later. A drawn item that does
not fit in the inventory stays owed (not rolled again) and is tried every 5 s ("No room in your inventory for ...");
an entry's one-time flag is set only once all its items are in. What is owed is kept through a return to the title
and given when the same character is back in the world. An invader's game keeps the vanilla drops. Tail cuts (the Drake Sword, the Gargoyle Tail Axe, the Moonlight Greatsword) are not
in the table: they are paid on the cut, not the death.

### World rules

- **Rest:** Lua `Util_RequestRegene` (`0xD5D820`, the bonfire rest event) first gives the items and withers the
  blossoms, then tells the others, who reset their world the way a rest does (`WorldChrMan+0x2CC`) and wither their
  blossoms too (`world::wither_blossoms`). The health the debuff took comes back: once the debuff's effect is off and
  the game has raised the max HP, the player gets 10% of the new max per Blossom, up to the max. A death whose reload
  starts the world over (a team wipe, or no co-op teammate in the session) withers them when its warp goes. The bonfire's
  "enemies about" test (`0xE3DF90` from `0xD58B49`) also answers yes while a living teammate reports a boss gauge
  (the 1.1.0 player report), so nobody rests during someone else's boss fight. A rest after a lost boss fight also
  reloads every game in which the boss is awake (see Spectating). The same answer stands a sitting player up: every
  shop of the bonfire talk leaves through its closing states once the bonfire's button is unavailable (talk function
  37), the game's own way out when an enemy comes near. While the local player sits past their rest (the sitting
  loop, or kindling or Reverse Hollowing played seated, with a second's grace between them) the teammate's fight is
  not added, so a player who sat down first keeps the bonfire. Sitting down (the rest happens during it), the first
  lighting, standing up and the warp animation keep the lock.
- **Bonfires:** `WarpNextStage_Bonfire` (`0xD62CA0`, from `Lua_Warp_1`; its argument is the bonfire object - 980) and
  `Util_RequestLevelUpFirst` (`0xD5D850`) are mirrored. A guest's warp does not go ahead: it becomes a request
  (message 25) that the host's game checks against its own world (flag 710, the Lordvessel; the warp list at
  `0x12DE888`, 20 entries {flag 200-219, bonfire object, text}, whose flag a rest sets through `0xC40650`) and then
  carries out for everyone, or turns down (message 26). A guest's warp list (`0xC40A50`, count `0xC406D0`) is answered
  from the host's flags. The bonfire menu's warp rows (710, 717) stay shut during a living teammate's boss fight, and
  for a guest when the host has no Lordvessel. Every warp, the host's own and a guest's request, and the
  Homeward Bone (outside a boss fight: `session::bone_hold`) wait on `session::travel_hold` (a boss fight, an invader, a lost fight's reload here or in a teammate's
  game, a fallen teammate), so no travel load replaces a respawn or a reload that is waiting. Following waits while a
  menu is open (`world::menu_open`), in a bonfire animation, in a cutscene, or while dead (the respawn goes first,
  with its own waits); the host takes a guest's travel only out of its menus and bonfire animations, and its own warp
  (animation 7725) replaces a waiting one. A trip blocks any other for 5 s, since its load begins frames later.
  The bone's trip takes that slot the moment it is used (a guest's travel asked for in the same tick would warp the
  host twice: the messages are read before the host's tick), replaces a guest's waiting travel, and checks the
  holds again when it goes on the next tick; the costs are taken only then. The Darksign waits only on a pending
  lost fight's reload (`session::darksign_held`); with an invader in the world it sends each one message 29 first (an
  older invader, which ignores it, gets the host's goodbye 5 s later). Each guest reports its Homeward Bones behind
  the player report (1.3.0, `InfoMore3`), and the trip's message (12) carries the cost: one bone, or the Darksign's
  souls and humanity, taken by each guest's game for itself (`items::take`, the game's RemoveItem; a guest who is
  loading when the bone comes has no character to take it from, and pays once standing). The miracle's trip carries
  neither cost: a 1.3.0 message with both bytes clear (a 1.2.0 host's shorter one is its bone). A guest who is down
  when the trip comes has its held death (or limbo) let go at once, and no new hold until it lives
  (`spectate::trip_release`): its own boss gauge stays while its body lies in the arena, so the fight would never end
  for it. A guest's load
  that follows the bone or the Darksign starts the host's world's temporary flags over, as the host's load at home
  did, so a boss fight left this way starts from its beginning in every game. In a session the game's
  `UseGoods` (`0xE50C70`, which applies the bone's effect and uses it up) is skipped, so a refused trip costs nothing.
  A co-op guest's bone, Darksign or miracle (1.3.0) is message 32 to the host (its kind), sent only to a host whose
  state says it takes them (`StateMore4`); the host's game runs the same checks and trip (`start_homeward`), waits
  for its own menu or bonfire animation as for a guest's travel (its own warp replaces the waiting trip), and
  answers a refusal with message 26 (now also `kTravelNoBone`, followed by the id of the player who has no bone:
  `TravelAnswerMsg`). The guest keeps one ask open until the host's game answers it (other than "waiting"), its trip
  comes, or 5 s pass with no answer (65 s after a "waiting"); the Homeward miracle stays closed meanwhile
  (`session::trip_slot_busy`). A trip whose guest is no longer admitted is called off before any cost. A host who is
  down when a guest's Darksign, or a guest's bone or miracle in a boss fight, goes is not warped: its held death is
  let go (`spectate::trip_release`, `world::trip_release`), and that respawn is at the trip's bonfire, its own last.
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
- **Party HP across areas** (ds1sc shows the HP of players in other areas) is not implemented yet.
- **Invasion rewards:** the Remastered mod's covenant reward for a winning invader is not reproduced; the side that
  wins an invasion gets Humanity instead (`InvasionRewards`).
- **Item descriptions** are written for the port; names, one-line infos and icons are the original's.
- Ghosts, messages and bloodstains of players with the same password still appear (ds1sc has none).
- The Remastered mod's GitHub ban list is not used.
- **Teams** ("allies in teams") put the host against the guests; the Remastered mod's exact team split could
  not be reproduced with PTDE's team table.
- The Remastered mod's character-type condition list (`EcTestChrType`) is not decoded yet.
- **The Cursed Pendant** also works without a session, and its curse lasts until the pendant lifts it (the
  Remastered mod's curse ends with the session).
- **Posture scaling works** (1.3.0): an enemy's poise grows by `EnemyPostureScaling` / `BossPostureScaling`
  percent of its own poise per extra player. The Remastered mod writes the posture into its rows'
  `changeSuperArmorPoint` as `(short)(1 + n * percent)`, a point or two, and leaves their `stateInfo` at 0,
  while the game adds that field only for effects of type 155 (the Wolf Ring's), so its posture setting
  changes nothing. The port keeps those rows and replaces the poise sum (`0xDBF700`: the effects' poise
  points, added to the character's own, vtable `+0x160`, NpcParam `+0x118` for an enemy) so that our rows add
  their percent of the enemy's own poise.

## Troubleshooting

`PTDE-FPS-Unlock.log` has `[coop]` lines for every step (Steam ready, session state, joins, kept sessions,
each item used, each hotkey press). A `Tick running` line every minute shows the co-op module is alive. If
any hook does not match the executable (it expects the 2015 Steam build), Prepare to Co-op turns itself off and
says which.
