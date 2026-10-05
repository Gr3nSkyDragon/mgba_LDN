---
title: The Game Boy link
parent: Pokémon Red and Blue
nav_order: 1
---

# The Game Boy link, and what runs on it

Read out of [pret/pokered](https://github.com/pret/pokered) (commit `d2704a6`), measured from mGBA link traces of a Red cartridge
ROM linked to a Blue cartridge ROM (the tracer is `src/gb/linktrace.c`; one capture had Red as master and each side pressing first),
and, for the permanent-slave behaviour at the end, measured live with the wrapper trading against a 3DS (Azahar and retail). Every claim is tagged:

- **[src]** read from the pret source, with `file:line` where it helps.
- **[measured]** seen in a trace.
- **[unknown]** not yet established.
- **[inferred]** follows from the source or from several measurements but was not observed directly.
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

`Serial_ExchangeByte` [`home/serial.asm:88-170`] loops until `hSerialReceivedNewData` is set. What it does with a stalled partner depends on
which interrupts are enabled (`rIE`):

- **Two counters.** `wUnknownSerialCounter` (16-bit) is decremented each time the byte received is `$FE` and set to `$FFFF` when it reaches
  zero: the "link inactive" detector. The cable club sets it to `$0300` before the "Please wait" sync (below). `wUnknownSerialCounter2`
  (16-bit, reloaded to `$5000`) counts down only while `rIE` enables serial and nothing else, which is the state inside
  `Serial_ExchangeBytes`; when it runs out (about 20,000 polls) the wait ends and the stale received byte is returned. **[src]**
  So a block exchange has a timeout and the other waits do not.
- **`$FE` is retried, or it is data.** When the received byte is `$FE` the routine returns it as it is if `rIE` is serial only (the block
  exchanges); in every other state it reloads the byte to send, waits a frame (`DelayFrame`) and exchanges again. **[src]** The consequence for
  anything that plays the partner: `$FE` is "no data, ask again" in the nybble syncs and the link menu, and it is stored as a data byte
  inside a block.

Apart from those, everything waits forever: a stalled partner never causes a crash, but it makes the game sit in a polling loop.

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
is 427 exchanges every time, 3 more than the 424-byte block, and the master's run of `fd` in front of the player block is 9 long, not 6. **[inferred]** That is
`Serial_ExchangeBytes` at work: the exchange that ends the sender's ignoring, and the repeats before the partner's `fd` arrives, are sent as well as the preamble, and
the receiver stores 424 replies after the first `fd` it sees, so the 424th stored byte is the first `fd` of the next block. The receiver skips leading `fd` bytes when it unpacks the block
(`cable_club.asm:152-180`), so a few extra only cost the last few bytes of the block; many more (18 in one live run) cut into the party data. See [measurement notes](#measurement-notes).

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

The wrapper (see [The 3DS Virtual Console link](vc_link.md#the-mgba-wrapper-a-game-boy-core-as-a-vc-joiner-measured-live)) leaves the ROM unmodified. It has two ways of
working, and the first is the one the project moved towards: a Game Boy core is only a stand-in for a cartridge.

## Wire mode: a cartridge as master, the wrapper as a permanent slave

This is the default mode (the environment variable `MGBA_VCLINK_WIRE=0` selects hook mode instead). mGBA calls the wrapper at one place only: when the ROM starts a serial transfer (a write of `$81` to `rSC`). The wrapper is
told the byte the master clocked out and answers with the byte the slave shifts back; it has no debugger, no breakpoints and no knowledge of ROM addresses (a heartbeat reads a few
RAM bytes for the log, nothing more). The same code (`uds-wire.c`, `uds-cable.c`) can sit behind a pin-level front end on a microcontroller. **[measured]** Trades complete against Azahar and a retail 3DS, Red and Blue.

The cartridge is always the master and the 3DS side is always the slave: the wrapper offers `$02` to the master's `$01` and never drives the clock. A real master cannot be paused, and a slave's
reply is loaded before the master clocks, so the reply to exchange *i* depends on bytes up to *i*-1 and on what the 3DS has delivered by then. What that forces, **[measured]**:

| phase | what the master does | what the wrapper does |
|---|---|---|
| role | the receptionist loop offers `$01` every frame | replies `$02`, then `00`, then `FE`; no unit goes to the 3DS (the VC has no role bytes) |
| idle | stale `00` / `FE` bytes between phases | drops them; a `6x` starts a sync, a `Dx` a menu, anything else a raw exchange |
| sync | `Serial_SyncAndExchangeNybble`: `60+n` until a `6x` reply, ten more, ten zero bytes (the tenth is stranded into the next burst) | the cartridge's loop counts are fixed and the 3DS's burst is another length, so the 3DS side of the sync runs by itself and the cartridge is given synthetic replies that end its loops with the 3DS's nybble |
| menu | three exchanges per call, `00 d d d` from a master; the slave's matching cycle is `fe d d 00` | one unit per `Dx` byte, replies the 3DS's selection; if the 3DS pressed A first, its further menu units are answered with its choice (the echo) until it has been quiet for 600 ms |
| gap | no exchange for 400 ms | ends a menu or a block exchange; the first byte afterwards is the byte stranded by the lag and is sent as a unit |
| blocks | three blocks per cycle, 17, 424 and 200 replies stored after an `fd` | see below |

Rules that fell out of the source and the live runs:

- **The slave's data lags one exchange.** The byte set for exchange *n* is shifted out in *n*+1, so every phase starts with a stale byte and ends with one stranded in the shift register.
- **Replies must never run dry inside a block.** The cartridge stores `size` replies after the `fd` that ends its ignoring and, with only the serial interrupt enabled, an `FE` is stored as data,
  not retried. So the first `fd` of a block is held back (`FE`) until the whole block has been received from the 3DS, and a reply is never missing inside it. If only the last byte is missing it is
  filled in, and the 3DS's matching unit is dropped later.
- **Units are index-paired, so a block must start at the same index on both sides.** The wrapper's unit *k* is what the 3DS receives in its exchange *k*. The 3DS stores 17, 424 or 200 bytes after the first
  `fd` it sees, so the cartridge's block has to begin exactly where the 3DS's own begins. Padding units sent while the cartridge waits (one `fd` per retry at first) displaced the cartridge's
  random-number list by 25 positions; the 3DS started its player block inside the padding and read a random number as the first byte of the name, giving a different garbled trade menu every run.
- **While the cartridge waits for a block, run in lockstep.** A retry after `FE` (same byte, previous exchange unanswered) is not a new exchange and sends nothing; the `fd` the cartridge repeats goes out only
  as the 3DS's matching unit arrives. Bytes that are real data (the tail of the previous block, stale bytes) always go out.
- **The random-number list is the exception.** The 3DS produces its list one unit per unit of ours, and the cartridge is held until the list is buffered, so only the wrapper can supply the units that
  let the 3DS finish it. Once the 3DS's list starts the wrapper sends `fd` for its remaining 17 positions and drops the cartridge's own list units. The list has no consequence for a trade (in the hook mode's trades the cartridge sent 37 to 44 `fd` in front of its list, so the 3DS's copy was `fd` too, and
  they work), and the player block then begins at the right index. The 3DS produces the player and patch blocks in one burst after its first `fd`.
- **`FF` is an idle line.** While the Pia session is not up the cartridge sees no partner.
- **Gen 2** (the game table says so; Gen 1 never takes these paths): syncs in the `$70` and `$80` ranges are syncs too, and each sync's replies use the range the
  cartridge started it in (a 3DS still in another range is ignored, as the ROM's own code does); after the patch lists the Trade Center's **mail block** is
  carried with every exchange sending its unit (an `FE` is stored there, so none is a retry; a live run that suppressed them left the 3DS's mail exchange
  29 units short and stalled both games) and a `00` stand-in for a reply that is not there yet, the real unit being dropped when it arrives. When the host's game
  leaves the room its VC sends a short end-of-session record on the system stream; the wrapper answers with its own, then leaves the network as a 3DS joiner does and
  does not rejoin for six seconds. The host's VC closes the network about five seconds after its own record whatever the partner does.

## Hook mode (the earlier design, kept as the fallback)

The ROM runs its link code as it is and only the places the VC itself replaces are hooked, with breakpoints at the pret `vc_hook` addresses; everything else (`Serial_ExchangeByte`, `Serial_ExchangeBytes`,
`Serial_SendZeroByte`, the receptionist's text, the trade screen) runs natively on a serial device whose transfers are paired with the peer's units. Because the emulator can stop the ROM at a hook, nothing
has to be buffered ahead, which is exactly what a real cartridge could not allow. Addresses are bank:address in the pret builds; a game is recognised by the first 11 characters of its header title.

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
above but was not tried. Gold, Silver and Crystal (`POKEMON_GLD`, `POKEMON_SLV`, `PM_CRYSTAL`) are recognised and refused in hook mode: they run in wire mode only.

What a native transfer looks like to the ROM: the byte on the wire for transfer *k* is `rSB` when the transfer starts (the **previous** exchange's `hSerialSendData`: the sent byte lags one exchange, see the interrupt handler above),
and the byte received is the peer's unit *k*. `$FE` from the peer means "no data"; `Serial_ExchangeByte` retries after a frame outside a block exchange and stores it inside one (see above).

# Measurement notes

- **Phase landmarks in a byte stream.** Runs of `$FD` mark block starts: RN list (7 plus repeats), player block (6 plus repeats), patch lists (3 plus repeats).
  Run lengths vary because a sender repeats its first byte while it waits for the partner's preamble (see `Serial_ExchangeBytes`).
- **Span from first `$FD` of a block to the first `$FD` of the next** is 17-18 (RN), 427 (player block), and 253-404 (patch lists
  plus the trade selection that follows them). The player-block 427 is 424 plus 3 unexplained exchanges before the patch-list preamble.
- **Round count.** Entering the room, and each completed trade, repeat the RN list / party / patch-list sequence.
- **The mGBA tracer** records both players' bytes in `hex` (sent) and `rx` (received) per hardware transfer, plus variable and
  buffer markers.

# Yellow, Gold, Silver, Crystal

- **Yellow [src]:** same protocol and constants as Red/Blue. Its VC build keeps the 10-frame nybble delays where Red/Blue use 26
  (`IF DEF(_RED_VC) || DEF(_BLUE_VC)`).
- **Gen 2 [src: pret pokegold and pokecrystal; measured]:** the byte layer (`Serial_ExchangeByte`, `Serial_ExchangeBytes`, `$FD`/`$FE`, the preamble and
  the retry rule) is the same code as Gen 1. The link layer differs:
  - **Nybble syncs:** one exchange per frame, `$60`+n in the Time Capsule and before a link mode is chosen, `$70`+n in the Trade Center, `$80`+n in the
    Colosseum (`LinkTransfer`); the loop structure (loop until a valid reply, ten more, ten zero bytes) is Gen 1's. A reply in another range is ignored.
  - **Room choice:** the receptionist's choice is confirmed by `Link_EnsureSync`, which sends `$D0`+room (`$D1` Trade Center, `$D2` Colosseum) twice per call and
    reads the partner's; there is no cursor menu. **[measured]** `d1 d1` and `d2 d2` on the wire.
  - **Blocks:** the random-number list (17), the party block (450 bytes: 6 preamble, 11 name, count, 6 species, terminator, 2-byte id, 6 × 48-byte structs, 6 OT
    names, 6 nicknames, 3 padding) and the patch lists (200), as in Gen 1, then in the Trade Center a **fourth block, the mail** (390 bytes: 5 × `$20` preamble,
    messages, metadata, patch set). The mail is exchanged by a plain `ExchangeBytes` with **no preamble check**: the first reply of any value ends the ignoring
    and every reply after it is stored, `FE` included. Battles have three blocks.
  - **Exit:** `WaitForOtherPlayerToExit` is a fixed sequence of transfers and delays (no waiting for the partner); the VC hooks its end (`Wireless_term_exit`).
- **Time Capsule [user-reported]:** a Gen 2 game exchanges Gen 1 mons with a Gen 1 game; an Azahar Gold to mGBA Yellow trade through the Time Capsule worked (2026-10-05).

# Open questions

- Which console is master when both ROMs reach the receptionist at once: on hardware the `$01`/`$02` handshake settles it. The Virtual Console has no such bytes on the wire: its hook forces `hSerialConnectionStatus` to
  `$02` on both consoles, and its host sends a single `EF` as its first unit (see the VC page). The hook mode does the same; the wire mode instead keeps the cartridge as master and answers its handshake itself.
- Whether the timing of a real cartridge on a microcontroller (a reply window of about a millisecond) holds up: wire mode has run only against an emulated cartridge, where the wrapper reacts in real time but the ROM
  runs at emulated speed.
- Whether a real console's `wUnknownSerialCounter2` timeout (block exchanges give up after about 20,000 polls) is ever reached in a trade; it was not seen.
- Whether a GB-Link adapter's Game Boy mode can carry wire mode. Its public firmware lists that mode (`0x02`) as an SPI passthrough, so the byte-level rules above would have to live on the board or the host; untested.
- Gen 2 is tested only in the Trade Center (Gold with Silver) and through the Time Capsule (Gold with Yellow); Crystal, battles and the mail block with real mail are not.
