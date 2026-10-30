# Tizen Steam Link

Play the games on your PC or Steam Deck on a Samsung TV. Tizen Steam Link is an unofficial
Steam Link app for Samsung TVs and smart monitors on Tizen 5.5 or later (2020 models on). It
uses Steam Remote Play, the same protocol as Valve's own Steam Link app.

![The computer list on the TV](docs/images/home.png)

The app finds the computers running Steam on your network and pairs with each one through a
PIN. It streams with the TV's hardware video decoder, and asks the TV for Game Mode to keep the
delay down. Controllers, keyboards and mice plugged into the TV go through to the computer.

This project is not affiliated with or endorsed by Valve.

## What you need

- A Samsung TV or smart monitor on Tizen 5.5 or later.
- A computer with Steam running and logged in, and Remote Play turned on (Steam, Settings,
  Remote Play), on the same network as the TV.
- A computer to install the app from, with [Apps2Samsung](https://github.com/Apps2Samsung/Apps2Samsung).
- A controller for playing. The TV remote works for the menus.

## Install

These are the main steps. [docs/install.md](docs/install.md) goes through each one, and
what to do when one fails.

1. Download `TizenSteamLink-<version>.wgt` from the
   [latest release](https://github.com/pebeto/tizen-steam-link/releases/latest).
2. Turn on Developer Mode on the TV: open Apps, type `1 2 3 4 5` with the remote, switch it
   on, and enter the IP address of the computer you install from. Then hold the power button
   until the TV restarts.
3. In Apps2Samsung, pick "Custom WGT File", choose the `.wgt` file and install it. On
   Tizen 7 and later, sign in with a Samsung account when asked, so Apps2Samsung can sign the
   app for your TV.
4. If the computer with Steam has a firewall, let the TV in: UDP ports 27031 to 27036 and TCP
   ports 27036 and 27037.
5. Open Tizen Steam Link from Apps on the TV.

## Your first stream

Select your computer. The TV shows a four-digit PIN, and Steam opens a dialog on the computer
that asks for it. Type the PIN there.

![Pairing: the TV shows a PIN to type into Steam](docs/images/pairing.png)

Select the computer again to start streaming. The stream opens in Steam's Big Picture mode,
which is made for a controller and a TV.

## Controls

| To | With the remote | With a controller |
|---|---|---|
| Move around the menus | Arrow keys | D-pad or left stick |
| Select | OK | A |
| Go back | Back | B |
| Forget the selected computer | Red (A) | X |
| Open the stream menu | Hold Back | LB + RB + View + Menu |
| Show or hide the stream statistics | Red (A), during a stream | From the stream menu |
| Send Escape to the computer | Press Back briefly | |
| Show the app's log | Blue (D) | |

During a stream every other button goes to the game. Keyboards and mice plugged into the TV
work as they would on the computer; click the picture to capture the mouse. A controller
buzzes briefly when the TV first sees it.

## The stream menu

Hold Back during a stream to bring up the menu. While it is open, the remote and the
controllers drive the menu instead of the game.

![The stream menu](docs/images/stream-menu.png)

Type text opens the TV's keyboard and sends what you type to the computer, as if you had
typed it there. Steam's own on-screen keyboard in Big Picture works too.

## Settings

![The settings](docs/images/settings.png)

| Setting | Choices | Default | Notes |
|---|---|---|---|
| Maximum resolution | 720p, 1080p, 1440p, 4K | 1080p | For 4K, use HEVC and a bandwidth limit of 50 Mbit/s or more. |
| Frame rate | 60 or 120 fps | 60 | On a Samsung S90C the app showed about 60 frames per second even at 120. |
| Video codec | H.264, HEVC, HEVC HDR | H.264 | Steam sends HEVC only when the computer can encode it in hardware. HEVC HDR also offers HDR10, which Steam sends when the computer's display is in HDR mode. |
| Player latency | Automatic, Low, Ultra low | Automatic | The TV player's latency mode, see [docs/how-it-works.md](docs/how-it-works.md#the-tvs-player). HEVC always uses Low. Changing it reloads the app. |
| Quality | Automatic, Fast, Balanced, Beautiful | Balanced | Steam's own quality preference. |
| Bandwidth limit | Adaptive, Automatic, 5 to 100 Mbit/s | 20 Mbit/s | A lower limit means fewer packets to lose on Wi-Fi. Adaptive has Steam lower the bitrate when packets get lost and raise it again, up to 30 Mbit/s, as Steam's own Steam Link app does. Automatic has Steam pick a fixed bitrate. |
| Audio | On, off | On | |
| Start in | Big Picture, Desktop | Big Picture | |
| Show stream statistics | Shown, hidden | Hidden | Same as the red key during a stream. |

Changes apply to the next stream.

## When the connection drops

Wi-Fi loses packets now and then. This is how the app handles it:

- Steam sends error correction data with the video, and the app uses it to rebuild lost
  packets, so losing a packet or two doesn't cost a frame.
- Lost audio is filled in, up to 80 ms per gap, so a short hiccup doesn't cut the sound.
- If the computer goes silent for 10 seconds (Wi-Fi out, the computer asleep), the app
  reconnects by itself, up to twice in a row, as long as the computer answers within 15
  seconds. A stream the computer ends itself is not reconnected.
- If the picture and the sound both stop for 15 seconds while the computer still answers, the
  app ends that stream and reconnects the same way.
- If the streaming part of the app crashes during a stream, the app restarts it and reconnects
  the same way, instead of staying frozen on the last picture.
- A computer that is offline shows "Wake" if it reported its network address while it was
  online. Selecting it sends a Wake-on-LAN packet. Steam on the Steam Deck reports that
  address; Steam on Linux desktops may not.

In tests over Wi-Fi from a Steam Deck, H.264 kept going where HEVC froze, so H.264 is the
safer choice on Wi-Fi.

## When something goes wrong

| What you see | What to do |
|---|---|
| The computer doesn't show up | Check that Steam is running and the firewall lets the TV in. You can also add it with Other computer, by its IP address. |
| "This device is not paired with the host" | Steam forgot the TV. Select the computer and pair again. |
| Sound but a black picture, from a Linux computer | Steam started the usual way can't capture a Wayland desktop (sway, for example). Steam's log suggests starting it with `steam -pipewire`. |
| The app stops opening after a TV update | The update turned Developer Mode off. Turn it on again and reinstall, as in [docs/install.md](docs/install.md). |
| "This TV can't run the streaming part of the app" | The TV is older than Tizen 5.5 (a 2019 model or earlier), and the app can't run on it. |
| "The streaming part of the app hasn't started" | Restart the TV fully by holding the power button, then open the app again. |
| Anything else | Press the blue key to see the app's log. |

## More

- [docs/install.md](docs/install.md): installing step by step, and fixing a failed install.
- [docs/how-it-works.md](docs/how-it-works.md): how the app streams, and how it copes with
  lost packets.
- [docs/development.md](docs/development.md): building, testing, releases, the desktop
  client `tsl-cli`, and debugging on the TV.

## License

GPL-3.0 (see `LICENSE`). ihslib is LGPL-3.0, mbedTLS is Apache-2.0, and Opus and protobuf-c
are BSD licensed. The Figtree font in `web/fonts` is under the SIL Open Font License
(`web/fonts/OFL.txt`).
