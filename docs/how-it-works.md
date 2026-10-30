# How it works

Tizen Steam Link is a web app for Samsung TVs with a WebAssembly module inside. The module
speaks Steam's Remote Play protocol through [ihslib](https://github.com/mariotaku/ihslib) and
feeds the picture and sound to the TV's hardware decoder. The TV side follows the approach of
[Moonlight for Tizen](https://github.com/OneLiberty/moonlight-chrome-tizen).

![From Steam on the computer to the TV](images/how-it-works.svg)

## A stream, step by step

1. The app sends a discovery broadcast and lists the computers running Steam that answer. A
   computer can also be added by its IP address, which the app then asks directly.
2. Pairing: the TV makes up a four-digit PIN, and Steam asks for it on the computer. The two
   then agree on a key (an X25519 key exchange masked with the PIN). The key is per computer,
   so the app keeps one for each.
3. The app asks Steam to stream, and says what it can take: H.264 or HEVC, HDR10 or not, the
   largest picture, the frame rate, the bitrate, and whether to open Big Picture. With HDR on,
   the TV's player takes HEVC's 10-bit profile, and the TV switches to HDR from the signalling
   in the stream.
4. Steam captures the screen, encodes it, and sends it over UDP with error correction data.
   Sound comes as Opus, which the module decodes to PCM.
5. The module hands video and sound to the TV's player, Samsung's
   ElementaryMediaStreamSource, on a `<video>` element.
6. Controllers are read from the browser's Gamepad API 250 times a second and reach Steam as
   generic gamepads, with the same 48-byte report as ihslib's SDL backend. Keyboard and mouse
   events go through too.

## Getting through Wi-Fi

Wi-Fi loses packets now and then, and sometimes stops for a second or more. The app deals
with each kind of loss like this:

| What goes wrong | What the app does |
|---|---|
| A few video packets are lost | Steam sends parity packets with each video message, and the app rebuilds a message that lost up to that many. The format isn't documented; `session/fec.h` in the FEC patch describes it as worked out from captured traffic. |
| A video frame is lost anyway | The app skips it as soon as a later frame is complete, and asks Steam for a keyframe. It asks again every 200 ms until one arrives (Steam's own client asks every 100 ms). |
| A keyframe arrives as a burst | A keyframe is a few hundred packets at once. The stream's socket asks for a 4 MB receive buffer, since the usual 200 KB or so drops whatever doesn't fit. |
| An audio packet is lost | The Opus decoder makes up the missing sound, for gaps of up to 80 ms. |
| A control message is lost | Control messages are resent every 10 ms until Steam confirms them, up to 20 times. The app's own confirmations are cumulative, as Steam expects. |
| The network stops for a few seconds | A channel that falls behind by more than its window (256 packets for audio, 2048 for video) starts over from the newest packet. |
| The computer goes silent for 10 seconds | The session ends, and the app reconnects by itself, up to twice in a row, if the computer answers within 15 seconds. |
| Picture and sound stop while the computer still answers | After 15 seconds the app ends that stream and reconnects the same way. |

A lower bandwidth limit helps on a busy network: Steam then makes smaller keyframes, which
means fewer packets to lose. With Adaptive, Steam sets the bitrate itself: it lowers it when
data gets lost and raises it again, up to 30 Mbit/s. In a Steam Deck's log the target fell to
about 2.5 Mbit/s at each loss, so a fixed 20 Mbit/s stays the default.

## The statistics overlay

The red key shows what the stream is doing. This picture comes from the browser preview,
with a simulated computer.

![The statistics overlay during a stream](images/streaming.png)

| Line | What it shows |
|---|---|
| `1920x1080 H.264` | The picture size and the codec Steam chose, with `HDR` after it when Steam sends HDR10. |
| `60 fps 20.0 Mbit/s` | Frames and data arriving each second. |
| `target 20.0 Mbit/s @ 60 fps` | What Steam is aiming for. |
| `keyframes`, `failed appends` | Keyframes received, and frames the TV's player refused. |
| `loss ... now, ... overall` | Video packets lost over the last second and since the start. |
| `FEC: ... rebuilt, ... lost` | Messages rebuilt from parity packets, and messages lost anyway. |
| `keyframe requests` | How often the app asked Steam for a keyframe. |
| `round trip` | The time from a control message to Steam's confirmation. |
| `player: appended, decoded, dropped, time, ready` | What the TV's player did with the frames. |
| `sessions, closes, append error, pipeline errors` | Problems the TV's player reported. |
| `audio: ... frames, ... lost ones filled in` | Sound frames passed to the player, and lost ones the decoder filled in. |

## The TV's player

The app asks the TV for Game Mode, which turns off the TV's picture processing and its delay.
It does that with the `use.game.mode` key in `config.xml`, which TVs from 2022 on read, as
Moonlight for Tizen does.

Samsung's player also has latency modes, and the app picks one when the page loads:

- On Tizen 9 and later it uses the low latency mode. Tizen 9 changed Game Mode for apps like
  this one: the ultra low latency mode no longer switches the TV to it. On a Samsung smart
  monitor on Tizen 9, the ultra low latency mode also showed HEVC frozen on its first frame and
  dropped about 17% of H.264 frames.
- On older versions it uses the ultra low latency mode for H.264, which also switches the TV
  to Game Mode there, and the low latency mode for HEVC.
- TVs without the ultra low latency mode use the low latency mode for both.

The Player latency setting overrides this: Low uses the low latency mode, and Ultra low uses the
ultra low latency mode for H.264 where the TV has it. HEVC always gets the low latency mode,
since on a Samsung QN90D TV on Tizen 9 the ultra low latency mode froze it on its first frame,
with or without HDR, as on the monitor. H.264 plays in the ultra low latency mode on that TV.
On TVs from before 2022, which don't read `use.game.mode`, the ultra low latency mode is what
turns Game Mode on, so Low leaves it off.

The player takes its mode when it starts, so the page reloads when the codec or the Player
latency setting changes. It also reloads after every stream, which gives each stream a fresh
player.

On a Samsung S90C, whose panel runs at 144 Hz, the app showed at most about 60 frames per
second. A 120 fps stream was decoded in full, and about half its frames were dropped.

## The code

| Directory | What's in it |
|---|---|
| `src/core` | Code shared by the TV app and the desktop client: the ihslib client and session, the device identity, keyboard codes, and the virtual gamepads. |
| `src/platform` | Thread and UDP backends for ihslib. They replace its SDL threads, and its POSIX UDP code, which calls `fcntl()` on sockets; Samsung's socket layer aborts on that call. |
| `src/tizen` | The WebAssembly module: the player, the Opus decoder, and the gamepad reader. |
| `web` | The interface: computer list, pairing, settings and the stream view, plus `config.xml`. |
| `src/desktop` | `tsl-cli`, a Linux command line client that streams to files. |
| `patches/ihslib` | Changes to ihslib, applied when CMake downloads it. |
| `tests` | Unit tests for the shared code and the patched parts of ihslib. |

The interface looks like Steam's Big Picture mode: a shelf of computer tiles as in the Steam
Link app, a white focus glow, and a hint bar whose button names follow the last input. The
colors were sampled from a Big Picture stream. It uses no Valve logos, artwork or fonts;
Figtree stands in for Motiva Sans.

CMake downloads the dependencies at pinned versions and checks them by hash: ihslib, mbedTLS
3.6, protobuf-c and Opus. Nothing is installed system-wide. ihslib is pinned to the head of
its pull request #4, which adds the key exchange current Steam requires for pairing.

## The ihslib patches

Each patch starts with a description of what it does and why.

| Patch | What it does |
|---|---|
| 0001 | Lets only one thread at a time send a control message. |
| 0002 | Starts the pairing and streaming tasks outside the client's lock. |
| 0004 | Gives the timer thread the signature WebAssembly expects, so the timer runs on the TV. |
| 0005 | Confirms reliable packets cumulatively, as Steam reads confirmations. |
| 0006 | Lets the data channels get past lost packets, instead of stalling or overflowing. |
| 0007 | Skips a lost frame as soon as a later one is complete. |
| 0008 | Counts packet loss, keyframe requests and the round trip, for the statistics. |
| 0009 | Asks for error correction on video and rebuilds lost packets. |
| 0010 | Lets the app choose the quality preference and the bitrate. |
| 0011 | Tells Steam which codecs and how large a picture the app wants. |
| 0012 | Computes packet timestamps in 64 bits; in 32 bits they overflowed on WebAssembly. |
| 0013 | Tells the app about lost audio frames, so the decoder can fill them in. |
| 0014 | Passes on the computer's MAC address, for Wake-on-LAN. |
| 0015 | Reads controllers 250 times a second. |
| 0016 | Ends the session when the computer goes silent for 10 seconds. |
| 0017 | Lets the app ask for more than 59.94 frames per second. |
| 0018 | Asks Steam for the desktop only when the app wants it, so Big Picture can open. |
| 0019 | Fixes a use-after-free when Steam's confirmation of a packet arrived just as it was about to be resent, which is likeliest during play. |
| 0020 | Starts a data channel over after a gap longer than its window, instead of leaving it quiet for good. |
| 0021 | Lets the app offer HDR10 to Steam, and passes on the colorspace Steam chose for the stream. |
| 0022 | Fixes a race in the silence check that could end a live stream with "Nothing from the host for 4294967295 ms". |
| 0023 | Checks the size of unconnected packets (Steam's MTU pings) without overflowing on 32 bits, and caps the padding a ping can ask for. |
| 0024 | Drops an error correction shard when its message's buffer can't be allocated, instead of writing through a null pointer. |
| 0025 | Skips empty audio codec data, in the audio and microphone channels, which led to a `memcpy` from a null pointer. |
| 0026 | Lets the app ask for an adaptive bitrate, as Steam's own Steam Link app does. |
