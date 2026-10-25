# LAN3D TCP protocol v6

One IPv4 TCP connection carries one sender's H.264, HEVC or AV1 desktop/SBS stream and system audio to one receiver.
The connection begins with an IDR/keyframe. Each frame repeats its codec
configuration, including codec-specific sequence headers, so a newly connected receiver does not depend
on an earlier connection. A dimension/configuration change also begins with a
keyframe. The sender recreates its encoder for each connection.

All header integers are unsigned and in network byte order. The fixed 80-byte header
is followed by `sequenceHeaderBytes` bytes, `payloadBytes` video bytes, and then
`audioFrames * 4` bytes of signed 16-bit little-endian interleaved stereo PCM. There
are no native C++ structs or padding on the wire.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | Magic `0x4c334453` (`L3DS`) |
| 4 | 4 | Protocol version, `6` (all other versions are rejected) |
| 8 | 4 | Header length, `80` |
| 12 | 4 | Flags: bit 0 = keyframe; bit 1 = Leia SBS; bit 2 = Leia left eye only; bits 1 and 2 are mutually exclusive; all other bits zero |
| 16 | 4 | Total visible image width (both eyes when SBS) |
| 20 | 4 | Visible height |
| 24 | 4 | Integer nominal frame rate |
| 28 | 4 | Target bitrate in bits/s |
| 32 | 8 | Original capture sequence number |
| 40 | 8 | Original capture timestamp in 100 ns units |
| 48 | 4 | Sequence-header byte count, at most 64 KiB |
| 52 | 4 | Access-unit byte count, 1..32 MiB; H.264/HEVC Annex B, AV1 low-overhead OBU |
| 56 | 4 | Codec `0x48323634` (`H264`), `0x48455643` (`HEVC`), or `0x41563031` (`AV01`) |
| 60 | 4 | Logical display dimensions: upper 16 bits width, lower 16 bits height; a zero pair derives aspect from packed pixels |
| 64 | 8 | Audio first-sample timestamp on the sender's QPC clock, in 100 ns units |
| 72 | 4 | Stereo sample-frame count, 0..4800 (at most 100 ms) |
| 76 | 4 | Audio flags: bit 0 = discontinuity; all other bits zero |

Audio is fixed at 48,000 sample frames/s, two channels, signed 16-bit PCM. It is
uncompressed and uses at most 1.536 Mbit/s of sample payload at continuous real-time
rate. No microphone or input forwarding is included. A zero frame count requires
zero audio timestamp and zero flags. A nonempty audio interval requires a nonzero
timestamp, and `audioTimestamp + floor(audioFrames * 10000000 / 48000)` must be no
later than the paired video's timestamp. Timestamp addition is validated without
overflow. Unknown flags, oversized blocks and future audio are rejected before
payload allocation. Audio discontinuity marks a fresh capture/playback interval;
the first audio block after a new connection starts a new timeline.

The entire bundle is atomic: the receiver publishes neither video nor audio until
the sequence header, video payload and PCM have all arrived. A complete video
payload followed by truncated audio is still an incomplete bundle and closes the
connection. This prevents audio from advancing independently through a network
stall. Playback additionally gates the audio interval against video presentation;
the transport itself does not schedule hardware playback or infer a receiver's
wall-clock offset. Source QPC timestamps are comparable within a sender, not
between the two computers. Both clients must be updated for v6.

Logical display dimensions describe one eye for SBS or left-only content and the whole image for
desktop content. They are either both zero or width 1..8192 and height 1..4320.
They preserve display aspect when pixels are packed non-uniformly: a 7680x2160
SBS source encoded at 3840x2160 carries display dimensions 3840x2160, so each
1920x2160 encoded eye is shown at its original 16:9 aspect. Windowed left-eye
display and diagnostic SBS preview use this aspect; Leia interweaving still
receives the actual packed texture dimensions. These fields travel with each
frame's metadata and do not reset the codec or change its coded dimensions.
Version 4 clients are rejected during capability negotiation so they cannot
silently show a squeezed half-SBS image.

When flag bit 1 is set, the image is progressive left-eye/right-eye SBS. When
clear, it is a single ordinary desktop image. This content kind is per frame,
independent of codec dimensions and retained through encoder/decoder metadata
queues. A desktop image must never initialize Leia or be interlaced. In the
desktop client, windowed SBS displays its left eye in 2D; only fullscreen SBS
uses the local Leia runtime. Diagnostic `--preview` still displays packed SBS.
Encoding converts top-down
BGRA8 SDR sRGB into NV12 with BT.709 coefficients and limited nominal range.
Odd visible heights repeat the final row to obtain an even coded height;
decoding removes that row. The decoder accounts for MFT output stride and
macroblock padding. Alpha is opaque after decoding. All supported 4:2:0 codecs are lossy; this
protocol does not promise a pixel-exact desktop image or preserve HDR.

NVENC uses D3D11 input, P5 low-latency tuning, CBR, spatial AQ, no B-frames or
lookahead, and a two-frame VBV. NVDEC uses the display driver's parser/decoder
and a GPU BT.709 conversion kernel. The common fallback is the Windows inbox
software H.264 MFT (Baseline, no B-frames). Original sequence numbers, content
kind and timestamps are preserved independently of private codec timestamps,
including duplicate/non-monotonic source times. Supported protocol dimensions are even widths 48..4096 and heights
48..2304, with 1..240 FPS and 64 kbit/s..1000 Mbit/s. These are validation limits;
the installed encoder may reject a dimension/rate combination inside them.

Bitrate `0` is a local automatic-mode option only; the wire always carries the
actual nonzero encoder target. Automatic NVENC starts at 200 Mbit/s and adjusts
within 20..1000 Mbit/s from sustained TCP send backpressure. Low-complexity
underfilled video does not prove link capacity. Updates reconfigure the current
encoder and refresh its keyframe/sequence headers. A pure target-bitrate metadata
change alone does not reset either decoder. Automatic software fallback stays at
20 Mbit/s. Manual targets are not silently reduced by congestion recovery.

TCP uses `TCP_NODELAY` and nonblocking sockets. Each operation has one total
deadline, rather than resetting the timeout after every received fragment. An
idle receive timeout keeps the connection open. A partial-header/payload
timeout closes it, preventing lost framing and indefinite dribble transfers.
EOF between complete packets returns a clean stream end; EOF inside a header,
sequence header, video payload, or PCM payload is a truncated-packet error. The receiver
does not report success for an unfinished explicit receive-frame target.
Invalid headers are rejected before allocating payload buffers. `Close()`
interrupts pending operations; callers join the worker before destroying or
reconnecting its object. Input and output metadata queues are limited to 64
frames. There is no unbounded video queue in this module.

GUI sessions (with a session token) and diagnostic `--recover-stream` sessions
use a 250 ms total deadline per outgoing video/audio bundle; other diagnostic
senders retain 1500 ms. A failed send closes the old TCP stream and discards its
remaining encoded packets. Recovery repeats authentication/capability negotiation
on a fresh connection and recreates the encoder, starting from a keyframe; it
never continues a partial payload or drops a predicted reference frame inside
the existing stream. On a send timeout, automatic mode immediately halves its
target, down to 20 Mbit/s, and retains that target across media reconnections in
the same sender session. Manual mode retains its selected target and logs advice
to lower it or use automatic mode. These deadlines bound send operations, not
total capture-to-display latency or the duration of a reconnect.

Desktop sessions require a token preface before any video: the receiver sends
exactly 20 bytes (`L3DH` followed by the 16 decoded bytes of the 32-hex-digit
session token); the sender verifies it and replies with four bytes `L3DA`.
Wrong/malformed tokens, missing acknowledgments and any handshake timeout close
the connection. The sender does not create its encoder before acceptance.
Tokens are never included in diagnostic error messages. Existing diagnostic
CLI sessions without `--session-token` omit this preface but still use wire v6.

After the optional token preface, the receiver sends a 12-byte capability preface:
`L3DC` (u32 `0x4c334443`), protocol version (u32 `6`), hardware decoder mask
(u32: H.264=1, HEVC=2, AV1=4). All integers are in network byte order. Unknown
bits and versions close the connection. H.264 software decoding is implicit even
with a zero mask. The sender intersects this mask with its NVENC capabilities,
prefers AV1 then HEVC then H.264, and identifies the selected codec in every
video header. The same codec is used for both eyes in a single SBS image. When
no mutual hardware path is available, auto mode uses software H.264 and caps its
target at 1920x1080/30 FPS/20 Mbit/s. Explicit HEVC/AV1 requests fail when the
pair cannot support them. Transport unit tests may exchange packets without the
worker-level capability preface; production sender/receiver always perform it.
The preface does not encrypt video or audio. Discovery/invitation belongs to the separate
desktop peer service. The caller
selects the bind address and port.
TCP packet loss can delay newer frames until retransmission; the sender's
operation deadline disconnects a stalled peer. A later GPU codec/transport
version can be introduced through a new protocol version.

After capability negotiation the receiver may send fixed 8-byte mode messages in
its direction of the same socket: `L3DM` (u32 `0x4c33444d`), followed by stereo
preference (u32 `0` = left-only, `1` = packed SBS). The receiver sends its current
choice initially, on F12 changes, and after reconnect. This is independent of
forward video/audio framing. The sender polls without waiting, buffers fragments,
rejects invalid values/magic or incomplete messages older than two seconds, and
limits each poll to 32 reads. The latest completed preference wins.

A mode change starts a new encoder keyframe even if coded dimensions are the
same. The sender crops the original GPU left eye before scaling/conversion, within
the selected output limits; target bitrate remains unchanged. Bit 2 marks this
already-cropped image so the receiver neither crops it again nor weaves it while
waiting for a requested SBS transition. Desktop content is never cropped.
Versions before 6 are incompatible; both clients must update together.

The local capture mapping remains version 2: `Local\\LAN3DShare.v2.<PID>` plus
its `.lock` mutex. Its explicitly validated header contains `kind` as uint32:
0 = Desktop, 1 = LeiaSbs. Old mappings cannot be mistaken for the new layout.

Implementation references: [Microsoft H.264 encoder](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder),
[H.264 decoder](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder),
[low-latency codec property](https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avlowlatencymode).
