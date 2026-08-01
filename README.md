# ThemeParks.wiki — Pebble App

A Pebble smartwatch app for [ThemeParks.wiki](https://themeparks.wiki) showing live wait times, show times, and opening hours. Per-destination apps (WDW Queues, UOR Queues) and a browse-everywhere uber app (ThemePark Queues) are all generated from this shared codebase.

Written in **pure Pebble C** targeting:
- **Pebble Time 2** (`emery` — 200×228 rectangular colour)
- **Pebble Round 2** (`gabbro` — 260×260 circular colour)

<!-- (An earlier version used Moddable/XS JavaScript — we dropped it to save heap and get predictable memory footprint on the watch.) -->

## Features

- **Live waits + shows** with traffic-light colouring, favourites
  pinned to the top (long-press SELECT on any ride to star it), sort
  modes, and a silent five-minute auto-refresh that never loses your
  place.
- **Launcher glance (AppGlance)**: park hours as a live countdown
  ("closes in 45 min", rendered by the firmware) plus up to a week of
  upcoming hours — and, with My Parks configured, a packed live-wait
  line like "MK 45m · EP 30m". Every slice expires rather than going
  stale.
- **Phone-side settings page** (gear icon in the Pebble app): AppGlance
  toggle, **My Parks** (up to 4 parks whose waits feed the glance line,
  filterable picker), and — uber app only — a **fixed destination**
  mode that makes the watch open straight into your home resort.
- **Locale aware**: times follow the watch's 12/24-hour setting;
  distances render km or miles.
- The uber app browses every ThemeParks.wiki destination, grouped by
  region with a GPS-sorted nearby list.

## Prerequisites

A container engine. That's it — no host-side Pebble SDK, Python, or Node install. Either of:

- **Docker + Docker Compose**

  ```bash
  docker --version        # 24+ recommended
  docker compose version  # v2+
  ```

- **wslc** (Windows Subsystem for Linux Container CLI) — auto-detected and
  preferred when installed. wslc has no `compose`, so the `./pebble` wrapper
  drives it directly (build, volumes, run, exec) to the same effect. Force a
  specific engine with `PEBBLE_ENGINE=docker` or `PEBBLE_ENGINE=wslc`.

  ```bash
  wslc version
  ```

  One difference: wslc doesn't support host networking, so the noVNC service
  is published explicitly on port 6080 (same URL as the compose setup).

## Quick Start

```bash
# Clone
git clone https://github.com/ThemeParks/pebble.git
cd pebble

# Build Walt Disney World (auto-starts the dev container on first run)
./pebble build wdw

# Install on the emery emulator
./pebble install wdw emery

# Or install on a physical watch via the phone's LAN IP
./pebble install wdw --phone 192.168.1.50
```

The compiled `.pbw` lands in `builds/<dest>.pbw` on the host.

### Dev container lifecycle

```bash
./pebble up           # start pebble-dev service (headless)
./pebble up --emu     # also start noVNC at http://localhost:6080
./pebble shell        # interactive bash in the container
./pebble down         # stop everything
./pebble help         # full command list
```

All of `screenshots`, `test`, `soak`, `publish` etc. run the existing `scripts/*.py` inside the container — same behaviour, no host SDK needed.

<details>
<summary>Host-native setup (without Docker)</summary>

```bash
# macOS / Linux
curl -LsSf https://astral.sh/uv/install.sh | sh
uv tool install pebble-tool --python 3.13
pebble sdk install latest

# Linux: extra deps for the QEMU emulator
sudo apt install libsdl1.2debian libsdl2-2.0-0 libfdt1 libglib2.0-0

# Windows: use WSL2 and follow the Linux instructions
```

Verify: `pebble --version` → `Pebble Tool v5.0.31 (active SDK: v4.9.148)`

Node 18+ is needed at build time for PKJS bundling.

```bash
python3 scripts/build_dest.py wdw --build --install emery
```

</details>

## Project Layout

```
pebble/
├── src/                          canonical source (edit here)
│   ├── c/                        watch-side C code
│   │   ├── main.c                entry point, lifecycle, glance publisher
│   │   ├── api.c / api.h         AppMessage transport (+ watchdog, retry, favs sync)
│   │   ├── parkdata.c / .h       pipe-delimited parser + static park store
│   │   ├── dest_parse.c / .h     destination-list parser (uber app)
│   │   ├── sort.c / .h           index-based sort (favs pinned)
│   │   ├── storage.c / .h        persist API + RAM caches (favs, hours, settings)
│   │   ├── format.c / .h         pure display formatters (12/24h, km/mi, glance)
│   │   ├── settings_parse.c / .h phone-settings wire parser
│   │   ├── favs_sync.c / .h      favourites sync payload encoder
│   │   ├── park_picker.c / .h    the destination's park list (custom icons)
│   │   ├── dest_picker.c / .h    destination browser (uber app)
│   │   ├── park_view.c / .h      attractions + shows pages, header, tabs
│   │   ├── settings_view.c / .h  sort mode + favourites sub-menus
│   │   ├── loading_view.c / .h   loading + error screen
│   │   ├── ui_common.h           shared interaction conventions
│   │   ├── colours.h             themed palette, round-display aware
│   │   └── config.h / config.c   GENERATED from destinations/<name>.json
│   └── pkjs/
│       ├── index.js              phone-side proxy: API fetches, settings page glue
│       └── lib.js                pure helpers (unit-tested with plain node)
├── destinations/                 one JSON per destination (parks, icons, uuid)
│   ├── wdw.json                  Walt Disney World
│   ├── universalorlando.json     Universal Orlando Resort
│   └── all.json                  the multi-destination uber app
├── tests/
│   ├── unit/                     host-compiled C tests (stubbed <pebble.h>)
│   └── js/                       node tests for src/pkjs/lib.js
├── scripts/
│   ├── build_dest.py             regenerate dist/<name>/ from src/ + JSON
│   ├── unit_tests.py             host gcc + node unit-test runner
│   ├── screenshots.py            boot emulator + capture every screen
│   ├── soak_test.py              100+ park switches, crash detection
│   ├── integration_tests.py      button-driven e2e tests
│   └── publish_dest.py           build + publish to the Rebble appstore
├── patches/                      pebble-tool / libpebble2 patches (see README inside)
├── docs/                         design notes, known issues, future features
├── wscript                       Pebble waf build rules
└── package.json                  app manifest (overridden per-destination at build)
```

## How per-destination builds work

The watch-side C code is destination-agnostic. Everything specific to a particular park collection lives in `destinations/<name>.json`:

```json
{
  "destinationId": "e957da41-3552-4cf6-b636-5babc5cbc4e5",
  "appUuid": "4c688d07-7860-4fba-82fc-036cc9852087",
  "name": "Walt Disney World",
  "appName": "WDW Queues",
  "shortName": "WDW",
  "parks": [
    { "id": "75ea578a-…", "name": "Magic Kingdom",    "icon": "castle", "color": "pink"  },
    { "id": "47f90d2c-…", "name": "EPCOT",             "icon": "sphere", "color": "cyan"  },
    { "id": "288747d1-…", "name": "Hollywood Studios", "icon": "star",   "color": "red"   },
    { "id": "1c84a229-…", "name": "Animal Kingdom",    "icon": "tree",   "color": "green" }
  ]
}
```

`scripts/build_dest.py wdw`:

1. Copies `src/` to `dist/wdw/src/` (skipping the generated files)
2. Writes `dist/wdw/src/c/config.{c,h}` with this destination's park list
3. Writes `dist/wdw/src/pkjs/index.js` with the park UUIDs patched into `ALL_PARK_IDS`
4. Writes `dist/wdw/package.json` with the destination's app UUID and launcher name

After that, `pebble build` in `dist/wdw/` produces the `.pbw`. Add `--build` (and optionally `--install <platform>`) to do it in one step.

The C struct `park_config_t` supports four icon tokens — `castle`, `sphere`, `star`, `tree` — and eight colour tokens (`pink`, `cyan`, `blue`, `red`, `green`, `orange`, `yellow`, `purple`). Unknowns fall back to a plain circle / dark grey.

## Testing & tooling

### Unit tests (host-side, no emulator)

```bash
python3 scripts/unit_tests.py            # all suites
python3 scripts/unit_tests.py -k sort    # filter by name
```

Compiles the pure-logic C modules (`parkdata`, `sort`, `storage`,
`format`, `dest_parse`, `settings_parse`, `favs_sync`) with plain gcc
against a stubbed `<pebble.h>` + instrumented in-memory persist, and
runs the node tests for `src/pkjs/lib.js`. The fast inner loop; CI runs
it before any builds.

### Soak test (memory stability)

```bash
python3 scripts/soak_test.py --switches 100
```

Builds the app with `PEBBLE_TEST_MODE=1` (a compile-time flag that makes the C app auto-cycle parks every 2 seconds), installs it, and watches the logs for crashes. Needs a running emery emulator.

### Integration tests

```bash
python3 scripts/build_dest.py wdw --build
python3 scripts/integration_tests.py
```

Eight tests on the per-destination builds (startup, scroll, page switch, park-reload cycles, long-press favourite, settings favourite toggle, BT drop, hours prefetch); `--dest all` adds six uber-specific navigation tests (fourteen total). The suites are deterministic and must-pass in CI.

### Screenshots

```bash
python3 scripts/screenshots.py                   # emery + gabbro
python3 scripts/screenshots.py --platform emery
```

Boots a fresh emulator on each platform, drives the app through picker / waits / shows / settings / sort / favourites, captures PNGs into `screenshots/<platform>/`. Useful for reviewing layout on both rectangular and round displays.

### Install on a physical watch

Put the watch into developer mode in the Pebble phone app, then:

```bash
pebble install --phone <phone-ip-address> builds/wdw.pbw
```

## Publishing to the Rebble appstore

`scripts/publish_dest.py` wraps `pebble publish`. First-time publish:

```bash
# One-time: log in to the Rebble appstore (see next section for the
# headless OAuth dance). Credentials persist in the pebble-sdk-home
# volume, so you only do this once per container tree.

# Pre-reqs: destinations/<name>.json exists, and the three files below
# are in assets/appstore/:
#   <name>-description.txt   — appstore listing text
#   <name>-144.png           — small icon
#   <name>-512.png           — large icon (use scripts/gen_appstore_icon.py)
python3 scripts/gen_appstore_icon.py all

# First-time create — captures static screenshots from the running
# emery emulator, so install the app there first. Uploads as a hidden
# draft; review in the Rebble dashboard before going live.
./pebble build all && ./pebble install all emery
python3 scripts/publish_dest.py all --init

# Re-release (skips screenshot upload so curated ones aren't overwritten):
python3 scripts/publish_dest.py all --version 1.0.2

# Flip the draft visible:
python3 scripts/publish_dest.py all --publish
```

### Listing screenshots

`pebble publish` auto-captures a single screenshot per platform at
`--init` time, and has no CLI for multi-screenshot upload. After that
single auto-capture, extra screenshots are uploaded through the Rebble
dashboard by hand, and re-releases intentionally preserve the dashboard
state (the script answers "3 — skip" to the screenshot prompt).

Upload this set via the dashboard once, in this order — they persist
across re-releases:

1. `01_picker.png` — park picker (hero shot of each destination's parks)
2. `03_park_waits.png` — attractions tab with traffic-light waits
3. `04_park_shows.png` — shows tab with upcoming performance times
4. `06_sort_menu.png` — sort-mode picker under settings

Both platforms get the same set: pick from [screenshots/<dest>/emery/](screenshots/)
and [screenshots/<dest>/gabbro/](screenshots/), produced by
`python3 scripts/screenshots.py --dest <name>`.

### Headless `pebble login` (no browser in the container)

The container has no browser, and `pebble login` wants to open one.
Do this the first time:

```bash
docker exec -it pebble-dev bash
pebble login --no-open-browser
# copy the printed auth URL into your host browser, sign in
```

Your browser's final redirect is `http://localhost:<port>/?code=…&state=…`
and won't reach the container automatically. In a **second** shell into
the container, deliver the callback by curl:

```bash
docker exec -it pebble-dev bash
curl "http://localhost:<port>/?code=…&state=…"   # the full URL, quoted
```

The original `pebble login` process receives the code and exits.
Verify:

```bash
pebble login --status   # should print "logged in" + your appstore identity
```

The credentials live in the `pebble-sdk-home` Docker volume, so subsequent
`./pebble up` runs are already authenticated.

## Architecture notes

- **Pure C on the watch.** The app lives entirely in `src/c/`. No Moddable/XS, no Piu, no embedded JavaScript engine. Memory footprint is ~7KB of Pebble's 130KB app heap at steady state.
- **PKJS on the phone.** `src/pkjs/index.js` is a thin proxy that calls the ThemeParks.wiki HTTP API and sends a compact pipe-delimited payload back to the watch over AppMessage. The watch never touches HTTP directly.
- **No JSON on the wire.** PKJS encodes park data as `pid|open|close\nA|id|name|status|wait\nS|id|name|status|t1,t2,…` and the C parser walks it into a static struct. This was originally a hack to survive the XS heap; it stayed because it's simpler than JSON.
- **Hours prefetch.** At startup, after the PKJS ready signal, the watch asks for every known park's hours in one batch (`REQUEST_ALL_HOURS` → `ALL_HOURS_DATA`). That lets the park picker show "9 AM - 10 PM" under each park on first launch rather than only after the user has opened each park individually.
- **AppGlance.** On exit the watch publishes a queue of launcher slices: an optional packed My Parks wait line (computed phone-side, pushed with settings), today's hours as a firmware-rendered `time_until` countdown, and up to a week of future days — each with its own expiration so the launcher never shows stale data.
- **Phone settings.** The config page is generated locally by PKJS and opened as a data URI (no hosting, works offline). Settings push to the watch as a tiny semicolon-separated `SETTINGS_DATA` payload; favourites flow the other way (`FAVS_SYNC`, full-state per toggle) into phone localStorage for future timeline features.
- **Persist storage.** Favourites (identity-keyed per park UUID via a small LRU allocation index — destination-proof in the uber app), sort mode, last-park, glance state, and the hours cache all live in the Pebble persist API behind RAM caches, so draw paths never touch flash. Key layout is documented at the top of `src/c/storage.c`.
- **Round displays.** `src/c/colours.h` uses `PBL_IF_COLOR_ELSE` and the C code uses `PBL_IF_ROUND_ELSE` for per-platform offsets. Custom row rendering in `park_view.c` and `park_picker.c` uses `graphics_text_attributes_enable_screen_text_flow` so long names wrap around the circle edge on gabbro.

## Troubleshooting

**Emulator install hangs or returns `TimeoutError: fetch_watch_info`**
Run `./pebble sdk-apply-patches` once. This applies `patches/pebble-tool-fetch-watch-info-timeout.patch` to the SDK inside the container and writes a marker so the entrypoint re-applies it on future starts.

**`./pebble build` fails with a permission error writing to `builds/` or `dist/`**
Bind-mounted files are created as root inside the container. On Linux this usually Just Works; if you hit permission issues, run `sudo chown -R $USER:$USER builds/ dist/`.

**`pebble install --logs` disconnects after a few seconds**
Separate `pebble logs --emulator emery` in another terminal works more reliably for long-running log capture.

**Emulator shows "Pebble" (system watchface) after install**
The Pebble firmware exited the app. Usually means either a crash (check logs), the emulator was installed-to in a dead state (run `pebble kill` and try again), or the `.pbw` is from an older heap config that no longer boots. Fresh build + install resolves it.

## Documentation

- [Contributing](CONTRIBUTING.md) — dev setup, tests, PR process
- [Security](SECURITY.md) — reporting vulnerabilities
- [Code of Conduct](CODE_OF_CONDUCT.md)
- [Known issues](docs/known-issues.md) — resolved + outstanding quirks
- [Future design notes](docs/future/) — timeline pins, ride alerts, ride detail card

## API Reference

- [ThemeParks.wiki API v1](https://api.themeparks.wiki/docs/v1/) — free, no auth, 300 req/min
