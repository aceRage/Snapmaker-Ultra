# Phone: sending a sliced plate to a printer (2026-09-03)

**Goal.** From the phone page (and for agents through the same JSON API), send a plate that the
slicer has sliced to a printer in both forms the desktop offers: *upload* (put the file on the
printer or print host without starting it) and *upload and print* (start it, after an explicit
confirmation). No dialog is shown on the PC; what reaches the printer is what the desktop's own
dialogs would have sent.

## 1. How the desktop sends today (anchors)

Three paths, chosen by the printer preset (`Plater::priv::on_action_print_plate`,
`src/slic3r/GUI/Plater.cpp` ~16162; `PresetBundle::use_bbl_network()`,
`src/libslic3r/PresetBundle.cpp:553`):

1. **Bambu printers** (a BBL-vendor preset without `bbl_use_printhost`): `SelectMachineDialog`
   (`src/slic3r/GUI/SelectMachine.cpp`, send at `on_send_print` ~2018) for *print*, and
   `SendToPrinterDialog` (`src/slic3r/GUI/SendToPrinter.cpp` ~740) for *upload to the printer's
   storage*. Both first export the plate: `Plater::send_gcode(plate_idx)` (`Plater.cpp:22302`)
   writes the **gcode 3mf** (`SaveStrategy::Silence | SkipModel | WithGcode | SkipAuxiliary`) to
   `plate->get_tmp_gcode_path()` with a `.3mf` extension; a cloud (non LAN-only) printer also gets
   `Plater::export_config_3mf(plate_idx)` (`Plater.cpp:22336`, `WithSliceInfo`). The paths come
   back through `Plater::get_print_job_data`. Then a `PrintJob` (`Jobs/PrintJob.cpp`) or
   `SendJob` (`Jobs/SendJob.cpp`) fills `BBL::PrintParams` (`src/slic3r/Utils/bambu_networking.hpp`)
   and calls the network plugin through `NetworkAgent`:
   - print, LAN printer: a tiny `start_send_gcode_to_sdcard` of `resources/check_access_code.txt`
     as `verify_job` proves IP + access code (`PrintJob.cpp:205-221`), then `start_local_print`
     (`PrintJob.cpp:564`), refused without an SD card;
   - print, cloud printer: `start_local_print_with_record` with a fallback to `start_print`
     (`PrintJob.cpp:532-561`), or `start_print` directly; `lan_mode_only` forces the local call;
   - upload: `start_send_gcode_to_sdcard` (`SendJob.cpp:277/295`), refused without an SD card.
   The dialog's inputs: dev_id/ip/access code/ftp folder/ssl flags/connection type from the
   `MachineObject`; project name = the plate's export name (or the objects' names when untitled,
   `SelectMachine.cpp:3168-3198`); checkboxes remembered in `AppConfig` section `print`
   (`bed_leveling`, `flow_cali`, `timelapse`, on unless "0", `SelectMachine.cpp:3234`); vibration
   calibration hard-coded `false`, layer inspection `true` (`SelectMachine.cpp:2183-2192`); AMS
   mapping from `MachineObject::ams_filament_mapping` turned into three JSON strings
   (`do_ams_mapping` / `get_ams_mapping_result`, `SelectMachine.cpp:1059-1205`), nozzle info for
   two-nozzle printers (`build_nozzles_info`, `:1207`); the printer is made the selected one when
   picked (`on_selection_changed`, `:2659-2666`), which connects to it.
2. **Print hosts** (OctoPrint, Moonraker, Duet, …): `Plater::send_gcode_legacy`
   (`Plater.cpp:22073`) → `PrintHostSendDialog` → `PrintHost::upload(PrintHostUpload{source_path,
   upload_path, post_action None|StartPrint, …})` (`src/slic3r/Utils/PrintHost.hpp:61`) through
   the `PrintHostJobQueue`. The file is the sliced G-code (post-processed by
   `BackgroundSlicingProcess::prepare_upload`), or the gcode 3mf when a Bambu printer sits behind a
   third-party host (`use_3mf`). The host class comes from the preset's `host_type`
   (`PrintHost::get_print_host`, `PrintHost.cpp:41`); `print_host` must be set.
3. **Snapmaker U1**: detected by `printer_model` containing "Snapmaker" and "U1"
   (`Plater.cpp:22144`); the desktop opens the Flutter `WebPreprintDialog` with the plate's temp
   G-code, and that page drives a `Moonraker_Mqtt` host the Device tab connected
   (`wxGetApp().get_connect_host`, built in `SSWCP_MqttAgent_Instance::sw_mqtt_set_engine`,
   `SSWCP.cpp:6433`): the file goes up as a Moonraker `/server/files/upload` POST
   (`Moonraker::upload`, `MoonRaker.cpp:435`, `print=false|true`) and a print starts over MQTT
   (`sw_MachinePrintStart` → `async_start_print_job` → `printer.print.start`,
   `MoonRaker.cpp:1371`). Note `Moonraker::upload` with `StartPrint` blocks on that page
   (`MoonRaker.cpp:440-468`), so a headless print must upload with `print=false` and start over MQTT.

## 2. What was built

`src/slic3r/GUI/RemoteSend.{hpp,cpp}` (new): `prepare()` on the GUI thread reproduces the dialog
inputs above and exports the plate file; `run()` on a worker thread performs the transfer and
reports progress; nothing is shown on the PC. `RemoteAccess` (the per-instance API) gained the
route, the job plumbing and the printer capabilities; the hub allow-lists the route; the phone page
gained a Send sheet.

### The API contract (per instance, `/r/<token>/i/<pid>/api/...`)

- `GET /api/printers` — every entry now carries `kind` (`bambu` | `printhost` | `connect`),
  `can_upload`, `can_print`, and:
  - Bambu: `lan_mode`, `access_code_set`, `sdcard`, `has_ams`, `ams_mapping`,
    `model_matches` + `profile_model` (the sliced profile's model vs the printer's report, with the
    P1P/P1S kit rule), `options` = the desktop's remembered defaults `{bed_leveling, flow_cali,
    timelapse, vibration_cali: false, use_ams}`;
  - `id: "host"` — the printer preset's print host when `print_host` is set and the preset does not
    use the Bambu network: `name` (host class + address), `url`, `upload_name` (the export name);
  - `id: "connect"` — the Snapmaker (Moonraker over MQTT) connected on the PC's Device tab.
- `POST /api/plates/{index}/send` (form body): `printer={id}&mode=upload|print[&confirm=1]
  [&force=1][&dry_run=1][&bed_leveling=0|1&flow_cali=0|1&timelapse=0|1&vibration_cali=0|1
  &use_ams=0|1][&name=]`.
  - 400: bad mode, `print` without `confirm=1`, no printer. 404: unknown printer / plate.
  - 409: the plate is not sliced (or not printable), the slicer is slicing, another send is running,
    the printer is offline / has no access code / is printing, the sliced profile's model differs
    from the printer's (send `force=1` to proceed, as the desktop's "continue" would), or the
    desktop's own preconditions (no SD card for a LAN send; a print host without `print_host`;
    a plain Moonraker host asked to print, which only the PC's preprint page can do).
  - 200: `{job, plate, kind, printer, mode, dry_run}`; one send at a time.
  - Options default to the desktop's remembered checkboxes; `name` is the file name on a print host
    (basename only; default the export name). `dry_run=1`, or `SNORCA_SEND_DRYRUN=1` in the
    instance's environment, does everything up to the transfer and reports the composed parameters.
- `GET /api/jobs/{id}` — `kind: "send"`, `state` running | done | error, `percent`, `text` (the
  plugin's stage or the upload percentage), `error`, `printer`, `mode`, `result`:
  - Bambu: `call` (the `NetworkAgent` function the desktop would use), `params` (every
    `PrintParams` field but the access code, which is reported as `password_set`), `result_code`,
    and for a print `printer_state` `printing` | `error` | `unknown` from watching the printer for
    12 s after the command left, with `printer_error {code, message}` (HMS text) when it refused;
  - print host: `host`, `url`, `source`, `upload_path`, `post_action`, `two_step`, `uploaded`,
    `start_reply` (the MQTT answer to `printer.print.start` for the connected Snapmaker).

### Decisions

- **One code path for the real send and the dry run.** `RemoteSend` composes the parameters itself
  (mirroring `PrintJob::process` / `SendJob::process` line by line) instead of running the
  `PrintJob` / `SendJob` classes: those report through `ctl.show_error_info` and the plater's
  error dialogs, and they cannot stop before the plugin call. The composed `PrintParams` is what a
  dry run returns, so the gate asserts exactly what a real send would carry.
- **Printer selection first, then a pause.** Picking a Bambu printer other than the selected one
  connects to it (as the dialog does); its SD-card and busy state arrive with the first push. The
  request thread waits for `is_connected()` + the SD state (≤ 15 s) before preparing, so a send to
  a printer the PC was not connected to is not refused for a card it has.
- **Upload+print on Moonraker hosts is two steps** (upload with `print=false`, then
  `printer.print.start` over MQTT) because `Moonraker::upload` with `StartPrint` opens the PC's
  preprint page. A plain HTTP Moonraker host (no MQTT) therefore refuses `mode=print`; the
  connected Snapmaker (`Moonraker_Mqtt`) and OctoPrint-style hosts (`print=true` in the upload)
  work in one go. The Snapmaker filament-mapping payload of the preprint page
  (`server.files.start_local_print`) is not reproduced: the print starts with the mapping as sliced.
- **Print-host file**: the plate's sliced G-code as the desktop's Snapmaker path uploads it (no
  post-processing scripts, no output-name template beyond the export name); the gcode 3mf when the
  preset is a Bambu one behind a third-party host, like `on_action_print_plate`.
- **What is exposed**: bed levelling, flow calibration, timelapse, use AMS (Bambu); vibration
  calibration is accepted but defaults to the desktop's fixed `false`; layer inspection stays `true`.
- **The model mismatch is a refusal, not a warning**: the desktop lists it in a confirmation the
  user reads; the phone gets a 409 with the two models and an "anyway" that adds `force=1`.
- **Confirmation lives on the phone**: `mode=print` needs `confirm=1`; the page asks first.
- **After a Bambu print command**, the job watches the printer's `print_error` / printing state
  for 12 s and fails with the HMS text when the printer refuses. That is where an H2-series
  printer's "command verification failed" (LAN-only mode with Developer Mode off) surfaces; the
  phone adds the hint.

### The phone page

Prepare tab, plate cards: a **Send** button next to Slice / Re-slice once the plate is sliced and
printable. It opens a full-screen sheet: the plate (time, filament, slicer), a printer picker
(online first, remembered per phone), the printer's notes (LAN / cloud, missing access code, no SD
card, busy, the model mismatch), the options (Bambu) or the file name (print host), then **Upload**
and **Upload & print**; the latter asks once more ("Start printing … now?") before it starts.
Progress and text come from the job every second; success and errors are shown, with the LAN-only
+ Developer Mode hint when a Bambu printer refuses a print. The sheet can be hidden while a
transfer runs; the transfer continues on the PC.

## 3. Connecting the Snapmaker from the phone (2026-09-03)

The send above only sees a Snapmaker when the PC's Device tab is *connected* to one
(`wxGetApp().get_connect_host`). Until someone picks a device in that page's "My Devices" list the
tab says "Unconnected", and the phone had no way to do the picking, so a phone send never had a
Snapmaker to send to.

### 3.1 What the desktop's pick does (anchors)

The Device tab is the Flutter page (`resources/web/flutter_web`, loaded as
`…/web/flutter_web/index.html?path=2`), talking to `src/slic3r/GUI/SSWCP.cpp`:

1. **The list** — `SSWCP_MachineManage_Instance::sw_GetLocalDevices` (`SSWCP.cpp:5797`) returns
   `wxGetApp().app_config->get_devices()` unchanged: `DeviceInfo` (`libslic3r/AppConfig.hpp:33`)
   with ip, dev_id, dev_name, model_name, preset_name, connected, nozzle_sizes, sn, protocol,
   api_key, user, password, ca, cert, key, clientId, port, link_mode, userid, id. There is no
   discovery here; the list is what pairing wrote. `connected` is session state — `AppConfig::save`
   writes it as `false` every time and drops `link_mode == "wan"` devices (`AppConfig.cpp:988-1000`).
   The page keeps itself up to date through `sw_SubscribeLocalDevices` (`:5808`), which the app
   pushes to with `GUI_App::device_card_notify` (`GUI_App.cpp:7336`, the
   `m_device_card_subscribers` map, one entry per webview) alongside a legacy
   `window.postMessage({command:"local_devices_arrived"…})`.
2. **The socket** — `sw_create_mqtt_client` (`:6100`) builds a `MqttClient`
   (`src/slic3r/Utils/MQTT.hpp:51`) from the page's own parameters: `server_address`
   (`mqtts://ip:port` when it holds ca/cert/key, `mqtt://ip:port` otherwise), `clientId`, ca, cert,
   key, username, password, clean_session; it is kept in `m_mqtt_engine_map`, keyed by the
   *webview*. `sw_mqtt_connect` (`:6185`) then calls `MqttClient::Connect` on a worker thread (20 s
   cap), and `sw_mqtt_subscribe` (`:6343`) subscribes `<sn>/response` and `<sn>/notification`.
3. **The host** — `sw_mqtt_set_engine` (`:6433`) copies the printer preset's config, sets
   `host_type = htMoonRaker_mqtt` and `print_host = ip:port`, builds the host with
   `PrintHost::get_print_host` (a `Moonraker_Mqtt`), and stores it with
   `wxGetApp().set_connect_host(host)` + `set_host_config(config)` (`GUI_App.cpp:2446`,
   `GUI_App.hpp:391`). It mirrors sn / ca / cert / key / user / password / port / clientId onto the
   host, hands the live socket over with `Moonraker_Mqtt::set_engine` (`MoonRaker.cpp:941`) and
   registers `set_connection_lost`. Note it never calls `PrintHost::connect`: the page already
   opened the socket, so `Moonraker_Mqtt::connect` (`MoonRaker.cpp:1111`) is dead code on this path.
4. **Proof and bookkeeping** — a worker thread clears `connected` on the previously connected
   device, asks the printer who it is with `SSWCP::query_machine_info` (`SSWCP.cpp:7489` →
   `machine.system_info` over MQTT), and on the GUI thread saves the `DeviceInfo` with
   `connected = true`, the model, the nozzle sizes and `preset_name` (`:6677-6789`).
5. **What the PC then shows** (`:6908-6951`): the card push above, `use_new_connect = "true"`,
   `sidebar().update_all_preset_comboboxes(reload)`, `mainframe->m_print_enable = true`,
   `update_slice_print_status(eEventPlateUpdate)`, and a reload of the Device tab's page unless it
   is already the Snapmaker one (`m_printer_view->isSnapmakerPage()`). That last check exists
   because `update_all_preset_comboboxes` only loads that URL the first time (its `is_sm_page`
   latch, `Plater.cpp:3663-3717`).
6. **Disconnect** — `sw_Disconnect` (`:4121`) → `GUI_App::sm_disconnect_current_machine`
   (`GUI_App.cpp:7674`): `host->disconnect`, `use_new_connect = "false"`, clear `connected`, notify
   the cards, `update_all_preset_comboboxes(need_reload)`, `set_connect_host(nullptr)`,
   `machine_filaments.clear()`, then `clear_filament_extruder_map()` and `load_current_presets()`.
7. **Nozzles** — connecting does *not* switch the printer preset by itself. The sidebar's
   "synchronise nozzle information" button does (`Plater.cpp:2136-2274`): `query_machine_info`, then
   `get_similar_printer_preset({}, diameter)`, `Tab::select_preset(name, false, "", true)`,
   `update_all_preset_comboboxes(true)`, `update_nozzle_settings(true)`.

**The credential finding.** The printer answers only a client it has issued a certificate to: its
plain MQTT port (1884) accepts a connection and a publish but never replies, and its MQTTS port
(8883) refuses a client without a certificate (measured against three U1s on the LAN). The
certificate comes from the pairing exchange (`Moonraker_Mqtt::ask_for_tls_info`, `MoonRaker.cpp:1000`
— `server.request_key` under the PIN's topic), and this fork stores it **only in the Flutter page's
own storage**: the success path deliberately blanks it before saving (`SSWCP.cpp:6716-6718`,
`info.ca = /* auth_info["ca"] */ ""`). So AppConfig alone is not enough to reconnect a device.

### 3.2 The headless connect

`src/slic3r/GUI/RemoteSnapmaker.{hpp,cpp}` (new) performs steps 2-5 above without the page:

- `list(json&)` (GUI thread) — the stored devices without anything secret, `is_host`, the current
  printer preset and whether it is a Snapmaker; `probe_online(json&)` (request thread) adds
  `online` from a short TCP probe of each device's MQTT port.
- `connect(dev_id)` (request thread, blocking) — finds the device, fills in its certificate (§3.3),
  builds the `Moonraker_Mqtt` host exactly as `sw_mqtt_set_engine` does, opens its own
  `mqtts://ip:port` `MqttClient`, hands it over with `set_engine`, subscribes the printer's topics
  (new helper `Moonraker_Mqtt::subscribe_device_topics`, the block `connect()` runs after its own
  `Connect`), registers a connection-lost handler that tears the host down without putting a modal
  on the PC, proves the printer answers with `SSWCP::query_machine_info` (10 s), then on the GUI
  thread saves the device as connected and runs the desktop's own follow-up: the card push,
  `use_new_connect`, `update_all_preset_comboboxes(true)`, the print button, and the Device tab's
  page reload.
- `disconnect()` — `GUI_App::sm_disconnect_current_machine(true)` plus the extra cleanup
  `sw_Disconnect` does.
- No pairing: a device with no certificate is refused with a 409 that says to connect it once on
  the PC. No dialog is ever shown, and nothing here starts a print.

### 3.3 The certificate, and why it is opt-in

`RemoteSnapmaker::remember_credentials(sn, connect_params)` is called from `sw_mqtt_set_engine`
(one line, where the connect parameters are complete) and keeps ca/cert/key/clientId/port for that
printer under `<datadir>/hub/snapmaker_keys.json`, which `connect()` then uses. Because that is a
private key at rest, it is **off unless the person turns it on**: app config `app` /
`snapmaker_remember_keys` = `"1"`. With it off, the phone can still list devices, disconnect, and
use a Snapmaker the PC connected; `connect` answers 409 and says what to do. Deleting the file (or
clearing the setting) takes the phone's connect away again. Nothing in that file is ever sent to
the phone: `/api/snapmaker/devices` reports only `can_connect`.

### 3.4 The API (per instance)

- `GET /api/snapmaker/devices` — `{devices: [{id, name, model, ip, port, sn, preset, nozzles,
  connected, online, is_host, can_connect, link_mode}], connected, host, host_online,
  printer_preset, is_snapmaker, use_new_connect}`.
- `POST /api/snapmaker/connect?id={device}` — 200 (also when it is already the host), 400 without
  an id, 404 unknown device, 409 no certificate (see §3.3), 502 the printer did not accept the
  connection, 504 it accepted but did not answer.
- `POST /api/snapmaker/disconnect` — 200, 409 when nothing is connected.
- After a successful connect, `GET /api/printers` carries the `connect` printer
  (`RemoteSend::list_hosts`) and a plate can be sent to it.

### 3.5 Also on the phone page

- **Devices tab**: a *Snapmaker* section above the Bambu list — one card per paired device (name,
  model, state, ip, nozzles) with **Connect**, and **Disconnect** on the connected one; the 5 s
  poll keeps it current. With nothing paired it says to pair on the PC first.
- **Prepare tab**: a **nozzle dropdown** beside the printer dropdown, hidden when the printer model
  has one variant. It lists `PresetCollection::diameters_of_selected_printer()` and shows
  `printer_variant`, both new in `GET /api/presets` as `nozzles: {choices, current}`;
  `POST /api/presets/select?type=nozzle&name={diameter}` does what the sidebar's combo does —
  `get_similar_printer_preset({}, diameter)`, `select_preset(…, force)`,
  `update_all_preset_comboboxes(true)`, `update_nozzle_settings(true)` — and refuses a diameter the
  model does not have (`get_similar_printer_preset` otherwise falls back to an arbitrary preset).
- The **printer** dropdown now runs those same two refreshes, so switching the printer from the
  phone updates the PC's nozzle box and brings its Device page up the way the sidebar does.
- The *Show on PC* / *Hide on PC* button is now an **eye icon** at the end of the slicer row: open
  when the window is on the PC, struck through when it is hidden. The tooltip keeps the old wording.

## 4. Safety rules

- A print never starts without `confirm=1` from the phone, and the page asks the person first.
- One send at a time per instance; the plate must be sliced and printable.
- Nothing is shown on the PC; `RemoteSend` never opens a dialog and never touches Tailscale or
  the hub's state.
- Dry runs (`dry_run=1`, `SNORCA_SEND_DRYRUN=1`) stop before the plugin / HTTP call and are the
  only thing the automated gate sends to a Bambu printer.
- The access code never leaves the PC (`password_set` only).

## 5. Verified (2026-09-03, gate `snorca_hubtest\test_phone_send.py`)

Against an instance on the isolated `dd_phone` data dir with `mock_printhost.py` on
127.0.0.1:18089 registered as the user preset "Snapmaker U1 (0.4 nozzle) - Mock host"
(`host_type` octoprint):
- manifest and hub allow-list carry `POST /api/plates/{index}/send`;
- refusals: print without confirm (400), bad mode (400), unknown printer (404), unsliced plate
  (409), missing plate (404), model mismatch without force (409 naming both models);
- print host: dry run (nothing reaches the mock), upload (mock receives the file with
  `print=false`), upload+print (`print=true`, same size); the phone page's own flow did the same
  in a browser (Send → pick the host → Upload & print → Start print → "sent");
- Bambu (four printers discovered on the LAN: X1C, H2S, H2D, H2C): upload dry run composes
  `start_send_gcode_to_sdcard` with dev_id, `bblp`, the access code set, plate 1, the gcode 3mf on
  disk and `<name>.gcode.3mf`; print dry run composes `start_local_print` for the LAN printer with
  the flags as requested (`bed_leveling=0 flow_cali=1 timelapse=0`), layer inspect on, vibration
  off, the plate's bed type and `<name>_plate_1` as preset name; no dry run reached the mock.

### 5.1 The Snapmaker connect (gate `snorca_hubtest\test_phone_snapmaker.py`)

Against an instance on the isolated `dd_sm` data dir (three U1s seeded from what the printers
themselves report, `seed_sm_device.py`), all green:
- the three routes are in the manifest and in the hub's allow-list, and nothing near them is
  proxied (`GET …/connect`, `POST …/devices`, `…/nope`);
- `GET /api/snapmaker/devices` lists the three printers with model, address, nozzles and `online`,
  and carries no certificate, key or password;
- refusals: unknown device (404), no id (400), and a device with no certificate (409 saying to
  connect it once on the PC);
- with a stand-in certificate in the store the device reports `can_connect` and its stored port,
  and a connect that the printer cannot accept ends as a clean 502 with no host left behind;
- nozzles: `choices` and `current` are listed, a switch moves `printer_variant`, the printer preset
  and the plate's printer name, switching back restores it, and a diameter the model does not have
  is refused (404);
- the page: the Snapmaker section, the eye icon (with the old wording as its tooltip) and the
  nozzle dropdown are all served. In a browser at 375×812 the Devices tab showed the three
  printers with Connect, the eye toggled the PC window off and on (the icon and tooltip following),
  and picking 0.6 mm moved the printer preset to "Snapmaker U1 (0.6 nozzle)".

`test_phone_send.py` (31 checks) and the phase-0a security gate pass unchanged on the same build.
What the gate cannot do is connect to a printer: the certificate for one only exists after a PC
connect with `snapmaker_remember_keys` on (§3.3).

## 6. For the user to verify on hardware (start with upload only)

1. **Bambu, upload only**: Prepare → Send → pick the printer (LAN mode) → Upload. The file should
   appear on the printer's SD card / storage under the project name; nothing starts.
2. **Bambu, upload and print** on a printer that accepts third-party commands (X1C in LAN mode, or
   an H2-series printer with LAN-only mode + Developer Mode on): the print should start; the sheet
   should end with "Printing started." within ~12 s. With Developer Mode off on an H2-series
   printer, expect the refusal with the hint instead of silence.
3. **Snapmaker U1, connecting from the phone**: set `snapmaker_remember_keys` to `1` in the app
   settings, then connect the U1 once on the PC's Device tab as usual — that connect leaves its
   certificate behind. Restart the slicer (the Device tab says "Unconnected" again), open the
   phone's **Devices** tab: the printer is listed under *Snapmaker* as *reachable*; tap **Connect**.
   Within a few seconds it should read *connected*, the PC's Device tab should show the same
   printer connected, and the phone's Send picker should offer "Snapmaker …". **Disconnect** takes
   it back down.
4. **Snapmaker U1, sending**: with it connected (from the phone or the PC), Upload, then
   Upload & print. The print starts with the filaments as sliced (no mapping page).
5. **A classic print host** (Moonraker/Klipper via HTTP): set `print_host` in the printer preset;
   Upload works; Upload & print is refused for plain Moonraker (needs the PC's preprint page) and
   works for OctoPrint-style hosts.

## 7. Not done / follow-ups

- Snapmaker filament mapping at print start (`server.files.start_local_print` payload).
- Post-processing scripts and the output-name template for classic print hosts.
- Cancelling a running send from the phone.
- "Print all plates" and sending an unsliced plate (slice-then-send in one tap).
