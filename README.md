# MeshDeck OS

Your own standalone MeshCore firmware for the **LilyGo T-Deck** — a free, open
alternative to MeshOS, built on the MIT-licensed [MeshCore](https://github.com/meshcore-dev/MeshCore)
stack. No licence fee, no serial number, fully yours to modify.

Default preset is **868 MHz UK/EU** (MeshCore *EU/UK (Narrow)* — 869.618 MHz / BW 62.5 / SF 8 / CR 8), and a first-boot picker lets you set your node name and choose any regional preset.

### Flash it in your browser

**One-click web flasher: https://meshdeck-os.github.io/meshdeck/**

Open that page in **Chrome or Edge**, plug the T-Deck in with a USB-C data cable, click **Flash MeshDeck** and pick the serial port. No install, nothing to build. (Full details and offline/command-line methods are in **FLASHING.md**.)

## Features

**Chat**
- Speech-bubble chat UI at full 320x240 resolution, touch & trackball scrolling
- Tabbed channels + DM threads, colour-coded usernames
- Delivery reports (double ticks), send-failure marks, message timestamps
- Tap a message to quote-reply; tap a message containing a link to show it as a **QR code** for your phone
- Emoji rendering (smilies, heart, thumbs-up, fire...)
- Message history survives reboots (saved to flash)
- New-message popups, wake-on-message, alert tones with volume control
- One-tap quick canned messages ("On my way", "OK", "SOS"...) — click the empty compose bar

**Map**
- Built-in offline vector map - no tile files or SD card needed
- Worldwide coastlines + extra detail for UK/Europe, 285 city labels, 9 zoom levels
- Shows your contacts, repeaters (orange diamonds) and your own position live
- Pan with trackball or touch-drag, `+`/`-` zoom, `u` = UK view, `w` = world
- **SD map packs**: drop `.mdm` files in `/meshdeck-maps/` on an SD card for
  extra high-detail regions - a ready-made UK+Ireland pack is included in
  `sdcard/`, and `tools/gen_sdmap.py` builds packs for any region on Earth

**Mesh power tools**
- **Last heard** list: signal, RSSI, hops, age and distance of recent stations
- **Noise floor monitor**: realtime 80-second graph, min/max/avg, last-RX readout
- **Trace route** with per-hop SNR bars
- **Repeater manager**: separate repeater list, login, one-key `stats` / `neighbors` / `advert` / `clock sync`
- **Full terminal**: every mesh event streams into a colour terminal; send DMs,
  channel messages, repeater CLI commands, change radio settings - also mirrored
  to USB serial so you can use it from a computer
- Home screen with big clock, mesh signal bars, battery and unread badge

**Discovery, diagnostics & safety**
- **First-boot setup**: choose your node name, then pick a regional radio preset from a built-in list (EU/UK, US, AU, NZ and more) so you match your local mesh
- **Discover**: one tap sends a flood advert and jumps to the Heard list
- **Radio diagnostics**: live frequency / SF / bandwidth / coding rate, time since last packet, RSSI & SNR, and a received-packet counter — the fast way to check you're hearing other nodes
- **Auto-advert**: optional periodic advert (5/15/30/60 min) so nearby nodes keep discovering you hands-free
- **SOS beacon**: repeatedly broadcasts your location on the public channel until cancelled — won't arm without a GPS fix or a manual position set
- **GPS ready**: plug an external UART/Grove GPS module into the T-Deck and enable it in Settings → GPS for live position in adverts, the map and SOS

**Extras MeshOS doesn't have**
- **BLE companion mode still works** - pair the official MeshCore phone app any time (PIN 123456)
- Open source: every feature is editable C++
- Firmware update from SD card (`firmware.bin` on card → Settings → Update)

## Layout

```
firmware/              the firmware source (drops into MeshCore's examples/)
  main.cpp             entry point
  MyMesh.cpp/.h        MeshCore companion core + MeshDeck event hooks
  ui/                  the MeshDeck UI (screens, drivers, map data)
platformio.local.ini   build environment (MeshDeck_TDeck_868)
build.sh / flash.sh    one-command build + flash
webflasher/            your own browser-based flasher page (GitHub Pages)
sdcard/meshdeck-maps/  ready-made UK+Ireland high-detail map pack (uk.mdm)
.github/workflows/     cloud build + web flasher deploy (GitHub Actions)
tools/gen_mapdata.py   regenerates the built-in map from Natural Earth data
tools/gen_sdmap.py     builds SD map packs (.mdm) for any region
```

## Quick start

```
./build.sh     # compile (first run ~10 min)
./flash.sh     # flash over USB
```

Full beginner instructions, download-mode steps and a no-install cloud build
path are in **FLASHING.md**.

## Controls

| Input | Action |
|---|---|
| Trackball | move / scroll / pan |
| Trackball click | select / send |
| Trackball long-press | back |
| Keyboard | type; enter = send/confirm; backspace/delete edits text; backspace on empty = back |
| Touch | tap = select, drag = scroll/pan, swipe right = back |
| Home screen keys 1-9 | quick-launch apps |

## Credits & licences

- [MeshCore](https://github.com/meshcore-dev/MeshCore) mesh stack - MIT, (c) meshcore-dev contributors
- Map data: [Natural Earth](https://www.naturalearthdata.com/) - public domain
- QR encoding: [qrcodegen](https://github.com/nayuki/QR-Code-generator) - MIT, (c) Project Nayuki
- MeshDeck UI written for this project - MIT. Not affiliated with the MeshCore
  store or MeshOS; "MeshOS" is used only to describe feature parity.
