# LAN3D discovery and session control v1

The GUI supplies a persistent 36-character GUID and a UTF-8 display name. Default
discovery uses UDP port 48731. Every second the client announces its identity,
name, TCP control port and current idle/sending/receiving/busy state. It sends to
the directed broadcast of every operational IPv4 adapter and the limited
broadcast address. Adapter addresses are refreshed every ten seconds. A peer
expires after four seconds without a valid announcement. The source IP always
comes from `recvfrom`, never from a payload field.

The control listener binds an OS-selected available TCP port by default and
announces that port. One control connection represents one requested/active
session. Clicking a peer makes this computer the **sender**: its request carries
the sender's media listening port. The receiving GUI gets an `IncomingRequest`
event, starts its own receiver using the socket's actual source IP, and calls
`Reply(requestId, true, ...)` when ready. A rejection returns its reason to the
sender. Incoming requests require an idle local client. Only one pending or
active session is allowed in either direction; simultaneous reciprocal requests
are rejected or expire within their bounded deadline, so neither side waits
indefinitely for the other.

`RequestReceive` blocks for the handshake and must run on a GUI background task.
`Reply`, `EndSession`, `SetState`, `Snapshot` and `PollEvents` are safe from the
UI thread. A failed/late `Reply` means the GUI must undo any receiver preparation.
An expired or disconnected incoming request also emits `SessionEnded`, carrying
its original request id and token. `EndSession` cancels a matching pending
handshake as well as ending an accepted session. UI code should match events by
the full peer id and token before stopping any local process.

Accepted sessions exchange heartbeats every second over the same TCP connection.
Four seconds without a valid heartbeat ends the session. Explicit end, TCP loss,
invalid control messages and service shutdown also emit `SessionEnded`. An
outbound session event has request id zero. Tokens are exactly 32 hexadecimal
digits, case-insensitive, and cannot be reused with a peer for 60 seconds.

## Wire encoding

All integers use network byte order. Every UDP datagram and TCP message begins
with a 16-byte header:

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 4 | `0x4c335053` (`L3PS`) |
| 4 | 2 | Version `1` |
| 6 | 2 | Message type: Hello=1, Request=2, Response=3, Heartbeat=4, End=5 |
| 8 | 4 | Payload byte count, maximum 512 |
| 12 | 4 | Reserved, zero |

GUIDs are fixed 36-byte ASCII strings; tokens are fixed 32-byte ASCII strings.
Strings use a 16-bit byte length followed by UTF-8 bytes. Names are 1..96 bytes;
reasons are 0..192 bytes. NUL, ASCII control characters and invalid UTF-8 are
rejected. Payloads have exactly the fields below, without trailing bytes:

| Type | Payload fields in order |
| --- | --- |
| Hello | GUID, controlPort:u16, state:u16, name:string |
| Request | senderGUID, receiverGUID, token, mediaPort:u16, senderControlPort:u16, replyTimeoutMs:u32, senderName:string |
| Response | responderGUID, token, accepted:u16 (0/1), reason:string |
| Heartbeat | senderGUID, token |
| End | senderGUID, token, reason:string |

State values are Idle=0, Sending=1, Receiving=2 and Busy=3. Requests have a total
caller deadline of 100..30000 ms. The receiving GUI's reply deadline is the
smaller of the caller's remaining time and the local request timeout (10 s by
default). Partial control messages have a bounded total deadline; receiving a
fragment does not reset it. Heartbeat polling deadlines do not prematurely
discard an already arriving fragmented message.

The implementation limits discovery to 128 peers, connection workers to 16,
queued GUI events to 64 and recent tokens to 1024. No remote message contains an
executable path, shell command, command-line arguments or arbitrary local file.
The control protocol has no authentication or encryption; the GUI controls the
local actions taken on a valid LAN request. Session tokens identify lifetimes;
they are not an authentication credential.

Tests set `enableBroadcast=false`, bind exclusively to `127.0.0.1`, and use
`testTargets` plus independent OS-selected UDP/TCP ports. Running those tests
does not discover or control real LAN clients.
