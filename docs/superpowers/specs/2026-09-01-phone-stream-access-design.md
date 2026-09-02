# Phone access to the Stream tab — design (2026-09-01)

**Goal.** View the slicer's Stream tab camera wall on a phone on the same Wi-Fi while the
slicer runs, reached through a QR code. View-only in this phase. The same door is meant to
carry later phases: printer status/control, plate views, slice, send-to-printer, and headless
drivability for AI agents.

## Approach

- **`RemoteAccess`** (`src/slic3r/GUI/RemoteAccess.*`): a boost.asio listener on all
  interfaces, port 13640 (+20 fallback), started only when the user turns on *Phone access*
  in the Stream tab. Every session gets a random 14-character token.
  - `/r/<token>/` serves `stream_center.html` (remote mode) and sets cookie `rt=<token>`.
  - `/r/<token>/state` returns the camera list stripped to `{id, alias, rkind, rname, rurl}`.
  - `/r/<token>/bambu?id=` tunnels to the local BambuCamRelay (MJPEG) after resolving the
    id to IP + access code on the PC.
  - `/r/<token>/ff?id=` runs the Flashforge camera-URL probe on the PC.
  - `/stream.html`, `/video-stream.js`, `/video-rtc.js`, `/api/ws` are tunneled to the
    local go2rtc when the request carries the `rt` cookie. go2rtc's player builds its
    WebSocket URL from `location.origin` + `/api/ws`, so these must live at the root.
  - Only private/loopback IPv4 peers are served. Tunnels are raw TCP splices, so HTTP
    responses and WebSocket upgrades pass unchanged.
- **PC page** pushes its full state (`stream_state:<json>`, including credentials and the
  precomputed go2rtc stream name/source per host) to the app on every save; the app keeps
  it in memory and registers every go2rtc-kind source itself, so the phone only sees opaque
  stream names. Credentials never leave the PC.
- **QR code**: the PC page renders it locally (vendored MIT `qrcode-generator` 1.4.4,
  `resources/web/orca/qrcode.js`), URL `http://<default-route IPv4>:<port>/r/<token>/`. On
  Windows the address comes from the interface owning the best 0.0.0.0/0 route (VPN clients
  normally use 0.0.0.0/1 + 128.0.0.0/1, so the real LAN adapter wins); elsewhere from a UDP
  connect to 8.8.8.8 (no packet). Other addresses are listed under the code.
- **Remembered toggle**: app config `stream_phone_access` / `stream_phone_token`; when left on,
  the same link comes back at the next start. `SNORCA_PHONE_ACCESS=<token>` in the environment
  starts it without touching settings (headless / agent use).
- **Phone page**: same `stream_center.html`, detecting `/r/<token>/` in its path: read-only
  list from `state`, own layout choice (default 1×2), streams via the routes above.
- **Lockdown**: the existing page/auth HTTP servers (13619/13650) now bind 127.0.0.1. They
  were reachable from the LAN and `/localfile/<absolute path>` served any file on the PC.

## Not in this phase

Printer control, plates, slicing, sending; authentication beyond the per-session token;
IPv6; HTTPS. Later JSON routes belong under `/r/<token>/api/...` on the same listener.

## Testing

Page logic in a browser served from 127.0.0.1 with a scratch go2rtc; the listener from this
PC via its LAN address (peer = private IPv4); token/cookie gating by direct requests.
