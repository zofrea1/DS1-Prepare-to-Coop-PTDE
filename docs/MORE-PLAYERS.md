# More than four players (MaxPlayers)

Status in 1.4.0: **on by default** (`MaxPlayers = 8`). Stages 1 and 2 are built and checked against the game's own
code, ran with five players on 2026-10-31 (below: what that test found), and with eight on 2026-11-02. `MaxPlayers = 4`
keeps the game's own number.

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
