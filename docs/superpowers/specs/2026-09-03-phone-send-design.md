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

## 3. Snapmaker over the LAN (2026-09-03) - the primary path

A U1 serves Moonraker's HTTP API on port 80 and, measured on all three of the user's printers,
`GET /access/info` answers `{"login_required": false, "trusted": true}`: `/server/info`,
`/printer/info`, `/machine/system_info`, `/printer/objects/query`, `/server/files/*`,
`/printer/gcode/script` and `/printer/print/start` all answer with no credential at all. So the
phone does not need the PC to be connected to a printer, and there is no connect step: listing,
watching and feeding a printer is plain stateless HTTP from whichever slicer instance the phone is
driving. (Why this replaced the MQTT connect of section 4: that connection succeeded only
sometimes, and it died with the slicer instance that made it, so the whole handshake had to be
redone before every send.)

### 3.1 What the printers answer (measured, 2026-09-03, firmware 1.5.2)

- `GET /machine/system_info` -> `result.system_info.product_info` = `machine_type` ("Snapmaker U1"),
  `serial_number`, `device_name`, `nozzle_diameter[4]`. This is the identity of a printer.
- `GET /printer/objects/query?print_stats&display_status&heater_bed&extruder&extruder1..3&print_task_config`:
  `print_stats.state` (standby | printing | paused | complete | cancelled | error), `.filename`,
  `.info.current_layer` / `.total_layer`, `display_status.progress` (0..1), the temperatures, and
  per toolhead `nozzle_diameter`.
- `print_task_config` carries the four toolheads as parallel arrays - `filament_type`,
  `filament_sub_type`, `filament_vendor`, `filament_color_rgba` ("RRGGBBAA" hex text),
  `filament_color` (ARGB int), `filament_official`, `filament_exist` (is anything loaded) - plus
  `extruder_map_table` (32 entries: the file's filament -> the toolhead that prints it) and
  `extruders_used`. `reprint_info` shows the same pair as the last print used them. This is the
  object the desktop's own `update_filament_info` reads (`SSWCP.cpp:1485`).
- `POST /server/files/upload`, multipart with `file` (plus `print=false` and `root=gcodes`), answers
  201 `{"action": "create_file", "item": {...}, "print_started": false}`;
  `GET /server/files/metadata?filename=` reads it back; `DELETE /server/files/gcodes/<name>` removes
  it. Verified end to end against 10.0.0.108 with a comment-only file, then deleted.
- `filament_detect` is the RFID spool reader: it is blank for third-party spools, so colours come
  from `print_task_config`, not from it.

### 3.2 Starting a print with a toolhead mapping

The U1 has four toolheads, so "which toolhead prints which of the file's filaments" is part of
starting a print. Three sources agree on how it is done, and it is *not* JSON-RPC:

- the fork's own Device page: the shipped Flutter bundle builds
  `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<file filament> MAP_EXTRUDER=<toolhead>` per filament,
  `SET_PRINT_USED_EXTRUDERS EXTRUDERS=<each toolhead once>`, `SET_PRINT_PREFERENCES ...`, and then
  `printer.print.start {filename}`. Its `server.files.start_local_print` path has the routing and
  the enum entry but **no request builder**;
- u1hub (dlgambill/u1hub, read only) sends exactly those macros over
  `POST /printer/gcode/script?script=...` and finishes with `SDCARD_PRINT_FILE`, which is what
  `/printer/print/start` runs;
- the printer itself: `server.files.start_local_print` over `POST /server/jsonrpc` answers
  `{"code": -32601, "message": "Method not found for transport HTTP"}` - it exists only on the
  websocket and MQTT transports.

So this build sends the macros in one `POST /printer/gcode/script`, then
`POST /printer/print/start?filename=`, all over plain HTTP. The upload always carries
`print=false`, so a print that will not start still leaves a usable file on the printer.

**The default mapping** is the nearest loaded colour: greedy over the file's filaments by "redmean"
colour distance, one toolhead per colour, a second filament of exactly the same colour shares that
toolhead, and anything left over falls back to the first loaded toolhead. That is u1hub's rule; the
desktop has none of its own (the Flutter page owns matching there, and `sw_GetFileFilamentMapping`
only reports the file's filaments plus whatever slots the person picked by hand). Material never
decides the match - a difference only earns a warning on the phone.

### 3.3 Where the printers come from

`<datadir>/hub/snapmaker_lan.json` (written atomically, shared by every instance on this PC) holds
`{id, name, model, ip, port, added_by}`, keyed by the printer's serial number when it is known and
its address otherwise. Four sources fill it:

1. **the PC's own paired devices** - `AppConfig::get_devices()`, the Device tab's "My Devices";
2. **mDNS** - one `Bonjour("snapmaker")` pass a minute at most, the same service and TXT keys the
   Device page's own search uses (`SSWCP.cpp:1790`);
3. **the Stream tab's cameras** - `<datadir>/hub/streams.json`; a camera host that answers as a
   Snapmaker is that printer, with the camera's alias as its name (Bambu cameras are left alone;
   those printers arrive through the device manager). Re-checked when that file changes;
4. **an address typed on the phone** - `POST /api/snapmaker/add?ip=`, which only remembers a printer
   that answers.

Each listing probes the printers side by side, with the answer cached for four seconds, so a
printer that is off never holds the list up.

### 3.4 The API

- `GET /api/snapmaker/devices` -> `{devices: [{id, name, model, ip, port, added_by, online,
  login_required, state, printing, task, percent, layer, total_layers, bed_temp, bed_target,
  nozzle_temp, nozzle_target, left_time_s, toolheads: [{index, type, sub_type, vendor, color,
  loaded, official, nozzle}]}], connect: {...}, printer_preset, is_snapmaker}`. `connect` is the
  optional PC-side MQTT connection of section 4.
- `POST /api/snapmaker/add?ip={address}` (accepts `host:port`) -> the identified printer, or 404.
- `POST /api/snapmaker/remove?id={device}` -> 200. A printer that a source keeps finding comes back
  on the next pass; the phone only offers Remove on one that was typed in.
- `GET /api/printers` carries every LAN printer as `sm:{id}`, `kind: "snapmaker"`, with `online`,
  `printing`, `can_upload` / `can_print` (both false when the printer asks for a login), and its
  `toolheads`.
- `POST /api/plates/{i}/send` with `printer=sm:{id}`:
  - `mode=upload` - multipart upload, then `/server/files/metadata` as proof;
  - `mode=print&confirm=1[&mapping=0:2,1:1,...]` - the same upload, then the mapping macros and the
    start. Every filament the file uses needs a toolhead; without `mapping` the colour match above
    is used. `force=1` allows a toolhead that has nothing loaded.
  - 409 when the printer is printing or paused, asks for a login, or is not answering.
  - The job result carries `filaments` (index, colour, type, grams, the toolhead each got),
    `mapping`, `mapping_script`, `uploaded`, `size`, `start`, and `printer_state` - what the printer
    itself reported in the ten seconds after the start.
  - `dry_run=1` stops before the transfer and reports all of the above: that is how the phone asks
    for the proposed mapping before it shows the mapping step.

A printer that is both on the LAN list and connected on the PC appears twice in the send picker -
once as `sm:{id}` (marked *LAN*) and once as `connect` (marked *connected*). They are two different
ways to reach the same machine and either works; the LAN one needs nothing set up.

### 3.5 The phone

The Devices tab lists the LAN printers with their live state, progress, temperatures and a swatch
per toolhead, polled every five seconds like the Bambu cards; below the list an address field adds a
printer, and a printer that was typed in can be removed. The PC-side MQTT connect is a small
secondary button on the printers that support it, marked optional. (Until §9 those printers had a
*Snapmaker* section of their own **and** a card in the printer list: one card each now.)

In the Send sheet, choosing **Upload & print** for a Snapmaker asks the PC for a dry run first and
turns the confirmation into a **mapping step**: one row per filament the file uses (its colour, its
material, its weight) with a toolhead picker showing what each toolhead holds, pre-filled with the
colour match and warning where the material differs or a toolhead is empty. **Start print** then
sends the mapping with the send.

## 4. The PC-side MQTT connect (optional)

The send above only sees a Snapmaker when the PC's Device tab is *connected* to one
(`wxGetApp().get_connect_host`). Until someone picks a device in that page's "My Devices" list the
tab says "Unconnected", and the phone had no way to do the picking, so a phone send never had a
Snapmaker to send to.

### 4.1 What the desktop's pick does (anchors)

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

### 4.2 The headless connect

`src/slic3r/GUI/RemoteSnapmaker.{hpp,cpp}` (new) performs steps 2-5 above without the page:

- `list(json&)` (GUI thread) — the stored devices without anything secret, `is_host`, the current
  printer preset and whether it is a Snapmaker; `probe_online(json&)` (request thread) adds
  `online` from a short TCP probe of each device's MQTT port.
- `connect(dev_id)` (request thread, blocking) — finds the device, fills in its certificate (§4.3),
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

### 4.3 The certificate, and why it is opt-in

`RemoteSnapmaker::remember_credentials(sn, connect_params)` is called from `sw_mqtt_set_engine`
(one line, where the connect parameters are complete) and keeps ca/cert/key/clientId/port for that
printer under `<datadir>/hub/snapmaker_keys.json`, which `connect()` then uses. Because that is a
private key at rest, it is **off unless the person turns it on**: app config `app` /
`snapmaker_remember_keys` = `"1"`. With it off, the phone can still list devices, disconnect, and
use a Snapmaker the PC connected; `connect` answers 409 and says what to do. Deleting the file (or
clearing the setting) takes the phone's connect away again. Nothing in that file is ever sent to
the phone: `/api/snapmaker/devices` reports only `can_connect`.

### 4.4 The API (per instance)

- `GET /api/snapmaker/devices` — `{devices: [{id, name, model, ip, port, sn, preset, nozzles,
  connected, online, is_host, can_connect, link_mode}], connected, host, host_online,
  printer_preset, is_snapmaker, use_new_connect}`.
- `POST /api/snapmaker/connect?id={device}` — 200 (also when it is already the host), 400 without
  an id, 404 unknown device, 409 no certificate (see §4.3), 502 the printer did not accept the
  connection, 504 it accepted but did not answer.
- `POST /api/snapmaker/disconnect` — 200, 409 when nothing is connected.
- After a successful connect, `GET /api/printers` carries the `connect` printer
  (`RemoteSend::list_hosts`) and a plate can be sent to it.

### 4.5 Also on the phone page

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

## 5. Safety rules

- A print never starts without `confirm=1` from the phone, and the page asks the person first.
- One send at a time per instance; the plate must be sliced and printable.
- Nothing is shown on the PC; `RemoteSend` never opens a dialog and never touches Tailscale or
  the hub's state.
- Dry runs (`dry_run=1`, `SNORCA_SEND_DRYRUN=1`) stop before the plugin / HTTP call and are the
  only thing the automated gate sends to a Bambu printer.
- The access code never leaves the PC (`password_set` only).

## 6. Verified (2026-09-03, gate `snorca_hubtest\test_phone_send.py`)

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

### 6.1 Snapmaker over the LAN (gate `snorca_hubtest	est_phone_snapmaker_lan.py`)

All green on an instance over the isolated `dd_lan` data dir, with `mock_printhost.py` standing in
for a U1 (four loaded toolheads, the real printer's own `print_task_config` shape):

- the three routes are in the manifest and the hub's allow-list, and `GET` on the two POST routes is
  not proxied;
- the mock is added by address, identifies itself from `/machine/system_info`, and is listed with
  four toolheads whose colours (`#FEE5A5`, `#E0E0E0`, `#F4C032`, `#000000`), materials, loaded flags
  and nozzle diameters come straight from `print_task_config`; an address with nothing on it is
  refused;
- `GET /api/printers` carries it as `sm:{sn}`, kind `snapmaker`, with its toolheads;
- a dry run reports the file's five filaments with grams and the colour match it proposes, and sends
  nothing;
- **upload**: the mock receives the plate's G-code with `print=false` and `root=gcodes`, the size is
  read back with `/server/files/metadata`, and nothing is started;
- **upload + print**: one `/printer/gcode/script` call carrying
  `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=2` … `SET_PRINT_USED_EXTRUDERS
  EXTRUDERS=2,1,3,0` … `SET_PRINT_PREFERENCES …`, then one `/printer/print/start` for that file, and
  the job waits until the printer reports `printing`;
- refusals: a printer mid-print (409), a toolhead the printer does not have, a malformed mapping, a
  mapping that leaves a filament out (400 each), an unknown printer (404);
- the printer can be removed again.

Against the user's own U1s, read-only: all three answer, 10.0.0.108 is listed with its four
toolheads and colours, `login_required` is false, and the upload contract was proven once with a
comment-only `snorca_phone_test.gcode` (201, `print_started: false`, metadata matched, deleted
again). No print was started on a real printer. In a browser at 375x812 the Devices tab showed the
two printers the Stream tab's cameras contributed with live state, temperatures and a swatch per
toolhead, and the Send sheet's mapping step pre-filled itself from 10.0.0.106's real toolheads
(black, red, blue, purple).

`test_phone_send.py` (31 checks) and the phase-0a security gate pass unchanged on the same build.

### 6.1a The Snapmaker connect (gate `snorca_hubtest\test_phone_snapmaker.py`)

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
connect with `snapmaker_remember_keys` on (§4.3).

## 7. For the user to verify on hardware (start with upload only)

1. **Bambu, upload only**: Prepare → Send → pick the printer (LAN mode) → Upload. The file should
   appear on the printer's SD card / storage under the project name; nothing starts.
2. **Bambu, upload and print** on a printer that accepts third-party commands (X1C in LAN mode, or
   an H2-series printer with LAN-only mode + Developer Mode on): the print should start; the sheet
   should end with "Printing started." within ~12 s. With Developer Mode off on an H2-series
   printer, expect the refusal with the hint instead of silence.
3. **Snapmaker U1 over the LAN, nothing connected**: open the phone's **Devices** tab. The U1s
   should be listed under *Snapmaker* with what they are doing, their temperatures and a swatch per
   toolhead — with no connect anywhere. One that is missing can be added by its address.
4. **Snapmaker U1, upload**: Prepare → Send → pick the printer (marked *LAN*) → Upload. The file
   appears on the printer's own file list under the project name; nothing starts.
5. **Snapmaker U1, upload and print**: Send → the printer → **Upload & print** → check the toolhead
   for each filament (pre-filled by colour; change any row) → **Start print**. The printer should
   begin that file on those toolheads, and the sheet should end with "Printing started."
6. **Optional, the old path**: *Connect on PC* on a printer's card still makes the PC's Device tab
   connect over MQTT (§4), and the picker then also offers it as *connected*. Sending does not
   need it.
7. **A classic print host** (Moonraker/Klipper via HTTP): set `print_host` in the printer preset;
   Upload works; Upload & print is refused for plain Moonraker (needs the PC's preprint page) and
   works for OctoPrint-style hosts.

## 8. Not done / follow-ups

- Snapmaker filament mapping at print start (`server.files.start_local_print` payload).
- Post-processing scripts and the output-name template for classic print hosts.
- Cancelling a running send from the phone.
- "Print all plates" and sending an unsliced plate (slice-then-send in one tap).

## 9. What the hardware pass found (2026-09-04)

Four things about the phone page itself, after Bambu send / print / pause / resume / stop and the
U1 LAN path all worked on real printers.

### 9.1 One card per printer on the Devices tab

A U1 was drawn twice: once by the *Snapmaker* section (state, temperatures, toolheads, Remove,
Connect on PC) and once as its LAN printer card from `/api/printers` (Pause / Resume / Stop). The
second renderer is gone. **`/api/printers` is the source of a card** - it already carried the state,
the job, the temperatures, the toolheads and the control predicates, and now carries the rest of
what the section showed: `ip`, `port`, `added_by`, `layer`, `total_layers`, `left_time_s`
(`SnapmakerLan::list_printers`). `/api/snapmaker/devices` is asked for one thing only: `connect`,
the optional PC-side MQTT connection, whose button the card still offers; adding a printer by
address and forgetting one still go to `/api/snapmaker/add` / `remove`.

So one card carries name and model, state and job, temperatures, a swatch per toolhead with its
material, Pause / Resume / Stop with the result line, Remove (on a printer somebody typed in) and
Connect on PC. The address field sits under the list. The Snapmaker cards are drawn first, where
their section used to be; Bambu and print-host cards are untouched. A Snapmaker the PC is connected
to over MQTT no longer gets a second card as the `connect` printer either - it is the same machine
as its LAN card, whose Disconnect covers it. (The send picker still offers both ways, as §3.4 says.)

### 9.2 The file name a send proposes

The Send sheet's *File name on the printer* was empty for a U1: `upload_name` was only ever set on
the print-host and connected-Snapmaker entries, and it named whichever plate the PC happened to
show. Now **every printer kind that takes a file name reports `upload_name`**, and
`GET /api/printers?plate={index}` names *that* plate: `RemoteSend::export_name_for(plate, ext)`
mirrors `Plater::priv::get_export_gcode_filename` (the project's name, or the first object's when it
was never saved, plus the plate's own name or `_plate_<n>`) without making that plate the current
one on the PC, which a listing must not do. A Snapmaker over the LAN always gets `.gcode` (that is
what `prepare_snapmaker` uploads); a print host gets `.gcode.3mf` when the preset is a Bambu one.
The sheet asks for its own plate, so upload *and* print show the name the send would use, and it
can still be typed over - the send's `name` parameter is unchanged, and so is what happens when it
is left empty. Bambu printers have no file-name field (their sheet has the options instead) and are
unchanged.

### 9.3 The toolhead chooser

The mapping step's per-filament `<select>` could only spell colours as hex. It is now a row of
tappable chips, one per toolhead, radio-like: colour swatch, toolhead number, and the loaded
material (type and sub type) or *empty* in italics for a toolhead with nothing in it. The
colour-matched default is the selected chip, an empty toolhead that is chosen is outlined in the
warning colour, and the material / empty warnings under the row are as they were. The mapping sent
is unchanged (`mapping=0:2,1:1,…`).
