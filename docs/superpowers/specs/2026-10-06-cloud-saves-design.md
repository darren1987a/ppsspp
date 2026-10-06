# Cloud Saves via DHService — Design

Date: 2026-10-06
Status: Approved in chat, pending written-spec review
Branch: `custom` (fork-only feature, never sent upstream)

## Goal

Let the owner back up and restore PSP game saves (`PSP/SAVEDATA/` only) between their own
PPSSPP builds on iPhone, iPad and Mac, using their existing backend DHService
(`https://dhservice.crazydarren.com`).

Success means: on device A, upload a game's saves; on device B, download them and the game loads
them. A mistaken upload can be rolled back. Nothing is overwritten silently.

## Decisions

| Topic | Decision |
|---|---|
| Users | Single owner. No user accounts. |
| Auth | Per-device token. Device registers once with the existing admin login, keeps only the token. |
| Storage | DHService server local disk. No backups (accepted risk: losing the server disk loses cloud copies; devices still hold local saves). |
| Scope | `PSP/SAVEDATA` only. No save states. |
| Versions | Keep the last 10 versions per save; older ones are deleted on upload. |
| Grouping | By game ID (first 9 characters of the save folder name). |
| Sync | Manual upload / download only. No automatic sync. |
| Token storage in app | PPSSPP's existing secret file (`NativeSaveSecret("cloudsave")`, as RetroAchievements does), not Keychain and not `ppsspp.ini`. |
| Sync state in app | Own JSON file `PSP/SYSTEM/cloudsave.json` (server URL, username, device name, per-save base). Avoids editing `Core/Config.cpp`, which upstream changes often. |
| Hashing | sha256 of a deterministic, **uncompressed** (`ZIP_CM_STORE`) zip: entries sorted, fixed mtime, hidden files skipped. |
| HTTP auth transport | `Authorization` header. PPSSPP's request layer gains custom header support. |

Out of scope: auto sync, save states, web UI, server backups, multi-user, Keychain.

## Terms

- **saveId**: a PSP save folder name, e.g. `ULUS10336DATA00`. Validated with
  `^[A-Za-z0-9_-]{9,64}$`.
- **gameId**: first 9 characters of saveId, e.g. `ULUS10336`.
- **version**: per-saveId integer, starting at 1, increasing by 1 per upload.
- **base version**: the cloud version a device last uploaded or downloaded for a saveId.

## API contract

Base URL: `https://dhservice.crazydarren.com/api/v1` (dev: `http://127.0.0.1:9001/api/v1`).
TLS is terminated by Cloudflare (valid certificate verified 2026-10-06), so iOS ATS is satisfied.
### Response envelope (DHService convention, confirmed by the dhservice session)

- JSON endpoints always answer **HTTP 200**. The real result is the in-body `code`.
- Success: `{"code":200,"data":<any>,"msg":""}`. A list is a JSON array; the new handlers always
  initialize slices so an empty list is `[]`, never `null` (the client still treats `null` as `[]`).
- Error: `{"code":<int>,"msg":"<text>"}` (no `data` key). Codes used by the save endpoints:
  `400` bad input, `401` missing/unknown/revoked token, `409` conflict, `413` too large.
  The `409` response is written directly by the handler and **does** include
  `data: {"latest": {...}}`.
- `POST /login` (gin-jwt): success `{"code":200,"token":"<jwt>","expire":"<RFC3339>"}`;
  failure `{"code":400,"message":"..."}` (key `message`, not `msg`). Bad/expired JWT: `code`
  `401` or `6401`.
- The zip download endpoint answers HTTP 200 with the raw zip on success. On error it answers the
  JSON error envelope instead; the client tells them apart by the zip magic bytes `PK`.
- Client rule: success = transport OK and `code == 200` and (`data` present or `token` present).
  Error text = `msg`, falling back to `message`.

### Device management (admin JWT, `Authorization: Bearer <jwt>` from existing `POST /login`)

```
POST   /saves/devices        body {"name": "DarreniPhone"}
                             → {"deviceId": 3, "token": "dsv_<base64url of 32 random bytes>"}
GET    /saves/devices        → [{deviceId, name, createdAt, lastSeenAt}]
DELETE /saves/devices/:id    → revokes the device (its token stops working immediately)
```

The token is returned only once. The server stores only `sha256(token)`.

### Saves (device token, `Authorization: Device <token>`)

```
GET  /saves?gameId=ULUS10336
     → [{saveId, gameId, title, versionCount,
         latest: {version, size, sha256, deviceName, uploadedAt}}]

GET  /saves/:saveId/versions
     → [{version, size, sha256, deviceName, uploadedAt}]   newest first, at most 10

GET  /saves/:saveId/versions/:version
     → raw zip, Content-Type: application/zip (not wrapped in the JSON envelope)

POST /saves/:saveId/versions[?force=1]
     body: raw zip, Content-Type: application/zip, max 20 MB
     headers:
       X-Save-Title:  title from PARAM.SFO (UTF-8, URL-encoded), optional
       X-Save-Sha256: lowercase hex sha256 of the body, required
       X-Base-Version: integer, 0 if this device never synced this save, required
     → code 200, data {version}
     → code 409, data {latest: {version, sha256, deviceName, uploadedAt}}
            when latest version > X-Base-Version and force is not set
     → code 400 bad saveId / hash mismatch / not a zip
     → code 413 body over 20 MB
```

Common errors: code `401` for a missing, unknown or revoked token. (All codes are in-body; see
the envelope section.)

## Backend (DHService, Go + Gin + GORM/MySQL)

Built by the `dhservice` Claude session, following that repo's conventions.

| File | Contents |
|---|---|
| `models/save_device.go` | `SaveDevice`: ID, Name, TokenHash (unique index), CreatedAt, LastSeenAt |
| `models/save_version.go` | `SaveVersion`: ID, SaveID, GameID (index), Version, Title, Size, Sha256, DeviceID, Path, UploadedAt. Unique (SaveID, Version). |
| `models/gorm/gorm.go` | Register both in AutoMigrate |
| `middleware/devicetoken.go` | Parse `Authorization: Device <token>`, hash, look up, set device in context, update LastSeenAt |
| `apis/saves.go` | Handlers above, `tools.Assert` errors, `app.OK` responses, Swagger annotations |
| `router/router.go` | Device routes under existing JWT group; save routes under a new device-token group; isolated from tgbot routes |
| `settings.yml` | `saves.dir` (default `runtime/saves`), `saves.maxVersions` (default 10) |

Files are stored at `<saves.dir>/<saveId>/<version>.zip`. The path is built only from the
validated saveId and the integer version, never from client file names.

Upload steps, in order:

1. Validate saveId.
2. Read body with a 20 MB limit (`413` if exceeded).
3. Check sha256 of body equals `X-Save-Sha256` (`400` if not).
4. Check the body opens as a zip (`400` if not).
5. In one DB transaction, locking the saveId's rows: read latest version; if
   `latest > X-Base-Version` and not `force`, return `409`; otherwise next version = latest + 1.
6. Write to a temp file in `<saves.dir>`, then rename into place.
7. Insert the `SaveVersion` row, commit.
8. Delete versions beyond `maxVersions` (rows and files). Failure here is logged, not returned.

## App (PPSSPP, `custom` branch)

### New files

| File | Contents |
|---|---|
| `Core/Util/CloudSaveZip.cpp/.h` | saveId validation, sha256 hex, deterministic zip of a save folder, safe extract with `.cloudtmp`/`.bak` swap. Reuses `HasParentDirComponent`. |
| `Core/Util/CloudSaveState.cpp/.h` | Pure status decision (table below) and the `cloudsave.json` sync-state file. |
| `Core/Util/CloudSaveClient.cpp/.h` | Async API calls (login+register, list game saves, list versions, upload, download) and envelope parsing. Uses `g_DownloadManager`; callbacks run on the UI thread. |
| `UI/CloudSaveScreen.cpp/.h` | Per-game cloud saves screen, plus the login form shown when there is no token. |
| `unittest/TestCloudSave.cpp` | Unit tests for the above (and the HTTP header plumbing). |

### Changes to upstream files

- `Common/Net/HTTPRequest.h/.cpp`, `HTTPNaettRequest.cpp`, `HTTPClient.h/.cpp`: custom request
  headers in both backends, and a `RequestManager::StartRequest(...)` that takes them (the existing
  helpers call `Start()` before a caller could add headers). Response bodies are already kept on
  non-200 results in both backends.
- `UI/GameScreen.cpp`: add a **Cloud Saves** button near **Delete Save Data**, shown only when the
  game has save data or the cloud may have some (always shown is acceptable).
- Build lists: `CMakeLists.txt`, and the other build files `AGENTS.md` / `docs/HLEModules.md` say
  new source files must be listed in (Windows `.vcxproj`/`.filters`, Android `Android.mk`,
  libretro `Makefile.common`), so other targets keep building.

### Status per save

Inputs: local folder exists?, local hash, base version, cloud latest version and hash.

| Status | Condition | Shown as |
|---|---|---|
| Synced | local hash == cloud latest hash | ✓ |
| Local newer | cloud latest == base version, local hash differs | ↑ Upload |
| Cloud newer | cloud latest > base version, local unchanged since base | ↓ Download |
| Both changed | cloud latest > base version and local changed | ⚠ Conflict |
| Local only | no cloud copy | ↑ Upload |
| Cloud only | no local folder | ↓ Download |

"Local unchanged since base" is tracked by also storing the hash of the version at the base in
`cloudsave.json` (`saveId → {version, sha256}`).

The local hash is the sha256 of a **deterministic zip**: files sorted by path, fixed timestamps,
fixed compression. This makes equal folders give equal hashes on every device.

### Screen behavior

- **Upload all**: uploads every local save folder of the game whose status is ↑ or ⚠.
  For ⚠, asks first ("Cloud has a newer version from iPad (date). Upload anyway?"); yes → `force=1`.
- **Download** (per save): shows the version list (device + date); default is latest.
  If the local save has changes not in the cloud (↑ or ⚠), asks before overwriting.
- On success, update the base version/hash in `cloudsave.json` and refresh the list.
- Not reachable during gameplay (button lives on GameScreen only).

### Safe unzip

1. Reject entries with absolute paths, `..`, or names outside the save folder.
2. Extract into `PSP/SAVEDATA/<saveId>.cloudtmp/`.
3. Rename the existing `<saveId>` to `<saveId>.bak`, rename `.cloudtmp` to `<saveId>`,
   then delete `.bak`. On failure, restore `.bak`.

### Errors

Shown as a message on the screen. No network → retry message. `401` → clear token and open the
login screen. `409` → conflict prompt. `413` / `400` → error message naming the save.

## Testing

**Backend**: unit tests for saveId/gameId validation, conflict decision, pruning selection.
Curl script against dev covering: register → upload → 409 → force → list → download (hash
matches) → 11th upload prunes the oldest → revoked token gets 401.

**App unit tests** (`unittest/UnitTest.cpp`): deterministic zip round trip and equal hashes for
equal folders, safe-unzip rejects `..` entries, status table, saveId/gameId parsing.

**End to end**: Mac against dev DHService (upload, modify, conflict prompt, download older version,
game loads it); then iPhone and iPad against production.

## Build order

1. Backend endpoints + device tokens (dhservice session), curl-tested on dev, deployed to prod.
2. App plumbing: headers, config, `CloudSaveClient`, `SaveZip`, `SaveSyncStatus`, unit tests.
3. App UI: login screen, cloud saves screen, GameScreen button.
4. End-to-end check on Mac → iPhone → iPad.

Steps 1 and 2 can run in parallel; they share only the API contract above.
