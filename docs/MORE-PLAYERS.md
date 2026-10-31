# More than four players (MaxPlayers)

Status in 1.4.0: **experimental, off by default** (`MaxPlayers = 4`). Stages 1 and 2 are built and checked offline
against the game's own code; they have not run with five or more players yet.

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

## Not widened

- **The party list itself.** Its add (`0xEA2510`) looks for a free entry and refuses when there is none, so a fifth
  player has no entry: none of the game's party bookkeeping (its white/black/total counts stop at four; the summon
  effect and "phantom died" states, which the mod's own death and spectating rules replace for co-op players). Moving
  its entries means about thirty accesses inside its methods and nine outside, and twenty-two outside read the field
  after them (+0x7C); not needed for anything a player sees.
- **NPC summons** take party entries: one summoned while four players or more are present has none (no floating
  bar, and the game's own end-of-fight dismissal may not run for it in every game), as with four players and an NPC
  in 1.3.0. One summoned earlier keeps its entry, and its bar while one is left after every player.
- **A "recently met" list at `SessMan+0x1B4`** (three names, its own bound) and the values `0xD280E0` writes into it
  per slot (`cmp eax, 2` kept): players past the fourth are not added. Harmless.

## Testing

Five or more copies of the game, each with its own Steam account, all on 1.4.0 with `MaxPlayers` set at least as high
as the session will be. Things to watch, in order:

1. With `MaxPlayers = 8` and only two to four players: nothing changes (start-up line "More players: up to 8 in a
   session"; joining, deaths, leaving, team wipes, invasions as before).
2. The fifth player joins: every game makes their character (log: "Other players' characters here"), they move,
   fight, die and respawn; enemies scale (log: "Enemy scaling level 4 (5 co-op player(s))").
3. Leaving and joining again in every order (slots are reused); the host closing the world with five in it.
4. Invaders with a full co-op party; the game's session limit is the same for them.
