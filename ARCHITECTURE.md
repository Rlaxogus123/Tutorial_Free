# Architecture

The mod is intentionally self-contained and keeps gameplay state local unless
the player explicitly confirms an upload.

## Data

- `SectionData` describes ordered level sections and their difficulty display.
- `FlagData` describes progress markers imported from or shared with presets.
- Save keys retain compatibility with earlier `tipp7.tutorial` releases.
- Imports are validated before replacing local data, and the first migration or
  edit creates a backup snapshot.

## UI and lifetime safety

Popup-owned callbacks use `geode::WeakRef` before touching UI nodes after an
asynchronous request. Gameplay nodes use `_spr` IDs so they remain compatible
with Node IDs and other mods.

## Online flow

Browsing performs read-only Firebase requests. Upload is available only for a
logged-in Geometry Dash account and requires a confirmation that lists every
public field. Delete removes both the user's preset and its map index entry.

## Free edition boundary

This repository includes all free gameplay behavior. It can render appearance
data found in compatible shared presets, but does not include inactive editors
for membership-only appearance customization.
