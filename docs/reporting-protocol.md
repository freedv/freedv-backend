# Reporting Protocol Reference

This document describes three pieces of "reporting" functionality implemented in
this repository:

1. [**On-air callsign encoding**](#1-on-air-callsign-encoding-rade-reliable-text) — how a callsign (including compound
   callsigns like `VE3/KG6AOV/MM`) is turned into bits that ride inside a RADE
   transmission, and how a receiver recovers it.
2. [**FreeDV Reporter client protocol**](#2-freedv-reporter-client-protocol) — how a client
   (e.g. `freedv-gui`) talks to the `qso.freedv.org` FreeDV Reporter server: connection,
   authentication, and the JSON messages exchanged in both directions.
3. [**UDP callsign broadcast protocol**](#3-udp-callsign-broadcast-protocol) — how a
   client broadcasts received-callsign records as JSON UDP datagrams on the local
   network/LAN, for consumption by other local software (e.g. logging applications).

Source of truth is the code in `src/pipeline/rade_text.{h,cpp}`,
`src/pipeline/ldpc_encode.{h,cpp}`, `src/pipeline/ldpc_decode.{h,cpp}`,
`src/reporting/FreeDVReporter.{h,cpp}`, `src/util/SocketIoClient.{h,cpp}`,
`src/reporting/UdpReporter.{h,cpp}`, and `src/util/UdpHandler.{h,cpp}`. Line
references below point at this revision; consult the files directly for anything not
covered here.

Usage examples referenced throughout come from
[`drowe67/freedv-gui`](https://github.com/drowe67/freedv-gui) (the reference GUI client)
and [`tmiw/freedv-integrations`](https://github.com/tmiw/freedv-integrations) (which
vendors this repo as its `backend` submodule).

---

## 1. On-air callsign encoding (RADE reliable text)

Implemented in `src/pipeline/rade_text.cpp`. This is the mechanism RADE uses to send a
station's callsign (or other short text) alongside voice, robustly enough to survive HF
channel conditions, without needing a separate "end of over" gap.

### 1.1 High-level shape

* A string (typically a callsign, possibly compound, e.g. `VE3/KG6AOV/MM`) is converted
  into 1–4 independent **LDPC(112,56)** codewords ("blocks"), 8 characters per block.
* Each block is bit-interleaved and streamed out **one BPSK symbol per RADE modem
  frame**, continuously and cyclically — there is no separate framing/sync marker
  between codewords; the transmitter just free-runs through block 0, block 1, ...,
  block N‑1, block 0, ... for as long as it is keying.
* The receiver has no a-priori knowledge of where a codeword starts. It keeps a
  112-symbol sliding window and performs LDPC decode attempts against rotations of that
  window until one converges cleanly *and* passes an 8-bit CRC embedded in the payload.
* Blocks carry a 2-bit index and a "last block" flag so a multi-block (>8 character)
  message can be reassembled even if the receiver tunes in mid-cycle or blocks are
  decoded out of order.

### 1.2 Character alphabet (38 symbols)

Only characters that can appear in a callsign are supported
(`convert_string_to_ota_chars_()` / `convert_ota_chars_to_string_()`,
`rade_text.cpp:244-308`):

| OTA value | Meaning |
|---|---|
| 0 | null — terminator / padding (never a "real" character) |
| 1–10 | `'0'`–`'9'` |
| 11–36 | `'A'`–`'Z'` (lowercase input is upper-cased) |
| 37 | `'/'` |

Any other input character is silently dropped.

### 1.3 Packing 8 characters into 42 bits

`RADE_TEXT_CHARS_PER_BLOCK = 8` characters are packed as a single base-38 integer
rather than 6 fixed bits/character (`pack_chars_base38_()` / `unpack_chars_base38_()`,
`rade_text.cpp:310-334`):

```
acc = 0
for each of the 8 chars (in order):
    acc = acc * 38 + char
```

`log2(38) ≈ 5.25`, so 8 characters cost `ceil(log2(38^8)) = 42` bits instead of the 48
bits fixed-width packing would need — the 6 bits saved is what pays for the framing
overhead below while still fitting 8 characters in one block.

### 1.4 56-bit LDPC message payload layout

Each block's LDPC(112,56) **input** (the 56 systematic/message bits, before parity is
computed) is laid out as (`rade_text.cpp:54-97`):

| Field | Bits | Offset | Notes |
|---|---|---|---|
| CRC-8 | 8 | 0 | See §1.6 |
| `last_block` | 1 | 8 | 1 if this is the final block of the message |
| `block_index` | 2 | 9 | 0–3, cycles through up to `RADE_TEXT_MAX_BLOCKS = 4` blocks |
| packed characters | 42 | 11 | base-38 packing of 8 OTA characters, see §1.3 |
| reserved | 3 | 53 | always 0 |

All multi-bit fields are packed **LSB-first** (`set_bits_lsb_first_()` /
`get_bits_lsb_first_()`, `rade_text.cpp:336-358`): bit 0 of the field's value goes into
the lowest bit offset.

A message longer than 8 characters (e.g. a compound callsign) is split across up to 4
blocks of 8 characters each, giving a hard cap of `RADE_TEXT_MAX_LENGTH = 32` characters
per message. The 2-bit `block_index` "for free" supports 4 blocks even though a 1- or
2-block message costs the same 2 bits as a 4-block one.

### 1.5 LDPC(112,56) encode/decode

Defined in `src/pipeline/ldpc_encode.{h,cpp}` and `src/pipeline/ldpc_decode.{h,cpp}`,
using the `HRA_56_56` parity-check matrix, `H = [H_a | H_b]` (56×112) where `H_b` is
lower-bidiagonal:

* **Encode** (`ldpc_encode()`): systematic — the 112-bit codeword is `[s | p]` where `s`
  is the 56 message bits verbatim, and parity `p` is solved by forward substitution:
  `r = H_a * s (mod 2)`, then `p[0] = r[0]`, `p[i] = r[i] XOR p[i-1]` for `i = 1..55`.
* **Decode** (`ldpc_decode()`): soft-decision sum-product belief propagation over BPSK
  symbols. BPSK mapping: bit `0 → +amplitude`, bit `1 → -amplitude`; LLR = `2 · amplitude
  · received_symbol / noise_var`. A decode is only accepted by `rade_text.cpp` if it
  converges (all parity checks satisfied) within `MAX_CONFIDENT_ITERATIONS = 10`
  iterations — a genuinely aligned codeword at workable SNR converges fast, so requiring
  *fast* convergence (not just eventual convergence) is a second, independent filter
  against false-positive rotation guesses, on top of the CRC check in §1.6.

### 1.6 CRC-8 framing check

`calculateBlockCRC_()` (`rade_text.cpp:360-393`) computes an 8-bit CRC over 9 bytes:

```
buf[0]   = block_index | (last_block ? 0x4 : 0)
buf[1:9] = the 8 OTA-alphabet character bytes (0-37 each)
```

Algorithm: bit-by-bit, MSB-first, polynomial `0x1D`, initial CRC `0x00`, no input/output
reflection, no final XOR:

```
crc = 0x00
for each byte b in buf:
    crc ^= b
    repeat 8 times:
        crc = (crc & 0x80) ? (crc << 1) ^ 0x1D : (crc << 1)
```

The CRC covers the framing bits (`block_index`/`last_block`) as well as the characters,
so a bit error that flips the framing is caught rather than silently misfiling the block
during reassembly. Note this fixed 9-byte buffer has **no** "stop at the first zero
byte" convention — `block_index == 0` with `last_block == false` is a legitimate
all-zero framing byte.

### 1.7 Interleaving

After LDPC encoding, the 112 codeword bits are bit-interleaved before transmission
(`interleave_bits()` / `deinterleave_syms()`, `rade_text.cpp:395-412`), using a simple
block interleaver with stride `INTERLEAVER_B = 37`:

```
newIndex = (37 * index) % 112
out[newIndex] = in[index]        // TX: interleave_bits()
out[index]    = in[newIndex]     // RX: deinterleave_syms()
```

This spreads a burst of bit errors (e.g. from fading) across the codeword rather than
letting them cluster, which belief propagation handles much better.

### 1.8 Transmit path

`rade_text_generate_tx_string(ptr, str, strlength)` (`rade_text.cpp:761-835`):

1. Converts `str` to OTA alphabet symbols (§1.2), splits into 1–4 blocks of 8 characters
   each (dropping trailing zero-padding).
2. For each block: computes the CRC (§1.6), assembles the 56-bit payload (§1.4), runs it
   through `ldpc_encode()` (§1.5), then interleaves the 112-bit codeword (§1.7) into
   `tx_text[block]`.
3. Resets the streaming cursor to block 0, symbol 0.

`rade_text_tx_next_symbol(ptr)` (`rade_text.cpp:837-852`) is called **once per RADE
modem frame** by the caller (bit `1 → -1.0`, bit `0 → +1.0`) and advances one bit at a
time through the current block's 112 interleaved bits, then cycles to the next block
(wrapping modulo `tx_num_blocks`) — this repeats indefinitely for as long as the caller
keeps pulling symbols, i.e. for as long as the station is transmitting.

### 1.9 Receive path

`rade_text_rx_symbol(ptr, sym)` (`rade_text.cpp:642-744`) is called once per successful
demodulated symbol. It maintains a 112-symbol circular buffer of the most recent
received soft symbols. Because the transmitter never pauses between codewords, this
buffer is *always* some cyclic rotation of a true codeword — the receiver just doesn't
know the rotation offset when it starts listening.

* The first time the buffer fills, its contents are frozen as a snapshot and a
  **rotation sweep** begins: up to 112 candidate rotations are tried a few at a time
  (`ROTATIONS_PER_CALL = 8` per call, ~14 calls / ~560 ms to cover all 112) rather than
  blocking the real-time callback by testing all of them in one call.
* A rotation "wins" if its LDPC decode converges within the iteration budget (§1.5) *and*
  its CRC checks out (§1.6). To guard against CRC's ~1/256 false-accept rate letting a
  wrong rotation slip through when many hypotheses are tested against the same noisy
  sample, the sweep only accepts a decode if there is a **unique** winner across all 112
  rotations.
* Once no sweep is pending, the receiver falls back to testing the naturally-sliding
  window on every new symbol (this is also what handles resynchronization if noise
  causes the initial sweep to fail).
* A successfully decoded, CRC-valid block is folded into per-message reassembly state
  (`rade_text_ingest_block_()`, `rade_text.cpp:573-628`): `block_index == 0` always
  (re)starts a fresh reassembly (the only available signal that a new pass through the
  transmitter's block cycle has begun, since a receiver may join mid-cycle). Once every
  block up to the one flagged `last_block` has been seen, the full string is
  reconstructed and delivered exactly once per cycle via the callback registered with
  `rade_text_set_rx_callback()`.

### 1.10 Wiring it up

The per-frame streaming calls (`rade_text_tx_next_symbol()` / `rade_text_rx_symbol()`)
already live inside this repo's own RADE pipeline steps, not in the application:
`RADETransmitStep.cpp` calls `rade_tx_set_data_symbol(dv_,
rade_text_tx_next_symbol(textPtr_))` once per TX frame, and `RADEReceiveStep.cpp` calls
`rade_text_rx_symbol(textPtr_, dataSym)` once per successfully demodulated data symbol.
An application built on top of this repo (e.g. `freedv-gui`) only needs to:

```c
rade_text_t rt = rade_text_create();
rade_text_set_rx_callback(rt, my_on_text_rx, my_state);

// Whenever the callsign to transmit changes (e.g. user edits it in settings):
rade_text_generate_tx_string(rt, callsign, strlen(callsign));

// -> my_on_text_rx(rt, decoded_str, len, my_state) is invoked automatically
//    once the pipeline above reassembles a full message.
```

This is exactly what `freedv-gui` (`ms-rade-v2` branch) does in
`FreeDVInterface::setReliableText()` (calls `rade_text_generate_tx_string(radeTextPtr_,
callsign, strlen(callsign))`) and `FreeDVInterface::OnRadeTextRx_()` (the registered RX
callback, which stashes the decoded string). That string is later surfaced through
`getReliableText()` and handed to `FreeDVReporter::addReceiveRecord()` (§2.4) as the
received callsign for a spot.

---

## 2. FreeDV Reporter client protocol

Implemented in `src/reporting/FreeDVReporter.{h,cpp}` (application-level protocol) on
top of `src/util/SocketIoClient.{h,cpp}` (Engine.IO/Socket.IO v4 transport) and
`src/util/TcpConnectionHandler.{h,cpp}` (raw TCP + optional TLS).

### 2.1 Transport

The client speaks **Socket.IO protocol v4 over Engine.IO protocol v4**, using the
WebSocket transport directly — it does **not** perform the usual HTTP long-polling
handshake/upgrade dance; it opens a WebSocket straight away:

```
ws://<host>:<port>/socket.io/?EIO=4&transport=websocket
```

(`SocketIoClient::onConnect_()`, `SocketIoClient.cpp:134-170`). When TLS is requested
(`useSecureConnection_` / `-DDISABLE_TLS_SUPPORT` not set), encryption is applied
underneath by `TcpConnectionHandler` at the raw-socket level (OpenSSL/LibreSSL) — the
URI scheme websocketpp sees is always literally `ws://` because it only builds the
WebSocket framing over an iostream-style transport; the actual bytes on the wire are
still TLS-wrapped when TLS is enabled.

* Default port: 80, or 443 when `useSecureConnection_` is true and TLS support is
  compiled in (`FreeDVReporter.cpp:288-363`).
* `hostname` may be `host` or `host:port`; an explicit port in the string overrides the
  default.
* Default server: `qso.freedv.org` (`FREEDV_REPORTER_DEFAULT_HOSTNAME`,
  `FreeDVReporter.h:51`), used whenever an empty hostname is passed to the constructor.
* Auto-reconnect: enabled unconditionally (`sioClient_->connect(host, port, true,
  tls)`); a dropped connection retries every 5000 ms (`RECONNECT_INTERVAL_MS`,
  `TcpConnectionHandler.cpp:58`).
* Keepalive: on the Engine.IO `open` packet the server-advertised `pingInterval` +
  `pingTimeout` (ms) become the client's own watchdog timeout. The server sends
  Engine.IO ping (`2`) periodically; the client replies pong (`3`) immediately and resets
  its watchdog. If no ping arrives within `pingInterval + pingTimeout`, the client treats
  the connection as dead and disconnects/reconnects (`SocketIoClient.cpp:36-43,
  201-260`).

### 2.2 Engine.IO / Socket.IO packet framing

Every WebSocket text frame is one Engine.IO packet: a single ASCII digit packet-type
prefix, optionally followed by a payload.

| Prefix | Engine.IO meaning |
|---|---|
| `0<json>` | `open` (server→client only) — session info incl. `pingInterval`/`pingTimeout` |
| `1` | `close` |
| `2` | `ping` (server→client) |
| `3` | `pong` (client→server, sent in reply to `2`) |
| `4<...>` | `message` — payload is itself a Socket.IO packet, see below |

Inside an Engine.IO `message` (`4`), the next character is the Socket.IO packet type:

| Prefix | Socket.IO meaning |
|---|---|
| `40<json>` | `CONNECT` (client→server) — namespace connect + **auth payload** (§2.3) |
| `40` | `CONNECT` ack (server→client) — connection to namespace accepted |
| `42<json array>` | `EVENT` — `[eventName, ...args]`; used for every application message in both directions (§2.4/§2.5) |
| `44` | connect `ERROR` (server→client) — client disconnects and lets its reconnect timer retry |

So a client emitting `tx_report` sends the literal WebSocket text frame:
```
42["tx_report",{"mode":"RADEV1","transmitting":true}]
```
(`SOCKET_IO_TX_PREFIX = "42"`, `SocketIoClient::emit()`, `SocketIoClient.cpp:79-118`).

### 2.3 Authentication

Authentication happens as the payload of the Socket.IO `40` (namespace CONNECT) packet,
sent immediately after the WebSocket opens (`SocketIoClient.cpp:146-151`,
`FreeDVReporter::connect_()`, `FreeDVReporter.cpp:288-379`). There is no separate
login step and no token — the "auth" is simply this JSON object, built once at
`connect()` time:

If the client has **no callsign or no grid square** configured
(`isValidForReporting()` is false — `FreeDVReporter.cpp:283-286`), it authenticates
read-only:
```json
{
  "role": "view",
  "protocol_version": 2
}
```

Otherwise it authenticates as a reporting station:
```json
{
  "role": "report",
  "callsign": "VE3XYZ",
  "grid_square": "FN25",
  "version": "FreeDV 2.0.1",
  "rx_only": false,
  "os": "macOS 14.5",
  "protocol_version": 2
}
```

Field notes:

| Field | Meaning |
|---|---|
| `role` | `"view"` (read-only, no callsign/grid needed), `"report"` (normal two-way client), or `"report_wo"` ("write-only" — set when the `FreeDVReporter` constructor's `writeOnly` argument is `true`; reports this station's own state without expecting/needing the full peer roster back) |
| `callsign` / `grid_square` | This station's callsign and Maidenhead grid square |
| `version` | Free-form client version string, e.g. `"FreeDV " + GetFreeDVVersion()` in `freedv-gui` |
| `rx_only` | Whether this station only receives (never transmits) |
| `os` | Free-form OS description from `GetOperatingSystemString()` (`src/os/os_interface.h`) |
| `protocol_version` | Currently `2` (`FREEDV_REPORTER_PROTOCOL_VERSION`, `FreeDVReporter.h:126-128`); always sent, regardless of role |

The server acks the namespace connect (Socket.IO `40`), then emits its own
`connection_successful` application event (§2.5) once it has fully accepted the client;
`FreeDVReporter` treats *that* — not the transport-level connect — as "fully connected"
and only then starts emitting queued state (§2.4).

### 2.4 Client → server events

All are Socket.IO `EVENT` (`42`) packets `[eventName, dataObject]`. Every one of these
public methods is a no-op unless `isValidForReporting()` is true (i.e. a non-empty
callsign and grid square were configured — a `role: "view"` client cannot send any of
these). Beyond that, gating differs slightly by event: `freq_change`/`tx_report`/
`message_update` cache their latest value locally regardless of connection state and
are flushed once `connection_successful` fires (or on `show_self`), so a report made
while briefly disconnected isn't lost, just delayed. `rx_report` has no such cache —
`addReceiveRecord()` requires the client to already be fully connected or the record is
simply dropped. `qsy_request` is not gated on connection state at all; if the socket
isn't open yet the message is silently lost.

| Event | Payload | Sent when |
|---|---|---|
| `freq_change` | `{"freq": <uint64>}` | `freqChange(frequency)` called; frequency in Hz. Cached and replayed on (re)connect. |
| `tx_report` | `{"mode": <string>, "transmitting": <bool>}` | `transmit(mode, tx)` called, e.g. mode `"RADEV1"`, `"700D"`, `"2020"`. Cached and replayed on (re)connect. |
| `rx_report` | `{"callsign": <string>, "mode": <string>, "snr": <int>}` | `addReceiveRecord(callsign, mode, frequency, snr)` called. **Note:** the `frequency` parameter is accepted by the C++ API but is *not* included in this payload — frequency context comes from the station's last `freq_change`. |
| `message_update` | `{"message": <string>}` | `updateMessage(message)` called (free-text status message). Cached and replayed on (re)connect. |
| `qsy_request` | `{"dest_sid": <string>, "message": <string>, "frequency": <uint64>}` | `requestQSY(sid, frequencyHz, message)` called — ask another connected station (by its `sid`) to move to a frequency. |
| `hide_self` | *(none)* | `hideFromView()` / `inAnalogMode(true)` — temporarily hide this station from other clients' rosters (e.g. while operating analog, off-list). |
| `show_self` | *(none)* | `showOurselves()` / `inAnalogMode(false)` — re-show, then immediately re-sends cached `freq_change`/`tx_report`/`message_update`. |

On every reconnect, once `connection_successful` fires again, the client automatically
re-sends its last known frequency, mode/tx state, and message (or re-sends `hide_self` if
currently hidden) so the server-side view of this station doesn't go stale — see
`FreeDVReporter::onFreeDVReporterConnectionSuccessful_()`.

### 2.5 Server → client events

Also Socket.IO `EVENT` (`42`) packets. Each handler validates every field's JSON type
before invoking the registered callback; a malformed message is silently dropped.

| Event | Payload | Meaning |
|---|---|---|
| `connection_successful` | *(none required)* | Auth accepted; server considers this client fully joined. Triggers the client to (re)send its cached state (§2.4). |
| `new_connection` | `{"sid", "last_update", "callsign", "grid_square", "version", "rx_only": <bool>, "connect_time"}` (all others string) | Another station joined / is now visible. |
| `remove_connection` | `{"sid", "last_update", "callsign", "grid_square", "version", "rx_only": <bool>}` | A station disconnected or hid itself. (No `connect_time` — always delivered to the app callback as `""`.) |
| `tx_report` | `{"sid", "last_update", "callsign", "grid_square", "mode", "transmitting": <bool>, "last_tx": <string or null>}` | Another station's TX state changed. Same event *name* as the client→server report but a different shape/direction. |
| `rx_report` | `{"sid", "last_update", "receiver_callsign", "receiver_grid_square", "callsign", "snr": <int or float>, "mode"}` | Another station reported hearing someone. `receiver_callsign`/`receiver_grid_square` = who heard it; `callsign` = who was heard. |
| `freq_change` | `{"sid", "last_update", "callsign", "grid_square", "freq": <uint64>}` | Another station changed frequency. |
| `message_update` | `{"sid", "last_update", "message"}` | Another station's free-text status message changed. |
| `qsy_request` | `{"callsign", "frequency": <uint64>, "message"}` | *This* client is being asked (by whoever sent `dest_sid` targeting it) to move to `frequency`. No `sid` — it's addressed to the recipient directly. |
| `bulk_update` | `[[eventName, eventArgs], ...]` | A batch of the above events (e.g. the initial roster snapshot right after connecting), replayed locally one at a time through the same handlers as if each had arrived individually. |

### 2.6 Minimal client example

```c++
FreeDVReporter reporter(
    "qso.freedv.org",   // hostname (":port" optional)
    "VE3XYZ",           // callsign
    "FN25",              // grid square
    "MyClient 1.0",      // software/version string
    /* rxOnly */ false,
    /* writeOnly */ false,
    /* useSecureConnection */ true);

reporter.setConnectionSuccessfulFn([]() { /* fully joined */ });
reporter.setOnUserConnectFn([](std::string sid, std::string lastUpdate,
                                std::string callsign, std::string grid,
                                std::string version, bool rxOnly,
                                std::string connectTime) { /* roster add */ });
reporter.setOnReceiveUpdateFn([](std::string sid, std::string lastUpdate,
                                  std::string receiverCallsign, std::string receiverGrid,
                                  std::string heardCallsign, float snr,
                                  std::string mode) { /* someone heard heardCallsign */ });

reporter.connect();

reporter.freqChange(14236000);
reporter.transmit("RADEV1", true);
reporter.addReceiveRecord("W1ABC", "RADEV1", 14236000, /* snr */ -2);
```

This mirrors how `freedv-gui` wires things up in
`MainFrame::initializeFreeDVReporter_()` (constructs the shared `FreeDVReporter`) and
`FreeDVReporterDialog::FreeDVReporterDataModel` (`freedv_reporter.cpp`, binds all the
`setOn*Fn` callbacks to populate its roster UI).

### 2.7 Related, but out of scope here

`src/reporting/` also contains `pskreporter.{h,cpp}` (PSK Reporter's own UDP spot
protocol) and `CsvReporter.{h,cpp}` — other `IReporter` implementations used for
reporting spots to services other than FreeDV Reporter. They don't share
`FreeDVReporter`'s Socket.IO protocol and are not covered by this document. `UdpReporter`
is also an `IReporter` implementation, but it speaks a protocol defined by this repo
(rather than a third party's), so it's documented in full below.

---

## 3. UDP callsign broadcast protocol

Implemented in `src/reporting/UdpReporter.{h,cpp}` on top of the generic
`src/util/UdpHandler.{h,cpp}`. Unlike the FreeDV Reporter client (§2), this is not a
connection to a central server — it's a **local, send-only, fire-and-forget UDP
broadcast/multicast** of each received callsign as a single JSON datagram, intended for
other software on the same machine/LAN to pick up (e.g. logging applications), in the
same spirit as WSJT-X's UDP broadcast (`UdpHandler.cpp:261` explicitly notes this: *"This
class is mainly for WSJT-X style logging"*).

### 3.1 Transport

* Plain UDP, one datagram per received callsign — no handshake, no acknowledgement, no
  session state, and nothing is ever read back (`UdpReporter::onReceive_()` is a no-op,
  `UdpReporter.h:62`).
* `UdpReporter(address, port)` opens a UDP socket bound to no particular local address/
  port, and always sends to the same configured destination `address:port`
  (`UdpReporter.cpp:45-55`, `UdpHandler::open("", 0, address, port)`).
* `address` is a literal IPv4 or IPv6 address (not a hostname — `UdpHandler`'s resolver
  uses `AI_NUMERICHOST`, `UdpHandler.cpp:265-290`). It can be:
  * a **unicast** address, in which case datagrams simply go to that one host, or
  * a **multicast** address (`224.0.0.0`–`239.255.255.255` for IPv4, `ff00::/8` for
    IPv6), in which case `UdpHandler` automatically joins that multicast group on the
    default interface when the socket is opened (`isMulticastAddress_()` /
    `joinMulticastGroup_()`, `UdpHandler.cpp:292-422`) — this is what lets other local
    processes listening on that multicast group receive the datagrams without needing
    to know this process's address ahead of time.
* `freedv-gui`'s reference default (`ReportingConfiguration.cpp`) and
  `freedv-integrations`'s `ReportingController` both use the same convention: multicast
  address **`224.0.0.1`** (the "all systems on this subnet" address), port **`7177`**,
  disabled by default and user-configurable in `freedv-gui`'s Tools → Options → Reporting
  tab (`udpBroadcastEnabled`/`udpBroadcastAddress`/`udpBroadcastPort`).
* Of `IReporter`'s methods, only `addReceiveRecord()` actually sends anything.
  `freqChange()` just caches the frequency on the object (currently unused elsewhere in
  the class); `transmit()`, `inAnalogMode()`, and `send()` are no-ops — they exist only
  to satisfy the shared `IReporter` interface (§2.7 above / `IReporter.h`).

### 3.2 Datagram payload

Each call to `addReceiveRecord(callsign, mode, frequency, snr)` serialises and sends
exactly one JSON object as the UDP payload (`UdpReporter::addReceiveRecord()`,
`UdpReporter.cpp:73-119`):

```json
{
  "type": "fdv_callsign",
  "version": 1,
  "timestamp": "2026-08-16T20:31:07Z",
  "callsign": "W1ABC",
  "mode": "RADEV1",
  "snr": -2,
  "frequency_hz": 14236000
}
```

| Field | Type | Notes |
|---|---|---|
| `type` | string | Always the literal `"fdv_callsign"` — a receiver can use this to distinguish this payload from other UDP traffic that might land on the same port. |
| `version` | int | Message schema version, currently always `1` (`JSON_MESSAGE_VERSION`, `UdpReporter.h:69`). Bump expected if the schema changes. |
| `timestamp` | string | UTC timestamp of the report, ISO-8601 `YYYY-MM-DDTHH:MM:SSZ` (`std::gmtime`/`strftime`, `UdpReporter.cpp:76-81`). |
| `callsign` | string | The received callsign, exactly as passed to `addReceiveRecord()` (e.g. as decoded via the RADE reliable-text channel in §1, or via another mode's equivalent). |
| `mode` | string | The FreeDV mode string in use, e.g. `"RADEV1"`, `"700D"`, `"2020"`. |
| `snr` | int | Signal-to-noise ratio in dB, as a signed byte widened to a JSON integer. |
| `frequency_hz` | uint | Frequency in Hz, taken directly from the `frequency` argument passed to `addReceiveRecord()` (**not** from a previously-cached `freqChange()` value — see §3.1). |

There is no equivalent "TX report" / "frequency change" / roster datagram — this
protocol only ever announces received callsigns.

### 3.3 Minimal listener example

Since this is one JSON object per UDP datagram on a well-known multicast group, a
listener is trivial to write in any language, e.g. Python:

```python
import socket, struct, json

MCAST_GRP, MCAST_PORT = "224.0.0.1", 7177

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("", MCAST_PORT))
sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                struct.pack("4sl", socket.inet_aton(MCAST_GRP), socket.INADDR_ANY))

while True:
    data, _ = sock.recvfrom(4096)
    record = json.loads(data)
    if record.get("type") == "fdv_callsign":
        print(record["callsign"], record["mode"], record["snr"], record["frequency_hz"])
```
