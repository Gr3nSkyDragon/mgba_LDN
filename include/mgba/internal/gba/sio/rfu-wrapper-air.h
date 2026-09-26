/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_RFU_WRAPPER_AIR_H
#define GBA_SIO_RFU_WRAPPER_AIR_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gba/sio/rfu-wrapper.h>

/*
 * The wireless side of the RFU Cable Wrapper: the translator between the game on the cable (Ruby/Sapphire, player 1 of
 * a cable link whose master is the wrapper) and an FRLG game that is the leader (host) of a wireless Direct Corner
 * trade.
 *
 * Toward the wireless side it behaves like the FRLG joiner: it opens a headless wireless-adapter backend (the same
 * "local"/"broadcast"/... backends a real adapter uses), finds the leader, joins it, runs the librfu link layer of a
 * child (NI handshake, one 16-byte UNI frame per host frame with a rolling command tag, block send/receive, standby and
 * close barriers, held keys) and answers the leader's pulls.
 *
 * Toward the cable it plays the rest of the link: the virtual master (player 0) whose commands are the leader's. The
 * trade menu's own protocol (20-byte link blocks with commands AABB, DDDD, ...) is identical in both families and
 * passes through as ordinary blocks; the translator only converts the transport: block framing (14-byte cable chunks
 * vs 12-byte RFU fragments), held keys, standby/close barriers, the LinkPlayer and trainer card exchanges and the
 * "send me block N" requests.
 *
 * Function names follow the pret decomps (pokeruby link.c, pokefirered link_rfu_2.c/librfu_rfu.c); the child-side
 * behaviour is checked against frlg-ldn-trade's frlgsim (reference only) and a real FireRed/LeafGreen joiner capture.
 */

// Attach the wireless side to the wrapper using the named backend ("local", "broadcast", "esp32"). Returns false for a
// backend that does not exist (the wrapper then keeps its stub peer). tracePath (optional) receives the backend's own
// protocol trace (Wi-Fi/serial/LDN details); the wrapper's translator lines go to the wrapper trace.
bool GBASIORFUWrapperAttachAir(struct GBASIORFUWrapper* wrapper, const char* backend, const char* tracePath);

CXX_GUARD_END

#endif
