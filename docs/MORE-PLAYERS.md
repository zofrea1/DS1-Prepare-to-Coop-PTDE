# More than four players (MaxPlayers)

Status in 1.4.0: **experimental, off by default** (`MaxPlayers = 4`). Stage 1 is built and checked offline against the
game's own code; it has not run with five or more players yet.

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

## Not widened yet (stage 2)

- **The party list** (`PartyMemberInfo`, `[GameMan+0xBEC]`): four entries of 0x14 inline from +0x18, with its own
  fields right after them (+0x74 NPC count, +0x78, +0x7C scripted summon, +0xAC). Its add (`0xEA2510`) looks for a
  free entry and refuses when there is none, so a fifth player simply has none: **no floating health bar** over them,
  none of the game's party bookkeeping (white/black/total counts stop at four, the summon effect and the "phantom
  died" states). The mod's own code (deaths, spectating, scaling, travel) does not use it. Widening it means moving
  either the entries or the fields after them and every access to them (`0xEA18A0`..`0xEA3390`, the bar filler
  `0x1012C30`, party.cpp, summons.cpp).
- **The floating health bars** (`MenuMan+0x8F4 + 0x20 * slot`, four records inline, one widget each): need more
  records and widgets, fed from the party list.
- **A per-slot table at `SessMan+0x1B4`** (packet type 4 values, three dwords): `0xD280E0` keeps `cmp eax, 2`, so the
  values of players past the fourth are dropped rather than written past it. What the game uses them for is not known.
- NPC summons take party entries too: with four players or more present they have none (no floating bar), as with
  four players and an NPC in 1.3.0.

## Testing

Five or more copies of the game, each with its own Steam account, all on 1.4.0 with `MaxPlayers` set at least as high
as the session will be. Things to watch, in order:

1. With `MaxPlayers = 8` and only two to four players: nothing changes (start-up line "More players: up to 8 in a
   session"; joining, deaths, leaving, team wipes, invasions as before).
2. The fifth player joins: every game makes their character (log: "Other players' characters here"), they move,
   fight, die and respawn; enemies scale (log: "Enemy scaling level 4 (5 co-op player(s))").
3. Leaving and joining again in every order (slots are reused); the host closing the world with five in it.
4. Invaders with a full co-op party; the game's session limit is the same for them.
