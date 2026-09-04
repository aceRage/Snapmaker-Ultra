# Phone app: home-screen install, notifications, and printer control — research

Date: 2026-09-03 · Branch: `docs/phone-mobile-research` (cut from `feat/ultra-preferences`) · Status:
research pass, no code. Companion to `2026-09-01-phone-stream-access-design.md` (the phone page and
the hub), `2026-09-02-remote-access-design.md` (Tailscale, hardening, phasing),
`2026-09-02-hidden-service-mode.md` (hidden instances) and `2026-09-03-phone-send-design.md`
(sending a plate, the Snapmaker connect, the safety rules).

Three questions, answered in order:

1. Can a button on the phone page make it an app-like icon on the home screen, or is a real app needed?
2. Can the PC watch the printers and tell the person's phone when something happens?
3. Can the Devices tab start, pause and stop a print?

Every external fact carries its source and a date. Facts about this codebase carry `file:line`
anchors against `feat/ultra-preferences` at commit `0cdf8a3972`.

---

## 0. Orientation: the ground all three stand on

### 0.1 The two origins

The phone reaches exactly the same page over two completely different origins:

| | LAN | Remote |
|---|---|---|
| URL | `http://<pc-lan-ip>:13640/r/<token>/` | `https://<pc>.<tailnet>.ts.net/r/<token>/` |
| Served by | `HubServer::serve`, main listener (`RemoteHub.cpp:2143`) | the same, through `tailscale serve --bg --https=443 http://127.0.0.1:<port>` |
| TLS | none | Tailscale's own auto-renewing certificate, publicly-trusted chain |
| Secure context | **no** | **yes** |
| Identity | the 14-symbol path token only | path token **plus** an allow-listed `Tailscale-User-Login` (`RemoteHub.cpp:2081-2087`) |
| Cookie | `rt=<token>; Path=/; SameSite=Lax` | the same **plus `Secure`** (`RemoteHub.cpp:2148`, flag built at `:2088`) |

This single table decides most of topic 1 and all of topic 2's delivery question. A browser's
"potentially trustworthy origin" list is `https:`/`wss:`, `127.0.0.0/8`, `::1/128`, `localhost` and
`file:` — **private LAN ranges are deliberately not on it**, because a `192.168.x.x` address cannot
be proven not to traverse the network the way `localhost` can (W3C Secure Contexts, Editor's Draft
2023-11-10, https://w3c.github.io/webappsec-secure-contexts/ ; MDN *Secure contexts*, last modified
2026-08-15, https://developer.mozilla.org/en-US/docs/Web/Security/Secure_Contexts).

What that costs on the LAN origin (MDN *Features restricted to secure contexts*, 2026-03-08):

| API | `http://192.168.x.x:13640` | `https://…ts.net` |
|---|---|---|
| Service workers | ✗ | ✓ |
| Push API | ✗ | ✓ (Android; iOS needs the home-screen app) |
| Notifications API | ✗ | ✓ |
| `crypto.subtle` | ✗ | ✓ |
| `navigator.clipboard` | ✗ | ✓ |
| Android Chrome install / WebAPK | ✗ (HTTPS required) | ✓ |
| Manifest fetched and parsed | ✓ | ✓ |
| iOS 26 "Add to Home Screen" → standalone | probably ✓ (**untested**) | ✓ |

And the origins are separate in every way that matters — scheme, host and port all differ, so they
are two origins by definition (MDN *Same-origin policy*, 2025-11-29). Two installs, two cookie jars,
two service-worker registrations, two push subscriptions, two localStorage buckets, two permission
grants. A token minted for one is meaningless on the other. MDN's *Installing web apps* page says
the same about installing one PWA from two browsers: *"two separate instances"* whose *"data is NOT
shared."*

### 0.2 The token, and what the page already does with it

- The token lives in the path. `HubServer::serve` pulls it out before comparing it in constant time
  (`RemoteHub.cpp:2130-2140`), so `/r/<token>/…` is a value comparison, not a prefix match.
- The page detects remote mode from its own path and re-sets the cookie from it on every load:
  `stream_center.html:253-259` — `var m = /^(\/r\/[a-z0-9]+)\/(?:index\.html)?$/.exec(location.pathname)`
  then `document.cookie = 'rt=' + REMOTE.split('/')[2] + '; path=/; SameSite=Lax'`.
  **This self-healing is what makes a home-screen icon viable at all** (§1.3).
- The cookie has no `Max-Age` and no `Expires` on either path — it is a **session cookie**. It only
  gates the go2rtc player at `/stream.html` and the `/api/ws` tunnel (`RemoteHub.cpp:2091-2107`);
  everything else is gated by the path token.
- **The token rotates.** `HubServer::set_phone` (`RemoteHub.cpp:1669-1679`): *"A new link every time
  it is turned on … unless the caller brings the one it remembered."* Turning phone access off and
  on from the tray, the hub page or the Stream tab mints a new token unless the slicer passes back
  the remembered one from `stream_phone_token`. This is the single biggest hazard for an installed
  icon, and is discussed at §1.3.
  **Update (P1, `feat/phone-token-stability`): it no longer does.** The token is the hub's and is
  remembered in `<datadir>/hub/settings.json`; `set_phone` never mints one, so off/on, a hub
  restart and a slicer restart all keep the same link. `HubServer::new_link()`
  (`POST /hub/newlink`, the hub page's *New link*, the tray's *New phone link*) is the only thing
  that replaces it, and the last three replaced tokens are kept so those links can explain
  themselves (§5.2).

### 0.3 What the hub is, in terms this document needs

- One permanent process (`snapmaker-orca --hub`), a windowless wx tray app whose HTTP work runs on
  worker threads; `HubServer::loop` ticks every two seconds and flushes logs (`RemoteHub.cpp:2233-2238`).
  **That loop is the obvious home for any watcher.**
- Two listeners: the phone-facing one (main, 0.0.0.0:13640 when phone access is on) and a
  loopback-only admin one on an ephemeral port that carries `/hub/*` and `/relay/h264` and nothing
  else (`RemoteHub.cpp:2076-2077`, `:2121-2127`).
- It proxies each slicer instance's loopback JSON API under `/r/<token>/i/<pid>/api/...` through an
  **explicit method+path allow-list**, `instance_api_allowed` (`RemoteHub.cpp:1909-1953`).
  `test_hardening.py` compares that list against the live manifest and fails on drift, so **every new
  instance route needs an entry there or it is invisible to the phone**.
- It already has an outbound HTTP client: the fork's libcurl wrapper (`#include "slic3r/Utils/Http.hpp"`,
  `RemoteHub.cpp:8`; used at `:2175` for the Flashforge probe and in `instance_post`, `:1795`).
- It already knows every camera's IP and, for Bambu printers, the **LAN access code**:
  `HubServer::lookup_host` reads `ip` and `code` out of `<datadir>/hub/streams.json`
  (`RemoteHub.cpp:1512-1529`). It does **not** know a printer's serial number — the pushed state has
  `id` (a page-generated `c<base36>` id), `ip`, `code`, `kind` and the go2rtc descriptor
  (`stream_center.html:316-336`), no `dev_id`/`sn`.
- Every response carries `Cache-Control: no-store`, `X-Content-Type-Options: nosniff` and
  `Referrer-Policy: no-referrer` (`RemoteHub.cpp:601-618`). There is no static-file route: each
  served file is an explicit `if` with a hard-coded content type.
- Libraries already linked into `libslic3r_gui` — which is where `RemoteHub.cpp` lives:
  `libcurl OpenSSL::SSL OpenSSL::Crypto` and `paho-mqttpp3-static` (`src/slic3r/CMakeLists.txt:840`,
  `:856`), plus `nlohmann/json` and Boost (`boost/beast/core/detail/base64.hpp` is already included
  by the hub, `RemoteHub.cpp:11`). OpenSSL is pinned at **1.1.1w** (`deps/OpenSSL/OpenSSL.cmake:42`).

---

## 1. An app-like home-screen link

### 1.1 What works today

`resources/web/orca/stream_center.html:1-10`:

```html
<meta name="viewport" content="width=device-width, initial-scale=1.0, viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="mobile-web-app-capable" content="yes">
<meta name="theme-color" content="#1f1f1f">
<title>Stream</title>
```

So the page is already **most of the way there**: the legacy Apple standalone tags are present, the
Chrome-preferred `mobile-web-app-capable` is present, a theme colour is present, and the whole layout
already respects `env(safe-area-inset-*)` (top bar at `:13`, grid at `:23`, expanded tiles at `:38-39`)
— which is exactly the padding a standalone web app needs once the browser chrome is gone. The page
also remembers its own view and layout in `localStorage` (`:307-308`, `:2419-2421`), so a relaunch
lands where the person left off.

Missing: **no web app manifest, no `<link rel="manifest">`, no `apple-touch-icon`, no icons served
at all, no install affordance, no service worker.** The hub has no route that could serve any of
them (`RemoteHub.cpp:2143-2185` — the `/r/<token>/…` handler knows `/`, `/index.html`, `/state`,
`/bambu`, `/ff`, then `handle_phone`).

Icons that exist in the tree: `resources/images/Snapmaker_Orca_192px.png` (192×192, confirmed from
the PNG IHDR), `Snapmaker_Orca_128px.png` (128), `Snapmaker_Orca.png` (256), plus SVG logos. **There
is no 512×512 PNG**, and Chrome's install criteria want both a 192 and a 512.

### 1.2 The platform facts

**iOS — the ground moved in 2025, in our favour.** Safari 26.0 (2025-09-15,
https://webkit.org/blog/17333/webkit-features-in-safari-26-0/): *every* website added to the Home
Screen now opens as a web app by default, and there are *"zero requirements for 'installability' in
Safari"* — no manifest, no meta tag, no service worker. The add sheet gains an "Open as Web App"
toggle the user can turn off. Before that, from iOS 16.4 (2023-02-16,
https://webkit.org/blog/13878/web-push-for-web-apps-on-ios-and-ipados/), a manifest with
`display: standalone` (or `fullscreen`) was the gate.

- **There is no programmatic "add to home screen" on iOS and there never has been.**
  `beforeinstallprompt` is Chromium-only and non-standard (MDN, 2023-10-25). Apple's only
  add-to-home-screen API, `SFAddToHomeScreenActivityItem`, is a native SafariServices API for
  third-party *browser* apps, unreachable from a page. **An in-page instruction sheet is the only
  option on iOS.**
- Manifest vs. Apple tags: WebKit parses `name`, `short_name`, `description`, `scope`, `display`,
  `start_url`, `theme_color`, `icons`. The one documented precedence rule is icons —
  *"If you do both, `apple-touch-icon` will take precedence over the Manifest-declared icons"*
  (webkit.org/blog/13878). So ship both and make them the same picture.
- `apple-mobile-web-app-capable` is not deprecated *by Apple*; Chrome DevTools warns about it and
  prefers `mobile-web-app-capable`. The page already has both, which is the right answer.
- The EU/DMA scare of Feb 2024 (iOS 17.4 betas disabling home-screen web apps in the EU) was
  reversed before 17.4 shipped (AppleInsider / 9to5Mac, 2024-03-01). Nothing has changed since;
  WebKit's WWDC26 / Safari 27 beta post (2026-06-08, https://webkit.org/blog/17967/) contains no
  web-app, manifest or push changes at all.

**Android/Chrome — install criteria** (web.dev, last updated 2024-09-19,
https://web.dev/articles/install-criteria):

- served over **HTTPS**;
- engagement heuristic (a tap, ~30 s on the page);
- a manifest with `name` or `short_name`, `start_url`, a `display` of
  `standalone`/`fullscreen`/`minimal-ui`/`window-controls-overlay`, and `icons` including **both a
  192 px and a 512 px** entry;
- **a service worker is no longer required** — dropped in Chrome 108 on mobile / 112 on desktop
  (https://developer.chrome.com/blog/update-install-criteria, 2023-12-05); MDN now states flatly
  that service workers are not required for installability, only for offline.

`beforeinstallprompt` can be `preventDefault()`ed, stashed, and fired from your own button's click
handler; `prompt()` must come from a user gesture. Meeting the criteria mints a real **WebAPK** —
launcher icon, an entry in Settings → Apps, no browser badge. Anything less is a bookmark shortcut.
Adding `screenshots` plus `description` upgrades the prompt to the richer bottom-sheet install UI
(https://developer.chrome.com/blog/richer-pwa-installation).

Chrome is also shipping a real programmatic path — the `<install>` element and
`navigator.install()`, origin trial in Chrome/Edge 148–153
(https://developer.chrome.com/blog/install-element-ot, 2026-05-12). Chromium only; worth knowing,
not worth building on yet.

**`start_url` and `scope`.** The manifest's `start_url` **replaces** the page the user was on when
they installed; it is resolved against the **manifest's** URL, not the document's, and must be
same-origin (W3C Web App Manifest; MDN `start_url`, 2026-08-31). If it is absent or invalid, the URL
of the page that linked the manifest is used. `scope` defaults to `start_url` minus filename/query/
fragment and is a path-prefix match; off-scope navigations are not blocked, but Chromium shows a URL
bar and **iOS opens them in a Safari View Controller sheet** (Apple WWDC23 *What's new in web apps*,
https://developer.apple.com/videos/play/wwdc2023/10120/).

**Detection.** Standalone: `navigator.standalone === true` (WebKit-only, `undefined` off-iOS) or
`matchMedia('(display-mode: standalone)').matches` (Baseline since Jan 2020). Already installed:
`navigator.getInstalledRelatedApps()` — Chromium only, top-level secure context only, needs a
self-referencing `related_applications` entry (MDN, 2026-07-08). **On iOS you can detect standalone
but never "installed".**

**Storage in an iOS home-screen web app.** It is *not* Safari's: *"Web applications added to the
home screen are not part of Safari and thus have their own counter of days of use"*
(https://webkit.org/blog/10218/, 2020-03-24). Cookies are copied once at add time and diverge from
then on; localStorage is not copied at all, and Apple's own advice is *"keep authentication state
saved within cookies"* (WWDC23). The ITP seven-day purge of script-writable storage — which does
include service-worker registrations and caches — is keyed to Safari's use counter; **home-screen
web apps have their own counter and Apple calls deleting their data "a serious bug."** There is a
real-world WebKit regression on session cookies resetting in home-screen web apps (bug 272325), so
do not lean on a session cookie surviving.

### 1.3 What breaks here specifically

1. **`start_url` versus the token.** A single static manifest with a fixed `start_url` **silently
   strips `/r/<token>/`** from every install: the icon would launch at `/`, which the hub answers
   404. Fixes: serve the manifest **under the token path** with `"start_url": "/r/<token>/"`, or omit
   `start_url` so the linking page's URL is used. Serving it per-token is better — it also lets the
   manifest's `id` and `scope` be token-scoped, and MDN's guidance about iOS resolving `start_url`
   against the manifest URL means a manifest at `/r/<token>/manifest.webmanifest` with
   `"start_url": "./"` resolves correctly by construction.
2. **Token rotation kills installed icons.** `set_phone` mints a new token whenever phone access is
   toggled on (`RemoteHub.cpp:1669-1679`). After that, an installed icon opens a dead URL and the
   only recovery is re-scanning the QR and re-installing. This is a **product decision, not a code
   detail**: either the token must become stable (remembered across toggles by default, with an
   explicit *Make a new link* button that is documented as breaking installed icons), or installs
   must be treated as disposable. Recommendation: make it stable — the slicer already remembers it
   in `stream_phone_token`, the hub already accepts it back through `POST /hub/phone?token=…`
   (`RemoteHub.cpp:1878-1880`), and the tray toggle at `:2284` is the one caller that passes `""`.
   **Update (P1, `feat/phone-token-stability`): done, stable.** The hub is the source of truth
   (`settings.json`, which survives the clean quit that deletes `hub.json`); `stream_phone_token`
   is the mirror a hub with no memory of its own is seeded from, and is no longer cleared when
   phone access is turned off. Both ways of handing a token in — `POST /hub/phone?token=…` and
   `--hub-token` / `SNORCA_PHONE_ACCESS` — now only seed a data folder that has no link yet, so a
   slicer holding an older copy cannot undo a *New link* somebody made from the tray.
3. **The `rt` cookie in standalone mode.** It is a session cookie on both paths, and on iOS the web
   app gets its own jar. It does not matter, because the page re-sets it from `location.pathname` on
   every load (`stream_center.html:259`) and everything except the go2rtc player is gated by the path
   token. **Provided `start_url` carries the token, standalone mode self-heals.** If it did not, the
   camera tiles would silently 404 and nothing else would.
4. **Secure context.** On the LAN origin there is no service worker, no push, no notifications, no
   `crypto.subtle`, and Android will not offer to install. iOS 26 will still add the icon and open it
   standalone (untested here — see §5). **The Tailscale origin is the only one where an install is a
   full-fat app.**
5. **Two origins, two installs.** A person on both paths gets two icons with two stores. Trying to
   unify them is not possible; the honest UI is to tell them which link they are installing.
6. **The QR flow.** Today the PC's QR encodes the LAN URL by default and the hub page shows the
   Tailscale URL separately (`remote_json_locked`, `RemoteHub.cpp:1709`). Scanning opens the browser
   at `/r/<token>/`; installing from there is the user's next manual step. That is fine — but the QR
   should preferably carry the **remote** URL when remote access is on, because that is the origin
   worth installing.
7. **`Cache-Control: no-store` on everything** (`RemoteHub.cpp:610`). Harmless for manifest and icons
   (they are re-fetched, and the hub is on the LAN or a fast tunnel), but it means a service worker
   must populate the Cache API by explicit `fetch` + `cache.put` rather than relying on the HTTP
   cache. Also note the page is a single 154 KB HTML file — precaching it is one entry.
8. **Scope and the player iframe.** The go2rtc player lives at `/stream.html` (root), outside a
   `/r/<token>/` scope. It is loaded in an **iframe** (`.cell iframe`, `stream_center.html:31`), and
   iframes are not navigations, so scope does not affect it. Do not widen `scope` to `/` just for
   that — a token-scoped scope is also a small privacy win.
9. **No 512 px icon** in `resources/images`. Android's install criteria want one.

### 1.4 Options

| Option | What it gives | Cost | Verdict |
|---|---|---|---|
| **A. Do nothing** | the person can already add a bookmark by hand | zero | the status quo; on iOS 26 that bookmark already opens standalone, but with a screenshot icon and the wrong name |
| **B. Manifest + icons + an Install button** (Android `beforeinstallprompt`, iOS instruction sheet) | a proper icon and name, standalone chrome, a WebAPK on Android, the platform ceiling on iOS | ~1–2 days | **recommended** |
| **C. B + a service worker** | offline shell, and the prerequisite for Web Push (§2) | +1 day, and only on the https origin | recommended **as part of topic 2**, not for its own sake |
| **D. A real native app** (iOS + Android) | push without a service worker, background behaviour, store presence | months, two store accounts, two review pipelines, a permanent release burden for a fork of a slicer | **no** |
| **E. A thin native shell** (WKWebView/WebView wrapping the same page) | an icon and native push, sidesteps iOS's home-screen-only push rule | weeks, still two store accounts, still an app that only works when the PC is on | **no** |

D and E both fail the same test: everything valuable the phone does here — cameras, Prepare, Send,
Devices — is already a web page that the hub serves and that the fork must keep working anyway. A
native app would be a second front end for the same API, and its only real advantage (push without
the home-screen gate on iOS) is available to the web app once it is installed to the home screen.

### 1.5 Recommendation

**Option B now, C when topic 2 phase 2 lands. No native app.**

Concretely:

- Serve a **per-token manifest** at `/r/<token>/manifest.webmanifest`, with the page linking it
  relatively (`<link rel="manifest" href="manifest.webmanifest">`) so the same file works on both
  origins with no absolute URLs anywhere.
- Serve **icons** under the same prefix, and keep `apple-touch-icon` pointing at the same PNG (it
  wins on iOS anyway).
- Add an **Install** entry in the top bar, shown only when not already standalone. On Android it
  fires the stashed `beforeinstallprompt`; on iOS it opens a short sheet: *Share → Add to Home
  Screen*, with a note that the link it installs is the one currently in the address bar.
- Show, in that sheet, **which link is being installed** (LAN or remote) and a one-line warning that
  the two are separate apps and that a new link breaks the icon.
- Make the phone-access token **stable across toggles** (see §1.3.2), with an explicit *New link*
  action that says what it breaks.

### 1.6 What must be added

**Hub (`RemoteHub.cpp`)** — three routes inside the `/r/<token>/…` branch at `:2143-2185`, next to
`/state`:

| Route | Response | Notes |
|---|---|---|
| `GET /r/<token>/manifest.webmanifest` | `application/manifest+json`, built from the token | `start_url: "./"`, `scope: "./"`, `id: "/r/<token>/"`, `display: "standalone"`, `name`, `short_name`, `theme_color`, `background_color`, `icons` (192, 512, and a `maskable` variant), optionally `description` + `screenshots` for the richer Android prompt |
| `GET /r/<token>/icon-<n>.png` | `image/png` from `resources/images/…` | 192 and 512; read through the existing `read_file` helper, same as the page |
| `GET /r/<token>/apple-touch-icon.png` | `image/png`, 180×180 | iOS ignores the manifest icons when this exists |

Add the same three to nothing else — they are under `/r/<token>/`, so the existing token gate covers
them, and they are *not* instance routes so `instance_api_allowed` is untouched. The manifest fetch
is credentials-omitted by default in browsers, which does not matter here because the gate is the
path, not the cookie — **this is a real advantage of the path-token design and should be written down
so nobody "improves" it into a cookie gate.**

**Resources** — one new PNG: a 512×512 (and ideally a `maskable` 512 with the safe-area padding
Android wants). `Snapmaker_Orca_192px.png` and a 180 px crop of `Snapmaker_Orca.png` cover the rest.

**Page (`stream_center.html`)** — `<link rel="manifest">` and `<link rel="apple-touch-icon">` in the
head; a `beforeinstallprompt` listener that stashes the event; an Install chip in `#topbar` next to
`#via`, hidden when `navigator.standalone === true || matchMedia('(display-mode: standalone)').matches`;
an iOS instruction sheet reusing the existing `#modal` pattern (`:52-60`).

**Effort: 1–2 days**, including the icon work and a browser check on both origins.

### 1.7 Risks

- **Untested:** whether iOS 26 really adds a plain-`http` LAN page as a standalone web app. WebKit's
  "zero requirements" wording implies yes; nobody has written it down for http origins. Test on the
  user's phone before promising it in the UI (§5).
- Token rotation is the failure mode people will actually hit. If §1.3.2 is not done, do not ship an
  Install button at all — a broken icon is worse than no icon.
- `black-translucent` for the status bar is widely reported as on the way out; the page's safe-area
  padding already handles the alternative, so dropping it later is free.

---

## 2. Notifications

### 2.1 What the code can see today

#### Bambu (`MachineObject`, `src/slic3r/GUI/DeviceManager.{hpp,cpp}`)

Everything a notification would want is already parsed out of the MQTT push and lives on the object:

| Field | Where | Meaning |
|---|---|---|
| `print_status` | `DeviceManager.hpp:775` | `FINISH` \| `SLICING` \| `RUNNING` \| `PAUSE` \| `INIT` \| `FAILED` — *the* state machine for "started / paused / finished / failed" |
| `print_error` | `DeviceManager.hpp:712` | the live error code; `0` when clear |
| `hms_list` | `DeviceManager.hpp:887` | `std::vector<HMSItem>` — `module_id`, `msg_level` (`HMS_FATAL`/`SERIOUS`/`COMMON`/`INFO`, `:360-367`), `msg_code`, `already_read`, and `get_long_error_code()` |
| `mc_print_percent`, `mc_left_time`, `curr_layer`, `total_layers` | `:705-716` | progress |
| `stage_curr` + `get_curr_stage()` | `DeviceManager.hpp:756`, `DeviceManager.cpp:1450` | the human stage string, from `get_stage_string()` at `DeviceManager.cpp:36`. **Stage 6 is `"Paused due to filament runout"`** (`:52`), 16 is "Printing was paused by the user", 17 "Pause of front cover falling" — the filament-runout signal already exists and is already translated |
| `bed_temp`/`bed_temp_target`, `m_extder_data.extders[].temp/target_temp` | | temperatures |
| `is_in_printing()`, `can_pause()`, `can_resume()`, `can_abort()` | `DeviceManager.cpp:2625-2642` | exactly the predicates a control UI needs |
| `sdcard_state` / `get_sdcard_state()` | `:883-884` | |

Error text: `HMSQuery::query_print_error_msg(int print_error, wxString&)` (`HMS.hpp:37`,
`HMS.cpp:312`), plus `get_hms_wiki_url` and `query_print_error_url_action`. The wiring is already
proven: `RemoteSend` watches a printer for 12 s after a print command, reads `print_error` and
resolves it through `HMSQuery` (`RemoteSend.cpp:607-640`, error text at `:622`). **That loop is a
working prototype of the watcher this topic needs** — poll `MachineObject` on the GUI thread every
second, compare against a remembered previous value, react to the edge.

**The constraint that shapes everything:** in LAN mode there is **one connected printer at a time**.
`DeviceManager::set_selected_machine` disconnects the previously selected LAN printer before
connecting the new one (`DeviceManager.cpp:6500-6516`, `m_agent->disconnect_printer()` at `:6514`).
Non-selected LAN printers are known by discovery only — name, model, online — with no `print_status`.
In cloud mode (signed in to a Bambu account) `DeviceManager::subscribe_device_list`
(`DeviceManager.cpp:6613`) subscribes to several devices at once; that is how the Multi-device page
works (`MultiMachineManagerPage.cpp:590`). So: **cloud = many printers watchable, LAN = exactly one.**

And the transport is closed: all Bambu MQTT lives inside the `bambu_networking` plugin DLL, reached
through `NetworkAgent`'s function pointers (`NetworkAgent.cpp:309-312`), with the callbacks wired in
`GUI_App::init_networking_callbacks` (`GUI_App.cpp:2033`, `:2080`). `MachineObject::parse_json`
(`DeviceManager.cpp:2888`) is what turns a push into the fields above.

#### Snapmaker U1 (Klipper/Moonraker)

- Print control exists and is already used by the Send feature: `Moonraker_Mqtt::async_start_print_job`
  (`MoonRaker.cpp:1394`, `printer.print.start`), `async_pause_print_job` (`:1418`,
  `printer.print.pause`), `async_resume_print_job` (`:1440`, `printer.print.resume`),
  `async_cancel_print_job` (`:1531`, `printer.print.cancel`).
- State: `Moonraker::get_machine_info` (`MoonRaker.cpp:314`) POSTs `printer/objects/query` over plain
  HTTP; `Moonraker_Mqtt::async_get_machine_info` (`:2540`) does the same over MQTT, and
  `async_subscribe_machine_info` exists for a push subscription.
- **But nothing in C++ names a single Klipper object.** `SSWCP_…::sw_SubscribeMachineState`
  (`SSWCP.cpp:2034`, targets built at `:2217-2230`) simply forwards whatever `objects` the Flutter
  page asks for. Grepping the tree for `print_stats`, `virtual_sdcard`, `filament_switch_sensor` or
  `runout` finds **nothing** outside a translated Bambu string. So for U1 the fork today has *no
  status model at all in C++*: state lives in the Flutter Device page.
- A U1 is only reachable from the slicer once `wxGetApp().get_connect_host()` is a `Moonraker_Mqtt`
  — the PC's Device page connect, or `RemoteSnapmaker::connect` from the phone
  (`2026-09-03-phone-send-design.md` §3.2), which needs a stored certificate and the opt-in
  `snapmaker_remember_keys`.
- The parallel branch `feat/phone-snapmaker-lan` is making U1 sends stateless over **plain LAN HTTP
  on port 80** (`/access/info` reporting `login_required: false` on the user's printers, per-toolhead
  filaments from `print_task_config`). That branch has no commits on top of `feat/ultra-preferences`
  yet, so nothing of it is visible here — but **if it lands, a hub-side U1 watcher becomes trivial**:
  plain `GET`/`POST` to `printer/objects/query` for `print_stats`, `virtual_sdcard`, `heater_bed`,
  `extruder` and `filament_switch_sensor …`, no MQTT, no certificate, no slicer instance. That
  changes the recommendation below and is flagged as a dependency.

#### What the hub knows on its own

`streams.json` gives it `ip` and, for Bambu, the LAN `code` for every camera the Stream tab has
(`RemoteHub.cpp:1512-1529`). It does **not** have the serial number, which Bambu's LAN MQTT needs for
its `device/<sn>/report` topic. And `paho-mqttpp3-static` is linked into the same library
(`src/slic3r/CMakeLists.txt:840`), so the hub *could* speak MQTT — but it would be reimplementing the
plugin's LAN protocol, and it would still need the serial.

### 2.2 Who watches

Four candidate homes, in increasing order of how much new machinery they need:

| # | Where | How | Sees | Cost | Verdict |
|---|---|---|---|---|---|
| **W1** | **Inside an instance**, a `RemoteEvents` module | a `wxTimer` or a worker polling `MachineObject` on the GUI thread every 2–5 s, exactly like `RemoteSend.cpp:607-640`; U1 through the connected `Moonraker_Mqtt` | the connected Bambu printer (all subscribed ones in cloud mode) and the connected U1 | ~1 day for Bambu, +1 for U1 | **best first step** — zero new protocol work, reuses proven code |
| **W2** | **In the hub, fed by an instance** | W1 detects the edge and POSTs it to a new `/hub/event` on the admin listener; the hub owns subscriptions and delivery | the same as W1 | +½ day on top of W1 | **the shape to build towards** — delivery survives the instance closing, and one hub can merge several instances |
| **W3** | **In the hub, polling instances** | the hub's 2 s `loop` (`RemoteHub.cpp:2233`) calls `GET /api/printers` on each live instance and diffs | the same as W1, one poll behind | ~½ day | tempting because `instance_post`/`probe_instance` already exist, but every poll marshals to the instance's **GUI thread** (`api_printers` → `run_on_main`, `RemoteAccess.cpp:943`), so it competes with the phone's own 5 s Devices poll and with slicing. Acceptable at 10–15 s; not a good primary |
| **W4** | **In the hub, talking to printers directly** | Bambu: paho MQTT to `mqtts://<ip>:8883` as `bblp` + access code — but the **serial is missing** from `streams.json`, and this reimplements the closed plugin's protocol. U1: plain Moonraker HTTP polling, which needs the LAN-HTTP branch or the stored certificate | printers with no slicer open at all | 3–5 days Bambu, ~1 day U1 *if* the LAN branch lands | **only worth it for U1**, and only after `feat/phone-snapmaker-lan`. Do not reimplement Bambu LAN MQTT |

The honest summary: **the hub has no `DeviceManager` and should not grow one.** The instance is where
printer state already lives; the hub is where delivery belongs, because it outlives every instance
and already owns the outbound HTTP client, the persisted settings file
(`<datadir>/hub/settings.json`, `RemoteHub.cpp:105`) and the two-second tick.

Events worth emitting, and where each comes from:

| Event | Bambu | U1 |
|---|---|---|
| print started | `print_status` → `RUNNING` from anything else | `print_stats.state` → `printing` |
| print finished | `print_status` → `FINISH` | `print_stats.state` → `complete` |
| print failed | `print_status` → `FAILED` | `print_stats.state` → `error`, `print_stats.message` |
| paused | `print_status` → `PAUSE`, with `get_curr_stage()` as the reason | `state` → `paused` |
| **filament runout** | `stage_curr == 6` ("Paused due to filament runout") | `filament_switch_sensor <name>.filament_detected` → false (needs a named sensor; verify per firmware) |
| printer error / HMS | `print_error != 0`, text via `HMSQuery::query_print_error_msg`; new `hms_list` entries at `HMS_FATAL`/`HMS_SERIOUS` | Klipper `webhooks.state == "shutdown"` + `state_message` |
| progress milestone (opt-in) | `mc_print_percent` crossing 25/50/75 | `virtual_sdcard.progress` |

Debouncing matters: `print_status` flaps during a reconnect, and `is_connected()` goes false after
`DISCONNECT_TIMEOUT` with no push (`DeviceManager.cpp:2761-2765`). Require a state to hold for two
consecutive polls, and never emit an event while `is_connected()` is false.

### 2.3 How it reaches the phone

#### (a) In-page notifications — cheapest, works today, on one origin

`ServiceWorkerRegistration.showNotification()` while the page is open, driven by the polling the
Devices tab already does. Two hard facts: the `new Notification()` **constructor throws on nearly all
mobile browsers** — *"Instead, you need to register a service worker and use
`ServiceWorkerRegistration.showNotification()`"* (MDN, 2026-05-25) — and in a normal **iOS Safari tab
`window.Notification` and `PushManager` are simply absent**; they exist only in a home-screen web app
(MDN BCD; webkit.org/blog/13878). On Android Chrome a plain tab is fine. Secure context required, so:
**Tailscale origin only.**

So even the cheap option needs a service worker and, on iOS, the home-screen install from topic 1.
That is the argument for doing topic 1 first.

Also worth having and free: **the hub's own tray balloon on the PC**. `HubTaskBarIcon` is a
`wxTaskBarIcon` (`RemoteHub.cpp:2275`) and `ShowBalloon` is one call. Not a phone notification, but it
is the zero-risk way to prove the watcher works.

#### (b) Web Push — the real answer, and it is buildable on this stack

Four RFCs: 8030 (delivery), 8188 (`aes128gcm` content coding), 8291 (message encryption), 8292
(VAPID). One `POST` to the opaque `subscription.endpoint`, with:

```
TTL: 2419200                          (mandatory — RFC 8030 §5.2; omit it and you get a 400)
Urgency: normal                       (optional)
Topic: <=32 base64url chars           (optional; coalesces/replaces)
Content-Encoding: aes128gcm
Content-Type: application/octet-stream
Authorization: vapid t=<ES256 JWT>, k=<base64url 65-byte P-256 public key>
<binary body>
```

Payload budget: RFC 8291 §4 does the arithmetic — *"at most 3993 octets of plaintext"* after the
86-byte header, one padding octet and the 16-byte GCM tag. A print notification is ~150 bytes.

**Every primitive is in OpenSSL 1.1.1w**, which is already linked:

| Step | OpenSSL |
|---|---|
| P-256 key generation (the persistent VAPID pair, and a fresh ephemeral pair per message) | `EVP_PKEY_CTX_new_id(EVP_PKEY_EC, …)` + `EVP_PKEY_keygen` |
| ECDH shared secret with the subscription's `p256dh` | `EVP_PKEY_derive_init` → `EVP_PKEY_derive_set_peer` → `EVP_PKEY_derive` (32 bytes) |
| the RFC 8291 §3.4 key schedule | plain `HMAC-SHA-256` — **no HKDF API needed**, because every expand step produces ≤32 bytes, i.e. one HMAC block: `HMAC(prk, info \|\| 0x01)` truncated |
| AES-128-GCM, 12-byte nonce (OpenSSL's default IV length for GCM, so no `SET_IVLEN`) | `EVP_aes_128_gcm`, tag via `EVP_CTRL_AEAD_GET_TAG` after `EVP_EncryptFinal` |
| ES256 JWT signature, DER → raw `r\|\|s` | `EVP_DigestSign*`, then `d2i_ECDSA_SIG` + `ECDSA_SIG_get0` + `BN_bn2binpad(r, out, 32)` / `BN_bn2binpad(s, out+32, 32)` |
| base64url | ~30 lines, or `boost::beast::detail::base64` (already included at `RemoteHub.cpp:11`) with `+/` → `-_` and padding stripped |

VAPID details that bite: `aud` is the **origin only** of the endpoint (`https://fcm.googleapis.com`,
`https://web.push.apple.com`) — not the path, and this is the commonest cause of 403s; `exp` ≤ 24 h
(Apple: don't refresh more than once an hour, so cache the JWT per push origin); `sub` must be a
`mailto:` or `https:` URI (SHOULD in the RFC, effectively MUST in practice). The `k` value and
`applicationServerKey` are the same uncompressed 65-byte point, and Apple rejects a mismatch with
`VapidPkHashMismatch`.

**Libraries: don't.** `web-push-libs/ecec` (MIT, C, OpenSSL-native) does the encryption but not VAPID
and has had no code push since **Feb 2019**; `rnascunha/pusha` (MIT, C/C++) does the whole thing but
is 4 stars, 33 commits, last pushed **Oct 2022**. Neither belongs in a shipping desktop app. Port
from `webpush-java` (**MIT** — the licence to read if you intend to lift structure; `web-push` npm and
`pywebpush` are MPL-2.0, which is file-level copyleft and fine to read but not to copy). Budget
roughly: 60 lines JWT, 120 lines encryption, 40 lines base64url, 60 lines libcurl POST and status
mapping.

**Platform rules that constrain the design:**

- **iOS 16.4+ (March 2023), home-screen web apps only.** Never a Safari tab, on any iOS browser (they
  are all WebKit). Requires a user gesture for the permission request, a service worker, a secure
  context. **No Apple Developer Program membership is needed** — it is standards-based VAPID.
  Endpoint host `https://web.push.apple.com`; allow `https://*.push.apple.com` outbound.
  Safari 18.4 (2025-03-27) added **Declarative Web Push**, which drops the service-worker requirement
  — still home-screen only, same transport.
- **Apple revokes the permission if you don't show a notification.** *"Safari doesn't support
  invisible push notifications… If you don't [show one], Safari revokes the push notification
  permission for your site."* Chrome's equivalent is to show its own "This site has been updated in
  the background" placeholder. `userVisibleOnly: true` is required by Chrome.
- **Android Chrome needs no Firebase project and no API key** with VAPID (Chrome for Developers,
  2016-07-27, https://developer.chrome.com/blog/web-push-interop-wins), works in an ordinary tab
  without any install, and is unaffected by the 2024 FCM legacy shutdown (that killed the
  proprietary `Authorization: key=` path, not VAPID).
- **Subscriptions do not expire** (`expirationTime` is null everywhere in practice) but do die.
  `404` and `410` are both terminal: delete the record, never retry. `429`: honour `Retry-After`.
  `pushsubscriptionchange` is **not supported on Safari iOS at all** (MDN BCD) and only landed
  spec-conformant in Chrome 138 — so the only mechanism that works everywhere is **the page
  re-POSTing `getSubscription()` to the hub on every launch.**

**The decisive fact for this architecture** — and it is good news — is that **the push never touches
our origin**. The PC posts to FCM/APNs over the public internet; the phone receives it over its own
internet connection; the already-installed service worker wakes and renders the notification from the
payload. The origin (LAN-only, or tailnet-only) does not need to be reachable at that moment (MDN,
*PWA offline and background operation*, 2025-06-23). Three rules follow and they are not optional:

1. `showNotification()` from the payload **first**, before any network work.
2. **Never** make a `fetch()` to our origin a prerequisite for showing it — off-LAN that rejects, the
   promise passed to `waitUntil()` settles without a notification, and you get Chrome's placeholder
   or Safari revoking the permission.
3. `notificationclick` must open a **cached shell** that renders from the notification's `data`, not
   a URL that needs the server.

New network requirement for the PC: outbound HTTPS to `fcm.googleapis.com` and
`web.push.apple.com`. Surface it in the UI when it fails.

#### (c) Third-party relays — a single generic POST covers almost all of them

| Service | Minimal send | Account | App | Cost | Privacy | iOS | Android | Actions |
|---|---|---|---|---|---|---|---|---|
| **ntfy.sh** | `POST https://ntfy.sh/<topic>`, body = text, `Title:`/`Priority:`/`Tags:`/`Click:`/`Actions:` headers (or one JSON POST to `/`) | none | app or web app | free, **250 msg/day per IP** anonymous (read live from `ntfy.sh/v1/account`), 4096-byte body, 60-burst + 1/5 s | server sees the whole body; **the topic is the password** — anyone who guesses it can read *and* publish | app exists; maintainer calls it *"bare bones and quite frankly a little buggy"*, iOS 26.2 sound bug documented | solid (Play + F-Droid) | **best in class**: `Click:` opens a URL, `Actions:` gives up to 3 buttons including `http` (the *phone* fires a request — e.g. Pause, straight at the PC over Tailscale) |
| **Pushover** | `POST https://api.pushover.net/1/messages.json` with `token`, `user`, `message`, `title`, `url`, `priority` | yes (user key) + one app token we embed | yes | **$4.99 one-time per platform**, 30-day trial; 10,000/month per app (per-account from 2026-05-01) | vendor sees content, deletes on delivery, states it does not sell | polished, reliable | reliable | one `url`+`url_title` link; emergency priority (2) re-alerts until acknowledged — genuinely right for filament runout |
| **Telegram bot** | `POST https://api.telegram.org/bot<TOKEN>/sendMessage` with `chat_id`, `text`, optional inline keyboard | BotFather; **the user must press Start first** | yes | free | not E2E; **the bot token on the PC is a hot, unscoped credential** | good | good | inline `url` buttons |
| **Discord webhook** | `POST https://discord.com/api/webhooks/<id>/<token>`, `{"content":"…"}` | own a server | yes | free | content persists in the channel forever | **push only if the channel is All Messages or the message `@mention`s the user** — silent failure is the default | same | links only |
| **Email/SMTP** | libcurl speaks `smtps://` natively | app password or OAuth | none | free | user's own provider, but the slicer holds a send-as credential | not push; minutes | same | none. **Email-to-SMS gateways are dead** (AT&T 2025-06-17, T-Mobile ~Dec 2024, Verizon hard cutoff 2027-03-31) |
| Gotify | `POST /message`, `X-Gotify-Key:` | self-host | Android only | free | perfect | **no official iOS client** | good | some |
| Bark | `GET https://api.day.app/<key>/<title>/<body>?url=…` | none | iOS only | free | good (self-hostable) | **best iOS ergonomics here** | none | `url` deep link |

**Two things this table implies.** First: one generic "HTTP POST notification" implementation — user
URL, optional headers, a body template with `{event}`/`{printer}`/`{file}`/`{percent}` substitutions —
covers ntfy (public *and* self-hosted), Gotify, Discord, Slack, Home Assistant, Bark and Apprise with
nothing but configuration. That is ~30 lines of libcurl on top of the `Http` wrapper the hub already
uses. Second: ntfy's `Actions: http, Pause, <url>, method=POST` is a genuinely interesting shortcut
for topic 3 — a Pause button *in the notification*, fired by the phone straight at the hub over
Tailscale, with no app open. Worth prototyping once topic 3's routes exist.

**Tailscale itself offers nothing here.** Taildrop is file transfer; Tailscale webhooks run the other
direction (tailnet events → your endpoint). But it does enable the best privacy answer: run ntfy on
the same PC and publish it with `tailscale serve`. On Android that is fully private; on iOS the ntfy
server still needs `upstream-base-url: "https://ntfy.sh"`, and then only a message id plus a SHA-256
of the topic URL leaves the tailnet — ntfy.sh fires a content-free APNs push and the phone pulls the
body from your server. That is a nice option to document, not one to bundle.

#### (d) A native app

Rejected for the same reasons as §1.4 D/E. The only capability it adds is push on iOS without the
home-screen install, and that install is one tap the person makes once.

### 2.4 Recommendation

**Phased, with the cheap and certain parts first.**

- **N0 — the watcher, with no delivery (½–1 day).** `RemoteEvents` in the instance (W1): poll
  `MachineObject` on the GUI thread every 3 s, debounce, emit a typed event
  `{kind, printer_id, printer_name, title, body, at, severity, data}`. Expose `GET /api/events?since=`
  (a ring buffer of the last ~50). Prove it with the hub tray balloon and a red dot on the phone's
  Devices tab. **Nothing can go wrong and everything later depends on it.**
- **N1 — relay delivery from the hub (1–1½ days).** Instance POSTs each event to a new
  `POST /hub/event` on the **admin** listener (loopback, `X-Hub-Secret`, `Content-Type: application/json`
  — exactly the shape of `/hub/state`, `RemoteHub.cpp:1867-1877`). The hub owns a small
  notification config in `<datadir>/hub/settings.json`: a list of destinations, each
  `{name, url, method, headers[], template, events[]}`, plus an **ntfy preset** that generates a
  30-char random topic and shows it as a QR ("install ntfy, scan, done") and a **Pushover preset**.
  Send with the existing `Http::post`, 3 s connect / 5 s total, at most one retry, treat 429 as
  terminal. **This gives iOS and Android push today with no service worker, no secure-context
  requirement and no manifest** — it works even for a LAN-only user.
- **N2 — in-page notifications (½ day, after topic 1).** Service worker on the https origin;
  `registration.showNotification()` from the events feed while the page is open. One code path for
  foreground and background, because the `Notification` constructor is unusable on mobile anyway.
- **N3 — Web Push (4–6 days).** VAPID keypair generated once by the hub and persisted alongside the
  settings; `POST/DELETE /r/<token>/push/subscription`; the send path described in §2.3(b); the page
  re-syncing its subscription on every launch. Ship it as an *alternative* to N1, not a replacement:
  it is the only channel where nothing leaves the house except an encrypted blob the vendor cannot
  read, but it is also the one with iOS's home-screen gate and its permission-revocation rule.
- **N4 (optional) — U1 watching without a slicer**, once `feat/phone-snapmaker-lan` lands: the hub
  polls `printer/objects/query` over plain LAN HTTP from its own two-second loop (W4-for-U1).

**Why relays before Web Push.** N1 is a day and a half and works for every user on every origin
including LAN-only. N3 is a week, only helps the subset who installed the app on the Tailscale
origin, and carries a permission-revocation failure mode. The order is not close.

### 2.5 What must be added

**Instance (`RemoteAccess.cpp` + a new `RemoteEvents.{hpp,cpp}`)**

| Route | Purpose |
|---|---|
| `GET /api/events?since={seq}` | the ring buffer since a sequence number |
| `GET /api/events/config`, `POST /api/events/config` | which events this instance emits (per printer, per kind) |

Both need adding to the manifest (`RemoteAccess.cpp:1851-1882`) **and** to
`instance_api_allowed` (`RemoteHub.cpp:1909-1953`), or `test_hardening.py` fails.

**Hub (`RemoteHub.cpp`)**

| Route | Listener | Purpose |
|---|---|---|
| `POST /hub/event` | admin | an instance hands over an event |
| `GET/POST /hub/notify` | admin | destinations config (hub page UI) |
| `POST /hub/notify/test` | admin | send a test notification |
| `GET/POST/DELETE /r/<token>/push/subscription` | main | N3 only: the phone's push subscription |
| `GET /r/<token>/push/key` | main | N3 only: the VAPID public key |
| `GET /r/<token>/events?since=` | main | the merged feed for the page (N2) |

New files: `RemoteEvents.{hpp,cpp}` (instance-side watcher), `HubNotify.{hpp,cpp}` (hub-side
destinations + send), and for N3 `WebPush.{hpp,cpp}` (VAPID + RFC 8291). New page files:
`resources/web/orca/sw.js`. No new third-party library on any phase.

**Storage.** Destinations and the VAPID private key go in `<datadir>/hub/` — **not** `hub.json`,
which `shutdown()` deletes (`RemoteHub.cpp:2255-2258`); the existing `settings.json` (added for
`remote_on` and the tailnet allow-list precisely because of that deletion — `RemoteHub.cpp:105`,
written at `:1289`, read at `:2205`)
is the right home. Treat an ntfy topic and a Pushover token as credentials: never log them, never put
them in a project file, never return them to the phone (report `configured: true` the way
`/api/snapmaker/devices` reports `can_connect`).

### 2.6 Risks

- **Duplicate or missed events.** A duplicate "print finished" is worse than a missed one. Debounce
  two polls, key each event by `(printer, kind, task_id)` and de-duplicate on the hub.
- **Only one LAN Bambu printer is watchable.** Say so in the UI rather than silently watching one of
  three.
- **Apple revoking the push permission** if a push arrives and no notification is shown. This is the
  single most dangerous failure mode for a LAN-only origin, and the reason for the three rules in
  §2.3(b).
- **Private-CA HTTPS and service workers.** Not relevant on the Tailscale path (a real public chain),
  but if anyone later proposes a self-signed hub certificate: a click-through exception does **not**
  produce a secure context and service-worker registration is blocked (Chromium issue 40423989).
- **The PC now needs outbound internet** for N1 and N3. A workshop PC behind a restrictive firewall
  gets nothing; detect and say so.
- **Relay privacy.** ntfy.sh sees the whole message and the topic is the only secret. Generate a long
  random topic; never let the user type "printer".

---

## 3. Start / pause / stop on the Devices tab

### 3.1 What the fork already offers

**Bambu.** Everything is present and used by the desktop:

- `MachineObject::command_task_pause()` (`DeviceManager.cpp:1879`) → `{"print":{"command":"pause"}}`
- `command_task_resume()` (`:1889`) → `"resume"`
- `command_task_abort()` (`:1856`) → `"stop"`; `command_task_cancel(job_id)` (`:1867`) is the same
  with a job id
- predicates: `can_pause()` (`:2632`, `print_status == RUNNING`), `can_resume()` (`:2625`, `== PAUSE`),
  `can_abort()` (`:2639`, any of `PAUSE`/`RUNNING`/`SLICING`/`PREPARE`)
- declarations at `DeviceManager.hpp:957-961`

The desktop's own UI is the model to copy: `StatusPanel::on_subtask_pause_resume`
(`StatusPanel.cpp:1943`) flips on `can_resume()` with **no confirmation** and closes the error dialog;
`StatusPanel::on_subtask_abort` (`:1963`) opens a `SecondaryCheckDialog` with *"Are you sure you want
to cancel this print?"* and only then calls `command_task_abort()`. `MultiTaskManagerPage.cpp:180-202`
does the same three from a list.

**"Print a file on the printer's storage"** exists but is not a one-liner: `MediaFilePanel`'s Print
action fetches the model's metadata off the printer (`MediaFilePanel.cpp:537-585`,
`fs->FetchModel(...)` then `load_gcode_3mf_from_stream` then
`plater()->update_print_required_data(...)`), posts `EVT_PRINT_FROM_SDCARD_VIEW`
(`MediaFilePanel.cpp:577`), which `Plater` turns into `SelectMachineDialog` in
`PrintFromType::FROM_SDCARD_VIEW` (`Plater.cpp:16199`), which fills `m_print_from_sdc_plate_idx` and
`dst_name` from the printer file (`SelectMachine.cpp:2139-2160`) and then runs the same `PrintJob`.
It needs a `PrinterFileSystem` connection, a plate choice, AMS mapping and the whole dialog's
preconditions. **Exposing it to the phone is its own feature, not part of this one.**

**Snapmaker U1.** `printer.print.pause` / `.resume` / `.cancel` / `.start` are all implemented on
`Moonraker_Mqtt` (`MoonRaker.cpp:1418`, `:1440`, `:1531`, `:1394`), and over plain HTTP
`Moonraker::send_gcodes` (`:352`, `printer/gcode/script`) and `get_machine_info` (`:314`,
`printer/objects/query`) exist too. The MQTT path needs the connected host —
`wxGetApp().get_connect_host()` — which the phone can now establish itself via
`POST /api/snapmaker/connect` (phone-send design §3.4).

**Already exposed to the phone:** `GET /api/printers` (`RemoteAccess.cpp:940-988`) returns per printer
`online`, `connected`, `status` (`print_status` verbatim), `printing` (`is_in_printing()`), `percent`,
`left_time_s`, `layer`/`total_layers`, `task`, `bed_temp`/`bed_target`, `nozzles[]`, `selected`, plus
everything `RemoteSend::describe_bambu` adds (`kind`, `can_upload`, `can_print`, `lan_mode`,
`access_code_set`, `sdcard`, `has_ams`, `model_matches`, `options`). The Devices tab already renders
all of it and polls every 5 s (`stream_center.html:2369-2405`, `:1275`).

### 3.2 What is missing

- **No control route at all.** The instance API has `slice`, `send`, `presets`, `settings`,
  `snapmaker/connect` — nothing that pauses a print. The proxy allow-list (`RemoteHub.cpp:1909-1953`)
  therefore has nothing to allow.
- `GET /api/printers` reports `printing` but not `can_pause`/`can_resume`/`can_abort`, so the page
  cannot render correct buttons without duplicating the `print_status` string logic in JavaScript.
- No `print_error`/HMS text in `/api/printers` — the Devices tab cannot say *why* a printer is
  stopped, only that it is.
- Nothing for the U1: `/api/printers` gets a `connect` entry from `RemoteSend::list_hosts` with send
  capabilities, but no state at all (see §2.1 — there is no U1 status model in C++).

### 3.3 What to add

**Instance**

| Route | Behaviour |
|---|---|
| `POST /api/printers/{id}/control` (form `action=pause\|resume\|stop&confirm=1`) | Bambu: check the predicate, call the matching `command_task_*`, then watch `print_status`/`print_error` for ~10 s exactly as `RemoteSend.cpp:607-640` does, and answer with what the printer did. `id == "connect"`: the U1's `async_pause_print_job`/`resume`/`cancel` on the connected `Moonraker_Mqtt`. Returns a job id so the page can poll `/api/jobs/{id}` like a send |

Status codes, matching the send route's grammar so the page's error handling is one function:
`400` bad action, or **`stop` without `confirm=1`**; `404` unknown printer; `409` the printer is
offline / not connected / the action is impossible in the current state (`!can_pause()` etc.) / another
control action is in flight; `502` the command was refused; `504` no state change within the window.

Extend `GET /api/printers` with `can_pause`, `can_resume`, `can_abort`, `stage` (`get_curr_stage()`),
`print_error` (`{code, message}` via `HMSQuery::query_print_error_msg`) and, for the `connect` entry,
whatever the U1 state work produces. Same `run_on_main` marshalling as `api_printers`.

**Hub.** One line in `instance_api_allowed` for `/api/printers/<id>/control` — note `{id}` is a Bambu
`dev_id` or the literal `host`/`connect`, so the existing numeric-segment matcher does not fit and the
allow-list needs a small string-segment case. And an entry in the manifest, or the gate fails.

**Page.** In each printer card in `refreshDevices` (`stream_center.html:2369`): a **Pause**/**Resume**
button (enabled from `can_pause`/`can_resume`) and a **Stop** button (from `can_abort`). Stop opens the
existing confirm sheet (`:2169` is the pattern — *"Start printing … now? Make sure the bed is clear…"*)
with *"Stop the print on <printer>? It cannot be resumed."*, and only then sends `confirm=1`. Show
`stage` and `print_error.message` under the progress line, so a runout pause reads as one.

**Starting a print** should stay where it is: the Prepare tab's Send sheet, which already does
upload / upload-and-print with `confirm=1` and a second question (`stream_center.html:2162-2199`,
phone-send design §2). Do not add a second, different way to start a print on the Devices tab; add at
most a *Send a plate…* link that switches tabs.

### 3.4 Safety rules (inherited, and extended)

From `2026-09-03-phone-send-design.md` §4, which apply unchanged:

- **Anything that starts or stops a print needs `confirm=1` from the phone, and the page asks the
  person first.** Extend it: `stop` needs `confirm=1`. `pause` and `resume` do not — the desktop
  itself does not confirm them (`StatusPanel.cpp:1943`), and both are reversible.
- One control action at a time per instance, as with sends.
- **Nothing is shown on the PC.** No dialog, no modal; a hidden instance must stay hidden. The
  existing `wxModalDialogHook` policy (hidden-service-mode §4) already covers accidental dialogs, but
  the control path must not open any in the first place.
- The access code and the U1 certificate never leave the PC.
- A refusal is a refusal: report the printer's own error text (HMS) rather than retrying.

### 3.5 The polling needed to render the controls

The Devices tab already polls `/api/printers` every 5 s while it is the active view
(`stream_center.html:1275`) and stops when it is not (`:1272`). That is enough:

- `printing` + `percent` + `left_time_s` + `layer`/`total_layers` → the progress line (already there);
- `can_pause`/`can_resume`/`can_abort` → which buttons are live;
- `stage` → "Paused due to filament runout" instead of a bare "PAUSE";
- `print_error` → the red line.

After an action, poll the returned job at 1 s until it settles (the same code the Send sheet uses),
then fall back to the 5 s cadence. Do not shorten the base interval: `api_printers` marshals to the
GUI thread (`RemoteAccess.cpp:943`), and a hidden instance is also slicing for somebody.

**Effort: 2–3 days** for Bambu plus the page, +1 day for the U1 once its state model exists.

### 3.6 Recommendation

Ship **pause / resume / stop for Bambu** first — it is the highest value per line of code in this
whole document, all four pieces (commands, predicates, the watch loop, the confirm sheet) already
exist and only need joining up. Add the U1 alongside the state work in topic 2. Leave
"print a file already on the printer" out of scope.

---

## 4. Phased plan across the three topics

| Phase | Content | Depends on | Effort |
|---|---|---|---|
| **P1** | **Token stability**: `set_phone` keeps the remembered token unless the person explicitly asks for a new link; the tray toggle and the hub page pass it back; a *New link* action that says what it breaks | — | **½ day** · **done** on `feat/phone-token-stability`: the token lives in `settings.json`, `POST /hub/newlink` (hub page, tray, Stream tab) is the only thing that replaces it, a replaced link answers a 404 page that says so, gate `test_phone_token.py` |
| **P2** | **Home-screen install**: per-token manifest + icon routes, a 512 px icon, `<link rel="manifest">`, `apple-touch-icon`, the Install chip, the iOS instruction sheet, "which link am I installing" | P1 | **1–2 days** |
| **P3** | **Printer control**: `POST /api/printers/{id}/control` for Bambu, `can_*`/`stage`/`print_error` on `/api/printers`, allow-list + manifest entries, the Devices-tab buttons and the Stop confirm | — (independent of P1/P2) | **2–3 days** |
| **P4** | **The event watcher**: `RemoteEvents` in the instance, `GET /api/events`, the hub tray balloon, the Devices-tab badge | P3's `/api/printers` extensions (shares the polling code) | **1 day** |
| **P5** | **Relay notifications**: `POST /hub/event`, hub-side destinations in `settings.json`, the generic POST sender, ntfy and Pushover presets with a QR, the hub-page UI, a test button | P4 | **1–1½ days** |
| **P6** | **In-page notifications**: service worker on the https origin, `showNotification()` from the events feed | P2, P4 | **½ day** |
| **P7** | **Web Push**: VAPID keypair, `WebPush.{hpp,cpp}` (JWT + RFC 8291 on OpenSSL), subscription routes, the SW `push`/`notificationclick` handlers, re-sync on launch, 404/410 pruning | P6 | **4–6 days** |
| **P8** *(opt.)* | **U1 without a slicer**: hub polls Moonraker over LAN HTTP from its own loop; U1 events and controls with no instance open | `feat/phone-snapmaker-lan` | **1–2 days** |
| **P9** *(opt.)* | ntfy `Actions: http` buttons that pause a print straight from the notification | P3, P5 | **½ day** |

**Total for the recommended core (P1–P6): about 6–8 days.** P7 roughly doubles it and should be
judged on its own merits after P5 is in the user's hands — if relay notifications turn out to be
enough, P7 buys privacy and independence, not capability.

Ordering notes:

- **P3 is independent and highest-value.** If only one thing ships, ship P3.
- **P1 gates P2.** An Install button without a stable token produces broken icons.
- **P4 is shared plumbing.** Both P5 and P6 are thin layers on it, which is why they are cheap.
- **Every phase that adds an instance route must add it to the manifest *and* to
  `instance_api_allowed`**, or `test_hardening.py` fails — by design.
- Each phase should get a gate script in `snorca_hubtest\` alongside the existing ones
  (`test_security.py`, `test_remote.py`, `test_hardening.py`, `test_phone_send.py`,
  `test_phone_snapmaker.py`), and those must keep passing.

---

## 5. Open questions, and what only hardware can answer

1. **Does iOS 26 add a plain-`http` LAN page to the home screen as a standalone web app?** WebKit's
   "zero requirements for installability" wording implies yes, but nobody has documented it for
   non-secure origins. *Test: add `http://<lan-ip>:13640/r/<token>/` on the user's iPhone and check
   `navigator.standalone`.* Decides whether the Install button appears on the LAN origin at all.
2. **Does the installed icon survive a token change?** It will not — but confirm the failure is a
   clean 404 page and not a confusing blank, and decide what that page should say.
   **Update (P1, `feat/phone-token-stability`): answered for the *replaced* case.** The hub keeps the
   last three tokens it replaced and answers any path under one of them with a small page —
   *"This link was replaced. A new phone link was made on the PC, so this one no longer works.
   On the PC, open the hub page from the Snapmaker Orca icon next to the clock and scan the new
   code."* — still HTTP 404, `X-Frame-Options: DENY`, no token, no address, nothing about this PC.
   Rotating four times or replacing the data dir puts a link past that memory and it goes back to
   the bare `not found`, which is the honest answer: the hub genuinely does not know it. What an
   *installed icon* does with a 404 (iOS standalone in particular) still needs a phone to answer.
3. **Does the `rt` cookie survive a relaunch of the standalone app** long enough for the camera tiles
   to render before the page re-sets it (`stream_center.html:259`)? WebKit bug 272325 says session
   cookies in home-screen web apps reset unpredictably; the self-heal should make this invisible, but
   watch for a first-frame flash of "camera relay is not running".
4. **Which Klipper objects does the U1 actually expose?** `print_stats`, `virtual_sdcard`,
   `heater_bed`, `extruder`, and *which* `filament_switch_sensor <name>` — nothing in this tree names
   them, and the answer decides the U1 half of §2.1. One `printer/objects/list` against a real U1
   settles it.
5. **Does the H2-series "command verification failed" behaviour extend to pause/stop?** The send
   feature found that an H2 printer without LAN-only mode + Developer Mode refuses a print command
   (phone-send design §2). Verify whether `pause`/`stop` are refused the same way, and reuse the same
   hint if so.
6. **Multiple Bambu printers.** Confirm on the user's four printers (X1C, H2S, H2D, H2C) that only the
   selected one reports `print_status`, and decide whether the Devices tab should say "not watched"
   on the others rather than showing a stale idle.
7. **Web Push on the Tailscale origin (if P7 happens).** The certificate chain is public, so it should
   behave like any HTTPS site — but verify a subscription is minted on both an installed iOS web app
   and Android Chrome before writing the sender.

---

## 6. Sources

**iOS / WebKit** — [Safari 26.0](https://webkit.org/blog/17333/webkit-features-in-safari-26-0/) (2025-09-15) ·
[Web Push for Web Apps on iOS and iPadOS](https://webkit.org/blog/13878/web-push-for-web-apps-on-ios-and-ipados/) (2023-02-16) ·
[Meet Declarative Web Push](https://webkit.org/blog/16535/meet-declarative-web-push/) (2025-03-27) ·
[Full Third-Party Cookie Blocking and More](https://webkit.org/blog/10218/full-third-party-cookie-blocking-and-more/) (2020-03-24) ·
[Safari 27 beta](https://webkit.org/blog/17967/news-from-wwdc26-webkit-in-safari-27-beta/) (2026-06-08) ·
[Apple WWDC23 — What's new in web apps](https://developer.apple.com/videos/play/wwdc2023/10120/) (June 2023) ·
[Apple — Sending web push notifications in web apps and browsers](https://developer.apple.com/documentation/usernotifications/sending-web-push-notifications-in-web-apps-and-browsers) ·
[Apple — APNs network requirements](https://support.apple.com/en-us/102266) ·
[AppleInsider on the EU PWA reversal](https://appleinsider.com/articles/24/03/01/apple-reverses-course-on-death-of-progressive-web-apps-in-eu) (2024-03-01)

**Standards / MDN** — [W3C Secure Contexts](https://w3c.github.io/webappsec-secure-contexts/) (ED 2023-11-10) ·
[W3C Web App Manifest](https://w3c.github.io/manifest/) ·
[MDN Secure contexts](https://developer.mozilla.org/en-US/docs/Web/Security/Secure_Contexts) (2026-08-15) ·
[MDN Features restricted to secure contexts](https://developer.mozilla.org/en-US/docs/Web/Security/Secure_Contexts/features_restricted_to_secure_contexts) (2026-03-08) ·
[MDN Same-origin policy](https://developer.mozilla.org/en-US/docs/Web/Security/Same-origin_policy) (2025-11-29) ·
[MDN Making PWAs installable](https://developer.mozilla.org/en-US/docs/Web/Progressive_web_apps/Guides/Making_PWAs_installable) (2025-11-30) ·
[MDN Installing web apps](https://developer.mozilla.org/en-US/docs/Web/Progressive_web_apps/Guides/Installing) (2026-05-15) ·
[MDN start_url](https://developer.mozilla.org/en-US/docs/Web/Progressive_web_apps/Manifest/Reference/start_url) (2026-08-31) ·
[MDN scope](https://developer.mozilla.org/en-US/docs/Web/Progressive_web_apps/Manifest/Reference/scope) (2025-06-23) ·
[MDN display-mode](https://developer.mozilla.org/en-US/docs/Web/CSS/@media/display-mode) (2026-04-20) ·
[MDN getInstalledRelatedApps](https://developer.mozilla.org/en-US/docs/Web/API/Navigator/getInstalledRelatedApps) (2026-07-08) ·
[MDN BeforeInstallPromptEvent](https://developer.mozilla.org/en-US/docs/Web/API/BeforeInstallPromptEvent) (2023-10-25) ·
[MDN Notification() constructor](https://developer.mozilla.org/en-US/docs/Web/API/Notification/Notification) (2026-05-25) ·
[MDN PWA offline and background operation](https://developer.mozilla.org/en-US/docs/Web/Progressive_web_apps/Guides/Offline_and_background_operation) (2025-06-23) ·
[MDN pushsubscriptionchange](https://developer.mozilla.org/en-US/docs/Web/API/ServiceWorkerGlobalScope/pushsubscriptionchange_event) (2025-09-18)

**Chrome / Android** — [Revisiting installability criteria](https://developer.chrome.com/blog/update-install-criteria) (2023-12-05) ·
[web.dev install criteria](https://web.dev/articles/install-criteria) (2024-09-19) ·
[Richer PWA installation UI](https://developer.chrome.com/blog/richer-pwa-installation) ·
[HTML install element / Web Install API origin trial](https://developer.chrome.com/blog/install-element-ot) (2026-05-12) ·
[Web Push interoperability wins](https://developer.chrome.com/blog/web-push-interop-wins) (2016-07-27) ·
[Local Network Access](https://developer.chrome.com/blog/local-network-access) ·
[Chromium issue 40423989 — service workers and self-signed certificates](https://issues.chromium.org/issues/40423989)

**Web Push protocol** — [RFC 8030](https://www.rfc-editor.org/rfc/rfc8030) (Dec 2016) ·
[RFC 8188](https://www.rfc-editor.org/rfc/rfc8188) (Jun 2017) ·
[RFC 8291](https://www.rfc-editor.org/rfc/rfc8291) (Nov 2017) ·
[RFC 8292](https://www.rfc-editor.org/rfc/rfc8292) (Nov 2017) ·
OpenSSL: [EVP_PKEY_derive](https://docs.openssl.org/3.0/man3/EVP_PKEY_derive/) ·
[EVP_KDF-HKDF](https://docs.openssl.org/3.0/man7/EVP_KDF-HKDF/) ·
[EVP_EncryptInit](https://docs.openssl.org/3.0/man3/EVP_EncryptInit/) ·
[ECDSA_SIG_new](https://github.com/openssl/openssl/blob/master/doc/man3/ECDSA_SIG_new.pod) ·
libraries: [web-push-libs/ecec](https://github.com/web-push-libs/ecec) (MIT, last push Feb 2019) ·
[rnascunha/pusha](https://github.com/rnascunha/pusha) (MIT, last push Oct 2022) ·
[webpush-java](https://github.com/web-push-libs/webpush-java) (MIT) ·
[web-push npm](https://github.com/web-push-libs/web-push) (MPL-2.0) ·
[pywebpush](https://github.com/web-push-libs/pywebpush) (MPL-2.0)

**Relays** — [ntfy publish docs](https://docs.ntfy.sh/publish/) · [ntfy config / self-hosting](https://docs.ntfy.sh/config/) ·
[ntfy known issues](https://docs.ntfy.sh/known-issues/) · [ntfy pricing](https://ntfy.sh/#pricing) ·
[Pushover API](https://pushover.net/api) · [Pushover pricing](https://pushover.net/pricing) ·
[Pushover privacy](https://pushover.net/privacy) · [Pushover blog](https://blog.pushover.net/) ·
[Telegram Bot API](https://core.telegram.org/bots/api) · [Telegram bots FAQ](https://core.telegram.org/bots/faq) ·
[Discord Execute Webhook](https://docs.discord.com/developers/resources/webhook) ·
[Discord mobile notification settings](https://support.discord.com/hc/en-us/articles/218892547) ·
[Gotify](https://gotify.net/docs/pushmsg) · [Bark](https://github.com/Finb/Bark) ·
[Apprise](https://github.com/caronc/apprise) ·
[Home Assistant companion notifications](https://companion.home-assistant.io/docs/notifications/notifications-basic/) ·
[Tailscale webhooks](https://tailscale.com/docs/features/webhooks) ·
[Carrier email-to-SMS gateway shutdowns](https://sigspan.com/carrier-gateway-shutdown)

All URLs accessed 2026-09-03.
