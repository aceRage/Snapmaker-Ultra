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
| `GET /api/settings/process` | the Process tab as the slicer builds it (`Tab::get_pages()` → option groups → lines → options) with each option's definition (label, tooltip, type, unit, mode, min/max, enum values), current and last-saved value and dirty flag; plus preset name, is_system, app mode |
| `POST /api/settings/process` (form `key=value…`) | `set_deserialize_strict` per key → `Tab::load_config` + `Plater::on_config_change`, i.e. the same effect as typing into the tab; 400 on a bad value, 404 on an unknown key |
| `POST /api/settings/process/revert` | `Tab::on_roll_back_value(false)` — reset all settings to the last saved preset |
| `POST /api/settings/process/save` | `Tab::save_preset(name)` under the same name; a system preset saves to the fork's `<name> - Custom` shadow preset (no name prompt) |

The phone's Prepare tab shows the project file name above the printer, and an **Edit** button next
to the process dropdown opens a full-screen editor: preset name with modified count, mode selector
(Simple / Advanced / Expert), Revert, Save, the tab's pages as tabs, groups as collapsible sections
(state remembered per group), and the slicer's lines with bool / enum / text controls; changed
options are marked and applied on change.

The phone page has Streams / Prepare / Devices tabs in remote mode. Prepare mirrors the slicer:
printer dropdown, filament rows (colour swatch + dropdown, "+ Add filament"), process dropdown,
then plate cards (preview, name, estimate, Slice / Re-slice, progress bar). Devices lists the
printers with status, progress and bed/nozzle temperatures.

## The hub: one permanent server, many slicer instances (2026-09-02)

Phone access moved out of the slicer window into a **hub process**: the same executable
started as `snapmaker-orca --hub [--datadir …] [--hub-token …] [--hub-phone]`
(`src/slic3r/GUI/RemoteHub.*`, wx-free, entered from `CLI::run` before any wxApp exists).
The first slicer instance that needs it spawns the hub detached (`CreateProcess` with
`CREATE_BREAKAWAY_FROM_JOB`; double fork + `setsid` elsewhere), so the hub — and the camera
streams — survive closing the slicer. A separate launcher (tray / service) can start it later
without any slicer at all.

- **Owns**: the token-gated listener on 13640 (bound to all interfaces while phone access is
  on, loopback only otherwise), go2rtc (spawned into a kill-on-close Job Object so it dies
  with the hub), the Bambu MJPEG relay, the Flashforge probe, and the camera list (persisted in
  `<datadir>/hub/streams.json`, re-registered in go2rtc at start). `<datadir>/hub/hub.json`
  holds pid, port, token, phone flag and version; the log is `<datadir>/log/hub.log.0`.
- **Instances**: every slicer instance starts a loopback-only API on an OS-picked port
  (`RemoteAccess`) and drops `<datadir>/hub/instances/<pid>.json`. The hub scans that
  directory, drops dead pids, probes `GET /api/info` (pid, project title/path, slicing flag —
  no GUI thread involved) and proxies `/r/<token>/i/<pid>/api/...` to `/api/...` on the
  instance as a raw splice. Ids are pids; the phone shows a 1-based index.
- **Uploads**: `POST /r/<token>/api/instances/open` (body = the file, `X-File-Name` = its
  name; .3mf/.stl/.obj/.step only, streamed to `<datadir>/hub/uploads`) starts a new instance
  with the file (`SNORCA_NEW_INSTANCE=1` bypasses the single-instance preference).
  `POST /r/<token>/i/<pid>/open?mode=load|import` opens it in an instance instead:
  *load* (.3mf, the default for projects) first saves the project on screen — to its own file,
  or to `<datadir>/hub/saves/Untitled_<stamp>.3mf` if it was never saved — then
  `Plater::load_project(file, "<loadall>")`; *import* adds the model to the current plate
  (`load_files(LoadModel)`). Only files under the uploads folder are accepted by the instance.
- **Prompts** during those operations answer themselves: `MessageDialog` /
  `RichMessageDialog` say Yes/OK, `UnsavedChangesDialog` transfers where offered and discards
  otherwise (the previous project was just saved with the modifications), and `show_error` /
  `ErrorDialog` (e.g. "Loading of model file failed.") is not shown at all: the text is logged
  and returned in the request's error (`nothing was imported: Loading of model file failed.`),
  so a modal never blocks later requests.
- **Slicer side**: the Stream tab asks the hub for relay ports on demand (`hub_start` →
  `__hubReady(go2rtc, relay)`), forwards the camera list (`POST /hub/state`), and toggles
  phone access (`POST /hub/phone?on=1&token=…`); `GET /hub/info` feeds the QR modal. The
  remembered toggle (`stream_phone_access` / token) and `SNORCA_PHONE_ACCESS` now bring the
  hub up with phone access on. The hub exits by itself once phone access is off and no
  instance has been alive for a minute; a version mismatch makes the slicer restart it.
- **Phone page**: the Prepare tab starts with a *Slicer* dropdown (index · project title) and
  a file picker with **Load/Import** (into that slicer) and **New** (spawn); with no instance
  open only the picker and **Open** remain. Uploads show progress; "New" polls the instance
  list until the new window registers and selects it. Streams come from the hub regardless
  of instances; Devices uses the selected instance.

## Not in this phase

Printer control, sending; authentication beyond the per-session token; IPv6; HTTPS; a
standalone hub launcher (tray / service); STEP import prompts (its mesh-parameter dialog is
not auto-answered).

## Testing

Page logic in a browser served from 127.0.0.1 with a scratch go2rtc; the listener from this
PC via its LAN address (peer = private IPv4); token/cookie gating by direct requests.
