# Pulsify Web

Pulsify split into two parts:

* **`server/` – C++ (Qt 6 Core + Network only).** yt-dlp (search, resolve, install/update), the YouTube
  stream relay, local-library scanner (ID3 / FLAC tags + covers), playlists & history, lyrics
  (Genius + LRCLIB), Deezer charts proxy, Google sign-in / playlist import, Discord Rich Presence.
  It serves the UI and a small JSON API on `http://127.0.0.1:8765` (127.0.0.1 only).
* **`web/` – JavaScript (no frameworks, no build step).** Home / Search / Library / Playlists / Lyrics /
  Queue / Settings, 14 themes + custom colours and background image, Media-Key support, keyboard shortcuts.
  It is compiled into the server executable (`web.qrc`), so you ship one `.exe`.

The yt-dlp, relay, playlist, Google, Discord and lyrics code is the original Pulsify code (copied
by script, with the Widgets/Gui parts removed). Same QSettings names and the same `pulse-data.json`,
so **your existing playlists, history, Google login and settings carry over.**

## Build (Windows, Qt tools in C:\Qt\Tools)

```bat
set PATH=C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;C:\Qt\Tools\CMake_64\bin;%PATH%
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\<version>\mingw_64
cmake --build build
set PATH=C:\Qt\<version>\mingw_64\bin;%PATH%
build\pulsify-server.exe
```

(Replace `<version>` with your Qt 6 folder, e.g. `6.8.1`. Only the Qt *Core* and *Network* libraries are needed.)
Linux / macOS: the same `cmake -S . -B build && cmake --build build`, then `./build/pulsify-server`.

Pulsify opens in its own window (Edge / Chrome app mode: no tabs or address bar) and **quits when you close that
window**, like a normal desktop program. Options: `--port 9000`, `--no-browser`, `--browser-tab` (normal tab instead of
an app window), `--keep-running` (don't quit when the window closes).

## Notes

* If yt-dlp can't read your browser's cookies (browsers lock them while open), Pulsify drops the cookies, retries, and
  shows a notice. Set **Settings → Sources → Use cookies from** to *Don't use cookies* to stop it trying.
* Open **Settings → Sources** once to let Pulsify download yt-dlp (or point it at your own copy).
* Local music: **Add folder** (a server-side folder picker, since browsers can't expose real paths).
  Only files inside folders you added can be streamed back to the page.
* Security: the server listens on 127.0.0.1 only, rejects foreign `Host`/`Origin` headers and requires an
  `X-Pulsify` header on every state-changing request, so other websites can't drive it.
* Not ported (needs a native window): the old Qt widgets, system-tray and the QtMultimedia player
  (the browser's `<audio>` does playback now).
