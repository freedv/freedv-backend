# Reporting Protocol Reference

This document describes the "reporting" functionality implemented in this repository:

1. [**FreeDV Reporter client protocol**](#1-freedv-reporter-client-protocol) — how a client
   (e.g. `freedv-gui`) talks to the `qso.freedv.org` FreeDV Reporter server: connection,
   authentication, and the JSON messages exchanged in both directions.
2. [**UDP callsign broadcast protocol**](#2-udp-callsign-broadcast-protocol) — how a
   client broadcasts received-callsign records as JSON UDP datagrams on the local
   network/LAN, for consumption by other local software (e.g. logging applications).
3. [**CSV callsign log**](#3-csv-callsign-log) — the file of received callsigns that
   `CsvReporter` appends to.

All three can report the **sideband** (USB or LSB) the station's radio is on, where the
application knows it (e.g. from rig control) and tells them through
`IReporter::sidebandChange()`.

Source of truth is the code in `src/reporting/IReporter.h`,
`src/reporting/FreeDVReporter.{h,cpp}`, `src/util/SocketIoClient.{h,cpp}`,
`src/reporting/UdpReporter.{h,cpp}`, `src/util/UdpHandler.{h,cpp}` and
`src/reporting/CsvReporter.{h,cpp}`. Line
references below point at this revision; consult the files directly for anything not
covered here.

Usage examples referenced throughout come from
[`drowe67/freedv-gui`](https://github.com/drowe67/freedv-gui) (the reference GUI client)
and [`tmiw/freedv-integrations`](https://github.com/tmiw/freedv-integrations) (which
vendors this repo as its `backend` submodule).

---

## 1. FreeDV Reporter client protocol

Implemented in `src/reporting/FreeDVReporter.{h,cpp}` (application-level protocol) on
top of `src/util/SocketIoClient.{h,cpp}` (Engine.IO/Socket.IO v4 transport) and
`src/util/TcpConnectionHandler.{h,cpp}` (raw TCP + optional TLS).

### 1.1 Transport

The client speaks **Socket.IO protocol v4 over Engine.IO protocol v4**, using the
WebSocket transport directly — it does **not** perform the usual HTTP long-polling
handshake/upgrade dance; it opens a WebSocket straight away:

```
ws://<host>:<port>/socket.io/?EIO=4&transport=websocket
```

(`SocketIoClient::onConnect_()`, `SocketIoClient.cpp:145-186`). When TLS is requested
(`useSecureConnection_` / `-DDISABLE_TLS_SUPPORT` not set), encryption is applied
underneath by `TcpConnectionHandler` at the raw-socket level (OpenSSL/LibreSSL) — the
URI scheme websocketpp sees is always literally `ws://` because it only builds the
WebSocket framing over an iostream-style transport; the actual bytes on the wire are
still TLS-wrapped when TLS is enabled.

* Default port: 80, or 443 when `useSecureConnection_` is true and TLS support is
  compiled in (`FreeDVReporter.cpp:362-379`).
* `hostname` may be `host` or `host:port`; an explicit port in the string overrides the
  default.
* Default server: `qso.freedv.org` (`FREEDV_REPORTER_DEFAULT_HOSTNAME`,
  `FreeDVReporter.h:51`), used whenever an empty hostname is passed to the constructor.
* Auto-reconnect: enabled unconditionally (`sioClient_->connect(host, port, true,
  tls)`); a dropped connection retries every 5000 ms (`RECONNECT_INTERVAL_MS`,
  `TcpConnectionHandler.cpp:60`).
* Keepalive: on the Engine.IO `open` packet the server-advertised `pingInterval` +
  `pingTimeout` (ms) become the client's own watchdog timeout. The server sends
  Engine.IO ping (`2`) periodically; the client replies pong (`3`) immediately and resets
  its watchdog. If no ping arrives within `pingInterval + pingTimeout`, the client treats
  the connection as dead and disconnects/reconnects (`SocketIoClient.cpp:38-45,
  223-313`).

### 1.2 Engine.IO / Socket.IO packet framing

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
| `40<json>` | `CONNECT` (client→server) — namespace connect + **auth payload** (§1.3) |
| `40` | `CONNECT` ack (server→client) — connection to namespace accepted |
| `42<json array>` | `EVENT` — `[eventName, ...args]`; used for every application message in both directions (§1.4/§1.5) |
| `44` | connect `ERROR` (server→client) — client disconnects and lets its reconnect timer retry |

So a client emitting `tx_report` sends the literal WebSocket text frame:
```
42["tx_report",{"mode":"RADEV1","transmitting":true}]
```
(`SOCKET_IO_TX_PREFIX = "42"`, `SocketIoClient::emit()`, `SocketIoClient.cpp:90-129`).

### 1.3 Authentication

Authentication happens as the payload of the Socket.IO `40` (namespace CONNECT) packet,
sent immediately after the WebSocket opens (`SocketIoClient.cpp:157-162`,
`FreeDVReporter::connect_()`, `FreeDVReporter.cpp:317-401`). There is no separate
login step and no token — the "auth" is simply this JSON object, built once at
`connect()` time:

If the client has **no callsign or no grid square** configured
(`isValidForReporting()` is false — `FreeDVReporter.cpp:312-315`), it authenticates
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
| `protocol_version` | Currently `2` (`FREEDV_REPORTER_PROTOCOL_VERSION`, `FreeDVReporter.h:145-147`); always sent, regardless of role |

The server acks the namespace connect (Socket.IO `40`), then emits its own
`connection_successful` application event (§1.5) once it has fully accepted the client;
`FreeDVReporter` treats *that* — not the transport-level connect — as "fully connected"
and only then starts emitting queued state (§1.4).

### 1.4 Client → server events

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
| `freq_change` | `{"freq": <uint64>, "sideband": <string>}` | `freqChange(frequency)` called; frequency in Hz. Also sent, with the last frequency, when `sidebandChange(sideband)` changes the sideband (an unchanged one sends nothing). `sideband` is `"USB"`, `"LSB"`, or `""` if not known (e.g. no rig control) — always included, so that going back to not knowing is passed on too. Both are cached and replayed on (re)connect. |
| `tx_report` | `{"mode": <string>, "transmitting": <bool>}` | `transmit(mode, tx)` called, e.g. mode `"RADEV1"`, `"700D"`, `"2020"`. Cached and replayed on (re)connect. |
| `rx_report` | `{"callsign": <string>, "mode": <string>, "snr": <int>}` | `addReceiveRecord(callsign, mode, frequency, snr)` called. **Note:** the `frequency` parameter is accepted by the C++ API but is *not* included in this payload — frequency context comes from the station's last `freq_change`. |
| `message_update` | `{"message": <string>}` | `updateMessage(message)` called (free-text status message). Cached and replayed on (re)connect. |
| `qsy_request` | `{"dest_sid": <string>, "message": <string>, "frequency": <uint64>, "sideband": <string>}` | `requestQSY(sid, frequencyHz, message)` called — ask another connected station (by its `sid`) to move to a frequency, and to the sideband this station has reported (`"USB"`, `"LSB"` or `""`, as in `freq_change`). |
| `hide_self` | *(none)* | `hideFromView()` / `inAnalogMode(true)` — temporarily hide this station from other clients' rosters (e.g. while operating analog, off-list). |
| `show_self` | *(none)* | `showOurselves()` / `inAnalogMode(false)` — re-show, then immediately re-sends cached `freq_change`/`tx_report`/`message_update`. |

On every reconnect, once `connection_successful` fires again, the client automatically
re-sends its last known frequency and sideband, mode/tx state, and message (or re-sends `hide_self` if
currently hidden) so the server-side view of this station doesn't go stale — see
`FreeDVReporter::onFreeDVReporterConnectionSuccessful_()`.

### 1.5 Server → client events

Also Socket.IO `EVENT` (`42`) packets. Each handler validates every field's JSON type
before invoking the registered callback; a malformed message is silently dropped.

| Event | Payload | Meaning |
|---|---|---|
| `connection_successful` | *(none required)* | Auth accepted; server considers this client fully joined. Triggers the client to (re)send its cached state (§1.4). |
| `new_connection` | `{"sid", "last_update", "callsign", "grid_square", "version", "rx_only": <bool>, "connect_time"}` (all others string) | Another station joined / is now visible. |
| `remove_connection` | `{"sid", "last_update", "callsign", "grid_square", "version", "rx_only": <bool>}` | A station disconnected or hid itself. (No `connect_time` — always delivered to the app callback as `""`.) |
| `tx_report` | `{"sid", "last_update", "callsign", "grid_square", "mode", "transmitting": <bool>, "last_tx": <string or null>}` | Another station's TX state changed. Same event *name* as the client→server report but a different shape/direction. |
| `rx_report` | `{"sid", "last_update", "receiver_callsign", "receiver_grid_square", "callsign", "snr": <int or float>, "mode"}` | Another station reported hearing someone. `receiver_callsign`/`receiver_grid_square` = who heard it; `callsign` = who was heard. |
| `freq_change` | `{"sid", "last_update", "callsign", "grid_square", "freq": <uint64>, "sideband"}` | Another station changed frequency or sideband. Delivered to the `FrequencyChangeFn` (`setOnFrequencyChangeFn()`), then — when `sideband` is present — to the `SidebandChangeFn` (`setOnSidebandChangeFn()`) as `IReporter::Sideband` (`USB`, `LSB`, or `UNKNOWN` for `""` or anything else). Servers that predate the sideband don't send it; the `SidebandChangeFn` isn't called then. |
| `message_update` | `{"sid", "last_update", "message"}` | Another station's free-text status message changed. |
| `qsy_request` | `{"callsign", "frequency": <uint64>, "message", "sideband"}` | *This* client is being asked (by whoever sent `dest_sid` targeting it) to move to `frequency`, on the requester's `sideband` where given. No `sid` — it's addressed to the recipient directly. Delivered to the `QsyRequestWithSidebandFn` (`setOnQSYRequestWithSidebandFn()`, with the sideband as `IReporter::Sideband`; `UNKNOWN` if missing) if one is set, otherwise to the `QsyRequestFn` (`setOnQSYRequestFn()`). |
| `bulk_update` | `[[eventName, eventArgs], ...]` | A batch of the above events (e.g. the initial roster snapshot right after connecting), replayed locally one at a time through the same handlers as if each had arrived individually. |

### 1.6 Minimal client example

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
reporter.sidebandChange(IReporter::Sideband::USB); // where known, e.g. from rig control
reporter.transmit("RADEV1", true);
reporter.addReceiveRecord("W1ABC", "RADEV1", 14236000, /* snr */ -2);
```

This mirrors how `freedv-gui` wires things up in
`MainFrame::initializeFreeDVReporter_()` (constructs the shared `FreeDVReporter`) and
`FreeDVReporterDialog::FreeDVReporterDataModel` (`freedv_reporter.cpp`, binds all the
`setOn*Fn` callbacks to populate its roster UI).

### 1.7 Related, but out of scope here

`src/reporting/` also contains `pskreporter.{h,cpp}` (PSK Reporter's own UDP spot
protocol) — another `IReporter` implementation, used for reporting spots to a service
other than FreeDV Reporter. It doesn't share `FreeDVReporter`'s Socket.IO protocol, and
isn't covered by this document (it ignores the sideband). `UdpReporter` and `CsvReporter`
are also `IReporter` implementations, but their formats are defined by this repo
(rather than a third party), so they're documented below (§2, §3).

---

## 2. UDP callsign broadcast protocol

Implemented in `src/reporting/UdpReporter.{h,cpp}` on top of the generic
`src/util/UdpHandler.{h,cpp}`. Unlike the FreeDV Reporter client (§1), this is not a
connection to a central server — it's a **local, send-only, fire-and-forget UDP
broadcast/multicast** of each received callsign as a single JSON datagram, intended for
other software on the same machine/LAN to pick up (e.g. logging applications), in the
same spirit as WSJT-X's UDP broadcast (`UdpHandler.cpp:276` explicitly notes this: *"This
class is mainly for WSJT-X style logging"*).

### 2.1 Transport

* Plain UDP, one datagram per received callsign — no handshake, no acknowledgement, no
  session state, and nothing is ever read back (`UdpReporter::onReceive_()` is a no-op,
  `UdpReporter.h:67`).
* `UdpReporter(address, port)` opens a UDP socket bound to no particular local address/
  port, and always sends to the same configured destination `address:port`
  (`UdpReporter.cpp:45-56`, `UdpHandler::open("", 0, address, port)`).
* `address` is a literal IPv4 or IPv6 address (not a hostname — `UdpHandler`'s resolver
  uses `AI_NUMERICHOST`, `UdpHandler.cpp:280-305`). It can be:
  * a **unicast** address, in which case datagrams simply go to that one host, or
  * a **multicast** address (`224.0.0.0`–`239.255.255.255` for IPv4, `ff00::/8` for
    IPv6), in which case `UdpHandler` automatically joins that multicast group on the
    default interface when the socket is opened (`isMulticastAddress_()` /
    `joinMulticastGroup_()`, `UdpHandler.cpp:307-433`) — this is what lets other local
    processes listening on that multicast group receive the datagrams without needing
    to know this process's address ahead of time.
* `freedv-gui`'s reference default (`ReportingConfiguration.cpp`) and
  `freedv-integrations`'s `ReportingController` both use the same convention: multicast
  address **`224.0.0.1`** (the "all systems on this subnet" address), port **`7177`**,
  disabled by default and user-configurable in `freedv-gui`'s Tools → Options → Reporting
  tab (`udpBroadcastEnabled`/`udpBroadcastAddress`/`udpBroadcastPort`).
* Of `IReporter`'s methods, only `addReceiveRecord()` actually sends anything.
  `freqChange()` just caches the frequency on the object (currently unused elsewhere in
  the class); `sidebandChange()` caches the sideband, which goes in each record from then
  on (§2.2); `transmit()`, `inAnalogMode()`, and `send()` are no-ops — they exist only
  to satisfy the shared `IReporter` interface (§1.7 above / `IReporter.h`).

### 2.2 Datagram payload

Each call to `addReceiveRecord(callsign, mode, frequency, snr)` serialises and sends
exactly one JSON object as the UDP payload (`UdpReporter::addReceiveRecord()`,
`UdpReporter.cpp:79-129`):

```json
{
  "type": "fdv_callsign",
  "version": 1,
  "timestamp": "2026-08-16T20:31:07Z",
  "callsign": "W1ABC",
  "mode": "RADEV1",
  "snr": -2,
  "frequency_hz": 14236000,
  "sideband": "USB"
}
```

| Field | Type | Notes |
|---|---|---|
| `type` | string | Always the literal `"fdv_callsign"` — a receiver can use this to distinguish this payload from other UDP traffic that might land on the same port. |
| `version` | int | Message schema version, currently always `1` (`JSON_MESSAGE_VERSION`, `UdpReporter.h:75`). Bump expected if the schema changes. |
| `timestamp` | string | UTC timestamp of the report, ISO-8601 `YYYY-MM-DDTHH:MM:SSZ` (`std::gmtime`/`strftime`, `UdpReporter.cpp:82-87`). |
| `callsign` | string | The received callsign, exactly as passed to `addReceiveRecord()` (e.g. as decoded from RADE's reliable-text channel). |
| `mode` | string | The FreeDV mode string in use, e.g. `"RADEV1"`, `"700D"`, `"2020"`. |
| `snr` | int | Signal-to-noise ratio in dB, as a signed byte widened to a JSON integer. |
| `frequency_hz` | uint | Frequency in Hz, taken directly from the `frequency` argument passed to `addReceiveRecord()` (**not** from a previously-cached `freqChange()` value — see §2.1). |
| `sideband` | string | The sideband the radio was on, as last given to `sidebandChange()`: `"USB"`, `"LSB"`, or `""` if not known. Added without changing `version`; listeners that don't know it ignore it, as they would any other field they don't know. |

There is no equivalent "TX report" / "frequency change" / roster datagram — this
protocol only ever announces received callsigns.

### 2.3 Minimal listener example

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
        # ("sideband" is "" if not known, and missing from older senders.)
        print(record["callsign"], record["mode"], record["snr"], record["frequency_hz"],
              record.get("sideband", ""))
```

---

## 3. CSV callsign log

Implemented in `src/reporting/CsvReporter.{h,cpp}`. Each received callsign is appended
as one line to a CSV file (RFC 4180: a field containing a comma, quote or line break is
quoted, with quotes doubled — over-the-air callsigns can contain commas). The file is
opened in append mode; a new (empty) file is given a header line first:

```
date,time,callsign,mode,frequency_hz,snr_db,sideband
2026-08-16,20:31:07,W1ABC,RADEV1,14236000,-2,USB
```

| Column | Notes |
|---|---|
| `date`, `time` | UTC, `YYYY-MM-DD` and `HH:MM:SS`. |
| `callsign`, `mode`, `frequency_hz`, `snr_db` | As passed to `addReceiveRecord()`, as for the UDP datagram (§2.2). |
| `sideband` | The sideband the radio was on, as last given to `sidebandChange()`: `USB`, `LSB`, or empty if not known. |

A file started before the `sideband` column was added has a header without it
(`date,time,callsign,mode,frequency_hz,snr_db`). When `CsvReporter` opens such a file, it
gives it the column (`CsvReporter::upgradeToSidebandColumn_()`):

* The original is copied to `<file>.bak` first (or `<file>.bak.1`, `.bak.2`… if that name's
  already taken, so an earlier backup is never replaced).
* The new header, then each existing six-column record with an empty sideband added
  (`…,-2,`), are written to `<file>.tmp`, which is then renamed over the original — so the
  log is never left half-written. Records are read as RFC 4180 records, so a quoted comma
  or line break in a callsign is kept intact; a record that doesn't have six fields is
  copied as it is rather than guessed at. Line endings become those `CsvReporter` writes
records with (the platform's own: CRLF on Windows).
* If any of that can't be done (e.g. the folder can't be written to, or on Windows another
  program has the file open), the temporary file and backup are removed, the file is left
  exactly as it was, and records appended to it keep to its six columns so that its header
  and records still match.

A reader that wants to handle any file should go by the header, or treat the seventh column
as optional.

Of `IReporter`'s other methods, `sidebandChange()` caches the sideband; `freqChange()`,
`transmit()`, `inAnalogMode()`, and `send()` do nothing.
