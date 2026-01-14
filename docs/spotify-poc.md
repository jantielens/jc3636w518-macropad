# Spotify Screen + OAuth (POC)

Goal: prove we can (1) authenticate to Spotify in a user-friendly way using our existing web portal + a hosted static callback page and (2) render a Spotify "Now Playing" screen (album art + prev/next) on the device.

This is a POC: we accept a few shortcuts (e.g. insecure TLS during early testing) as long as the design is compatible with a production-hardening pass.

## Requirements

### Product requirements

- The device can show a dedicated Spotify screen on the 360x360 round display.
- The screen shows:
  - album art of the currently playing track
  - playback controls at minimum: Previous and Next
  - (optional for POC) track + artist text
- Playback control and now-playing data are retrieved via Spotify Web API REST endpoints (no Spotify client libraries).

### Authentication requirements

- Authentication must work even when:
  - the device is not reachable via a stable IP address
  - mDNS is not reliable
  - the device is not exposed to the internet
- Authentication UX should be portal-friendly:
  - one-click connect from the portal (ideal path)
  - must have a fallback for mobile/popup-blocked browsers

### Memory / performance requirements

- The device has PSRAM; we should allocate large buffers in PSRAM-first.
- Album art download + decode should avoid large transient allocations on internal heap.
  - Prefer bounded downloads into a PSRAM buffer.
  - Prefer decoding into a PSRAM-backed pixel buffer.
- UI refresh should not thrash the heap:
  - cache album art until the track changes
  - throttle now-playing polling to a reasonable cadence (the POC uses ~2s)

## Proposed approach (high level)

### OAuth UX: hosted static callback page + portal popup

Spotify OAuth requires a single fixed redirect URI to be pre-registered.

Since the device IP/hostname is not stable, we do not redirect back to the device.
Instead, we redirect back to a hosted static page that relays the authorization `code` back to the portal tab.

Happy path (desktop browsers):
1. Portal calls the device to generate an `authorize_url`.
2. Portal opens `authorize_url` in a popup.
3. User approves in Spotify.
4. Spotify redirects the popup to the static callback page.
5. Callback page JS uses `window.opener.postMessage(...)` to deliver `{ code, state }` to the portal.
6. Portal calls the device to complete auth (code exchange).

Fallback path (mobile / popup blocked / no opener):
- The callback page shows the `code` + `state` and offers a copy button.
- Portal offers a manual paste flow.

### Token model

- OAuth Authorization Code + PKCE on-device.
- Store refresh token in NVS.
- Fetch short-lived access tokens on demand via refresh.

Notes:
- Spotify `client_id` is safe to commit.
- PKCE means no client secret is required on-device.

### Spotify screen

- On activation, fetch now-playing (the POC polls every ~2s).
- If track changes:
  - pick a high-res album image URL from Spotify JSON (prefer 640px, else largest available)
  - download the JPEG into a PSRAM-first buffer (bounded; supports chunked and read-until-close responses)
  - decode JPEG -> RGB565 (PSRAM-first) via `lvgl_jpeg_decoder`
  - center-crop to square and resample to exactly 360x360 (one-time per track)
  - update the LVGL image widget
- UI: large touch targets for Prev/Next, sized for a round 360x360 display.

## REST endpoints (device)

- `POST /api/spotify/auth/start`
  - Response: `{ success, authorize_url, state }`
  - Stores: `state` + PKCE `code_verifier` temporarily in RAM.

- `POST /api/spotify/auth/complete`
  - Body: `{ code, state }`
  - Returns `202` and queues token exchange for the main loop (AsyncWebServer handler must not block).
  - Persists refresh token when available.

- `GET /api/spotify/status`
  - Response: `{ connected }`

- `POST /api/spotify/disconnect`
  - Clears refresh token.

## Current implementation status (in repo)

- Static callback page (tracked): [docs/spotify-callback.html](docs/spotify-callback.html)
  - Deploy this file to a stable HTTPS URL and register it as the Spotify redirect URI.
  - Happy path: `window.opener.postMessage({ source: 'spotify-oauth', code, state }, '*')`.
  - Fallback: shows copy/paste UI when opener/popup is unavailable.

- Firmware config header: [src/app/spotify_config.h](src/app/spotify_config.h)
  - Set `SPOTIFY_REDIRECT_URI` to your hosted callback URL.

- NVS token storage (refresh token only): [src/app/spotify_store.cpp](src/app/spotify_store.cpp)
  - Namespace: `spotify`
  - Key: `refresh`

- Spotify manager (runs from main loop): [src/app/spotify_manager.cpp](src/app/spotify_manager.cpp)
  - OAuth Authorization Code + PKCE
  - Polls `/v1/me/player/currently-playing` when the Spotify screen is active
  - Downloads album art and decodes JPEG -> RGB565 (PSRAM-first)
  - Post-processes album art to a fixed 360x360 RGB565 buffer via center-crop + bilinear resample
  - Queues prev/next actions and executes them in the main loop

- Portal API endpoints: [src/app/api_spotify.cpp](src/app/api_spotify.cpp)
  - `POST /api/spotify/auth/start`
  - `POST /api/spotify/auth/complete` (returns `202` and completes exchange in main loop)
  - `GET /api/spotify/status`
  - `POST /api/spotify/disconnect`

- LVGL screen: [src/app/screens/spotify_screen.cpp](src/app/screens/spotify_screen.cpp)
  - Screen id: `spotify`
  - Minimal UI: title + status line + album art background + large Prev/Next buttons
  - Album art is assumed pre-scaled to 360x360 and rendered 1:1

## Notes for production hardening (beyond POC)

- TLS: stop using insecure TLS; validate Spotify certs (requires correct system time via NTP).
- Token storage: refresh token is stored in NVS; consider encrypted NVS and a clear UX for revoke/reauthorize.
- Heap behavior: PSRAM is used for large image buffers, but TLS/lwIP still consumes internal heap; keep HTTP parsing allocation-light and keep strict response size limits.
- Polling/backoff: handle `204 No Content`, rate limits (`429`), and transient network errors with backoff.
- UX polish: show explicit states (connecting / connected / error) and add a reconnect flow.

Security hardening checklist:
- Callback page should use a strict `postMessage` target origin (not `'*'`) and the portal should validate `event.origin`.
- Keep the invariant: portal completes auth at most once per auth session.

## Replay / re-implement as a proper feature

The key integration points to preserve:

1. Portal UX: start auth via `/api/spotify/auth/start`, then complete via `/api/spotify/auth/complete`.
   - Callback posts `{ source: 'spotify-oauth', code, state }` and the portal immediately completes auth.
   - Keep the copy/paste fallback for popup-blocked/mobile browsers.

2. OAuth model: Authorization Code + PKCE on-device.
   - `client_id` is embedded in firmware.
   - No client secret on-device.
   - Refresh token persists in NVS; access token is ephemeral and refreshed as needed.

3. Image pipeline (prevents LVGL tiling and keeps UI simple):
   - Always produce a fixed-size RGB565 image buffer at exactly 360x360.
   - Center-crop to square + bilinear resample once per track change.
   - UI treats the buffer as a 1:1 background image (no zoom/stretch).

4. Async constraints: `/api/spotify/auth/complete` returns `202` and the exchange happens in the main loop.
   - Avoid long, blocking HTTPS requests inside AsyncWebServer handlers.

## Spotify Web API calls (minimal set)

- Refresh access token:
  - `POST https://accounts.spotify.com/api/token`
- Now playing:
  - `GET https://api.spotify.com/v1/me/player/currently-playing`
- Playback control:
  - `POST https://api.spotify.com/v1/me/player/previous`
  - `POST https://api.spotify.com/v1/me/player/next`

## Static callback page deployment

- Deploy [docs/spotify-callback.html](docs/spotify-callback.html) to a stable HTTPS URL.
- Register that hosted URL as the only redirect URI in the Spotify Developer Dashboard.
- Set `SPOTIFY_REDIRECT_URI` in [src/app/spotify_config.h](src/app/spotify_config.h) to exactly match the hosted URL.

## POC configuration notes

Spotify app registration (existing):
- Client ID: `a8110cde066b4d1890b165740df81942`
- Redirect URL: update to the chosen static callback page URL

Notes:
- Spotify may not always return a refresh token if the user has previously authorized the app; the POC uses `show_dialog=true` to increase the chance of getting one.
