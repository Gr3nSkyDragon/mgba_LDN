---
title: The 3DS Virtual Console link
parent: Pokémon Red and Blue
nav_order: 2
---

# The 3DS Virtual Console link, and what runs on it

Measured from one capture: Azahar (VC Red, `uds-real` branch, joiner) trading with an unmodified retail 3DS (VC Red, host) over the
air, one trade and one trade back. Preserved original: `logs/azahar/azahar_log_original_trade_20261003.txt`
(SHA-256 `18c6b5ef...`); translated copy: `logs/azahar/azahar_trade_20261003.jsonl`; reproduce with
`docs/azahar/azahar_log_to_jsonl.py` and `docs/azahar/align_azahar_mgba.py`. The Game Boy side is on
[The Game Boy link](gb_link.md). Tags as there: **[measured]**, **[src]** (pret source or the VC hook table), **[unknown]**.

One capture is one capture. Anything marked [measured] holds for this session only until a second one agrees.

# What the VC is

The Game Boy VC titles run the original ROM in an emulator that lives in the app's `code.bin`. For link play the emulator patches
the ROM at the pret `vc_hook` / `vc_patch` points and replaces the cable with the 3DS local-wireless service (UDS, the same layer
Gen 6/7 trades use). The patch table is in `docs/hook_table/hook_table.json` (generated from the pret sources).

## What the hooks replace [src]

| hook | where (Red) | what the VC does |
|---|---|---|
| `Link_fake_connection_status` | `CableClubNPC` | sets `hSerialConnectionStatus` = `$02` (internal clock) so the receptionist passes "Please wait" |
| `Wireless_TryQuickSave_block_input` | `CableClubNPC` | blocks input around the save |
| `Wireless_prompt` / `Wireless_net_recheck` | `CableClubNPC` | the "Please wait" nybble sync and its result check |
| `Wireless_WaitLinkTransfer` / `_ret` | `Serial_SyncAndExchangeNybble` | whole nybble exchange |
| `Wireless_ExchangeBytes_RNG_state_unknown_Type5` | `CableClub_DoBattleOrTradeAgain` | the RN list (17 bytes), type 5 |
| `Wireless_ExchangeBytes_party_structs` | same | the player block (424 bytes), type 4 |
| `Wireless_ExchangeBytes_patch_lists` | same | the patch lists (200 bytes), type 4 |
| `Wireless_net_stop` / `Wireless_net_end` | `LinkMenu` | leave the link |
| `Trade_save_game_end` | `TradeCenter_Trade` | post-trade save |
| `Wireless_start_exchange` / `_end_exchange`, `_start_send_zero_bytes` / `_end_...` | `LinkBattleExchangeData` | battle byte exchanges |
| `Wireless_net_delay_1..4` (byte patches) | nybble sync, battle exchange | frame delays: **26** frames on Red/Blue (**10** on the cartridge) |
| `Change_link_closed_inactivity_message` | text | replaces the "inactivity" text |

**[measured]** The wire does not carry the hook events themselves. It carries one record per serial exchange (below), and the
byte values match the cartridge's, so the VC re-creates the cable stream inside the emulator and sends that. The block hooks
are visible only as the points where the same bytes show up on the wire.

# The UDS session

## Joining

From the Azahar log, times relative to the first data frame:

| t | event |
|---|---|
| -10.1 s | `NWM::Initialize` (version `0x0400`) |
| -5.3 s | first beacon seen from the host, channel **6**, comm ID **`0x00171010`** |
| -1.9 s | beacon decrypted: `maxNodes=2`, application data **16 bytes**, network ID (random per session; `0xD7578A27` here) |
| -0.2 s | `ConnectToNetwork`: key derivation input is 12 bytes, ASCII `TRL_NETWORK` + NUL |
| -0.06 s | association request, EAPOL 4-way handshake (CCMP) |
| -0.02 s | `GetConnectionStatus`: status 9, **this console is node 2, 2 nodes total**, bitmask `0x0003` |

- **Roles.** The retail host is **UDS node 1**, the joiner node 2. Max 2 players.
- **Comm ID `0x00171010`.** The title ID is `0004000000171000` (the data folder name of Red); the comm ID shares its upper bits with the title ID.
  Whether Blue, Yellow and Gold/Silver/Crystal share it is [unknown] (H3).
- **Application data.** 16 bytes in the beacon. Its contents, and whether the VC checks them on join, are [unknown] (H4).
- The beacon, association and EAPOL are standard UDS and are handled by the platform, not by the VC.

All VC traffic uses UDS **channel 243** ("GAME"). Nothing else crosses it.

# The frames

Every UDS payload on channel 243 starts with a 24-byte header.

## Frame header (24 bytes) [measured]

| offset | size | field |
|---|---|---|
| 0 | 2 | `01 01` constant |
| 2 | 2 | little-endian, **frame length - 12** (96-byte frame: `54 00`) |
| 4 | 6 | zeros |
| 10 | 2 | message type (table below); constant for a given type |
| 12 | 4 | session id (`32 ab 98 64` for the whole capture) |
| 16 | 1 | `01` |
| 17 | 1 | sender id byte: `AA` in Azahar's frames, `2C` in the host's. `00`/`01` in the first setup frames |
| 18 | 2 | big-endian frame counter per sender (rises by 1-5 per data frame: 25, 27, 29, ... 990) |
| 20 | 4 | changes every frame, increasing smoothly, looks time-based. Meaning [unknown] |

The 4-byte field at 20 and the type id do not look constant by design. Neither was derived. [unknown] whether either is part of any check.

## Message types [measured]

Counts are Azahar-sent / host-sent over 245 s.

| type | length | role | count (tx / rx) | cadence |
|---|---|---|---|---|
| `589a` | 96 | **data**, 1 unit | 297 / 251 | one per exchange while the game is live |
| `01ca` | 1440 | **data**, 25 units | 113 / 115 | bursts (re-send window) |
| `0c3c` | 1384 | data, 24 units | 6 / 0 | |
| `51f6` | 208 | host: control, every 10 s. Azahar's 3 frames of this length are 3-unit data frames | 3 / 28 | |
| `7606`, `7bf0`, `4e72` | 656, 600, 320 | data, 11 / 10 / 5 units | 0 / 3, 0 / 2, 0 / 2 | |
| `59c3` | 84 | **acknowledgement** | 305 / 183 | about 10 per second while exchanging (median 83 ms from Azahar, 117 ms from the host) |
| `5865` | 76 | keep-alive / status | 334 / 239 | median 0.7 s from Azahar, 0.9 s from the host |
| `5f41` | 60 | keep-alive | 103 / 43 | every 2 s from Azahar, every 4 s from the host |
| `5a28` | 120 | control | 6 / 10 | about every 2 s |
| `5ad7` | 132 | control | 3 / 8 | |
| `5ee7`, `5435`, `5b71`, `593c`, `5b8e`, `556c`, `5721`, `c594`, `913f` | 68, 140, 124, 104, 112, 168, 188, 20, 52 | session setup, seen in the first 5 s | 1-5 each | |

Data frames are `24 + 56 x units + 16` bytes. Control frames carry no units. The 1440-byte frames are 25 units, the largest observed.

The control frame bodies (after the 24-byte header) share a pattern **[measured, partly understood]**:

- byte 0 is `00`; byte 1 is `01` in Azahar's frames and `00` in the host's;
- bytes 2-3, big-endian, are the inner length, equal to `frame length - 60` for 60, 68, 76, 84, 140 and 208 byte frames;
- bytes 4-7, big-endian: `1` in Azahar's frames and `2` in the host's. That is the **destination** UDS node id: Azahar sends to the host
  (node 1), the host sends to Azahar (node 2). The same value is at unit offset 7 (below);
- the rest is type-specific and mostly not decoded.

## Setup sequence [measured]

| t | frames |
|---|---|
| 0.00 | host broadcasts a 52-byte frame (destination 65535); Azahar sends 20 bytes |
| 0.08-0.35 | 124 B, 68 B and 140 B each way. The 140 B frame is repeated by Azahar at 0.18, 0.69, 1.19, 1.69 s until answered |
| 0.30-1.5 | first 76 B frames each way |
| 1.36-1.48 | host 188 B, Azahar 104 B, host 208 B, first 84 B ack |
| 3.35-3.46 | 132 B, 120 B, 112 B: end of the setup exchange |
| 4.7 | Azahar 168 B; the first data units (index -2000) go both ways at 4.75 s |
| 4.75-202.5 | data units |
| 202.5-245.4 | only keep-alive and status frames; the session is closed at 248.7 s |

The meaning of each setup frame is [unknown].

# The unit: one serial exchange

A data frame holds `n` back-to-back **56-byte units**; each is one hardware exchange of the Game Boy stream.

## Unit layout [measured]

| offset | size | field |
|---|---|---|
| 0-3 | 4 | `00 01 00 24` in Azahar's units, `00 00 00 24` in the host's. Byte 1 is `01` for the joiner, `00` for the host. `24` is constant |
| 7 | 1 | `01` in Azahar's units, `02` in the host's (the same value as control-frame bytes 4-7) |
| 12 | 1 | `30` constant |
| 21 | 1 | `03` constant |
| 23 | 1 | `0c` constant |
| 28 | 4 | **stream index**, big-endian signed: starts at **-2000**, +1 per exchange, same numbering both ways |
| 32 | 4 | peer index, big-endian signed: a per-frame value that tracks the other side's stream index (an ack carried in-band). Exact rule [unknown] |
| 44 | 1 | **the Game Boy serial byte** |
| 45 | 3 | flags: `01 01 00` in Azahar's units, `00 00 00` in the host's |
| 37-40 | 4 | `00 00 00 00`, except `01 ff ff ff` in 225 of Azahar's 3275 units. Meaning [unknown] |
| 48-51 | 4 | `00 00 00 00`, except `5c 91 a4 19` in 248 of the host's 3189 units. Meaning [unknown] |
| 52 | 2 | little-endian counter = stream index + 2001 for **every** one of the 6464 units |
| others | | zero |

The byte at offset 44 is the Game Boy byte, the same value that the cartridge puts in `rSB` for that exchange. Everything on the
[Game Boy page](gb_link.md) about byte meaning applies unchanged: `$60+nybble`, `$D0` menu bytes, `$FD` preambles, the party block.

## The frame tail (16 bytes) [unknown]

Every data frame ends with 16 bytes. All 792 data frames had a different tail, including frames re-sent with identical units (0 of 170).
It is **not** MD5, SHA-1, SHA-256, BLAKE2s or a CRC32 of the frame, the body, or the units (tested over header offsets 0-29).
Whether it is a random nonce or a keyed authentication tag is [unknown]. This decides whether a peer that is not the VC can be
accepted. It is the main open risk.

# Reliability: re-sends and acknowledgements

The VC does not rely on UDS delivering every frame. It layers its own scheme on top, as measured:

- **Re-sends.** Frames carry recent units again. 3275 transmitted unit copies hold 2095 distinct stream indexes (each copied 1-6 times). De-duplicating on the
  stream index gives no conflicting copies (0 of 2095 TX, 0 of 2092 RX), so a copy is always the same byte.
- **Windows.** A frame holds at most 25 units. When the sender resends from an earlier point the stream index in the next frame steps
  back (observed steps of -24, -148, -199, -298, -423).
- **Acks.** The 84-byte frame carries, at body offset 32 (frame offset 56), a big-endian signed index: **the highest stream index received from the peer + 1**
  (exact for 243 of 305 Azahar acks and 172 of 183 host acks; the others are the re-send rewinds).
- **Gaps.** Only 2 TX and 6 RX stream indexes are missing from the log, so the log itself lost a few frames.

# Mapping the VC stream onto the cartridge stream

The de-duplicated stream per direction is 2097 (TX) / 2098 (RX) exchanges. It is the same sequence of byte values as the Game Boy
stream, with differences in pacing and framing **[measured]**:

| item | cartridge (mGBA hardware) | VC |
|---|---|---|
| role bytes | `01` / `02` at the start | **absent**; stream starts with `60` sync bytes |
| idle polling | `60`×186, `fe`×175, `62`×111 | a handful of each: polling is cut down |
| pre-RN-list exchanges | 260 | 45 |
| RN list | 17 | 17 |
| player block, first `fd` to patch-list `fd` | 427 (6-9 `fd` preamble) | 427 |
| patch lists + trade selection | 404 / 373 | 257 / 253 / 210 |
| rounds | 2 (enter, one trade) | 3 (enter, trade, trade back) |
| mon pick | `60 + index` | `65` (Azahar, slot 6) / `64` (host, slot 5) |
| confirm | `62` | `62` |

## Where the exchanges are in time

Azahar side, de-duplicated stream index from the first byte (`t` as in the setup table):

| round | RN list | player block | patch lists | next round |
|---|---|---|---|---|
| 1 (enter room) | index 45 (19.6 s) | 62 (21.2 s) | 489 (22.1 s) | 746 (110.9 s) |
| 2 (after trade 1) | 746 | 763 (112.5 s) | 1190 (113.4 s) | 1443 (177.0 s) |
| 3 (after the trade back) | 1443 | 1460 (178.9 s) | 1887 (179.6 s) | end of stream |

The 424-byte player block takes about 0.9 s on the wire, around 470 exchanges per second. The time between rounds (60-90 s) is the
player choosing a mon and confirming, not protocol time.

## What the trade looked like, decoded [measured]

| t | direction | content |
|---|---|---|
| 21.2 s | Azahar to host | party `6f 95 4a 83 31 15` (slot 6 = Mew, OT YOSHIRA) |
| 21.3 s | host to Azahar | party `b0 b2 07 96 b9 54`; slot 5 is the Oddish `b9`, nickname `8e 86 7f 96 84 84 83` ("OG WEED") |
| 22.1 s and later | both | `65 ×8 ... 62 ×7`, `64 ×5 ... 62 ×7`: pick slot 6 and slot 5, both confirm |
| 112.5 s | Azahar to host | party `6f 95 4a 83 31 b9`: slot 6 is now the Oddish |
| 179.0 s | host to Azahar | party back to `b0 b2 07 96 54 b9` with the Mew returned |

The Poliwrath (`6f`) is slot 1 in every list and was never traded.

# Gen 1 on the VC, in one picture

1. UDS join (beacon, association, EAPOL): the platform.
2. Setup frames (about 4.7 s): [unknown] contents; they end with the first data unit.
3. Receptionist: `60` sync bytes, `00`, then the menu bytes `D0`, `D4`.
4. Link menu bytes, then the nybble syncs.
5. RN list, player block, patch lists, as three exchanges of 17, 424 and 200 bytes.
6. Mon pick and confirm nybbles.
7. After the trade, steps 5 and 6 repeat.
8. Keep-alive and ack frames run throughout, and for about 40 s after the last data.

# Open questions

- The 16-byte tail, and the 4-byte changing field at header offset 20.
- Contents and purpose of the setup and keep-alive frames (`5865`, `5f41`, `51f6`, `5a28`, `5ad7`, ...). Whether they must be answered in
  order for the link to stay open.
- Who sends first, and who is the clock master on the VC (H2 and H6). The two streams are symmetric and carry no `01`/`02`.
- Whether the comm ID and application data are shared across the Game Boy VC titles (H3, H4).
- Whether the VC accepts a peer that is not the VC.
- The behaviour for battles (`Wireless_start_exchange` and friends); only a trade was captured.
