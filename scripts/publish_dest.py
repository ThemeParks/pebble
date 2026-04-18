#!/usr/bin/env python3
"""
publish_dest.py — build + publish a destination to the Rebble appstore.

Wraps `pebble publish` so the whole pipeline (regenerate dist/, build
the .pbw, push to the appstore) is one command. Metadata is sourced
by convention:

    destinations/<name>.json              -> app name, UUID
    assets/appstore/<name>-description.txt -> prose description
    assets/appstore/<name>-144.png         -> small icon
    assets/appstore/<name>-512.png         -> large icon

Two modes:

  --init              First-time app creation. Passes --non-interactive
                      to `pebble publish` and lets it capture static
                      screenshots from the emulator, because the
                      appstore API rejects new-app creation without
                      screenshots. You can replace them later in the
                      Rebble dashboard. GIF capture is disabled because
                      it depends on x11grab (broken under Wayland/WSL).

  default             Re-release. Uploads a new .pbw version and
                      *skips* screenshot upload — assumption being
                      you've curated the screenshots in the dashboard
                      and don't want them overwritten. Answers the
                      interactive "Choose screenshot source" prompt
                      with "3" (do not upload new screenshots).

By default releases go up as hidden drafts. Pass --publish to flip
them visible immediately.

Usage:
    python3 scripts/publish_dest.py wdw --init          # first time
    python3 scripts/publish_dest.py wdw --version 1.0.2 # re-release
    python3 scripts/publish_dest.py wdw --publish       # publish live
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DESTINATIONS_DIR = REPO_ROOT / "destinations"
DIST_DIR = REPO_ROOT / "dist"
ASSETS_DIR = REPO_ROOT / "assets" / "appstore"

# All destinations are built from this repo, so advertise the same
# source URL. Override per-destination by adding a "source" field to
# destinations/<name>.json.
DEFAULT_SOURCE_URL = "https://github.com/ThemeParks/pebble"


def asset_paths(name: str) -> dict[str, Path]:
    return {
        "description": ASSETS_DIR / f"{name}-description.txt",
        "icon_small":  ASSETS_DIR / f"{name}-144.png",
        "icon_large":  ASSETS_DIR / f"{name}-512.png",
    }


def require_assets(paths: dict[str, Path]) -> None:
    missing = [f"{k}: {p}" for k, p in paths.items() if not p.exists()]
    if missing:
        sys.exit("missing appstore assets:\n  " + "\n  ".join(missing))


def run(cmd: list[str], cwd: Path | None = None,
        input_text: str | None = None) -> None:
    print("  $", " ".join(cmd))
    result = subprocess.run(cmd, cwd=cwd, input=input_text, text=True)
    if result.returncode != 0:
        sys.exit(f"command failed (exit {result.returncode})")


def main() -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("destination",
                   help="destination name — matches destinations/<name>.json")
    p.add_argument("--version",
                   help="override the publish version (defaults to the "
                        ".pbw's own version from package.json)")
    p.add_argument("--release-notes", default="",
                   help="release notes text for this version")
    p.add_argument("--init", action="store_true",
                   help="first-time create: capture screenshots from the "
                        "emulator and create the app in the appstore")
    p.add_argument("--publish", action="store_true",
                   help="set the release visible immediately (default: "
                        "uploads as a hidden draft)")
    p.add_argument("--source",
                   help=f"project source URL advertised on the listing "
                        f"(default: {DEFAULT_SOURCE_URL})")
    args = p.parse_args()

    dest_json_path = DESTINATIONS_DIR / f"{args.destination}.json"
    if not dest_json_path.exists():
        sys.exit(f"destination not found: {dest_json_path}")
    dest = json.loads(dest_json_path.read_text())

    paths = asset_paths(args.destination)
    require_assets(paths)

    # 1. Regenerate dist/<name>/ and build the .pbw.
    run(["python3", "scripts/build_dest.py", args.destination, "--build"],
        cwd=REPO_ROOT)

    dest_dir = DIST_DIR / args.destination

    # 2. Assemble the `pebble publish` command.
    cmd = ["pebble", "publish"]
    if args.version:
        cmd += ["--version", args.version]
    if args.release_notes:
        cmd += ["--release-notes", args.release_notes]
    if args.publish:
        cmd += ["--is-published"]

    if args.init:
        source = (args.source
                  or dest.get("source")
                  or DEFAULT_SOURCE_URL)
        app_name = dest.get("appName") or dest["name"]
        description = paths["description"].read_text().strip()
        cmd += [
            "--non-interactive",
            # GIF capture uses x11grab which doesn't work under Wayland
            # or WSL's X stack; static screenshots are enough for the
            # initial create.
            "--no-gif-all-platforms",
            "--all-platforms",
            "--name", app_name,
            "--description", description,
            "--source", source,
            "--icon-small", str(paths["icon_small"]),
            "--icon-large", str(paths["icon_large"]),
        ]
        run(cmd, cwd=dest_dir)
    else:
        # Existing app: answer the "Choose screenshot source" prompt
        # with "3" (do not upload new screenshots). When the app
        # already exists, allow_skip=True in publish.py, so option 3
        # is offered.
        run(cmd, cwd=dest_dir, input_text="3\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
