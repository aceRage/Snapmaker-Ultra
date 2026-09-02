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

## JSON API (phases 1–3 of the remote-control roadmap)

Under `/r/<token>/api`, same listener and token. Every request is marshalled to the GUI thread
(`wxGetApp().CallAfter` + promise, 15 s timeout → 503 when a modal dialog blocks the app).
Manifest at `GET /api`.

| Route | Purpose |
|---|---|
| `GET /api/plates` | project, printer preset, filaments (name/colour), plates: objects, printable/locked, sliced/ready, slicing percent, time (s), filament (mm³, g from `filament_density`) |
| `GET /api/plates/{i}/thumbnail.png` | plate render (`update_all_plate_thumbnails(false)` on the GUI thread, PNG via `compress_thumbnail`) |
| `GET /api/printers` | `DeviceManager` machines (my + local): online/connected, status, percent, time left, layers, bed/nozzle temps, task, selected |
| `POST /api/slice?plate={i}\|all` | selects the plate and posts the same toolbar event as the Slice button; returns a job id; 409 while slicing |
| `GET /api/jobs[/{id}]` | job state (running/done/error/cancelled, percent, text) fed by hooks in `Plater::priv::on_slicing_update` / `on_process_completed` |
| `GET /api/presets` | printer / process choices exactly as the sidebar and Process tab list them (label rows skipped), filament choices, filament slots with colours, dirty flags |
| `POST /api/presets/select?type=printer\|process\|filament&name=…[&index=…]` | `Tab::select_preset(name, force)` (printer, process) or `PresetBundle::set_filament_preset` + the sidebar's follow-ups (filament slot); same name → no-op. Never shows the transfer/discard dialog: unsaved modifications are captured first and re-applied to the new preset (auto-transfer; Revert on the PC still discards them) |
| `POST /api/presets/filament_color?index=…&color=%23RRGGBB` | `PlaterPresetComboBox::ApplyFilamentColor` on that slot |
| `POST /api/presets/filament_add` | `Sidebar::add_filament()` |

The phone page has Streams / Prepare / Devices tabs in remote mode. Prepare mirrors the slicer:
printer dropdown, filament rows (colour swatch + dropdown, "+ Add filament"), process dropdown,
then plate cards (preview, name, estimate, Slice / Re-slice, progress bar). Devices lists the
printers with status, progress and bed/nozzle temperatures.

## Not in this phase

Printer control, plates, slicing, sending; authentication beyond the per-session token;
IPv6; HTTPS. Later JSON routes belong under `/r/<token>/api/...` on the same listener.

## Testing

Page logic in a browser served from 127.0.0.1 with a scratch go2rtc; the listener from this
PC via its LAN address (peer = private IPv4); token/cookie gating by direct requests.
