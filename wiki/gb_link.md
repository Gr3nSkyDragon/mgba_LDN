---
title: The Game Boy link
parent: Pokémon Red and Blue
nav_order: 1
---

# The Game Boy link, and what runs on it

Read out of [pret/pokered](https://github.com/pret/pokered) (commit `d2704a6`) and measured from mGBA traces of a Red cartridge
ROM linked to a Blue cartridge ROM (`logs/gbtrace_20261003-154856.jsonl`, `src/gb/linktrace.c`). Every claim is tagged:

- **[src]** read from the pret source, with `file:line` where it helps.
- **[measured]** seen in a trace.
- **[unknown]** not yet established.
- **[user-reported]** stated by the person running the captures, not found in the source or a trace.

Only Gen 1 (Red/Blue) is covered. Yellow is the same code apart from the constants noted in the last section. Gen 2 is listed as
a to-do at the end.

# The hardware link

## One byte per transfer, both directions at once

The DMG/GBC link port is a shift register. A transfer swaps one byte: the byte in `rSB` goes out while the partner's byte comes
in. Both consoles always send and receive at the same moment, so there is no such thing as "send now, receive later" at this layer.
`rSC` bit 7 starts a transfer, bit 0 selects who drives the clock [src: `home/serial.asm`].

| role | `rSC` value to arm | clock |
|---|---|---|
| master (internal clock) | `SC_START \| SC_INTERNAL` | the console generates it, 8192 Hz, about 1 ms per byte |
| slave (external clock) | `SC_START \| SC_EXTERNAL` | waits for the master's clock |

On real hardware a master that clocks a byte while nothing is armed on the other end reads `$FF` (the idle line). The mGBA lockstep driver
used to hand such a master the partner's stale last byte instead, which let a console that had only just booted look like a partner and
produced a false handshake; it now returns `$FF` to a master whose partner has not armed `rSC` (`master_rx_ff_slave_idle`,
`src/gb/sio/lockstep.c`).

## Serial interrupt handler

`Serial` runs on every completed transfer [src: `home/serial.asm:1-52`]:

1. Latch `rSB` into `hSerialReceiveData` ($FFAD), put `hSerialSendData` ($FFAC) into `rSB` for the next transfer.
2. If the console is the slave, re-arm `rSC` with the external clock.
3. Set `hSerialReceivedNewData` = 1, and reset `hSerialSendData` to `$FE` (no data).

Until the connection is established (`hSerialConnectionStatus` = `$FF`), the handler uses the received byte to pick the role (below).

## Constants

| name | value | meaning [src: `constants/serial_constants.asm`] |
|---|---|---|
| `ESTABLISH_CONNECTION_WITH_INTERNAL_CLOCK` | `$01` | byte the master sends in `rSB` to introduce itself |
| `ESTABLISH_CONNECTION_WITH_EXTERNAL_CLOCK` | `$02` | byte the slave sends in `rSB` |
| `USING_EXTERNAL_CLOCK` | `$01` | value in `hSerialConnectionStatus` for the slave |
| `USING_INTERNAL_CLOCK` | `$02` | value in `hSerialConnectionStatus` for the master |
| `CONNECTION_NOT_ESTABLISHED` | `$FF` | `hSerialConnectionStatus` before the handshake |
| `SERIAL_PREAMBLE_BYTE` | `$FD` | starts a block |
| `SERIAL_NO_DATA_BYTE` | `$FE` | idle / nothing to send |
| `SERIAL_PATCH_LIST_PART_TERMINATOR` | `$FF` | ends a patch list part, and replaces `$FE` inside data |
| `SERIAL_PREAMBLE_LENGTH` / `SERIAL_RN_PREAMBLE_LENGTH` | 6 / 7 | `$FD` count before the party block / RN list |
| `SERIAL_RNS_LENGTH` | 10 | random numbers in the RN list |

## Role handshake

The two bytes `$01` and `$02` swapped at the start decide who is the master.

- Every iteration of the receptionist's 90-frame loop sets `hSerialConnectionStatus` = `$FF`, puts `$02` in `rSB` and arms the external clock, then
  overwrites `rSB` with `$01`, starts an internal-clock transfer and waits a frame [src: `engine/link/cable_club_npc.asm:20-60`]. So each console
  offers both roles every frame and whichever transfer completes first against an armed partner decides who is master.
- The handler stores the byte it received as the new `hSerialConnectionStatus`. The console that receives `$02` (the other side's external-clock
  byte) is therefore the master (`$02`, internal clock); the console that receives `$01` becomes the slave (`$01`).
- **[measured]** The first bytes in the trace: the master's stream begins `01 00 00 00 60...`, the slave's begins `02 00 fe fe 00...`.
  Everything after that is symmetric.

## The slave never knows it is waiting

`Serial_ExchangeByte` [`home/serial.asm:88-170`] loops until `hSerialReceivedNewData` is set. Two counters inside it are the only
timeouts in the byte layer:

- `wUnknownSerialCounter` (16-bit): decremented while the byte received is `$FE`, set to `$FFFF` when it reaches zero. This is
  the "link inactive" detector. The cable club sets it to `$0300` before the "Please wait" sync (below).
- `wUnknownSerialCounter2` (16-bit, reloaded `$0050`): only counts while `rIE` enables serial and nothing else. Its exact purpose
  is [unknown].

Everything else waits forever. A stalled partner never causes a crash, but it makes the game sit in a polling loop.

# The cable club, from the receptionist to the table

Hook names in brackets are the VC hooks that sit at the same place in the ROM. See [The 3DS Virtual Console link](vc_link.md); the addresses and what the mGBA wrapper does at each hook are in [How the mGBA wrapper uses the Game Boy link](#how-the-mgba-wrapper-uses-the-game-boy-link).

## 1. The receptionist (`CableClubNPC`)

[src: `engine/link/cable_club_npc.asm`]

1. Loop up to `wLinkTimeoutCounter` = **90 frames** trying to establish the connection. Each iteration sets `rSB`=`$01`/`rSC`=internal,
   calls `DelayFrame`, and checks `hSerialConnectionStatus`. [`Link_fake_connection_status`, the VC forces it to `$02` here.]
   On timeout the text is "This area is reserved for 2 friends who are linked by cable" (`CableClubNPCAreaReservedFor2FriendsLinkedByCableText`).
2. On success: `Serial_SendZeroByte` twice, 50 frames, then "Please apply here / have to save" and a Yes/No.
3. Yes: save the game [`Wireless_TryQuickSave_block_input`], set `wUnknownSerialCounter` = `$0300` (768 loops), then
   `Serial_SyncAndExchangeNybble` [`Wireless_prompt`, `Wireless_net_recheck`].
4. If the counter is still `$FFFF` afterwards the partner never answered: 10 zero bytes, `CloseLinkConnection`, and the text
   "Link closed because of inactivity".
5. Otherwise go to `LinkMenu`.

The step ends when both consoles reach it; the `$0300` counter is only the **limit**: if the partner has not answered within 768 loops (about 12.8 s at
60 Hz, derived from the source and not timed) the link is closed with the "inactivity" text.

**[measured]** In the mGBA trade the master sent `60` about 186 times while the slave, whose player had not reached the screen yet, answered `fe` about 175 times;
the exchange completed as soon as the slave sent its own `60`.

## 2. The nybble exchange (`Serial_SyncAndExchangeNybble`)

Used for every small decision (menu, trade confirm, mon pick) [`home/serial.asm:232-285`]. It exchanges four bits wrapped as
`$60 + nybble`:

- send byte = `wSerialExchangeNybbleSendData + $60` (so `$60..$6F`),
- a received byte counts only if its high nybble is `6` (`Serial_ExchangeNybble.doExchange`), anything else is ignored,
- repeat every frame until a valid nybble is received (the `$FF` sentinel in `wSerialExchangeNybbleReceiveData` clears),
- then `Serial_ExchangeNybble` 10 more frames and `Serial_SendZeroByte` 10 frames. [`Wireless_net_delay_3` and `_4` change
  both counts to **26** on Red/Blue VC.]

**[measured]** After the patch lists the master's stream in the mGBA trade is `60`×13 `00`×10 `62`×111 `00`×10 `62`×24 `00`×10 `62`×13 `00`×12. Each run of `6x`
is one nybble sync (first the mon pick, then the confirm, then the post-trade syncs), each `00` run is the ten-frame zero-byte phase, and the long `62` run is the polling
while the other player decides. The Virtual Console sends far fewer repeats of the same nybble (see the VC page).

## 3. The link menu (`LinkMenu`)

[src: `engine/menus/main_menu.asm:134-280`] The player picks Trade Center / Colosseum / Cancel. The selection byte is:

    $D0 + ((A or B pressed) << 2) + menu item

| byte | meaning |
|---|---|
| `$D0` | cursor on Trade Center, nothing pressed |
| `$D1` | cursor on Colosseum |
| `$D2` | cursor on Cancel |
| `$D4` | A pressed on Trade Center (commit) |
| `$D5` | A pressed on Colosseum |
| `$D8` | B pressed (cancel) |

`Serial_ExchangeLinkMenuSelection` holds the byte twice (`wLinkMenuSelectionSendBuffer` and `+1`) and does **three** exchanges, discarding the first
reply as possibly stale and keeping the other two ("sent thrice and read twice" in the source). The top nybble of a kept reply must be `$D`. If both press A or B in the same cycle, the master's choice wins (`USING_INTERNAL_CLOCK`
tie-break). After the choice the master waits two extra frames before it clocks again.

**[measured]** The mGBA master sends `d0 d0 d0 00` once per menu cycle (three exchanges), ending `d4 d4 d4 00 00 fe` when A is pressed on Trade Center. The Virtual Console
sends `d0 d0 00` per cycle, two exchanges.

## 4. Entering the room (`CableClub_DoBattleOrTrade`)

The player walks into the Trade Center and sits at the table. The ROM then runs a fixed exchange sequence
[src: `engine/link/cable_club.asm:4-150`, label `CableClub_DoBattleOrTradeAgain`]. This is the same routine called again after every trade.

| step | what | length | VC hook |
|---|---|---|---|
| a | `Serial_SyncAndExchangeNybble` | nybble | `Wireless_WaitLinkTransfer` |
| b | master sends two `$00` bytes, 1 frame apart (sync) | 2 | |
| c | `Serial_ExchangeBytes` of the RN list | 17 | `Wireless_ExchangeBytes_RNG_state_unknown_Type5` |
| d | `Serial_ExchangeBytes` of the player block | 424 | `Wireless_ExchangeBytes_party_structs` |
| e | `Serial_ExchangeBytes` of the patch lists | 200 | `Wireless_ExchangeBytes_patch_lists` |
| f | master's RN list copied into both consoles | | |

### Serial_ExchangeBytes

Not a plain loop [`home/serial.asm:56-110`]. Each byte goes through `Serial_ExchangeByte`, followed by a delay of 48 loop iterations.
While `hSerialIgnoringInitialData` is set, every received byte is dropped **and the pointer is not advanced**, so the console keeps sending its own first byte
(`$FD`) again and again until the partner's `$FD` arrives. That first `$FD` is dropped too, the flag is cleared, the first byte is sent once more, and from then on
received bytes are stored and the buffer advances. So a block only starts once both consoles have reached the exchange, and the run of `$FD` on the wire is longer than the
preamble by however many exchanges a console waited for its partner.

### The RN list (17 bytes)

    FD FD FD FD FD FD FD   r0 r1 r2 r3 r4 r5 r6 r7 r8 r9

- 7 × `$FD` (`SERIAL_RN_PREAMBLE_LENGTH`), then 10 random numbers, each below `$FD` (rerolled otherwise) [`cable_club.asm:26-48`].
- **[src]** The numbers are the RNG seed for the battle. Both consoles use the **master's** list (`cable_club.asm:147-155`).
- **[measured]** The `$FD` run in front of the RN list is 8 to 14 long in the traces, not 7: the extra `$FD`s are the sender repeating its first byte while it waited (see `Serial_ExchangeBytes`).

### The player block (424 bytes)

`SERIAL_PREAMBLE_LENGTH + NAME_LENGTH + 1 + PARTY_LENGTH + 1 + (PARTYMON_STRUCT_LENGTH + NAME_LENGTH*2) * PARTY_LENGTH + 3`
= 6 + 11 + 1 + 6 + 1 + (44 + 22) × 6 + 3 = **424** [`cable_club.asm:133`].

| offset | size | field |
|---|---|---|
| 0 | 6 | `FD` preamble |
| 6 | 11 | trainer name, Gen 1 charset, `$50` terminator, padded with `$00` |
| 17 | 1 | party count (1-6) |
| 18 | 6 | species ids (internal index numbers), then |
| 24 | 1 | `$FF` list terminator |
| 25 | 264 | 6 party mon structs of 44 bytes (`PARTYMON_STRUCT_LENGTH`) |
| 289 | 66 | 6 OT names, 11 bytes each |
| 355 | 66 | 6 nicknames, 11 bytes each |
| 421 | 3 | padding |

Counting the preamble gives the full 424-byte span. **[measured]** In the first Azahar capture the block run begins `fd×6 a7 a8 b1 ae 50 00×6 06 ...` (trainer
"hiro", count 6) with the species list `6f 95 4a 83 31 15 ff`. In the Virtual Console captures the span from the first `fd` of the block run to the first `fd` of the patch-list run
is 427 exchanges every time, 3 more than the 424-byte block; the 3 extra exchanges sit between the two blocks (`50 ff ff` on one side) and their origin is [unknown]. See
[measurement notes](#measurement-notes).

### The 44-byte party mon struct

[src: `constants/pokemon_data_constants.asm`]

| offset | size | field |
|---|---|---|
| 0 | 1 | species (internal index) |
| 1 | 2 | current HP |
| 3 | 1 | box level |
| 4 | 1 | status |
| 5 | 2 | type 1, type 2 |
| 7 | 1 | catch rate |
| 8 | 4 | moves |
| 12 | 2 | original trainer ID |
| 14 | 3 | experience |
| 17 | 10 | HP, Attack, Defense, Speed, Special EVs (2 bytes each) |
| 27 | 2 | DVs |
| 29 | 4 | PP of the four moves |
| 33 | 1 | level |
| 34 | 10 | max HP, Attack, Defense, Speed, Special (2 bytes each) |

**[measured]** The Oddish received by Azahar: `b9 00 14 0d ...` (species `$B9`, HP 20, box level 13); OT ID 31693; nickname bytes
`8e 86 7f 96 84 84 83` ("OG WEED" in the Gen 1 charset).

### The patch lists (200 bytes)

A party block cannot contain `$FE`, because it means "no data". Before sending, `CableClub_DoBattleOrTradeAgain` scans the **264 bytes of party mon structs**
(`wPartyMons` up to `wPartyMonOT`; names are not scanned) in two parts, the first 252 bytes and the remaining 12, replaces each `$FE` with `$FF`, and records its
**1-based offset within the part** in the patch list [`cable_club.asm:55-101`]. The receiver puts `$FE` back at every recorded offset.

    FD FD FD   (7 x 00)   [part 1 offsets] FF   [part 2 offsets] FF   00 ... (zeros to length 200)

**[measured]** `fd fd fd 00×7 ff ff 00×190` when no `$FE` was in the party (both lists empty, which is the usual case).

### After the exchanges

Both ends reload `rIE`. The master's RN list is used by both consoles: the master keeps its own, the slave copies the
other's list into its own `wLinkBattleRandomNumberList` [`cable_club.asm:147-155`].

## 5. The trade screen

[src: `engine/link/cable_club.asm:312-580`, `TradeCenter_SelectMon`] Every decision is one `Serial_SyncAndExchangeNybble`.

| exchange | send nybble | wire byte | meaning |
|---|---|---|---|
| pick a mon | 0-5 | `$60`-`$65` | `wTradingWhichPlayerMon` (party index) |
| pick Cancel | `$F` | `$6F` | leave the selection screen |
| confirm menu: accept | 2 | `$62` | `.tradeConfirmed` |
| confirm menu: decline | 1 | `$61` | `.tradeCancelled` |

Order: both players pick (`$60+idx`); a received `$6F` sends the game back to the start of the selection menu. Then both send
confirm (`$62`) or decline (`$61`). If the received value minus 1 is zero, the partner cancelled. A completed trade does
the in-ROM swap (`.doTrade`, copying the OT, OT ID and nickname), saves, and calls `CableClub_DoBattleOrTradeAgain` again, which exchanges
the new parties.

**[measured]** In the mGBA trace round 2 (after one trade) shows the full RN list, party block and patch lists again. The Azahar trace shows
three rounds (enter, after trade, after trade back).

## 6. Leaving

- **Link menu Cancel** (`.choseCancel` in `LinkMenu`, `engine/menus/main_menu.asm:289-300`): `Delay3`, `CloseLinkConnection`, the "Link canceled" text, clears
  `BIT_LINK_CONNECTED`. The two Virtual Console hooks `Wireless_net_stop` and `Wireless_net_end` sit on this path. It is the way out of the link menu, not out of the Trade Center.
- **The Trade Center** has no clean exit [user-reported, not in the source I read]: the way out is the Reset entry that replaces Save in the start menu. A session therefore has no teardown exchange.
- **Leaving the selection screen** is the `$6F` (Cancel) nybble. **[measured]** Every trade capture ends with a run of `6F` from both sides (4 to 7 exchanges).

# How the mGBA wrapper uses the Game Boy link

The wrapper (see [The 3DS Virtual Console link](vc_link.md#the-mgba-wrapper-a-game-boy-core-as-a-vc-joiner-measured-live)) leaves the ROM unmodified and runs its link code as it is. Only the places the VC itself replaces are hooked, with breakpoints at the pret `vc_hook` addresses;
everything else (`Serial_ExchangeByte`, `Serial_ExchangeBytes`, `Serial_SendZeroByte`, the receptionist's text, the trade screen) runs natively on a serial device whose transfers are paired with the peer's units.
Addresses are bank:address in the pret builds (`docs/hook_table`); a game is recognised by the first 11 characters of its header title.

| routine | Red and Blue | Yellow | in the wrapper |
|---|---|---|---|
| `Link_fake_connection_status` | 01:7202 | 01:7077 | sets `hSerialConnectionStatus` to `$02`; serial transfers are not paired until the next hook |
| `Wireless_prompt` | 01:7260 | 01:70D8 | ends that stretch |
| `Serial_SyncAndExchangeNybble` (`Wireless_WaitLinkTransfer`) | 00:227F, `ret` at 00:22C2 | 00:20DB, `ret` at 00:211E | replaced: exchanges `60 + nybble` units with the host (see the VC page) and returns the host's nybble |
| `Serial_ExchangeLinkMenuSelection` | 00:2247, `ret` at 00:226D | 00:20A3, `ret` at 00:20C9 | replaced: three selection units out, the last `D0`-class byte of three in |
| `DelayFrame` | 00:20AF | 00:1E64 | used to wait: a hook that cannot finish pushes its own address and jumps here |

| RAM | address (all three games) |
|---|---|
| `hSerialConnectionStatus` | `$FFAA` |
| `wSerialExchangeNybbleSendData` / `wLinkMenuSelectionSendBuffer` (two bytes) | `$CC42` |
| `wSerialExchangeNybbleReceiveData` | `$CC3E` |
| `wSerialSyncAndExchangeNybbleReceiveData` / `wLinkMenuSelectionReceiveBuffer` (two bytes) | `$CC3D` |
| `wUnknownSerialCounter` (16-bit) | `$CC47` |

A hook that finishes sets the registers the ROM routine would leave (for the nybble sync: A = the nybble, B = 0, Z set; for both, BC, DE and HL as on entry) and continues at the routine's own `ret`.
The other VC hooks (`Wireless_net_stop` / `Wireless_net_end` on the Cancel path, `Trade_save_game_end`, the battle exchanges and the 26-frame delays) are not needed for a trade and are not implemented. Yellow has the table
above but was not tried; Gold, Silver and Crystal (`POKEMON_GLD`, `POKEMON_SLV`, `PM_CRYSTAL`) are recognised and refused.

What a native transfer looks like to the ROM: the byte on the wire for transfer *k* is `rSB` when the transfer starts (the **previous** exchange's `hSerialSendData`: the sent byte lags one exchange, see the interrupt handler above),
and the byte received is the peer's unit *k*. `$FE` from the peer means "no data" and `Serial_ExchangeByte` retries after a frame.

# Measurement notes

- **Phase landmarks in a byte stream.** Runs of `$FD` mark block starts: RN list (7 plus repeats), player block (6 plus repeats), patch lists (3 plus repeats).
  Run lengths vary because a sender repeats its first byte while it waits for the partner's preamble (see `Serial_ExchangeBytes`).
- **Span from first `$FD` of a block to the first `$FD` of the next** is 17-18 (RN), 427 (player block), and 253-404 (patch lists
  plus the trade selection that follows them). The player-block 427 is 424 plus 3 unexplained exchanges before the patch-list preamble.
- **Round count.** Entering the room, and each completed trade, repeat the RN list / party / patch-list sequence.
- **The mGBA tracer** records both players' bytes in `hex` (sent) and `rx` (received) per hardware transfer, plus variable and
  buffer markers; see `docs/lua/gen1_hooks.txt` for the marker addresses (`ram` lines).

# Yellow, Gold, Silver, Crystal

- **Yellow [src]:** same protocol and constants as Red/Blue. Its VC build keeps the 10-frame nybble delays where Red/Blue use 26
  (`IF DEF(_RED_VC) || DEF(_BLUE_VC)`).
- **Gen 2 [unknown, not yet read or measured here]:** from the public GB-Link trade client
  ([GB-Link/gb-pokemon-web](https://github.com/GB-Link/gb-pokemon-web), GPLv3, used only as a reference): selections use the `$70` range
  instead of `$60`, the party structs are 48 bytes, there are 4 sections instead of 3 (mail is the fourth), and eggs exist. None
  of that is confirmed against pret or a trace yet.
- **Time Capsule [unknown]:** exchanges Gen 1 mons with Gen 2; depends on the same byte layer.

# Open questions

- The 3 unexplained exchanges between the player block and the patch lists in the Virtual Console captures (and why the cartridge trace shows the same 427 span). **[inferred]** They come from the ROM's own exchange loop, not from the VC: the wrapper runs that loop natively and the VC accepts the stream.
- Which console is master when both ROMs reach the receptionist at once: on hardware the `$01`/`$02` handshake settles it. The Virtual Console has no such bytes on the wire: its hook forces `hSerialConnectionStatus` to
  `$02` on both consoles, and its host sends a single `EF` as its first unit (see the VC page). The mGBA wrapper does the same hook and trades.
- The exact behaviour of `wUnknownSerialCounter2` in a real cable.
- Everything for Gen 2, including the Time Capsule room and whether Gen 2 has a clean exit.
