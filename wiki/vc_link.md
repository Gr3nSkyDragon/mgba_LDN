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

Three sessions of the same scenario have been captured (Azahar VC Red joining a retail VC Red host: one trade and one trade back each).
Where a value differs between them the text says so; everything else was identical in all three. Role swaps, other games and battles are not captured yet.

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

# Where the layers are

Top to bottom, for one Game Boy byte:

| layer | what it is | where it runs | what we see of it |
|---|---|---|---|
| Game Boy ROM | Gen 1 code, `rSB` / `rSC` | the VC emulator in the app (`code.bin`) | the serial byte at unit offset 44 |
| VC glue | the hooks and the per-exchange record | app | the 56-byte unit; stream index, counter |
| **Pia** (`nn::pia`) | Nintendo's peer-to-peer middleware: session, mesh, clock sync, packet ids, per-packet tag | **inside the app process**; its strings and the `32 AB 98 64` magic are in `code.bin` | header bytes 12-23 and the 16-byte HMAC-MD5 tail of every frame |
| **UDS** (`nwm::UDS` service) | the 3DS local-wireless API: create/join a network, node ids (1-8), comm ID, beacon application data, data channels, `SendTo`/`PullPacket` | system service, called by the app over IPC | `channel 243`, node numbers, `secureSequence`; the join trace and `ConnectToNetwork` |
| **NWM** (system module `nwm`) | the module that owns the Wi-Fi hardware; UDS is one of its services (`nwm::UDS`, plus EXT, INF, SAP, ...) | system process | Azahar's `Service.NWM` log lines (`nwm_uds.cpp`) |
| 802.11 | beacons with the Nintendo vendor element, association, EAPOL 4-way handshake, CCMP-encrypted data frames | Wi-Fi chip; in Azahar `uds_real` (ldnd or ESP32) | the beacon/association/EAPOL lines; not the frames themselves |

- **The capture sits between Pia and UDS.** Azahar's `UDS DATA TRACE` lines are the payload the app hands to UDS `SendTo` (and what `PullPacket` returns). That payload is a Pia datagram, so the log shows Pia's header and tail but not 802.11.
- **Two separate protections.** The air is encrypted with CCMP, keyed from the UDS passphrase (`TRL_NETWORK`). Pia then adds its own HMAC-MD5 tag (fixed key `PokemonSIO`) at the end of each datagram. A peer that is not the VC must satisfy both.
- **Bytes 0-11 of the frame** (`01 01`, length, six zeros, message type) come before the Pia magic. Which layer writes them is [unknown].

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
| 12 | 4 | **Pia magic `32 AB 98 64`**, constant (every Pia datagram starts with it, see [pokeldn's Pia page](https://github.com/Decryptu/pokeldn/blob/main/docs/pia.md)) |
| 16 | 1 | Pia version byte `01`. The `0x80` "encrypted" bit is clear, which matches the plaintext payload |
| 17 | 1 | sender id byte: `AA` in Azahar's frames, `2C` in the host's. `00`/`01` in the first setup frames. Probably Pia's connection id |
| 18 | 2 | big-endian **packet id**, per sender. Azahar's count 1, 2, 3 ... 1051 with no gaps (every frame, control included); the host's rises with gaps because the log missed some of its frames |
| 20 | 2 | big-endian **sender's clock in milliseconds** (16-bit, wraps every 65.5 s; ticks at 1000.0 per second against the log time) |
| 22 | 2 | big-endian **sender's estimate of the peer's clock**, same units. The difference of the two halves is about -15,435 in Azahar's frames and +15,480 in the host's, so the pair is mirrored |

Bytes 0-11 (`01 01`, length, six zero bytes, message type) sit in front of the Pia magic and are not part of Pia's own header as pokeldn documents it.
The VC code does contain Pia: `Pia Send`, `Pia Receive`, `SyncClockProtocol`, `Mesh`, `BackgroundScheduler` strings and the magic as a literal in five places
(`vc_work/Red/exefs/code.bin`). The pair of clocks at offset 20 fits Pia's clock-synchronisation protocol.

The type id at offset 10 is constant for a given type and tracks the frame length (`5865` is always 76 bytes). Its derivation is [unknown].

## Message types [measured]

This table classifies **frames by length**. For the messages inside them (what each frame actually carries) see [Messages inside a frame](#messages-inside-a-frame).

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

Frame-level view; the message-level handshake is under [The setup handshake](#the-setup-handshake-measured).

| t | frames |
|---|---|
| 0.00 | host broadcasts a 52-byte frame (destination 65535); Azahar sends 20 bytes |
| 0.08-0.35 | 124 B, 68 B and 140 B each way. The 140 B frame is repeated by Azahar at 0.18, 0.69, 1.19, 1.69 s until answered |
| 0.30-1.5 | first 76 B frames each way |
| 1.36-1.48 | host 188 B, Azahar 104 B, host 208 B, first 84 B ack |
| 3.35-3.46 | 132 B, 120 B, 112 B: end of the setup exchange |
| 4.7 | Azahar 168 B; the joiner's first game unit (index -2001) and the host's next (index -2000) go at about 4.7 s; the host's first unit (`EF`) is sent earlier, see [The first game unit](#the-first-game-unit) |
| 4.75-202.5 | data units |
| 202.5-245.4 | only keep-alive and status frames; the session is closed at 248.7 s |

The meaning of each setup frame is [unknown].

# The unit: one serial exchange

A data frame holds `n` back-to-back **56-byte units**; each is one hardware exchange of the Game Boy stream.
A unit is one Pia *message* of the game stream: a 20-byte message header with a 36-byte payload ([Messages inside a frame](#messages-inside-a-frame)).

## Unit layout [measured]

| offset | size | field |
|---|---|---|
| 0-3 | 4 | `00 01 00 24` in Azahar's units, `00 00 00 24` in the host's. Byte 1 is `01` for the joiner, `00` for the host. `24` is constant |
| 7 | 1 | `01` in Azahar's units, `02` in the host's (the same value as control-frame bytes 4-7) |
| 12 | 1 | `30` constant |
| 21 | 1 | `03` constant |
| 23 | 1 | `0c` constant |
| 28 | 4 | **stream index**, big-endian signed: starts at **-2001**, +1 per exchange, same numbering both ways |
| 32 | 4 | peer index, big-endian signed: a per-frame value that tracks the other side's stream index (an ack carried in-band). Exact rule [unknown] |
| 44 | 1 | **the Game Boy serial byte** |
| 45 | 3 | flags: `01 01 00` in Azahar's units, `00 00 00` in the host's |
| 37-40 | 4 | `00 00 00 00`, except `01 ff ff ff` in 225 of Azahar's 3275 units. Meaning [unknown] |
| 48-51 | 4 | `00 00 00 00`, except `5c 91 a4 19` in 248 of the host's 3189 units. Meaning [unknown] |
| 52 | 2 | little-endian counter = stream index + 2001 for **every** one of the 6464 units |
| others | | zero |

The byte at offset 44 is the Game Boy byte, the same value that the cartridge puts in `rSB` for that exchange. Everything on the
[Game Boy page](gb_link.md) about byte meaning applies unchanged: `$60+nybble`, `$D0` menu bytes, `$FD` preambles, the party block.

## The frame tail (16 bytes): HMAC-MD5 with a fixed key [measured]

**tail = HMAC-MD5(key = `PokemonSIO`, message = frame[12 : length - 16])**

That is, the Pia magic through the end of the payload, with only the tail itself left out. The key is the 10 ASCII bytes
`50 6F 6B 65 6D 6F 6E 53 49 4F` (no NUL, no padding beyond HMAC's own).

- **Verified** on **2071 of 2073** UDS GAME frames in the capture (`docs/azahar/verify_vc_hmac.py`). The two that do not verify are the first
  frames of the session: a 20-byte frame sent by Azahar (header `01 21 ...`) and a 52-byte frame sent by the host (header `01 11 ...`). Neither has
  the Pia magic or a tail.
- **How it was found.** Azahar's GDB stub was used to stop the VC at its MD5 routine (`code.bin` offset `0xbc2b8`, guest `0x1bc2b8`). The buffer it was
  given was `66 59 5d 53 5b 59 58 65 7f 79` then `36 36 ...`: the HMAC inner-pad block (key XOR `0x36`). XORing back gives `PokemonSIO`, the string that
  sits next to `TRL_NETWORK` in `code.bin`. The earlier plain-MD5 test could not match, because the tail is keyed.
- **The key is a constant.** It is not derived from the session, the MAC addresses, the network id or the passphrase. Anyone can compute a valid tail.
- **Other titles.** `PokemonSIO` is present in `code.bin` of all six VC titles (Red, Blue, Yellow share one `code.bin`; Gold, Silver share another; Crystal has its own).
  Only Red was captured, so the key for the Gen 2 titles is [unconfirmed].
- **Why Pia-level authentication is still there.** The air is CCMP-encrypted with a key from the UDS passphrase (`TRL_NETWORK`), and each Pia datagram carries this HMAC.
  Both are fixed strings from the game, so neither needs anything from the 3DS's keys.
- **What did not fire.** In the traced session (join and one trade) the AES-ECB loop, the two cipher wrappers and four hash functions at other addresses were never called.
  AES in `code.bin` is therefore not on the path of ordinary Pia traffic.

# Messages inside a frame

Everything after the 24-byte frame header and before the 16-byte tail is a sequence of **messages**. All 2071 frames with a Pia header split
exactly into messages (`docs/azahar/pia_messages.py`; 0 unclassified). The 20-byte and 52-byte opening frames have no Pia header and are covered at the end.

## Message header (20 bytes) [measured]

| offset | size | field |
|---|---|---|
| 0 | 1 | `00` always |
| 1 | 1 | **sender's station index**: host `00`, joiner `01`; the joiner sends `FD` until the host assigns it (about 1.4 s into the session) |
| 2 | 2 | big-endian payload length |
| 4 | 4 | big-endian **destination station id**: `0` = broadcast / not yet joined, host = `1`, joiner = `2` (equal to the UDS node ids) |
| 8 | 4 | `00 00 00 00` |
| 12 | 1 | **protocol**: `00` keep-alive, `01` session setup, `02` system channel, `06` ping/pong, `30` game stream |
| 13 | 1 | `C0` on keep-alive, `10` on clock sync, otherwise `00` |
| 14 | 1 | `00` |
| 15 | 1 | `01` on the reliable-stream messages of protocol `02`, otherwise `00` |
| 16 | 4 | `00 00 00 00` |

Messages are padded to a multiple of 4 bytes. A frame can hold several messages (a 104-byte frame carries a ping and an ack).

## Message kinds [measured]

Counts are over the 245-second capture (Azahar = joiner = tx, retail = host = rx).

| kind | protocol / length | tx / rx | cadence | payload |
|---|---|---|---|---|
| keep-alive | `00`, 0 bytes | 103 / 43 | joiner every 2.0 s, host every 4.0 s | none |
| sequence ack | `01`, 8 | 3 / 3 | one per sequenced setup message | `05 00 00 00` + the acknowledged sequence value |
| station info | `01`, 62 | 1 / 1 | once, first message of the session | `01`, **station constant id** (a **per-session** byte; equal to the Pia header's byte 17 of that side's frames: `AA`/`2C`, `F7`/`BC`, `14`/`1E` in the three sessions), `05`, host flag (`01` host), 0, **own station id** (`2` joiner, `1` host), zeros, sequence value |
| profile | `01`, 80 | 4 / 1 | once; the joiner repeats it every 0.5 s until acked | `02 00 05 02`, player name UTF-16LE (10 chars), the name again UTF-16BE, `06` (joiner) or `01` (host), a **per-session 32-bit value that is identical in both sides' profiles** (`37274928`, `A82A1086`, `A9CD6264`), sequence value |
| join request | `02`, 16 | 1 / 0 | once, 0.30 s | `01 FD 00 00`, 0, own station id, sequence value |
| mesh state | `02`, 128 | 0 / 1 | once, after the join request | the station table (ids 1 and 2), sequence value |
| ping / pong | `06`, 16 | 139 + 87 / 89 + 88 | each side pings about every 2 to 3 s | `00000000 00000000 <tick hi> <tick lo>`; the pong has first word `00000001` and echoes the same tick |
| clock sync request | `02`, flag `10`, 16 | 121 / 0 | joiner every 2.0 s | `<tick hi> <tick lo> 0 0` |
| clock sync response | `02`, flag `10`, 16 | 0 / 82 | the host answers about two thirds | the request's tick, then the **host's session clock in milliseconds** (9041 at 3.46 s, then +1.95 per 1.94 s) |
| system data | `02`, flag 1, 148 | 0 / 28 | host every 10 s | reliable stream message: sub-header `00 03 00 7C`, own index, ack index, the station table again |
| system ack | `02`, flag 1, 24 | 28 / 0 | one per system data message | reliable-stream ack |
| game data | `30`, 36 | 3279 / 3197 | one per Game Boy serial exchange | reliable-stream message, see below |
| game ack | `30`, 24 | 283 / 193 | about 10 per second while exchanging | reliable-stream ack |

## The tick clock

The ping, pong and clock-sync payloads hold a **64-bit big-endian tick count** (high word first). Fitting it to the log timestamps gives 268.11 MHz on the retail host
and 267.95 MHz on Azahar, which is the 3DS ARM11 clock (268,111,856 Hz). The high word therefore rises by one about every 16 s.
All 228 pings were answered with a matching pong (round trip median 17 ms, range 1 to 86 ms).

## The setup handshake [measured]

Sequenced setup messages carry a 32-bit **sequence value** in their last four bytes, and the receiver answers each with a `sequence ack` carrying the same value.
Each side starts at the low 32 bits of its own tick clock when the session opens (`0x37E63854` and `0xA822C162`, within 50 ms of the fitted clocks) and counts up by one per message.

The joiner's view of the first 2 s:

| t | direction | message |
|---|---|---|
| 0.00 | host to all | 52-byte opening frame (below) |
| 0.00 | joiner to host | 20-byte opening frame (below) |
| 0.08 | joiner to host | station info, seq `S`, sender index `FD` |
| 0.14 | host to joiner | seq ack `S` |
| 0.15 | host to joiner | station info, seq `T` |
| 0.17 | joiner to host | seq ack `T` |
| 0.18 | joiner to host | profile, seq `S+1` (re-sent at 0.69 and 1.19) |
| 0.25 | host to joiner | profile, seq `T+1` |
| 0.27 | joiner to host | seq ack `T+1` |
| 0.30 | joiner to host | join request, seq `S+2` |
| 0.33 | host to joiner | seq ack `S+2` |
| 1.36 | host to joiner | mesh state, seq `T+2` |
| 1.38 | joiner to host | seq ack `T+2`; from here the joiner's sender index is `01`; first ping and first clock sync request |
| 1.46 | host to joiner | first system data message (index -2001) |
| 1.48 | joiner to host | system ack |
| 1.69 | joiner to host | profile again, now with sender index `01` |
| 1.72 | host to joiner | seq ack `S+1`: the host only acknowledged the profile after it carried the assigned index |
| 4.7 | both | the game stream starts (index -2001, see [The first game unit](#the-first-game-unit)) |

## What changes between sessions [measured, 3 captures]

| item | constant across sessions | per session |
|---|---|---|
| frame header | `01 01`, the Pia magic, version `01`; message types and lengths | packet ids restart at 1; the two clocks |
| opening frames | `01 11 18 ...`, `91 3F 02`, `01 00 02 00`; joiner's `01 21 ... C5 94 02` | the host's 4-byte value at header offset 16 (`79CD0B19`, `D2A9711E`, `820A9F0C`) |
| station info | layout, `05`, host flag, station ids 1 and 2 | the station constant id byte (both sides), the sequence value |
| profile | layout, player names, the `06` / `01` byte | the shared 32-bit value, the sequence value |
| join request, mesh state | layout | the sequence value |
| sequence values | | each side starts at the low 32 bits of its own tick clock and counts up by 1 per message |
| system data (host, every 10 s) | the whole payload and the station table | only the stream index |
| UDS beacon | comm ID `0x00171010`, node data fingerprint `0x8D33BB51`, key-derivation input `TRL_NETWORK` | network ID (`D7578A27`, `DA87CAB9`, `CD3E7254`) and the 16-byte application data (fingerprints `044C1228`, `E24F1E1C`, `77B29C94`) |

- **The shared 32-bit profile value** is not the sender's tick: it differs between sessions but the host and joiner send the same number, so one side takes it from the other
  (probably from the host's beacon application data, which also changes every session; the log only prints that data's fingerprint). It sits 1 to 10 s before the host's first ping on its clock, with no fixed offset.
- **The handshake order is not fixed.** The host's station info arrived at 0.15 s, 0.27 s and 1.25 s in the three sessions, and the joiner re-sent its first messages every 0.5 s until they were acknowledged.
  Implementation consequence: a peer has to keep re-sending unacknowledged setup messages and answer whatever arrives, not follow a script.
- **Cadences are the same every time:** pings, clock sync requests and the joiner's keep-alive every 2.0 s; the host's keep-alive every 4.0 s (2.0 s in one session); system data every 10.0 s; the host answers 68 to 70 % of clock sync requests in all three.
- **Game stream:** all three sessions have three rounds, an RN list of 17, a player block of 427 and the same patch-list shape. Every stream starts at index -2001 (an earlier version of this page said -2000: the first unit was missed by the first parser).

## The first game unit

In every session the **host's first game unit is `EF`**, at stream index -2001, sent unprompted before the joiner's first unit. The joiner's first unit is `00`.
After that the host's stream reads `EF 00 60 60 60 ...` and the joiner's `00 60 60 60 ...`: the host's stream is the joiner's with `EF` in front. `EF` is not a byte the Gen 1 ROM sends in the link-up phase;
it looks like the VC's role marker, standing in for the `01` / `02` bytes of the hardware handshake.

| session | host | `EF` sent at | joiner's first unit at |
|---|---|---|---|
| 1 (retail hosts) | retail | 3.35 s | 4.70 s |
| 2 (retail hosts) | retail | 2.48 s | 3.84 s |
| 3 (retail hosts) | retail | 2.59 s | 3.95 s |
| 4 (Azahar hosts) | Azahar | 3.71 s | 5.28 s |

## The reliable streams

The game stream (protocol `30`) and the system stream (protocol `02`, flag 1) use the same record. **Data** (payload):

| offset | size | field |
|---|---|---|
| 0 | 2 | `00 03`: data |
| 2 | 2 | big-endian length of the next part: `0x0C` on the game stream, `0x7C` on the system stream |
| 4 | 4 | `00 00 00 00` |
| 8 | 4 | **own stream index**, signed big-endian, starts at -2001 |
| 12 | 4 | **ack index**: the highest index received from the peer + 1 |
| 16 | 20 | trailer; on the game stream: byte 8 (payload offset 24) is the Game Boy byte, then `01 01 00`; the last 4 bytes hold a counter (index + 2001, little-endian 16 bits) |

An **ack** has payload length 24 and the same layout with type `00 00`, a zero own index and the ack index at offset 12.
Re-sends use the original index, so the stream index steps back (see [Reliability](#reliability-re-sends-and-acknowledgements)).
Each of the 28 system messages the host sent was acknowledged by the joiner; a missed ack made the host send one twice (111.54 and 111.59 s).

## The opening frames [partly decoded]

Two frames precede the Pia traffic and carry no Pia magic or tail:

| t | from | length | header (first 24 bytes) | body |
|---|---|---|---|---|
| 0.00 | host, broadcast | 52 | `01 11 18 00 00 00 00 00 00 00 91 3F 02 00 00 00 79 CD 0B 19 01 00 00 00` | `00000000 01000200` then zeros |
| 0.00 | joiner to host | 20 | `01 21 00 00 00 00 00 00 00 00 C5 94 02 00 00 00 00 00 00 00` | none |

The second byte is a frame type: `01` Pia data, `11` host hello, `21` joiner hello reply, and `12` **host bye** (below). The host repeats its hello every 0.5 s until a joiner answers.
`79 CD 0B 19` is unexplained (it changes every session).

**Session closed notice [measured once]:** when Azahar (host) gave up on a silent joiner it broadcast, twice, 3 ms apart, `01 12 00 00 00 00 00 00 00 00 85 65 00 00 00 00` (16 bytes, destination 65535, no Pia header or tail).
It came exactly 10.0 s after the joiner's last Pia message, which matches a 10-second silence timeout (the VC contains Pia's `maxSilenceTime` checks). Anything that keeps a session alive must send a Pia message at least every few seconds; the normal 2-second keep-alive does.

# Role swap: Azahar hosting [measured, 2 captures, neither completed]

**First host run.** `logs/azahar/azahar_log_roleswap_host_20261003.txt`: Azahar (VC Red, host, UDS node 1) and a retail 3DS (VC Red, joiner, node 2). Names `AZAHAR` (host) and `1` (joiner).

**What matched the retail-hosts sessions:**

- Frame, message and tail formats are identical (all 931 frames with a Pia header pass the HMAC check); the role is the only difference. The host's sender index is `00`, the joiner's `FD` until the host's mesh state assigns `01`, and the host's station id is `1`.
- The host broadcasts the 52-byte opening frame every 0.5 s until the joiner answers with the 20-byte frame (it did so twice, at 0.57 and 0.59 s).
- The handshake completed in 1.5 s: station info, profiles, join request, mesh state, all acknowledged; the host then sent its first system data message (index -2001).
- Pings, clock sync and keep-alives ran as before; the host answers the joiner's clock sync requests.

**What went wrong.** The Trade Center was chosen on both consoles and the game reached "Please wait", where it froze:

| t (s) | event |
|---|---|
| 3.71 | Azahar (host) sends `EF` |
| 5.28 | the 3DS sends its first unit |
| 10.8 to 18.2 | no game traffic (only pings, clock sync, keep-alives): the game was idle, not the link |
| 20.9 to 27.0 | RN lists exchanged |
| 27.0 | both player blocks (424 bytes each) sent |
| about 32 | the 3DS acknowledges Azahar's whole block |
| 33 to 34 | the 3DS sends its **patch list** (200 bytes) and keeps re-sending |
| 33 | **Azahar sends nothing more**, not even its patch list, pings or acks; its own acknowledgement of the 3DS's stream stays at index -1534 |
| 73.2 (log time) | the last frame; the VC shuts UDS down at 75.1 s ("communication error") |

- Azahar's stream is a complete 424-byte block that stops at index -1532. The 3DS's runs to -1335 because it had already sent its patch list. In the retail-host sessions Azahar's VC (as joiner) went straight on to the patch lists.
- No radio-level fault is visible: no deauthentication, no failed UDS call, no errors in the log near the end; the VC itself called Shutdown. The stall is inside the VC after it received the 3DS's party block.
- Not yet known: whether the hang is reproducible, and whether the VC is waiting for something (an event or UDS result that Azahar's host mode does not deliver) or has crashed. `docs/azahar/vc_stall_snapshot.py` shows where each of the VC's threads is blocked.

## Second host run [measured, 1 capture, did not complete]

`logs/azahar/azahar_log_roleswap_host2_20261003.txt`. Azahar hosted again (the game was not frozen this time). Both players chose Trade Center (`D4 D4` on both streams at 13.5 s, game stream index -1983), walked into the room and the 3DS player used the table.

| t (s) | event |
|---|---|
| 0 to 1.9 | handshake (identical to the other sessions, with the usual re-sends) |
| 4.9 / 7.2 | host's `EF` / joiner's first unit; the receptionist and link-menu bytes follow |
| 13.5 | last game unit from either side (both pressed A on Trade Center) |
| 14 to 18 | pings, clock syncs, keep-alives only |
| **18.07** | **the 3DS's last Pia message** (a ping): no teardown frame, nothing after it |
| 19.1 onward | the 3DS keeps sending UDS **channel-3 management** requests about every 1.1 s, which Azahar answers; it never deauthenticated |
| 21.9 | Azahar re-sends its 10-second system message (index -1999) every 0.36 s, unacknowledged |
| 28.1 | Azahar broadcasts the `01 12` bye frame, 10.0 s after the 3DS went quiet |
| 70 | the VC shuts UDS down (the user left) |

- The game stream never reached the RN list; the failure is at the moment the 3DS player stepped to the table, about 4.6 s after the last game byte (the first host run resumed its game traffic at 18.2 s, the same point in the session).
- The 3DS stayed associated and its UDS layer kept polling the host, so the 3DS's **VC (app) stopped its own Pia traffic**; the host did nothing visible at that moment. The cause is on the 3DS side and not visible on the wire.
- Azahar's VC was alive throughout: it answered every ping, clock sync and keep-alive until it closed the session itself.
- A GDB client (the snapshot tool) attached 1.9 s after the join in this run. The first host run had no client and failed differently (see above), so the tool is not the cause of the problem.
- `docs/azahar/vc_stall_snapshot.py` taken after the error shows all ten VC threads idle in system calls (ArbitrateAddress, WaitSynchronization, one SleepThread): nothing is hung.
- In the sampled beacon records (every 100th) the host's beacon body changed once, when the joiner was added, and not again; the log shows no `SetApplicationData` call.

# Reliability: re-sends and acknowledgements

The VC does not rely on UDS delivering every frame. It layers its own scheme on top, as measured:

- **Re-sends.** Frames carry recent units again. In the first capture 3279 transmitted unit copies hold 2098 distinct stream indexes (each copied 1 to 6 times). De-duplicating on the
  stream index gives no conflicting copies, so a copy is always the same byte.
- **Windows.** A frame holds at most 25 units. When the sender resends from an earlier point the stream index in the next frame steps
  back (observed steps of -24, -148, -199, -298, -423).
- **Acks.** The 84-byte frame carries, at body offset 32 (frame offset 56), a big-endian signed index: **the highest stream index received from the peer + 1**
  (exact for 243 of 305 Azahar acks and 172 of 183 host acks; the others are the re-send rewinds).
- **Gaps.** None: in all four captures every index from -2001 to the last one is present in each direction (an earlier version of this page reported a few missing; that was a parser miss, not lost frames).

# Mapping the VC stream onto the cartridge stream

The de-duplicated stream per direction is 2098 (TX) / 2099 (RX) exchanges in the first capture. It is the same sequence of byte values as the Game Boy
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

Indexes in the next two tables count from stream index -2000 (the very first unit, -2001, was not in that analysis); add 1 for the position from the first unit.

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

- Where the shared profile value and the opening frame's 4-byte value come from, what the `06` / `01` byte in the profile means, and the 20-byte trailer of the game record besides the Game Boy byte.
- Which of these the VC checks: whether it needs the pings answered, the clock sync, the station table, or the profile contents, and what it does when one is missing.
- Who is the clock master on the VC: the host's `EF` first unit suggests the host, but nothing else in the stream distinguishes the roles.
- Whether the comm ID, application data and `PokemonSIO` are shared across the Game Boy VC titles (H3, H4). Only Red was captured.
- How a session ends. Gen 1 has no clean way out of the Trade Center (you leave by choosing Reset), so no teardown exchange exists in any capture; Gen 2 may differ.
- Battles (`Wireless_start_exchange` and friends); only a trade was captured.
