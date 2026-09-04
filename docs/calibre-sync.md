# Calibre Library Sync

Device-direct pull of `.rsvp` books from a local [Calibre](https://calibre-ebook.com/)
content server over WiFi.

---

## Architecture

The device acts as an HTTP client against a standard `calibre-server` instance on the
local network — the same "device pulls files over the LAN" pattern used by RomM for
game ROMs. No cloud relay, no companion app required (though the companion can configure
and trigger the sync remotely).

```
┌─────────────────────┐        WiFi / LAN        ┌──────────────────────┐
│   rsvpnano device   │ ─── HTTP GET ──────────▶ │  calibre-server      │
│   (ESP32-S3)        │ ◀── JSON / .rsvp bytes ─ │  port 8080 (default) │
│                     │                           │                      │
│  CalibreSyncManager │                           │  /ajax/library-info  │
│  CalibreClient      │                           │  /ajax/search        │
│  SD manifest        │                           │  /ajax/book/<id>     │
│ /library/.calibre-  │                           │  /get/rsvp/<id>/<lib>│
│    sync.json        │                           └──────────────────────┘
└─────────────────────┘
```

Key source files:

| File | Role |
|------|------|
| `src/calibre/CalibreClient.h/.cpp` | HTTP client + pure JSON parsers (`calibreparser` namespace) |
| `src/calibre/CalibreSettings.h` | The engine's own settings struct — plain Arduino `String`, no dependency on the firmware settings stack, so `test/calibre/` can compile it against the host shim |
| `src/calibre/CalibreSyncPlan.h` | Pure, host-testable reconcile core (`calibresync::computeSyncPlan`) |
| `src/calibre/CalibreSyncManager.h/.cpp` | On-device orchestrator: WiFi up → search → diff → download → delete → manifest rewrite → reindex |
| `src/network/HttpFetch.h/.cpp` | Streaming HTTP GET used by the client |
| `src/companion/http/CompanionCalibreApi.cpp` | `GET`/`PUT /api/v2/calibre` for the companion app |
| `src/settings/SettingsModel.h` | Persisted form: `settings::CalibreSettings` inside `DeviceSettings`, password in `DeviceSecrets` |

The JSON parsers and reconcile core are deliberately free of WiFi/SD/clock dependencies so
they can be unit-tested on the host with the `test/support/Arduino.h` shim
(`test/calibre/`). The networking layer mirrors the `OtaUpdater` / `ReleaseParser` split.
The repo does **not** use ArduinoJson; all parsers hand-roll `String::indexOf` /
`String::substring` scanning, matching the existing `ReleaseParser` and `FeedParser`
style.

### Hardware requirement

All current targets use `board = esp32-s3-r8-opi` (ESP32-S3 with 8 MB octal-PSRAM).
The PSRAM is required to buffer HTTP response bodies for JSON parsing in heap without
exhausting IRAM. Do not attempt to port the Calibre sync to boards without PSRAM.
See `docs/esp32-s3-multi-target-layout.md` for the multi-board build layout.

---

## Server setup

### 1. Install the Calibre RSVP Output plugin

The device downloads `.rsvp` files. Calibre does not produce `.rsvp` natively; the
[calibre-rsvp-plugin](https://github.com/jonathanglima/calibre-rsvp-plugin) adds RSVP
as an output format and registers the `application/x-rsvp` MIME type so the content
server serves it correctly.

```bash
calibre-customize -a RSVP_Output.zip
```

### 2. Convert books to RSVP

Select books in Calibre → **Convert books → Output format → RSVP**. This stores the
`.rsvp` alongside the other formats on each book.

### 3. Tag books for sync

The device only fetches books matching its `searchQuery` setting. The recommended
convention is the tag `rsvp`:

```
tag:rsvp
```

Add this tag to every book you want on the device. Any valid Calibre search expression
works — including saved searches (`search:<name>`). See the plugin README for details.

### 3b. Tag articles (optional)

The device shelves books and articles separately — `/library/books` vs `/library/articles`
on SD — and draws them differently (`BookLibrary::isArticle()` tests nothing but the
`/library/articles/` path prefix; `LibraryScreen` gives articles their own spine palette
and a shorter spine). The sync routes on the Calibre tag **`article`**, matched
case-insensitively:

| Tags in Calibre | Lands in |
|-----------------|----------|
| `rsvp` | `/library/books` |
| `rsvp`, `article` | `/library/articles` |

The two tags are independent axes: `rsvp` decides *whether* a book syncs, `article`
decides *how it is shelved*. Web clippings added by `calibre-url` carry `article` already.

Retagging an existing book is cheap. The `.rsvp` bytes do not change, so the sync does not
re-download it — it renames the file on SD, carrying the reading-progress
(`.rstate.toml`) and index (`.ridx`/`.rdat`) sidecars with it, so you keep your place.
Removing the tag moves it back.

To keep the curated shelf clean in Calibre itself, use a Virtual Library
(`not tag:article`) rather than a second Calibre library: the sync engine is
single-library by construction (`CalibreSettings` has one `libraryId` and one
`searchQuery`), and the SD manifest is keyed by bare `book_id`, so two libraries would
collide on ids and Mirror deletion would wipe whichever library was not synced last.

### 4. Start calibre-server

```bash
# Unauthenticated (trusted LAN):
calibre-server --port 8080 /path/to/Calibre\ Library

# With HTTP Basic auth:
calibre-server --port 8080 --enable-auth --manage-users /path/to/Calibre\ Library
```

The server listens on all interfaces and is reachable at `http://<host-lan-ip>:8080`.

---

## Device configuration

### On-device: Settings → Calibre

> **Not available as of v0.0.9-calibre.** Upstream replaced the whole screen
> system with `src/ui/screens/*Screen.cpp` in v0.0.9, and the Calibre submenu
> has not been rebuilt against it yet. Configure via `settings.toml` (below)
> or the companion API until it is.

### By hand: /config/settings.toml on the SD card

Settings are stored as TOML on the card, so the `[calibre]` table can simply be
written there and picked up on the next boot:

```toml
[calibre]
enabled = true
baseUrl = "http://192.168.0.120:8080"
libraryId = ""
searchQuery = "tag:rsvp"
username = ""
deletionPolicy = "mirror"
```

There is deliberately no `password` key: the HTTP Basic password lives in the
encrypted-secrets half of the store (`DeviceSecrets::calibrePassword`), never on
the card. A server without `--enable-auth` needs no password at all.

### Via the companion app

The firmware serves two routes for this over the device's AP/STA HTTP server. Note
that the *app side* of this is not rebuilt yet — v0.0.9 reorganised the Kotlin shared
module into per-domain packages and the Calibre screens have not been ported — so for
now these are reachable with `curl` but not from the app UI:

| Method | Route | Purpose |
|--------|-------|---------|
| `GET` | `/api/v2/calibre` | Read current settings |
| `PUT` | `/api/v2/calibre` | Write settings |

JSON contract:

```json
{
  "enabled": true,
  "baseUrl": "http://192.168.0.120:8080",
  "searchQuery": "tag:rsvp",
  "username": "alice",
  "password": "",
  "libraryId": "",
  "deletionPolicy": "mirror"
}
```

`deletionPolicy` accepts `"mirror"` or `"keep"`. See the security note below regarding
the `password` field.

---

## Sync flow

`CalibreSyncManager::runSync()` executes the following steps:

1. **Connect WiFi** using the stored SSID/password (`DeviceSettings::network` / `DeviceSecrets::wifiPassword`).
2. **Resolve library** — `GET /ajax/library-info` to confirm or discover `library_id`.
3. **Search** — `GET /ajax/search?query=<searchQuery>&library_id=<lib>` → `book_ids[]`.
4. **Resolve each book** — `GET /ajax/book/<id>?library_id=<lib>` → `RsvpRef` (url, size, mtime).
5. **Compute sync plan** — `calibresync::computeSyncPlan(remote, manifest, policy)` (pure, no I/O).
6. **Download** new and changed files to `<routed folder>/<sanitized-title>.rsvp` via streaming `net::get` (write to `.tmp`, then rename). The folder comes from `CalibreSyncManager::targetDirectoryFor()` — `/library/articles` for a book tagged `article`, `/library/books` otherwise.
7. **Move** books whose bytes are unchanged but whose folder changed (a retag), renaming the `.rsvp` and its sidecars. No network.
8. **Delete** removed books from SD, sidecars included (Mirror policy only).
9. **Rewrite manifest** `/library/.calibre-sync.json`.
10. **Reindex** — `StorageManager::refreshBooks()` so the library reflects the new/removed files.
11. **Tear down WiFi**.

Progress is reported via `ProgressCallback` with phases `"search"`, `"download"`,
`"move"`, `"delete"`, `"done"`, `"error"` and a `current/total` count for percentage
display.

---

## Ajax endpoints

All requests are `GET`. Authentication uses HTTP Basic when credentials are configured.

### `GET /ajax/library-info`

```json
{
  "default_library": "Calibre_Library",
  "library_map": { "Calibre_Library": "Calibre_Library" }
}
```

Fields read by firmware: `.default_library`, `.library_map` (to validate a stored `library_id`).

### `GET /ajax/search?query=<q>&library_id=<lib>`

```json
{
  "book_ids": [1, 2, 3],
  "total_num": 3,
  "num": 3,
  "offset": 0
}
```

Fields read by firmware: `.book_ids[]`, `.total_num`, `.num`. When `total_num > num + offset`
there are more pages — append `&num=<n>&offset=<o>` to paginate.

### `GET /ajax/book/<id>?library_id=<lib>`

```json
{
  "title": "Example Book",
  "authors": ["Jane Doe"],
  "tags": ["rsvp", "article"],
  "last_modified": "2026-06-17T15:37:34+00:00",
  "formats": ["epub", "rsvp"],
  "other_formats": {
    "rsvp": "/get/rsvp/1/rsvplib"
  },
  "format_metadata": {
    "rsvp": {
      "size": 475,
      "mtime": "2026-06-17T15:37:34.528479+00:00"
    }
  }
}
```

Fields read by firmware:

| Field | Type | Use |
|-------|------|-----|
| `other_formats.rsvp` | string | Relative download URL; absent when no `.rsvp` format exists — check key presence, not null |
| `format_metadata.rsvp.size` | integer (bytes) | Part of change-key |
| `format_metadata.rsvp.mtime` | ISO-8601 with sub-second precision | Part of change-key |
| `last_modified` | ISO-8601, second precision | Fallback timestamp when `mtime` absent |
| `tags` | array of strings | Folder routing; `article` (any case) selects `/library/articles` |

> **Trap:** `tags` appears **twice** in this payload — the real top-level array, and again
> inside `category_urls` as an *object* (`"tags": {"article": "/ajax/books_in/..."}`).
> Calibre does not guarantee key order, so `calibreparser::extractStringArray()` takes the
> first `"tags"` whose value starts with `[`, which makes the object occurrence
> unmatchable either way round. Both orders are covered by
> `test_book_tags_ignores_category_urls_object`.

Format names (in `.formats`, `.other_formats`, `.format_metadata`) are **lowercase**: `"rsvp"`, not `"RSVP"`.

### `GET /get/rsvp/<book_id>/<library_id>`

Downloads the `.rsvp` bytes. Note: `library_id` is a **path segment** here, not a query
parameter (unlike `/ajax/*` endpoints where it is `?library_id=<lib>`).

The firmware can use the path from `other_formats.rsvp` directly (prepend `baseUrl`) or
construct the URL from the template — both are equivalent.

---

## Incremental sync and the SD manifest

The manifest at `/library/.calibre-sync.json` tracks every file the device has downloaded
from Calibre. It is keyed by `book_id`:

```json
{
  "books": {
    "1": { "key": "475|2026-06-17T15:37:34.528479+00:00", "path": "/library/books/Example_Book.rsvp" },
    "2": { "key": "12048|2026-05-01T10:00:00.000000+00:00", "path": "/library/books/Another_Title.rsvp" }
  }
}
```

The **change-key** is the raw string concatenation `<size>|<mtime>`. The device has no
reliable RTC, so mtime is never parsed into an epoch — keys are compared byte-for-byte.
`mtime` is taken from `format_metadata.rsvp.mtime` (sub-second precision); if absent,
`last_modified` is used as fallback.

The manifest's `path` is load-bearing beyond bookkeeping: it is what the reconcile compares
against the routed destination to detect a retag.

### Reconcile logic (`calibresync::computeSyncPlan`)

| Condition | Action |
|-----------|--------|
| Book in remote search, not in manifest | Download (new) |
| Book in both, keys differ | Download (changed), to the routed folder; the old copy is removed when the folder also changed |
| Book in both, keys equal, routed folder differs | Move (retagged) — rename, no download |
| Book in both, keys equal, same folder | Skip (unchanged) |
| Book in manifest, not in remote, policy = `Mirror` | Delete from SD |
| Book in manifest, not in remote, policy = `Keep` | Leave on SD |

The folder axis is only consulted when the caller fills `RemoteEntry::path`. An empty
path means "this caller does no routing" and the diff collapses to the original key-only
behaviour — which is what keeps the pre-routing tests in `test/calibre/test_sync_plan.cpp`
meaningful.

**Why a move and not a re-download:** adding a tag in Calibre does not rewrite the
`.rsvp`, so `size|mtime` is byte-identical. Treating the retag as a change would re-fetch
data the device already has, and — because reading progress and the prebuilt index are
sidecars keyed by *document path* — a naive rename of just the `.rsvp` would silently
reset the reader to word zero and force a reindex. `moveBookFiles()` carries
`.rstate.toml`, `.ridx` and `.rdat` along.

### Deletion policy

- **Mirror** (default): the device mirrors the Calibre search scope exactly. Books removed
  from the search (untagged, deleted, or the query changed) are deleted from SD.
- **Keep**: downloaded files are never deleted. Use this if you want to read books offline
  after removing the `rsvp` tag from Calibre.

---

## Where settings are persisted

v0.0.9 replaced the flat `Preferences`/`kPref*` key space with a typed settings
store, so Calibre no longer owns NVS keys of its own. It is a field on the
device settings struct:

| Where | What |
|-------|------|
| `settings::DeviceSettings::calibre` | `enabled`, `baseUrl`, `libraryId`, `searchQuery`, `username`, `deletionPolicy` — serialised to `/config/settings.toml` by glaze |
| `settings::DeviceSecrets::calibrePassword` | HTTP Basic password, kept in encrypted NVS alongside the Wi-Fi password and never written to the card |

Writes go through `SettingsStore::acceptChanges()` (and `acceptSecretChanges()`
for the password), the same path every other setting uses.

`CalibreSyncManager` still takes the older Arduino-`String` `CalibreSettings`
struct rather than reading the store directly, so its reconcile core stays
compilable on the host. `App.cpp` converts between the two before each run.

> **Upgrading from v0.0.8-calibre:** the old `cal_*` NVS keys are gone, so
> Calibre configuration does not survive the update and has to be entered again.

---

## Security

- HTTP Basic credentials are transmitted in cleartext over plain HTTP. This is acceptable
  because `calibre-server` is a local, trusted-network service — the same model Calibre
  itself uses for its companion apps.
- Credentials are stored in NVS only. They are **never written to the SD card**.
- `GET /api/v2/calibre` always returns `"password": ""` — the stored password is
  never echoed back over the HTTP API. A `PUT` with an empty or absent `password` field
  preserves the stored credential (sentinel: empty string = "no change").
- If security on an untrusted LAN is required, run `calibre-server` behind a TLS
  terminator (e.g. nginx with a self-signed cert) and set `baseUrl` to `https://...`.
  The firmware's `net::get` selects `WiFiClientSecure` for `https://` URLs. **Note:** it
  currently calls `setInsecure()` (`HttpFetch.cpp`, marked `TODO`), so TLS *encrypts*
  the connection but does **not** verify the server certificate — i.e. no protection
  against an active MITM yet. Certificate pinning/verification is tracked as follow-up.

---

## Hardware feasibility

The sync engine requires the ESP32-S3 with PSRAM (`board = esp32-s3-r8-opi`, 8 MB octal
PSRAM). HTTP response bodies are buffered in heap for JSON parsing; without PSRAM the
heap available to Arduino tasks is insufficient for large search result payloads.

For the multi-board build layout that makes the ESP32-S3 target explicit at compile time,
see `docs/esp32-s3-multi-target-layout.md`. Board-specific pin assignments and display
geometry are in `src/platforms/<board>/BoardConfig.h`.
