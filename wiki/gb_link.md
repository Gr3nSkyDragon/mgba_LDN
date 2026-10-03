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

A slave that is not armed when the master clocks a byte reads `$FF` from the master's point of view. The mGBA lockstep driver
returns `$FF` to a master whose partner has not armed `rSC` (`master_rx_ff_slave_idle`, `src/gb/sio/lockstep.c`). That edge case is
the cause of a false handshake when the partner has just booted.

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

- Both consoles set `hSerialConnectionStatus` = `$FF`, put `$02` in `rSB` and arm the external clock; the receptionist loop also
  alternately sets `$01`/`SC_INTERNAL` each frame [src: `engine/link/cable_club_npc.asm:20-60`].
- Whoever receives `$02` in the interrupt handler was the one clocking, so it is the master (`hSerialConnectionStatus` = `$02`); the
  other receives `$01` and becomes the slave (`$01`).
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

Hook names in brackets are the VC hooks that sit at the same place in the ROM. See [The 3DS Virtual Console link](vc_link.md).

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

**[measured]** In the stable trade the "Please wait" step takes on the order of 175 `fe` polls from the slave while the master sends
`60`. The wait is the 768-loop counter: 768 frames is roughly 12.8 s at 60 Hz [src-derived, not timed].

## 2. The nybble exchange (`Serial_SyncAndExchangeNybble`)

Used for every small decision (menu, trade confirm, mon pick) [`home/serial.asm:232-285`]. It exchanges four bits wrapped as
`$60 + nybble`:

- send byte = `wSerialExchangeNybbleSendData + $60` (so `$60..$6F`),
- a received byte counts only if its high nybble is `6` (`Serial_ExchangeNybble.doExchange`), anything else is ignored,
- repeat every frame until a valid nybble is received (the `$FF` sentinel in `wSerialExchangeNybbleReceiveData` clears),
- then `Serial_ExchangeNybble` 10 more frames and `Serial_SendZeroByte` 10 frames. [`Wireless_net_delay_3` and `_4` change
  both counts to **26** on Red/Blue VC.]

**[measured]** trade confirmation, per round: `60`×13 `00`×10 `62`×111 `00`×10 ... The long `62` run is the polling loop waiting for the other
side to reach the same screen; the `00×10` blocks are the zero-byte phases.

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

`Serial_ExchangeLinkMenuSelection` sends the byte **twice** (`wLinkMenuSelectionSendBuffer` and `+1`) and reads twice, discarding the first
as possibly stale. The top nybble must be `$D`. If both press A or B in the same cycle, the master's choice wins (`USING_INTERNAL_CLOCK`
tie-break). After the choice the master waits two extra frames before it clocks again.

**[measured]** `d0 d0 00 d0 d0 00 ... d4 d4 d4 00 00 fe` on the master, one pair per menu cycle.

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
While `hSerialIgnoringInitialData` is set the received byte is dropped until `$FD` arrives, so a block does not start until the
partner's preamble appears. The preamble that triggers it stays in the output, so the buffer begins at the first non-`$FD`
byte after the run. The receiving side keeps what comes after the preamble.

### The RN list (17 bytes)

    FD FD FD FD FD FD FD   r0 r1 r2 r3 r4 r5 r6 r7 r8 r9

- 7 × `$FD` (`SERIAL_RN_PREAMBLE_LENGTH`), then 10 random numbers, each below `$FD` (rerolled otherwise) [`cable_club.asm:26-48`].
- **[src]** The numbers are the RNG seed for the battle. Both consoles use the **master's** list (`cable_club.asm:147-155`).
- **[measured]** Receive side shows `fd ×12` in front of the RN list as well (the slave's first-seen preamble stretched by idle `$FD`s).

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

Counting the preamble gives the full 424-byte span. **[measured]** The block run begins `fd×6 a7 a8 b1 ae 50 00×6 06 ...` (trainer
"hiro", count 6) with the species list `6f 95 4a 83 31 15 ff` in the Azahar capture, and the span from the first `fd` to the
last `ff ff ff` of the block to the next `fd` run is 427 exchanges, 3 more than the 424-byte block. The 3 extra exchanges sit between the
blocks (`50 ff ff` on one side) and their origin is [unknown]. See [measurement notes](#measurement-notes).

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

A party block cannot contain `$FE`, because it means "no data". Before sending, `CableClub_DoBattleOrTradeAgain` scans the first
252 bytes after the preamble in part 1 and the rest in part 2, replaces each `$FE` with `$FF`, and records its **1-based offset** in the
patch list [`cable_club.asm:55-101`]. The receiver puts `$FE` back at every recorded offset.

    FD FD FD   (7 x 00)   [part 1 offsets] FF   [part 2 offsets] FF   00 ... (zeros to length 200)

**[measured]** `fd fd fd 00×7 ff ff 00×190` when no `$FE` was in the party (both lists empty, which is the usual case).

### The trailer

After the block exchanges, both ends reload `rIE` and the master's RN list is used by both consoles. The slave copies the
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

`Wireless_net_stop` / `Wireless_net_end` mark `LinkMenu` exits [`engine/menus/main_menu.asm`]. In the ROM they just stop the exchange loop and clear `BIT_LINK_CONNECTED`.

# Measurement notes

- **Phase landmarks in a byte stream.** Runs of `$FD` mark block starts: RN list (7-8), player block (6-9), patch lists (3-4).
  The fd run lengths in traces vary because idle `$FD` bytes from the previous exchange extend the run on one side.
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

- Which of the two consoles is the master when both ROMs reach the room at once: this is settled by the `$01`/`$02` handshake
  on hardware, but the VC skips that handshake (see the VC page).
- The exact behaviour of `wUnknownSerialCounter2` in a real cable.
- Everything for Gen 2.
