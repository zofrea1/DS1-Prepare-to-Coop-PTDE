# More than four players (MaxPlayers)

Status in 1.4.0: **on by default** (`MaxPlayers = 8`). Stages 1 and 2 are built and checked against the game's own
code, ran with five players on 2026-10-31 (below: what that test found), and with eight on 2026-11-02. `MaxPlayers = 4`
keeps the game's own number.

Status in 2.7.0: **up to 64** (`MaxPlayers` 2 to 64, default still 8). Everything below that held eight holds 64 with
the same patches; the party list, the floating health bars, the game's network heap, the menus' draw buffers and the
enemy scaling needed more (see [64 players](#64-players-270)). Tested in the game with NPC phantoms standing in for up
to 61 other players; not yet with more than eight real players.

## Is eight harder than six?

No. The hard part is going past four at all; the number above it is a constant. PTDE has no limit tied to a player
count of six or eight: the game session's room is made with whatever member limit the session manager holds
(`SessMan+0x8C`, set to 4 at `0xD2CB1B` and passed to the room as attribute `0x80000001` in `0xD2D4F0`), its members,
packets and send queues are lists, player ids run to `0x7FFE`, character handles keep 14 bits of index and network
handles 11. What holds four are fixed arrays in the game's managers, and each is widened by the same patch whatever the
number. Eight costs more upload (every player sends to every other: about 7 x 2 KB a second at 30 updates a second)
and needs eight copies of the game to test.

## What holds four, and what stage 1 does

| Where | The game's | Stage 1 |
|---|---|---|
| Session member limit `SessMan+0x8C` | 4 (`0xD2CB1B`) | MaxPlayers |
| Other players' records, `GameDataMan+0xC` | one block of 3 x 0x4C0 (`0xC06710`: 0x10 + 0xE40 bytes, count at block+0xC) | MaxPlayers - 1 records, constructed by the game's own loop |
| Their state bytes, `GameDataMan+0x10` | 3 bytes (0x1F = ready to be made; a slot is the first free byte, `0xC02CA0`) | MaxPlayers - 1, cleared once the manager exists |
| Their inline bytes, `GameDataMan+0x84 + slot` | 3 bytes (`+0x88` is another field) | moved to an array of our own (6 indexed and 6 unrolled writes) |
| Slot checks | 16 x `cmp reg, 2 / ja`, 3 x `cmp reg, 3`, 9 x `cmp reg, 0E40h` | widened |
| Session start / join resets (`0xD29940`, `0xD2D200`) | written out for 3 slots | the call that follows each (`0xD29590`) clears the extra slots first |
| Readers written out for 3 slots | `0xC5AF40` (co-op records in use), `0xD59B7E` (a chr type 0xC record), `0xD6A850` (records of a summon type) | replaced by loops (assembly that touches only the registers the game's did) |
| Player characters, `WorldChrMan+0x24` | a set of 4 (`0xE63C90`: 4 x 0x20 entries) | MaxPlayers |
| Joining | the host's 4 | the host admits past four only when every game in the session, the joiner's too, reports room (`HelloMore` / `InfoMore4.max_players`) |
| Enemy scaling | rows for 1..5 extra players | rows for 6 and 7 (ids 2147418010..13) |

Everything is checked against the bytes it replaces before anything is written; a build of the game that differs
anywhere keeps four. The member limit is written last.

## Stage 2: the floating health bars

The HUD already has **seven** floating health bars: their records (`MenuMan+0x8F4 + 0x20 * slot`) and the widget
(`0xFA89C0`) and its filler (`0x1012C30`) all count to seven. What held them to four is the filler's source: bar
`slot` showed party entry `slot` while `slot` was below the party's count, and the party list (`PartyMemberInfo`,
`[GameMan+0xBEC]`, 0x8C bytes, four entries of 0x14 inline from +0x18 with its own fields right after) holds four,
entry 0 our own character, which draws nothing. The filler's two reads of the party (its count at `0x1012C47`, the
entry's handle at `0x1012CDA`) take ours instead, at four players too: every other player's character first (the
player set, under the handle the game made it with), then the party's members but us in the party's order, which
adds its NPC phantoms, seven at most. A player never goes without a bar for an NPC: with three players and Solaire,
the game's own filler gave Solaire a party entry's bar and a fourth player none; with eight players and Solaire,
the eighth had none. An NPC gets a bar when one is left after every player, and gets it back when a player leaves.
The nameplate text (OverheadPlayerDisplay) follows the same list. The log says which bars draw ("Floating health
bar N draws").

## The party list: eight members (partylist.cpp)

The party list (`PartyMemberInfo`, `[GameMan+0xBEC]`) holds every member of the world in the game's own way: each
player and each NPC summon has an entry {character handle, type 0 own / 1 player / 2 NPC, state, f1, f2}, and the
game's summon handling runs on it (the summon-in animation and its flag, the NPC's death and departure, the "send
phantoms home" menu, the white/black/intruder counts the sign limit reads). It had four entries: with four players
and an NPC, or more players, the last to come had none, and an NPC summoned into a full list none of that handling
(an ally only its summoner's game could see). It has eight now, at any `MaxPlayers` (installed from DllMain with the
rest, before the game makes GameMan; all or nothing, checked against the game's bytes; the log says "Party list: 8
members").

- **Layout.** 0x8C bytes: a vtable, five counts (+4 .. +0x14), the four entries (+0x18 .. +0x67), and the list's own
  fields after them: the NPC summons' three handles (+0x68 .. +0x70) and their count (+0x74), the scripted summon
  (+0x78 .. +0x84; +0x7C is the slot every "is client" test reads) and a debug menu (+0x88). Eight entries need
  +0x18 .. +0xB7, so those fields move before the list instead: the block (`0xEBB55B`) is 0xDC bytes, the list's
  pointer 0x24 into it, and each field is at its old offset - 0x8C (-0x24 .. -0x04). Every access to one of them is
  the same instruction with its displacement changed, an 8-bit displacement staying 8-bit, so nothing is moved or
  trampolined. GameMan's destructor gets the block back for the heap (`0xEBA598`, `0xEBA5B2`).
- **What changed.** The entries are reached by index or by a pointer stepped 0x14 at a time, so only their bounds
  change: 19 bounds (4 to 8, the compactions after a leave or a reset 3 to 7, and the leave's count of slots
  after the first) in the constructor's vector, the add, the leave, the reset, the update, the recount, the resend
  to a joiner, the dismissal and packet 49's receiver (`0xEA16F0` .. `0xEA2FD0`). The fields: 61 accesses in the
  party's code and the vtable destructor, 21 tests of +0x7C elsewhere (three of them under the mod's loot hooks,
  which read the moved slot), one `add eax, 0x68`. 104 sites in all, with the allocation's size. The blank of the
  whole list (`0xEA1900`, written out for four entries) is replaced. The add's room test (`0xEA252A`) compares with
  the eight entries instead of the session's member limit, and so does the touch summon (`0xDB3590`, also every NPC
  invasion) for an NPC's sign: an NPC does not take a player's place, it needs an entry. An NPC also needs one of
  the list's three NPC handles: the touch summon stores its handle right after the test (`0xEA1770`, which stores
  nothing once the three are taken, and the summon's add at `0xDB3E2F` ignores a refusal), so an NPC's sign is
  refused when the eight entries or the three handles are full, and the add refuses an NPC whose handle is not one
  of the three when they are. A player's sign keeps the game's test (`SessMan+0x8C`). The mod's own blank of an
  entry takes an NPC's handle out of the three as the game's leave does (`0xEA1720`): the party is kept through
  loads in a session, so the reset that drops them does not run, and stale handles would refuse every NPC.
- **How it was found.** A sweep of the party's code (`0xEA1600` .. `0xEA3400`, every displacement 0x68 .. 0x8B and
  every bound 3/4, each checked by hand: the same displacements on characters, vtables and other managers are left
  alone) and of the 76 reads of GameMan+0xBEC elsewhere (only the counts, entries bounded by +0x10, and +0x7C; one
  getter `0xEB87B0` and three dead helpers have no callers). The mod's own reads follow `partylist::entries()` and
  `field()`.
- **Left at four.** The debug menu's per-member items and one debug menu callback (`0xDB2080`, three bytes per
  member) that retail never draws. The NPC summons' own list stays three: the game's phantom limit.
- **With eight players and an NPC.** Eight entries hold eight; an NPC summon's sign is refused then (no entry is
  left). A player who joins a world of seven players and an NPC has no entry, as a fifth player had before: the
  bars come from the players' characters anyway, and the mod's own death and spectating rules replace the party's
  for co-op players.

## Not widened

- **A "recently met" list at `SessMan+0x1B4`** (three names, its own bound) and the values `0xD280E0` writes into it
  per slot (`cmp eax, 2` kept): players past the fourth are not added. Harmless.

## 64 players (2.7.0)

**Is 64 the limit?** No: none of the game's limits that were found is tied to 64. Steam allows lobbies of up to 250
members (both the game's own session lobby, made with the session's member limit, and ours). What 64 bounds is the
widening itself: the party list's bounds are 8-bit immediates (up to 127 entries) and the bars' slot bounds too, and
64 is what was asked for and tested. The real cost of a large session is upload (every game sends to every other: 63
other players at 30 updates a second is about 125 KB a second each way, double at 60) and the work of drawing and
updating every other player's character.

What changed for more than eight:

- **The game's player structures** (players.cpp): the same patches as for eight, with 64 in them (the remote player
  records' block 63 x 0x4C0, 63 state bytes, the inline slot bytes in an array of 64, the character set 64 x 0x20, the
  slot bounds 62 / 63). All are 8-bit bounds or 32-bit sizes.
- **The network library's sessions** are made for `MaxPlayers + 1` people (65), as for eight. Its member pool is made
  up front: 65 places take about 64 KB each of the network heap.
- **The network heap** (made once at start-up) is 16 MB up to eight players and 1 MB more for each player above eight:
  72 MB at 64. Measured: a session of one with `MaxPlayers = 64` uses 6.4 MB of it (67,219 KB free of 73,728); each
  player in a session took about 0.2 MB more in the eight-player tests, so a full session of 64 should use about 20 MB.
- **The party list** (partylist.cpp) has 72 entries: 64 players and eight more for NPC phantoms. Its bounds are now
  computed from `kWide` (the table holds the eight-entry values; each bound is the game's plus `kWide - 4`) and the
  block is 0x24 + 0x18 + 72 x 0x14 bytes. The NPC summons' own list stays three (the game's phantom limit).
- **The floating health bars** (players.cpp): the HUD's seven bars are seven dialogs of class
  FrpgMenuDlgFloatingPCGauge, one menu-file definition ("F21-00") seven times in a plain dialog inside the HUD, each
  drawing the record of its slot (MenuMan+0x8F4 + 0x20 x slot, slot < 7 in the gauge's update and placing, a null read
  past seven). The records move into an array of 71 of ours (every read of MenuMan that feeds a record's address takes
  the array's instead: the update, the placing, the filler's four, a getter and a setter; the bounds 6 become 70), and
  more bars are made in the HUD's dialog the way the menu loader makes a nested dialog of a class: the gauge from its
  class's constructor, its init from the same definition, a nested-dialog shape around it and a piece holding it, at
  the end of the dialog's piece list (grown by the game's own allocation), its slot set. The HUD frees them with its
  own; each load's new HUD is grown again (59 bars made in 0.1 ms at the load). As many are made as a full session needs
  (`MaxPlayers - 1` and three NPC phantoms: 10 at eight players, 66 at 64); a bar not in use is not drawn. The
  filler's line-of-sight test (one bar a frame, going round seven) goes round the bars in use: with many players a
  bar hidden behind a wall can take longer to go (61 bars: 0.2 s at 285 FPS, 2 s at 30 FPS).
- **The menus' draw buffers**: the menus record their drawing into one of three command buffers a frame, made at
  start-up with 256 KB each and written without a room check (a full one hands out a null place). 66 bars overran
  them in the first try; they are 1 MB each now (3 MB in all, from the menus' heap).
- **Enemy scaling** (world.cpp): rows of our own for 8 to 63 extra players (SpEffect ids 2147418100 + n for enemies,
  2147418200 + n for bosses), the same formula as before (1 + extra players x `EnemyHealthScaling`, and so on), so
  with the default 25% an enemy has x16.75 health with 64 players.
- **The co-op's own lists** that were sized for eight: the gait table (63 entries), the party slots, the character
  list, the nameplates, the settings page (`MaxPlayers` choices up to 64), the session limit.
- **Kept at eight**: the group invasion messages hold eight Steam ids (their layout is the protocol's; an older game
  reads the fields after them at the same place), so a group of more than eight invaders is held for eight.
- **The host's hand-out of enemies** (handout.cpp, handout_pair.h): every second the host's game settles each pair of
  members' bid lists (every enemy of a member's loaded map blocks, with its bid) so that each enemy goes to the highest
  bid. The game's loop walks the later member's list for every entry of the earlier one's, for every pair: players^2 x
  enemies^2, and most when players are in different places (their lists share nothing, so no walk stops early). The
  loop is replaced with one that gives the same results in the same order (the same entries leave the same lists, the
  ties read the same counts) in time linear in the entries: each list indexed once a hand-out (a member's list is a
  "later" list in all its pairs before it is ever the earlier one, and changes meanwhile only by the removals, which keep
  the index), and a pair whose lists share no enemy is skipped (lists only lose entries during the loop, so such a pair
  changes nothing). Checked against the game's own machine code of the pair step on a million random hand-outs (every
  list, count and freed entry identical) and in the game on 300 more made in its network heap. Measured in the game,
  160 enemies a player:

  | Players | Game's loop | Ours |
  |---|---|---|
  | 8 in one place / 8 in eight places | 0.17 / 0.67 ms | 0.03 / 0.008 ms |
  | 32 in one place / 32 apart | 0.79 / 12.2 ms | 0.14 / 0.04 ms |
  | 64 in one place / in 8 places / all apart | 1.7 / 7.6 / 49.9 ms | 0.43 / 0.31 / 0.19 ms |

**Stress test** (2026-11-24, diagnostics builds only: remote commands `npcs N`, `perf S`, `bars 0|1`, stress.cpp).
With no 64 games to join, the Undead Burg's own characters stand in: `npcs N` brings the N nearest to the player, makes
each a white phantom with the game's own steps (chr type 1 and its team, as a summon sign does; the human NPCs that
summons are can do it through the game's own call, the hollows need its steps applied directly), and adds each to the
party list as an NPC member (0xEA2510, a live summon: the "Phantom summoned" notice). Each takes a party entry and a
floating bar. With 61 phantoms the party held 62 members and 61 bars drew (records checked in memory), and the game
ran on without fault, through loads and with the HUD grown again after each.

Frame time at the Undead Burg's statue courtyard, 1280x720, uncapped, this PC (Ryzen 7 5800X3D), 10 s each:

| Phantoms | Mean frame time | FPS |
|---|---|---|
| 0 | 3.51 ms | 285 |
| 8 | 3.71 ms | 270 |
| 15 | 3.91 ms | 256 |
| 30 | 4.91 ms | 204 |
| 47 | 5.74 ms | 174 |
| 61 | 6.72 ms | 149 |

About 0.05 ms a phantom: each is a whole character the game animates, runs AI for and draws. A real player's character
runs no AI (the network moves it) but is a heavier model (armour, FaceGen), so expect the same order. Of that, the
floating bars are 0.1 to 0.17 ms for 60 (bars on 6.79 / 6.91 ms, off 6.68 / 6.75 ms). The extra bars cost nothing while
nobody uses them: with no phantoms, `MaxPlayers = 64` (66 bars) ran at 3.509 ms and `MaxPlayers = 8` (10 bars) at
3.512 ms (`MaxPlayers = 4`, the game's seven, 3.474 ms).

Not tested: more than eight real players (the network library, the host's hand-out of enemies to every game with
real lists, every game's upload). Nothing found in the game's session or network code holds a count of members in a bit mask or a
fixed array; the eight-player test is the evidence so far.

## The five-player test (2026-10-31)

Five games (four PCs and a Steam Deck under Proton, the Deck the fifth to join) played the Undead Asylum, the Asylum
Demon with two players in its fight, the crow to Firelink Shrine and two invasions of the full party. Joining,
the widened records, slots and party list, the bars, the cutscene follow and the invasions held. What went wrong,
none of it in the widened parts:

- **Characters paired with the wrong players** (`session.cpp`, update_remote_players). A lost boss fight's reload
  brought all five back at one bonfire, and each game matched the others' characters to their players by the
  positions they report: three games got some wrong. The session member records, which the game pairs with its
  characters itself, were compared with character names only, while they hold Steam names. Now the member's Steam
  id comes first (session member id +0: `DLNRD::SteamPlayerData`, vtable `0x10FB604`; its +0xC
  `SteamOnlineIDData`, vtable `0x10FD7FC`, holds the 64-bit id at +0x28, as `0x715EE0` reads it), then the
  record's Steam name; a position match is refused while two players stand within 3 m of each other.
- **What followed from it.** When 6700k fell, three games took the dying character for a living player's and rebuilt
  it while 6700k still lay dead (`party.cpp` rebuilds a character whose player is back on their feet). The host then
  had no character for 6700k and sent its game no more enemy lists (0xE57850 sends a member's list only while the
  member has a character), so 6700k's game kept the Asylum Demon for five minutes while three other games took it in
  turn; the Deck's copy took both games' updates and jumped between two places and two attacks every frame. The
  others froze the demon in its last attack for 22 s first: an attack another game moves held every bid for 20 s,
  updates or not. Two fallen players later spectated a character that bore the last standing player's name but was
  another fallen player's, and saw "Waiting for the fight to be decided".
- **Guards added** (`rules.cpp`): a guest's game whose last list from the host is more than 5 s old gives up an enemy
  of its list as soon as another game sends that enemy's position (hook at `0xE57C3F` for the list, the receiver's
  mark on the entry for the position); another game's attack holds an enemy only while that game's updates for it
  come (1 s).
- **Living players dying on the Deck alone.** Three times in its first three minutes the Deck ran a living player's
  death on its copy: an update from that player said health 0 (NetworkManipulator `0xE1F9E4`, the sample's +0x14),
  the next said 594, and the copy stayed dead, unseen, its party entry "dying". No other game saw it, and the cause is
  not known yet. Such an update is now held back while the player's own game says they live (1.5 s at most), and
  logged with its bytes.
- **DynamicSmoothing** (`netrate.cpp`) counted the receiving game's own frame stalls and frame steps as jitter; the
  Deck's smoothing values jumped about on the health bars. Gaps across a long frame of ours are left out, deviations
  within half a frame are not counted, and the smoothing falls back 2.5 times faster.
- Not from the widening: the invader's "other world" was another Prepare to Co-op session (an invader's search does
  not filter by password); the Dried Fingers' 1.5 weight gave the party's world 60 % of each search (2 of 7 picks:
  about a 1 in 10 run).

## The second five-player test (2026-11-01)

The host and two guests at Firelink Shrine, two guests in the Undead Burg (the Hellkite's flyover, the Taurus Demon
alone), an invasion of the party by the Deck. Every character was matched by Steam id in every game, and none of the
first test's faults came back. Found:

- **The host's souls from far kills** (`rules.cpp`, hk_kill_reward at `0xE78AB1`). Every game pays its player the
  souls of an enemy that dies in it (`0xE78A40`). The host's game takes the hits on enemies it no longer runs
  (WorldChrMan's update gives a character whose map entity is not active the rate -1 at chr+0xF4: no update), and its
  copy dies at the killing blow; the guests' games at Firelink had no such death. The souls and the Ring of the Evil
  Eye's heal are now skipped for an enemy at rate -1, in the host's game and the guests'.
- **Enemies sliding** (`netrate.cpp`): the NetAIManipulator's move (`0xE20C50`) takes the frame's step times 30
  (`0xE20E92`) against 2.5 m/s for walk or run; at 90 or 144 FPS a running copy walked. The factor is 1 / dt now.
- **The faulting reads at loads** were our own character's effect checks (the Crimson Blossoms' debuff, Chameleon)
  in its first frames (chr+0x1E0 not yet a `SpecialEffect`, vtable `0x11932B8`); the effect list is read only behind
  that vtable.
- **Not known yet:** an enemy at Firelink whose health was gone in two games fought on and never died (two 20 s holds
  of its attack 3040); an early Burg ambush that seemed to wake as a plain enemy (11015110 or 11015111; neither
  event's animation ran in any game). Both are logged now: an enemy at 1 HP or less that still attacks or has not
  begun to die, the details of a 20 s attack hold, and every event that switches an ordinary enemy's AI on.

## Testing

Five or more copies of the game, each with its own Steam account, all on 1.4.0 with `MaxPlayers` set at least as high
as the session will be. Things to watch, in order:

1. With `MaxPlayers = 8` and only two to four players: nothing changes (start-up line "More players: up to 8 in a
   session"; joining, deaths, leaving, team wipes, invasions as before).
2. The fifth player joins: every game makes their character (log: "Other players' characters here"), they move,
   fight, die and respawn; enemies scale (log: "Enemy scaling level 4 (5 co-op player(s))").
3. Leaving and joining again in every order (slots are reused); the host closing the world with five in it.
4. Invaders with a full co-op party; the game's session limit is the same for them.
