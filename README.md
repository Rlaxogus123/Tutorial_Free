# Process Difficulty

Process Difficulty is a Geode mod for defining level sections, assigning a
difficulty face to each section, and displaying that information while playing.
SectionData and FlagData presets can also be shared through the built-in server
browser.

This repository contains the free distribution. It has the same core gameplay
features as the membership distribution. The membership benefit is additional
visual-element customization: custom flag text, percentage source, icons,
colors, detail controls, and progress-HUD appearance settings.

The free distribution can view shared FlagData and edit fixed percentages.
Controls reserved for membership are visibly locked and show an explanatory
message when selected.

## Data compatibility

This build uses the same mod ID (`tipp7.tutorial`), save directory, and
SectionData format as earlier Process Difficulty releases. Existing data is
snapshotted once before migration or editing. Legacy `sections-<level-id>` data
appears as **Legacy / Unassigned** in My Data List and can be copied to a named
map without deleting the source.

The free and membership distributions are alternatives and cannot be installed
at the same time because they intentionally share the same mod ID.

## Building

Every tagged release is built from this public source by GitHub Actions for
Windows, Android32, Android64, iOS, and macOS. The workflow combines all five
binaries into one `.geode` package.

For local Windows and Android builds, install the Geode CLI, set `GEODE_SDK` and
`ANDROID_NDK_ROOT`, then run:

```sh
python build_all.py
```

Local builds use `GEODE_DONT_INSTALL_MODS=ON`, so they do not replace an
installed membership build.

See [FIREBASE_SETUP.md](FIREBASE_SETUP.md) for the shared preset database schema
and rules.

## License

Licensed under the [MIT License](LICENSE).
