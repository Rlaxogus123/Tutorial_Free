# Shared preset database

Process Difficulty uses the Firebase Realtime Database REST API to upload,
list, download, and delete user-selected SectionData and FlagData presets. It
does not include Firebase Authentication or the Firebase SDK.

The public database URL used by released builds is declared in
`src/FirebaseConfig.hpp`:

```text
https://progressdifficulty-default-rtdb.firebaseio.com
```

Uploads happen only after the player explicitly chooses the upload action. A
preset stores the map name, uploader name, update time, optional player icon
appearance, section definitions, and optional FlagData. It does not upload the
player's position or gameplay inputs.

## Schema

```text
section-data/
  map-<normalized-map-name>/
    user-<normalized-user-name>/
      mapName
      userName
      updatedAt
      playerIcon, playerColor1, playerColor2, playerGlow, playerGlowColor
      sections/<index>/{start,difficulty,partName,faces}
      flagData/{version,items}

map-index/
  map-<normalized-map-name>/
    user-<normalized-user-name>: "Original Map Name"
```

`map-index` is a lightweight lookup used by Server Map List. Custom difficulty
image paths are local device paths and are never uploaded.

## Rules

`firebase-rules.json` contains only the two paths used by Process Difficulty:
`section-data` and `map-index`. Deploy it from Firebase Console under
**Realtime Database > Rules**.

The current compatibility protocol has public reads and path-validated writes.
It does not prove ownership of a Geometry Dash username, so a malicious client
that guesses a normalized user key could overwrite that key. A future authenticated
protocol should use Firebase Auth or a trusted relay before storing sensitive or
identity-dependent data. Do not add private keys, service-account files, or
access tokens to this repository.
