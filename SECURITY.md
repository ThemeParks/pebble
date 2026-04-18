# Security Policy

## Reporting a vulnerability

If you've found a security issue in this app or the related ThemeParks.wiki API integration, **please don't open a public GitHub issue**. Email the details to:

**cube@themeparks.wiki**

Include:
- A clear description of the vulnerability
- Steps to reproduce (if applicable)
- The affected component (watch app C code, PKJS, build scripts, CI workflow, etc.)
- Any PoC or screenshots
- Your preferred credit name if you'd like to be acknowledged in the fix

You'll receive an acknowledgement within 72 hours. We'll keep you updated as we investigate and ship the fix.

## What's in scope

- The Pebble watch app binary (`src/c/`, `src/pkjs/`)
- Build + publish scripts (`scripts/`)
- The CI workflows (`.github/workflows/`, `.gitea/workflows/`)
- The Docker dev environment (`Dockerfile`, `docker/`)

## What's out of scope

- Vulnerabilities in Pebble SDK / Rebble appstore infrastructure — report those upstream at https://rebble.io
- Vulnerabilities in the ThemeParks.wiki HTTP API itself — report at https://themeparks.wiki (same email address, different response process)
- Issues that require physical access to the user's unlocked phone + paired watch
- Lack of HTTPS on LAN-only connections (e.g. `pebble install --phone <lan-ip>`)

## Supported versions

We ship fixes for the latest release on the Rebble appstore. Older versions don't receive backports — upgrade to the current build.

## Coordinated disclosure

We aim for 90-day coordinated disclosure: once a fix ships, you're welcome to publish details. If the fix requires API-side coordination, the timeline may extend — we'll tell you if that's the case.
