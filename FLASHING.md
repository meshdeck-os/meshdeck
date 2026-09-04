# Flashing MeshDeck onto your T-Deck (beginner guide)

You only need three things: a Mac/PC, a USB-C cable (a data cable, not charge-only), and your T-Deck.

## Easiest: one-click web flasher (no install)

1. Open **https://meshdeck-os.github.io/meshdeck/** in **Chrome or Edge** (Web Serial isn't supported in Safari/Firefox).
2. Plug the T-Deck into the computer and switch it **on**.
3. Click **Flash MeshDeck**, pick the serial port, choose **Install**.
4. Wait ~1 minute, then power-cycle the T-Deck. Done — it reboots into MeshDeck.

If the device doesn't appear in the port list, put it in download mode: switch it **off**, hold the **trackball button** in, switch **on** while holding, release, then click Flash again. Power-cycle once after flashing.

The two methods below (command line / SD card) are optional alternatives.

## One-time setup (macOS)

1. Install the command line tools if you have never used Terminal for development:
   open **Terminal** and run `xcode-select --install` (skip if already installed).
2. Check Python is available: `python3 --version` (macOS ships with it).

## Build and flash

Open Terminal, go to this folder, and run:

```
cd ~/path/to/MeshDeck
chmod +x build.sh flash.sh
./build.sh
```

The first build downloads the ESP32 toolchain (5–10 minutes). Every build after that takes under a minute. When it finishes you'll have `output/meshdeck.bin`.

Then plug in the T-Deck, switch it **on**, and run:

```
./flash.sh
```

The screen goes dark during flashing, then reboots into MeshDeck. Done.

## If flashing doesn't start

The T-Deck sometimes needs to be put into download mode manually:

1. Switch the T-Deck **off** (side switch).
2. **Hold the trackball button down** (press it in and keep holding).
3. Switch the T-Deck **on** while still holding, then release.
4. Run `./flash.sh` again.

After flashing from download mode, switch the device off and on once more.

## No computer setup? Get your own web flasher instead

This folder includes a GitHub Actions workflow that compiles the firmware AND
publishes **your own one-click web flasher page** (like meshcore.co.uk/flasher.html):

1. Create a repo at github.com (free account is fine), upload this folder's contents.
2. In the repo: **Settings → Pages → Source: "GitHub Actions"** (one-time).
3. Open the **Actions** tab — the "Build MeshDeck firmware" run starts on its own.
4. When it turns green, your flasher is live at
   `https://<your-username>.github.io/<repo-name>/`
5. Open that page in **Chrome or Edge**, plug in the T-Deck, click **Flash MeshDeck**.

Every time you push a change, the firmware rebuilds and the flasher page updates
itself with the new version. The compiled binaries are also downloadable from the
Actions run (meshdeck-firmware artifact) if you prefer manual flashing.

## Updating later — no computer at all

Once MeshDeck is on the device you can update from an SD card (the first install
still has to be the flasher above — SD updates only work once MeshDeck is running):

1. Download the latest app image: **https://meshdeck-os.github.io/meshdeck/meshdeck.bin**
   (the browser saves it as `firmware.bin`).
2. Copy `firmware.bin` to the root of a FAT32 SD card and insert it.
3. On the device choose **Settings → Update firmware from SD**. It flashes and reboots.

## SD card maps

Two home-screen apps share `/meshdeck-maps/` on a FAT32 card:

| App | Files | Source | Generator |
|-----|-------|--------|-----------|
| **Map** (classic) | built-in + optional `.mdm` | Natural Earth coastlines / towns | `tools/gen_sdmap.py` |
| **NewMaps** | `.mdv` (MDV2 v3) | OpenStreetMap streets / water | `tools/gen_newmap.py` |

Copy the repo's `sdcard/meshdeck-maps/` folder onto the card so it contains
`/meshdeck-maps/uk.mdm` (and any `.mdv` packs you build). MeshDeck loads packs
at boot, or use **Settings -> Reload SD map packs**.

### Classic `.mdm` (coastline detail)

The classic map works with no SD card (built-in world + Europe coastlines from
`tools/gen_mapdata.py`). An `.mdm` adds 10 m-grade coastline and extra town
labels for a region. The included `uk.mdm` is UK + Ireland. To make another:

```
python tools/gen_sdmap.py <natural-earth-geojson> alps.mdm 5.5 43.5 16.5 48.5
```

Arguments: `lon-min lat-min lon-max lat-max` (optional simplify tolerance last).
Data: https://github.com/martynafford/natural-earth-geojson
(needs `10m/physical/ne_10m_coastline.json` and
`50m/cultural/ne_50m_populated_places_simple.json`). Up to 4 `.mdm` packs load.

### NewMaps `.mdv` (OSM streets)

Build from a Geofabrik PBF (recommended) or Overpass. A large bbox becomes
several overlapping `name_00.mdv`, `name_01.mdv`, ... files -- copy all of them.

```
pip install osmium
python tools/gen_newmap.py --pbf idaho-latest.osm.pbf --bbox -117.5 46.0 -115.5 49.0 -o sdcard/meshdeck-maps/northern-idaho.mdv
```

PBF extracts and generated `_NN.mdv` files stay on disk / the SD card; do not
commit them. Full flags, install steps, controls, and troubleshooting:
**docs/NEWMAPS.md**.

## First boot

- The device generates its identity on first boot, then shows a short setup:
  **type your node name → Enter → pick a radio preset** from the list.
- Choose the preset your local mesh uses. The default is the UK/EU MeshCore
  **EU/UK (Narrow)** preset: **869.618 MHz, BW 62.5, SF 8, CR 8**. You can re-run
  the picker any time from **Settings → Radio preset setup**.
- Both radios must match on frequency, bandwidth and spreading factor or they
  won't hear each other. Use **Home → Radio** to check live signal/packet counts.
- Tap **Discover** (or Settings → Send advert) so nearby nodes learn about you.
- Your old MeshCore contacts/identity from other firmware are kept if you were
  already running MeshCore companion firmware (same storage format).

## Troubleshooting

- **`pio: command not found`** — close and reopen Terminal, or run `python3 -m platformio run -e MeshDeck_TDeck_868 -t upload` from `build/meshcore`.
- **No serial port found** — try another USB cable/port; check the T-Deck is on.
- **Screen stays black after flash** — power-cycle the device with the side switch.
- **Keyboard backlight** — press `alt+B` on the T-Deck keyboard (hardware feature).
