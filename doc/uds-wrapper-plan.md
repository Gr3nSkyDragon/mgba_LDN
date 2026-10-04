# UDS wrapper plan: Game Boy Pokémon (Gen 1-2) to the 3DS Virtual Console

Status: planning, with the Pia and session layers written and unit-tested. Nothing is attached to a core yet.
Menu: a `Virtual Console (Gen 1-2)` box (the label is a placeholder) in the Wireless Adapter menu, saved as `vcwrapper.enabled`. It is a
stub: `CoreController::setVCWrapper` records the choice and logs it. The wrapper is a Game Boy link cable signal inside UDS and has
nothing to do with the GBA wireless adapter (RFU); the menu location is only where the box was put and can move.

## 1. Goal

Let a Game Boy Pokémon game running in mGBA (and later a real cartridge behind an ESP32 proxy) trade with the 3DS Virtual
Console release, by presenting the VC with a Pia peer that behaves like another VC console. The first target is the **joiner**
role: the 3DS hosts, the wrapper joins. That covers both end goals (mGBA to retail VC, then cartridge to retail VC) because the
player decides which side hosts.

Not in scope now: the wrapper hosting. Azahar as host against a retail joiner is unsolved (see `docs/wiki/vc_link.md`, "Role
swap", and the 2026-10-04 notes), and nothing here needs it.

## 2. What is already known

All measured; evidence and tags are in `docs/wiki/vc_link.md` and `docs/wiki/gb_link.md`.

* **Pia over UDS channel 243**, early Pia (≤ 5.6 per the kinnay/Pretendo header tables): unencrypted, a 20-byte message header,
  HMAC-MD5 tail with the fixed key `PokemonSIO`. Verified on every captured frame (6,974 + 1,678).
* **The 12 bytes before the Pia magic**: `01 <kind> <len-12 LE> 00×6 <CRC-16/ARC of bytes 0..9, LE>`. The CRC was identified
  on 2026-10-04 (it explains why the "type id" depends only on the frame length).
* **Setup handshake** (hello, hello reply, station info, profile, join request, mesh state), the cadences (ping and clock sync
  and keep-alive every 2 s, system data from the host every 10 s, 10 s silence timeout) and the **reliable unit stream** (index
  from -2001, ack index, windows of 25 units, re-send after about 0.5 s).
* **New on 2026-10-04**, from the retail-to-retail passive capture:
  * the profile's shared 32-bit value is bytes 4..7 of the host's beacon application data, read little endian, written big endian;
  * Pia connection id is 0/1 (host/joiner) and packet id 0 until the host's mesh state, then the station constant id and a counter
    from 1; a joiner's clock sync request is always a "direct" frame (connection id 0, packet id 0);
  * sequence acks are addressed to 0 even after the station index is assigned;
  * a frame's peer-clock field is the peer's last header clock plus the time since it arrived.
* The 424-byte player block, the RN list and the patch lists are **bulk exchanges** inside the VC (the pret `vc_hook`s): both
  consoles create all of a block's units at once and consume the peer's as they arrive. The retail pair moves a block in
  about 55 ms; throughput is limited by how fast each side acks, so a peer that acks late (or in 25-unit steps) starves the other.

## 3. Layers

```
  GB core (mGBA)                    <- unmodified ROM, real serial protocol
    L4  GB link driver + VC glue    NOT WRITTEN   hooks the same routines the VC patches (src/gb/linktrace.c has the table)
    L3  Pia session (joiner)        DONE          include/mgba/internal/gb/sio/uds-session.h, src/gb/sio/uds-session.c
    L2  UDS link                    NOT WRITTEN   beacon, auth, assoc, EAPoL, node ids, secure-data header, channel 243
    L1  air                         NOT WRITTEN   one of: Azahar bridge (UDP on localhost), ESP32 UDS bridge
```

`uds-pia.c` (the wire codec, DONE) sits under L3. Both files depend only on `mgba-util/common.h` and `mgba-util/md5.h`, so they
can move into an ESP-IDF component unchanged (mbedtls has MD5) when the cartridge proxy needs the stack on the ESP32.

### L2: the UDS link

A generic layer over "air packets" `(type, transmitter, destination, body)`, which is exactly Azahar's `Network::WifiPacket`
(Beacon, Data, Authentication, AssociationResponse, Deauthentication, NodeMap). Joiner side:

1. scan: parse the Nintendo vendor element (tag 221, OUI 00:1F:32, type 21: comm id, network id, node counts, SHA-1, 16 bytes of
   application data) and the encrypted node list (type 24, AES-CTR with the NWM beacon key);
2. authentication request, association request (SSID = network id as eight hex characters), EAPoL-Start with our node info, EAPoL-Logoff
   reply (the host's node list and our node id);
3. data frames: LLC `AA AA 03 00 00 00 87 6D`, the 14-byte secure-data header (channel, source and destination node, sequence),
   payload. Channel 243 carries Pia; channel 3 carries small management frames.

Reference: Azahar `src/core/hle/service/nwm/` (`uds_data.cpp` 401 lines, `uds_beacon.cpp` 366, `uds_connection.cpp` 85, the join parts of
`nwm_uds.cpp`). Estimated 1,200-1,500 lines of C. Everything except CCMP is shared between the two air backends.

### L1: two air backends

The original `ldnd.exe` (the raw 802.11 daemon behind a named pipe) is **dropped**: the Azahar ESP32 board does the same job.

| air | carries | CCMP | status |
|---|---|---|---|
| **Azahar bridge** | `WifiPacket`s as UDP datagrams on 127.0.0.1 to an Azahar build | none (Azahar's room multiplayer is plaintext too) | needs a small Azahar patch (below) |
| **ESP32 UDS bridge** | raw 802.11 over a USB serial link | yes: key = AES-CTR(slot 0x2D key, MD5(passphrase)) with counter MD5(host MAC, comm id, id, network id), passphrase `TRL_NETWORK\0` | Azahar's `firmware/esp32-uds-bridge` is a UDS-capable raw radio (promiscuous RX filter, TX with per-frame rate, hardware ACK for an emulated MAC, `SetChannel`, `SetWatch`). **Its serial framing is not mGBA's**: Azahar's frame is `version, type, seq, flags, length(2), payload, crc32` (6-byte header, types Hello/Start/SetChannel/TxFrame/SetWatch/Rx...), mGBA's `esp32-wire.c` is the GB-Link LDN frame (12-byte header with request and session ids). Same COBS and CRC-32, so a second small wire module is needed |

The LDN `ldnd` and the GB-Link ESP32 firmware cannot be used for this: they speak LDN (Switch).

**How the VC box uses the Wireless Adapter backend menu** (to be built; today the box attaches nothing). The existing backends are RFU
backends (`struct GBASIORFUBackend`: rooms, 24-byte broadcast data, GBA packets), so the VC wrapper does not go through
`GBASIORFUBackendCreate`. It reads the same menu choice and interprets it itself:

* `ESP32` + VC box: open the board with the **existing serial layer unchanged** (`esp32-serial.c`: same chip class, VID 303A native USB
  Serial/JTAG, 921600 baud, DTR on and RTS off as Azahar sets them, the "ESP32 board" picker and the saved port) and speak the
  **Azahar firmware's protocol** on it (a new `uds-esp32.c`: Hello/HelloAck, Start{channel, MAC}, SetChannel, SetWatch, TxFrame{flags,
  rate, mpdu}, Rx; Azahar's `esp32_wire.cpp` is 171 lines and its session loop in `nl80211_monitor.cpp` about 190). If the board answers
  like the GB-Link firmware instead, report "wrong firmware" in the status line.
* `Local` + VC box: a plain UDP pair to Azahar. The existing Local backend cannot be used: it is the RFU adapter's own protocol
  (datagrams starting `0x52`, beacon/connect/accept/data, ports 45600-45607, base overridable with `MGBA_RFU_UDP_PORT`), which Azahar does
  not speak and which carries no UDS frames. The VC wrapper gets its own transport: one fixed port pair on 127.0.0.1 (Azahar listens on
  one and sends to the other, mGBA the reverse; both overridable, e.g. `AZAHAR_UDS_BRIDGE_PORT` / `MGBA_UDS_PORT`). With exactly two
  peers on one machine no discovery is needed: the host's beacons are periodic, so whichever program starts second hears the next one.
  Copy the `SIO_UDP_CONNRESET` handling from `rfu-udp.c` so datagrams sent before the other side is listening do not break receives.
* `ldnd` + VC box: not supported (LDN).

The 3DS UDS key (AES slot 0x2D) must come from the user's own key dump; the same file Azahar reads. Never ship or log it.

**The ESP32 question.** Two different things were called "the ESP32 firmware". The GB-Link board (LDN) stays as it is. The Azahar board
(`esp32-uds-bridge`) is a dumb radio, so with it the whole stack runs on the PC and **no firmware change is needed** for mGBA-to-retail.
Only the stand-alone cartridge proxy (no PC) needs the stack on the ESP32; that is then a third firmware variant that links the
portable C above plus a link-cable driver. Keep the three apart in the repository (`esp32-ldn`, `esp32-uds-bridge`, `esp32-uds-proxy`).

### L4: the Game Boy side (the hard part, not started)

The wire carries one 56-byte unit per serial exchange, but the VC creates units in bulk at its hooks, so a byte-by-byte
emulation of the cable cannot reproduce what the 3DS expects. Plan: hook the same ROM routines the VC patches (breakpoints, as
`linktrace.c` already does for tracing; the table is in `docs/hook_table/hook_table.json`) and implement them at the routine level:

* `Wireless_WaitLinkTransfer` / nybble syncs: one unit each way carrying `60+n`;
* `Wireless_ExchangeBytes_*` (RN list 17, player block 424, patch lists 200): read the send buffer from WRAM, queue the units,
  flush, wait for the peer's units, write the receive buffer, and return past the ROM's loop;
* the receptionist and link menu bytes (`D0..D4`, `FE`): per-byte units;
* `Link_fake_connection_status`: the joiner is not told it is master; the host's `EF` first unit stands in for the role handshake;
* the exchange timing the VC uses (26-frame delays on Red/Blue) and the inactivity message.

Facts that constrain it (all measured; see the wiki):

* the host's first unit is `EF` at index -2001; the joiner's first unit is `00`;
* in a block exchange the joiner's units lead the host's by 3 exchanges; the joiner sends 3 "extra" units after its block, the host
  1; they replay the bytes four positions earlier;
* the per-run 32-bit value (unit offset 48) is stamped by the host only; the joiner's is zero;
* a joiner must ack promptly: after each frame that advances the stream, send a `game ack`, and put the current ack index in every unit.
  Late acks starve the host's windows; the retail XL acked Azahar's host stream only 25 units per 0.3 s on 2026-10-04 and a block took
  5 s instead of 55 ms. Whether that is the whole story is not proven, but a joiner that acks at once is the safe design.

Open design question: whether to HLE the routines (above) or to keep the unpatched ROM's byte loop and *translate* between its
exchange stream and the VC's. The first is what the VC does and is robust; the second needs the glue to invent the VC's bulk behaviour
from the cable's. The plan is the first.

## 4. Test environments

1. **Unit test (exists).** `uds-test` replays 18 real frames: codec and HMAC on all of them, byte-exact rebuild, the joiner session
   against the 2DS's actual messages (station info, acks, profile, join request, post-join messages, system ack), unit encode/decode,
   25-unit windows, re-send, out-of-order delivery, silence timeout and bye. 354 checks pass.
2. **Local + VC against Azahar (the easy full-stack test).** Azahar already funnels every UDS frame through one `SendPacket` and one
   `OnWifiPacketReceived`. A bridge patch (est. 150 lines, behind an env var) would also send each `WifiPacket` as a UDP datagram to
   127.0.0.1 and feed received datagrams back in. The mGBA "Local" air then needs only L2 without CCMP. This tests the wrapper against a
   VC that hosts with **no radio in the way** (Azahar's host failures were all over the air), and the Azahar log shows exactly what the
   VC thinks of the wrapper's frames. Azahar's own room multiplayer cannot be used from mGBA (its ENet room protocol is a different
   thing); the bridge is the shorter path.
3. **ESP32 board against a retail 3DS host.** The end-to-end test; needs L2 with CCMP and the key.
4. **Passive sniffer on the air** (`uds-sniffer` branch of Azahar, `run-azahar-sniffer.cmd`) during test 3 to compare the wrapper's frames
   with a retail joiner's.

If test 2 is not worth building, go straight to 3 with the sniffer; the cost is that a failure then has no inspectable peer.

## 5. Milestones

| | milestone | acceptance |
|---|---|---|
| M0 | Pia codec and joiner session | `uds-test` passes against the captured frames (done) |
| M1 | menu stubs | the two entries appear and persist; nothing attached (done) |
| M2 | L2 UDS link over a loopback `WifiPacket` pipe | a test host (the unit test's made-up host plus Azahar's node-map behaviour) is joined in memory |
| M3 | Azahar UDP bridge + Local air | Azahar (host, VC Red at the table) and the wrapper reach "mesh state" and exchange pings |
| M4 | L4 for the nybble sync and link menu | the Game Boy ROM reaches the Trade Center selection against Azahar |
| M5 | L4 bulk exchanges | RN list, block and patch lists complete; the first trade completes against Azahar |
| M6 | ESP32 UDS bridge air with CCMP (a second wire module for Azahar's framing) | M5 against a retail 3DS host |
| M7 | (spare) | |
| M8 | Blue/Yellow, then Gen 2 (comm id and key to be measured) | |
| M9 | stand-alone cartridge proxy firmware | |

## 6. Unknowns to resolve on the way

* The host hello's 4-byte value (header offset 16); possibly bytes 0..3 of the beacon application data (`08 BA B3 23` in the new
  capture). Only needed if the wrapper ever hosts. The joiner's reply has no per-session value.
* The 8 "loss bits" (unit offsets 36-43) and the per-run 32-bit value. The joiner writes zeros; the retail 2DS did too.
* Whether the VC checks the beacon application data, the comm id (`0x00171010`), or the passphrase for Blue and Yellow (shared `code.bin`
  with Red, so probably yes) and for Gold/Silver/Crystal (unknown).
* The `PokemonSIO` key for the Gen 2 titles, and whether they use the same units.
* Who the clock master is; whether the VC needs the joiner's pings/clock sync at all (probably yes: the 10 s silence timeout is Pia's).
* How a session ends: Gen 1 has no clean exit; the wrapper should send nothing special and stop; Gen 2 may differ.

## 7. Status and next steps

Done, not yet attached to mGBA (nothing creates a `UDSJoiner`):

* Azahar side (commit 36b2af359 on `uds-beacon` in the Azahar repository): `uds_bridge.cpp`, enabled by `AZAHAR_UDS_BRIDGE`. One datagram per
  `WifiPacket`: `UDSB`, version 1, type, channel, reserved, transmitter MAC, destination MAC, body. Azahar listens on N (45710) and sends
  to N + 1; mGBA does the reverse. While it is on the radio monitor is not started.
* mGBA side: `uds-udp.c` (the socket pair), `uds-room.c` (beacon parsing, authentication, association, EAPoL start, EAPoL logoff,
  SecureData on channel 243, the 1.1 s channel-3 keep-alive), `uds-joiner.c` (UDP + room + Pia session). `uds-bridge-test` plays a
  made-up Azahar host over real sockets (25 checks). The packet bodies come from reading Azahar's generators, so the first live run
  is what proves they match.
* The UDP pair is only for the VC wrapper with Wireless Adapter > Local; the RFU Local backend is untouched.

Next:

1. First live check without any Game Boy side: start Azahar with `AZAHAR_UDS_BRIDGE=1`, host from VC Red, and run a small console program
   that opens a `UDSJoiner` and prints the state until `udsJoinerReady()` (milestone M3).
2. Review the menu stubs in a running build; decide what greys out when the VC box is ticked.
3. L4 with the nybble sync only, then the bulk exchanges (M4, M5).

## 8. Stage 2: mGBA to a retail 3DS over the real radio

Stage 1 is done: a Game Boy Red/Blue game in mGBA trades with Azahar's VC through the UDP bridge (commit "Working mGBA to Azahar
trade"). Stage 2 swaps the bridge for the real air. Everything above `uds-room.c` stays as it is.

**Layout.** `uds-room.c` talks to an "air" that exchanges `WifiPacket`s (type, channel, MACs, body). The bridge is one air. The real one is
new and sits between the room and the board:

```
uds-room.c   (join, SecureData, Pia)         unchanged
  uds-air-radio.c   WifiPacket <-> 802.11 MPDU: management frames, data frames, sequence numbers, packet numbers
    uds-ccmp.c        AES-128, CCM with an 8-byte tag, key derivation (AES-CTR of MD5(passphrase), slot 0x2D)
    uds-esp32.c       Azahar firmware framing (version, type, seq, flags, length, payload, CRC-32, COBS) and its commands
      esp32-serial.c    existing: port picker, VID 303A, 921600 baud, DTR/RTS
```

The firmware's own `main/uds_wire.c` is portable C with golden vectors; it is reused rather than rewritten. The board needs no change.

**Known from Azahar** (the same code already trades as a client against a retail host): the passphrase `TRL_NETWORK\0`; the CCMP nonce and
AAD layout (`uds_data.cpp`); the retail association request body (SSID = network id as eight hex characters, rates, extended rates); that
frames to the host are unicast ToDS, the host's game frames are broadcast no-DS, its EAPoL frames FromDS; the board uses a twin MAC
(first octet XOR 0x02) so the host's unicast frames are retransmitted rather than ACKed by hardware, and that already works.

**Milestones**
| | work | check |
|---|---|---|
| R0 | `uds-ccmp.c` and the frame builders in portable C | byte-exact against the MPDUs Azahar logged (`mpdu=` next to `plaintext=` in its UDS DATA/JOIN TRACE lines) and the passive-sniffer capture |
| R1 | `uds-esp32.c` over the existing serial layer: Hello/HelloAck, Start, SetChannel, SetWatch, TxFrame, Rx | the status line shows the firmware version; wrong firmware (the GB-Link LDN board) is reported |
| R2 | scan: channel hop, read a retail 3DS host's beacon (network info tag, application data) | the room reports the host as it does on the bridge |
| R3 | join: authentication, association request, EAPoL start, the host's EAPoL reply | "joined" with a node id from a retail host |
| R4 | Pia session over the air | "game stream may start", then the sync and menu of stage 1 |
| R5 | a trade with a retail 3DS | the stage 1 trade, against retail |
| R6 | robustness: retries and ACK behaviour, channel changes, a second session, errors in the status line | |

**Needs from you:** a retail 3DS that can host VC Red or Blue, and the 3DS UDS key in a file of your own (`slot0x2DKeyN=...`, or
`slot0x2DKeyX` and `slot0x2DKeyY`, as in `aes_keys.txt`; never shipped, never logged). The backend choice is the existing menu: `ESP32` plus the Virtual Console box means the
real air, `Local` plus the box means the bridge.

**Risks:** a retail host may be stricter than Azahar about timing and acknowledgements; the board cannot serve Azahar and mGBA at the
same time; the first join attempt against a retail host was deauthenticated by it once in Azahar's logs.

**R0 status (done):** `uds-keyfile.c` (reads `slot0x2DKeyN`, or makes the key from `slot0x2DKeyX` and `slot0x2DKeyY` with the built-in generator constant, or the file's own `generatorConstant`/`generator` line;
Settings > BIOS > "3DS UDS key file", saved as `vcwrapper.keyfile`, with a status line that never shows the key), `uds-ccmp.c` (AES-128, CCM with the
published FIPS-197 and RFC 3610 vectors, the per-network data key, protected data frames, management frames, the association request body). Tests:
`uds-key-test`, `uds-ccmp-test` (37 checks), and `uds-ccmp-golden`, which decrypts and rebuilds every `TX MPDU` line in an Azahar log:
1,181 of 1,181 frames from the 2026-10-03 trade decrypt to the logged plaintext and rebuild byte for byte, and the association request frame is identical.
The key file for the check is the user's own (a `slot0x2DKeyN=` line, or KeyX and KeyY); nothing in the repository contains key material.

**R1 status (written, framing tested, board not yet tried):** `uds-esp32.c` is the serial protocol of Azahar's `esp32-uds-bridge` firmware (its own
COBS and CRC code, written from the documented framing; not the GB-Link LDN framing) on top of the existing `esp32-serial.c`: Hello with a
wait for the board to boot, Start, SetChannel, SetWatch, TxFrame, SetBeacon, Ping, and the Rx, Status, Log, TxDone events.
`uds-esp32-test` (635 checks, no hardware) matches three frames from the firmware's own host test byte for byte and round-trips every payload size
around the COBS block limit. `uds-esp32-probe [COMx] [--seconds N] [--channel N]` prints the firmware version and board MAC, then hops channels
1, 6 and 11 and lists the 3DS hosts it hears (R2's scan; no key needed). `udsRoomParseBeacon` is now public, shared with the bridge join.

**R2, R3, R4 (done, live):** `uds-esp32-probe` heard the retail XL's beacons (293 in 30 s, none bad). `uds-air-radio.c` joins a retail 3DS Game Boy VC host over
the air: `uds-air-probe` reached "joined, node 2 of 2" in 0.55 s and "Pia session: game stream may start" in 0.67 s, and held the link for 20 s with no
send failures and nothing dropped for decryption (13 host retransmissions were filtered by packet number). The radio opens without blocking (the board
needs a few seconds): `UDS_AIR_BOOTING` sends Hello from the poll, then Start with the decoy-MAC flag.

**R5 (written, not yet tried):** `Wireless Adapter > ESP32` plus the `Virtual Console (Gen 1-2)` box runs the same `GBVCLink` on the real radio; `Local` plus
the box stays on the Azahar bridge (`GBVCLinkConfig`). The board is the "ESP32 board" menu choice (empty: find it), the key file is Settings > BIOS > "3DS UDS key
file" (read when the game starts). `MGBA_VCLINK_TRACE` now also logs every unit sent and received and, on the radio, counters every 5 s.

## 9. Decoupling the wrapper from the emulator (branch UDS-decoupling)

Goal: the translation between a Game Boy's serial port and the VC's unit stream must not need the emulator, so the same code can sit
behind a real cartridge (a GB-Link GB-mode adapter or direct pins) on the ESP32. The emulated ROM is to be treated as a retail
cartridge on a cable.

**Model: a permanent slave.** On a cable the console that receives `$02` during the receptionist's handshake becomes master. The
wrapper always offers `$02` with an armed external clock, so the cartridge is always master and drives every transfer, which is what
the VC's hook forces today (`Link_fake_connection_status`). The wrapper never generates a clock. It must have its reply byte ready
before the cartridge clocks it (the "reply lags one exchange" already measured), and it answers `$FE` ("no data", the ROM retries)
when the other console has nothing for it yet.

```
  cartridge or emulated ROM
    cable port        mGBA serial driver (today: hooks)  |  GB-Link GB mode / pins (later)
    L4b  translator   uds-cable.c   NO emulator types: units through a UDSUnitPort, time and intents passed in
    L3   Pia session  uds-session.c
    L2/L1             uds-room.c, uds-air-radio.c, uds-ccmp.c ...
```

Steps (the hook wrapper stays as the reference oracle throughout, behind its current code path):

1. **Done.** `uds-cable.c/.h`: transfer pairing, the nybble sync burst, the link menu exchange and the first-unit handling moved out of
   `uds-gblink.c`, with the unit stream behind `struct UDSUnitPort`. `uds-gblink.c` now only reads the ROM's RAM (the nybble, the menu
   selection) and fixes registers; behaviour is meant to be identical. `uds-cable-test` checks the moved logic against a scripted
   host (39 checks). **Not yet re-run live** against Azahar or a retail 3DS after the move.
2. **Next.** Record a real master-to-slave cable trade (two mGBA instances on the lockstep driver) as ground truth for what the cartridge
   puts on the wire in every phase and what the slave replies.
3. Wire-level front end for the translator: derive the intents from cable bytes (`60|n` is a nybble sync, a `Dx` byte is a menu
   selection, `$FD` starts a block, ...) instead of from RAM, as a slave that returns the preloaded reply for each clocked byte.
   Offline test: replay the recorded master stream and compare the replies with the real slave's, and the unit stream with the
   hook wrapper's and the retail captures.
4. mGBA serial driver as the permanent slave (no breakpoints); live trades against Azahar, then a retail 3DS.
5. Move translator, session, CCMP and radio into an ESP-IDF component; cable port from the GB-Link GB mode or direct pins (a DMG's
   link line is 5 V, a Game Boy Color's 3.3 V: level shifting is needed for direct pins).

Open: what the GB-Link GB mode passes through and how fast (to ask the GB-Link developers); how the translator paces the 424-byte
block (the cartridge clocks it at about 1 byte per ms while the VC creates it at once); Gen 2 and Yellow cable behaviour is unmeasured.

### 9.1 Findings that shape the wire-level front end (step 3)

From two recorded cable trades (two mGBA windows on the lockstep driver, Red master / Blue slave both times; the 4 October one has the
slave pressing A first), the ROM source (`home/serial.asm`) and the recordings' phase split (`docs/cable/cable_phases.py`,
`cable_blocks.py`; both traces split into the same nine bursts):

* **Roles on the wire.** Master's first exchange is `01`/`02`, the slave's `02`/`01`. The slave then answers `00` once and `FE` until it
  has something. The role is not tied to the lockstep id (it swapped between the two recordings); it went to the window that talked to
  the receptionist first. A wrapper that always presents `02` with an armed external clock always gets a cartridge that is master.
* **`FE` is flow control.** `Serial_ExchangeByte` re-sends the same byte one frame later whenever the reply is `FE`. So when no host unit
  is available at the moment the cartridge clocks, the front end answers `FE` and sends no unit; the cartridge retries. Units then
  exist only for transfers that completed, as in the emulator model.
* **A cartridge cannot be paused.** The emulated ROM waits (in virtual time) for the 3DS's unit; a real master clocks about once per
  2 ms. Blocks must be buffered: the host's block arrives in Pia bursts and is answered from a buffer at wire speed, and the
  cartridge's bytes are queued as units in bulk.
* **Blocks.** After the room sync (26 exchanges: `fe`, `60`x13, `00`x12) come, per block, a run of `fd` preamble bytes and then the data
  (`hSerialIgnoringInitialData` discards what arrives until an `fd` is received): 10-byte random-number list, the 424-byte player block
  and the 200-byte patch lists. The wrapper must recognise the preamble to know when a block starts.
* **Menu cycle.** The master sends `d0 d0 d0 00` per cycle and the slave `d0 d0 00 fe`; when the slave presses A the master's next cycle is
  unchanged (it receives `d4 d4 00 fe`, adopts, and leaves 13 s later at the room sync). It never echoes `d4`.
* **Sync length is the cartridge's.** The master sends `60|n` until it has received a 6x, then a fixed number more and then `00`s; the
  VC host's burst has another length, so after the cartridge's last sync byte the front end keeps answering the host's remaining sync
  units itself (the tail in `udsCableSync`).

Plan for the front end: a slave state machine driven by "the cartridge clocked byte b at time t" returning the byte to preload, with phases
role, sync, menu, and a pass-through for the rest that answers from the host's queued units (`FE` when none) and bulk-buffers blocks.
It is tested offline by replaying the recorded master bytes, with the host's units taken from the Azahar and retail captures.

### 9.2 Step 3, first part: `uds-wire` (role, sync, pass-through)

`uds-wire.c/.h` is the permanent slave. Interface: `udsWirePoll(now)`, `udsWirePreload()` (the byte the slave shifts out next, decided before the
master clocks) and `udsWireExchanged(now, byte)`. Phases DOWN, ROLE, IDLE, SYNC, PASS:

* **DOWN**: no Pia session, or a sync that timed out: the cartridge sees an idle line (`FF`). A session begins once per link-up.
* **ROLE / IDLE**: replies `02`, then `00`, then `FE` to the master's `01 00 00`, as the recorded real slave did; no units (the VC has no role bytes).
* **SYNC**: starts at the master's first 6x. The 3DS side runs by itself (`udsCableSync`); the cartridge gets `FE` until the 3DS's nybble is known, then
  `60|nybble` while it sends 6x and `00` while it sends `00`, so its own fixed loops end with the 3DS's nybble. It ends when the 3DS side has finished and
  the cartridge sends something that is not sync; that exchange gets no unit of its own.
* **PASS**: one unit per completed exchange. A unit from the 3DS is the reply and the master's byte goes out; with none the reply is `FE`, the byte goes
  out once and the cartridge's retry does not send it again; an `FE` that is the 3DS's own unit is data (the retry is a new exchange).

`uds-wire-test` (39 checks) runs a model of the cartridge (`Serial_ExchangeByte` retry after a frame, the three sync loops) against a scripted 3DS, including a late
3DS and a 3DS that never answers, and replays the first 99 recorded exchanges (`uds-wire-test-vectors.h`): the sync starts at exchange 4 and ends at 67 as in the recording,
and the 3DS receives exactly the units its side of the sync produces on its own. Not yet: the link menu and the `fd` blocks as phases of their own (bulk buffering),
a second sync in the room (needs the block context to tell it from data), the mGBA driver around it (step 4).
