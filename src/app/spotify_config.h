#pragma once

// Spotify POC configuration
//
// NOTE:
// - `SPOTIFY_CLIENT_ID` is not secret.
// - For this POC we use OAuth Authorization Code + PKCE, so no client secret is required on-device.
// - You must register the redirect URI in your Spotify Developer Dashboard.

#ifndef SPOTIFY_CLIENT_ID
#define SPOTIFY_CLIENT_ID "a8110cde066b4d1890b165740df81942"
#endif

// Set this to your hosted static callback page, e.g.
//   https://<your-gh-pages>/spotify-callback.html
#ifndef SPOTIFY_REDIRECT_URI
#define SPOTIFY_REDIRECT_URI "https://thestudyerae6tb6fwoabyqa.blob.core.windows.net/$web/spotify-callback.html"
#endif

// Scopes needed for now-playing + transport controls.
#ifndef SPOTIFY_SCOPES
#define SPOTIFY_SCOPES "user-read-currently-playing user-read-playback-state user-modify-playback-state"
#endif
