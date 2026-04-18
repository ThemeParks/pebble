# Contributing

Thanks for your interest in contributing to the ThemeParks.wiki Pebble app. Whether you're fixing a bug, adding a new destination, or tweaking the UI, this guide should get you started.

## Before you open a PR

- **Open an issue first** for anything non-trivial. A quick conversation avoids surprise rewrites.
- **One concern per PR.** A PR that adds a feature and refactors three unrelated files is hard to review.
- **Read the codebase for a few minutes.** The app is small (~5k lines of C + ~500 lines of PKJS). Follow existing patterns.

## Development setup

Just Docker and Docker Compose — no host-side Pebble SDK needed.

```bash
git clone https://github.com/ThemeParks/pebble.git
cd pebble
./pebble build wdw
./pebble install wdw emery   # installs on the emery emulator
```

See [`README.md`](README.md) for the full command reference and the host-native setup if you prefer.

## Running the tests

Integration tests boot the emulator, drive the app with button presses, and assert on log output and screenshots.

```bash
./pebble test              # WDW integration tests (7)
./pebble test --dest all   # uber-app tests (includes the 7 WDW tests + 5 uber-specific)
```

All tests must pass before a PR can be merged. If you're adding a feature that touches navigation or data flow, add an integration test for it. See `scripts/integration_tests.py` for the patterns.

## Running screenshots

Useful for reviewing visual changes to the UI, especially on the round display (gabbro).

```bash
./pebble screenshots --dest wdw --platform emery
./pebble screenshots --dest wdw --platform gabbro
./pebble screenshots --dest all --platform gabbro
```

Output lands in `screenshots/<dest>/<platform>/`.

## Adding a new destination

1. Add `destinations/<slug>.json` with the destination UUID, app UUID (generate a fresh one), and park list. Copy the structure from `destinations/wdw.json`.
2. If a park needs a custom icon, add an entry to `PARK_STYLE` in `src/pkjs/index.js`. If a new icon shape is needed, add a `draw_<shape>` function in `src/c/park_picker.c` and extend the dispatch in `draw_park_icon`.
3. Build + test: `./pebble build <slug>` and `./pebble install <slug> emery`.
4. Add a screenshot run if you want CI to capture it.

## Coding style

- **C**: follow the existing style (tabs for indent, snake_case, `#ifdef MULTI_DESTINATION` for uber-only code). Files are small and focused — if one grows past ~500 lines, it's probably doing too much.
- **Python**: PEP 8, 4-space indent, snake_case, type hints on public functions.
- **JavaScript (PKJS)**: `var` not `let`/`const` in functions that might run on Moddable XS (older Pebble runtimes). `const` at module scope is fine.
- **Comments**: explain *why*, not *what*. Readers can see the what in the code.

## Commit messages

One-line subjects, present tense, imperative mood:

```
feat(api): PKJS ready timeout + exponential backoff retry
fix(uber): single-park BACK skips spurious picker
docs: spec for multi-destination uber app
```

The body (optional) explains the why in more detail. If the change has any non-obvious impact (heap usage, protocol change, dependency on other commits), note it.

## Questions

Open a discussion or issue on GitHub. For security-sensitive reports, see [`SECURITY.md`](SECURITY.md).
