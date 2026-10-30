# Development

## Building

The desktop client and the unit tests need CMake, Ninja and a C and C++ compiler:

```sh
scripts/build-desktop.sh
```

It builds `build/desktop/src/desktop/tsl-cli` and runs the tests.

The TV app builds in Docker:

```sh
scripts/build-tizen.sh
```

The first run builds a toolchain image of about 2 GB, with Tizen Studio 5.6 (command line
only) and Samsung's Emscripten 1.39.4.7. The result is `out/TizenSteamLink.wgt`, signed with
a throwaway Tizen author certificate made inside the image. On Tizen 7 and later,
Apps2Samsung re-signs it for the TV. The build also runs `scripts/check-tizen-build.py`, which
looks for problems that only show up on the TV, such as a thread or timer callback with a
signature the WebAssembly build can't call.

CMake downloads ihslib and applies `patches/ihslib` to it. After changing a patch, delete the
patched copy so the next build downloads it again, then build as usual:

```sh
rm -rf build/{desktop,tizen}/_deps/ihslib-{src,build,subbuild}
```

## Tests

`tests/test_core.cpp` covers the shared code and the patched parts of ihslib: the device
identity and PIN, keyboard and gamepad mapping, a probe against a fake host, the packet
window, error correction (against an independent encoder), lost audio frames, the
retransmission race, and Wake-on-LAN packets.

To run them under AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
ninja -C build/asan && build/asan/tests/test-core
```

ThreadSanitizer works the same way with `-fsanitize=thread` in `build/tsan`. It reports races
on the interrupt flags in ihslib's `base.c` and `ch_data.c`, which come from upstream.

## CI and releases

Day-to-day work goes to `develop`, and `master` gets a new version each time `develop` is
merged into it. Two GitHub Actions workflows run on top:

- `.github/workflows/test.yml` runs on every push outside `master` and on every pull request.
  It builds and runs the unit tests in a Release build, and again under AddressSanitizer and
  UndefinedBehaviorSanitizer. Any sanitizer finding fails the job.
- `.github/workflows/release.yml` runs on every push to `master`. Each push becomes a new
  version, in four steps:
  1. It works out the version. The first release uses the one in `CMakeLists.txt` (0.1.0).
     After that, the commit messages since the last tag decide, following Conventional
     Commits (see `.github/scripts/next-version`). A `!` after the type, as in `feat!: ...`,
     or a `BREAKING CHANGE:` line bumps the major version. `feat: ...` bumps the minor one.
     Anything else bumps the patch.
  2. It writes the version into `CMakeLists.txt` and `web/config.xml` for the build only, so
     the app shows it in its footer. Nothing is committed back.
  3. It runs the tests and builds the TV package. The toolchain image comes from the Actions
     cache unless `docker/Dockerfile` changed.
  4. It tags the commit `vX.Y.Z` and publishes a GitHub release with
     `TizenSteamLink-X.Y.Z.wgt`, `tsl-cli-X.Y.Z-x86_64-linux`, a `.sha256` file for each,
     and notes generated from the commits.

## The interface in a browser

```sh
python3 -m http.server -d web 8000
```

Open `http://localhost:8000/index.html?mock` at 1920x1080. With `?mock` the page uses
`web/dev/mock.js` instead of the WebAssembly module. The mock fakes two computers, pairing
and a stream. The arrow keys, Enter and Escape stand in for the remote. Setting
`window.tslMockFrozen = true` in the console freezes the fake stream, which shows the app
reconnecting after 15 seconds.

![The interface in the browser preview](images/home.png)

## tsl-cli

`tsl-cli` is a Linux command line client that talks to Steam the same way the TV does and
writes the stream to files. It's the quickest way to try a change against a real computer.

```
tsl-cli discover                 list computers that answer a broadcast
tsl-cli probe <ip>               ask one computer directly (also shows its MAC address)
tsl-cli wake <mac>               send a Wake-on-LAN packet
tsl-cli pair <ip>                pair, typing the printed PIN into Steam on that computer
tsl-cli stream <ip> --seconds 20 --out /tmp/stream
                                 stream, then write video.h264 (or .hevc) and audio.wav
```

`pair` also takes `--pin NNNN` and `--wait SECONDS` (120 by default). `stream` also takes:

| Option | What it does |
|---|---|
| `--hevc` | Ask for HEVC. |
| `--no-audio` | Stream without sound. |
| `--max 1920x1080` | The largest picture to ask for. |
| `--quality auto\|fast\|balanced\|beautiful` | Steam's quality preference. |
| `--bitrate KBPS` | The bitrate; -1 lets Steam pick a fixed one, -2 lets it adapt to the network. |
| `--fps N` | The frame rate to ask for. |
| `--no-fec` | Turn off error correction. |
| `--big-picture` | Open Big Picture instead of the desktop. |
| `--hdr` | Offer HDR10, which comes as HEVC; the summary then says `HDR10` if Steam sent it. |
| `--input-test` | Plug in a virtual gamepad, press A and D-pad down, and send Escape, so the effect shows in the recording. |

At the end it prints the frame count, the audio frames lost and filled in, and a network
summary: packets lost, messages rebuilt, keyframe requests and the round trip. For the full
log, put `-v` before the command, as in `tsl-cli -v stream <ip>`.

The CLI keeps its device identity and the paired computers, with their keys, in
`~/.config/tizen-steam-link`. It has its own identity, so you pair it separately from the TV.

### Simulating Wi-Fi on a wired network

The desktop build can drop packets on purpose:

| Variable | Effect |
|---|---|
| `TSL_TEST_RECV_LOSS=0.5` | Drop 0.5% of incoming datagrams of 1000 bytes or more, and log each drop, each retransmission, and each dropped packet that arrives again. |
| `TSL_TEST_RECV_LOSS_MIN_BYTES=0` | Include the small packets too (audio and control). |
| `TSL_TEST_RECV_BLACKOUT_AFTER=10` | Drop everything after 10 seconds, like a computer going to sleep. |

For example, `TSL_TEST_RECV_LOSS=0.5 tsl-cli stream <ip>`. The TV build doesn't have these.

## Debugging on the TV

The blue key shows the app's log on the TV. For the full log and a JavaScript console, attach
Chrome's inspector. The TV has to be in Developer Mode with your computer's IP address.

```sh
docker run --rm -it --network host tizen-steam-link-toolchain bash
sdb connect <tv-ip>
sdb shell 0 kill TSLinkTV01                    # debug hangs if the app is already open
sdb shell 0 debug TSLinkTV01.TizenSteamLink    # opens the app and prints a port
```

Then open `chrome://inspect` in Chrome, add `<tv-ip>:<port>` under Configure, and inspect the
app. The console shows the WebAssembly module's log lines.
