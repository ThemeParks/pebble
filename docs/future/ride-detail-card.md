# Ride Detail Card — Design Notes

## Concept

A detail window for a single ride/show, opened from the park view. The
list rows are deliberately dense; the card is where per-ride actions and
full-fidelity info live:

- Full un-ellipsised name (long names like "The Twilight Zone Tower of
  Terror" get cut in the 26px rows).
- Big wait time + status, traffic-light coloured.
- Favourite toggle (already available in-list via long-press SELECT, but
  the card gives it a discoverable, labelled home).
- **Alert toggle** — the natural UI home for the geofenced ride alerts
  design (docs/future/ride-alerts.md): "notify me when this comes back
  up". The card is why the alerts feature doesn't need its own settings
  sub-menu.
- For shows: the full list of remaining showtimes (the row shows 2-3).

## Interaction

- The park view's cursor row is already visibly highlighted; **SELECT
  currently switches tabs**, so the card needs a different entry gesture.
  Options, in preference order:
  1. Move tab-switching to the tab strip metaphor it already visually
     has: SELECT opens the card, and tab switching moves to UP/DOWN
     long-press — risky, muscle-memory break.
  2. Keep SELECT = tabs; open the card with a **double-click SELECT**.
     No conflicts, cheap to add (`window_multi_click_subscribe`), but
     less discoverable.
  3. Dedicated "info" row rendering: pressing SELECT while the header is
     collapsed (i.e. user is deep in the list) opens the card, while at
     row 0 it switches tabs — too magic, rejected.
  Decision needed before implementation; (2) is the safe default.
- BACK returns to the list with cursor position preserved (the
  `s_selected_id` machinery already restores by id).

## Implementation sketch

- New `detail_view.c` window: header (name, wraps to 2 lines), body
  (wait/status or showtimes), action rows (Fave, Alert once alerts land).
- Data comes straight from `g_park` by entity id — no new protocol.
- Wait-trend ("45m, was 60m") is intentionally out of scope: the 5-minute
  refresh cadence is too coarse for trustworthy deltas (user decision,
  2026-07).

## Scope

Small-medium: one new window + click plumbing; no PKJS or protocol
changes until the alert toggle arrives with the ride-alerts feature.
