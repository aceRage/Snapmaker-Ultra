# Phone: pausing, resuming and stopping a print (2026-09-03)

**Goal.** P3 of `2026-09-03-phone-mobile-capabilities-research.md` §3: from the phone's Devices tab
(and for agents through the same JSON API) pause, resume and stop a running print, on a Bambu
printer through the commands the desktop's own buttons send, and on a Snapmaker through Moonraker.
No dialog is shown on the PC. Nothing here can *start* a print — that stays in the Send sheet
(`2026-09-03-phone-send-design.md`), behind its own confirmation.

Branch `feat/phone-print-control`, cut from `feat/ultra-preferences` at `a55aaeb519`.

## 1. What the fork already had (anchors)

- **Bambu**: `MachineObject::command_task_pause()` / `_resume()` / `_abort()`
  (`DeviceManager.cpp:1879`, `:1889`, `:1856`) — each one MQTT publish — and the three predicates
  `can_pause()` (`:2632`, `print_status == RUNNING`), `can_resume()` (`:2625`, `== PAUSE`),
  `can_abort()` (`:2639`, any of PAUSE/RUNNING/SLICING/PREPARE). The desktop's own buttons:
  `StatusPanel::on_subtask_pause_resume` (`StatusPanel.cpp:1943`) flips on `can_resume()` with **no
  confirmation**; `on_subtask_abort` (`:1963`) opens a `SecondaryCheckDialog` first.
- **Snapmaker**: `Moonraker_Mqtt::async_pause_print_job` / `_resume` / `_cancel`
  (`MoonRaker.cpp:1418`, `:1440`, `:1531`) over the MQTT socket the Device tab opens, and the plain
  Moonraker HTTP API the printer serves on port 80 (measured on a U1, firmware 1.5.2, by the
  `feat/phone-snapmaker-lan` work).
- **The phone**: `GET /api/printers` with per-printer live status, polled every 5 s while the
  Devices tab is open (`stream_center.html:1275`); the Send sheet's confirm pattern and its
  H2-series hint; `/api/jobs/{id}` for following work started through the API.

## 2. What was built

`src/slic3r/GUI/RemoteControl.{hpp,cpp}` (new, ~430 lines): `prepare()` on the GUI thread validates
and composes the command, `run()` on a worker thread sends it and watches what the printer then
reports. `RemoteAccess` gained the route and the job plumbing; the hub allow-lists it; the phone
page grew three buttons per printer card and a Stop confirmation.

### The API contract (per instance, `/r/<token>/i/<pid>/api/...`)

**`POST /api/printers/{id}/control`** — form or query body:
`action=pause|resume|stop[&confirm=1][&dry_run=1]`.

`{id}` is a Bambu `dev_id`, or the literal `host` (the printer preset's print host) or `connect`
(the Snapmaker connected on the PC's Device tab) — the same ids `GET /api/printers` reports.

| status | when |
|---|---|
| 400 | no `action`, an action that is not one of the three, or **`stop` without `confirm=1`** |
| 404 | unknown printer |
| 409 | another control command is still running; the printer is offline / has no access code / is not connected; the printer's own state does not allow the action (`can_pause()` and friends for Bambu, Klipper's `print_stats.state` for a Moonraker printer); the address does not answer as a Klipper printer |
| 503 | the GUI thread did not answer in 20 s |
| 200 | `{job, kind, printer, name, action, dry_run}` — one command at a time per instance |

**`GET /api/jobs/{id}`** — `kind: "control"`, `state` running \| done \| error, `text`, `error`,
`printer`, `action`, and `result`:

- Bambu: `call` (`command_task_pause` \| `_resume` \| `_abort`), `command` (the word in the MQTT
  payload), `status_before`, `result_code`, then from watching the printer for up to 10 s
  `printer_state` (`paused` \| `printing` \| `stopped` \| `error` \| `unknown`), `status_after`, and
  `printer_error {code, message}` (HMS text) when it refused;
- Moonraker: `url` (the `printer/print/…` endpoint), `method` (`printer.print.pause` and friends),
  `transport` (`http`, or `mqtt` when the HTTP call failed and the PC's socket was used),
  `reply`, and `http_error` when the printer said no.

**`GET /api/printers`** now also carries, per printer:

- `can_pause`, `can_resume`, `can_stop` — the printer's own predicates, so the page never has to
  re-derive them from a status string;
- `print_status` — the same string `status` has always carried, under the name the desktop's code
  uses;
- `stage` — `MachineObject::get_curr_stage()` for Bambu ("Paused due to filament runout" rather
  than a bare `PAUSE`), the job's file name for a Moonraker printer;
- `print_error` — `{code, message}` with the HMS text, or `null`;
- `hms` — `{count, code, message}`, the summary of what the printer is reporting;
- `status_error` on a print host that did not answer the status probe.

### Decisions

- **One job, not a blocking answer.** The command itself is one publish or one POST, but whether
  the printer *took* it only shows in what it reports over the next few seconds. The route returns
  a job id straight away and the watch runs on its own thread, exactly as a send does — the phone
  polls the job at 1 s and falls back to the 5 s printer poll.
- **`stop` needs `confirm=1`; `pause` and `resume` do not.** Stopping throws the print away; the
  other two are reversible and the desktop does not confirm them either. The page asks first, in
  the Send sheet's own style.
- **The predicates decide, not the phone.** `prepare()` refuses an action the printer's own
  `can_*()` says no to, with the state it reported in the message, and the page dims the button
  with the same reason. A phone and the PC therefore agree about what is possible.
- **A Snapmaker is reached over Moonraker's HTTP API first.** It works for a printer the PC has
  never connected to, it is the path `feat/phone-snapmaker-lan` is building on, and it is what a
  mock can be written against. The Device tab's MQTT socket is the fallback for a `connect`
  printer whose HTTP API refuses — nothing else changes.
- **What a print host speaks is decided by what it answers**, not by the preset: this fork has no
  `host_type` string for Moonraker at all (`s_keys_map_PrintHostType`, `PrintConfig.cpp:80`; only
  the Device tab's connect sets that enum in code). So `/api/printers` probes each print-host
  address once per call with a read-only `printer/objects/query?print_stats`, off the GUI thread,
  and remembers the answer; `prepare()`, which may not touch the network, reads that.
- **The H2 hint is the send feature's.** An H2-series printer that is not in LAN-only mode with
  Developer Mode on refuses third-party commands; the watch reports its HMS text and the page shows
  the same sentence the Send sheet shows.
- **Starting a print stays out.** No control action can start one, and "print a file already on the
  printer" remains out of scope (research §3.1).

### The hub

One case in `instance_api_allowed` (`RemoteHub.cpp`): unlike `/api/plates/<index>/…` and
`/api/jobs/<id>`, the segment here is a **name** (a `dev_id`, `host`, `connect` — and `sm:<id>`
once the LAN work lands), so it is matched as a bounded string of id characters rather than as an
index. The manifest carries the route as well, because `test_hardening.py` compares the two.

### The phone page

Devices tab, each printer card: **Pause**, **Resume** and **Stop**, lit by `can_pause` /
`can_resume` / `can_stop`. A button the printer says no to stays in place, dimmed, with the reason
in its tooltip ("Only a paused print can be resumed (it reports RUNNING)"). Stop opens a
full-screen confirmation in the Send sheet's style — the same CSS, now shared — saying that the
print cannot be resumed; Pause and Resume act straight away. A compact line under the card follows
the command and ends with "Paused." or the printer's own refusal, plus the H2 hint. The card's meta
line now also shows `stage` and `print_error`.

## 3. Safety

- Nothing is shown on the PC; `RemoteControl` opens no dialog and touches neither Tailscale nor the
  hub's state.
- One control command at a time per instance (its own flag, so a send and a control do not block
  each other).
- `dry_run=1`, and the send feature's `SNORCA_SEND_DRYRUN=1`, compose everything and send nothing.
- The status probe is a `GET`; a command is only ever sent by the route, after the predicates.
- The access code never leaves the PC.

## 4. Verified (2026-09-03, gate `snorca_hubtest\test_phone_control.py`, 76 checks, all green)

Against two instances built from this branch on the isolated `dd_ctl` data dir (one of them started
with `SNORCA_SEND_DRYRUN=1`), with `mock_moonraker.py` on 127.0.0.1:18091 registered as the user
preset "Snapmaker U1 (0.4 nozzle) - Mock moonraker":

- **manifest and allow-list**: the route is in both; every manifest route is proxied by the hub
  (`test_hardening.py`'s own probe, reproduced for this build); `GET …/control`, `…/nope`,
  an empty id, a second path segment, a space, `%2F` and a 65-character id are all refused by the
  hub, while `host`, a `dev_id`, `sm:abc123` and `sm%3Aabc123` are proxied;
- **`/api/printers`**: a printing Moonraker printer reports `print_status=printing`,
  `can_pause=true`, `can_resume=false`, `can_stop=true` and its job name as `stage`; a Bambu
  printer carries all seven new fields and `print_status == status`;
- **refusals**: `stop` without `confirm=1` (400), an unknown or missing action (400), an unknown
  printer (404), `connect` with nothing connected (409), resume on a printing printer and all
  three on a standby one (409, naming the state) — and none of them reached the printer;
- **dry runs**: pause and stop name the Moonraker endpoint they would call and send nothing;
- **the four Bambu printers on this LAN** (X1C, O1S, O1D, O1C2, all discovered): every one of the
  twelve commands was refused before anything was composed — "is not connected" for the three the
  PC had not connected to, and "cannot be paused right now (it reports FINISH)" for the connected,
  idle one. Nothing was ever sent to a real printer;
- **the real thing, against the mock**: pause → the printer reports paused and the buttons flip;
  resume → printing; `stop&confirm=1` → cancelled; the mock saw exactly pause, resume, cancel; a
  printer that refuses answers `error` carrying its own reply; a second command while one is in
  flight is refused 409 and never reaches the printer;
- **`SNORCA_SEND_DRYRUN=1`**: on the instance started with it, a plain `action=pause` came back
  `dry_run: true` and nothing reached the printer;
- **the page**: served, posts to the route, adds `confirm=1` for stop only, carries the Stop sheet,
  the predicates, the reasons and the H2 hint.

In a browser at 375×812 against the same hub: the Devices tab showed the four Bambu cards with all
three buttons dimmed and "The PC is not connected to this printer." in their tooltips, and the
Moonraker card with Pause and Stop live and Resume dimmed reading "Only a paused print can be
resumed (it reports printing)."; Pause turned the card into "paused · gate_job.gcode" with a green
"Paused."; Stop opened the confirmation sheet naming the printer, and confirming it left "Stopped."
with every button dimmed and its reason.

`test_phone_send.py` (31 checks) passes unchanged on the same build and data dir. `test_hardening.py`
cannot be pointed at this build (its `EXE` and `DATADIR` are the main checkout and the user's real
data dir), so its section F — the only part this change can break — is reproduced in section A of
the control gate.

## 5. For the user to verify on hardware

1. **Bambu, pause and resume** on a printer the PC is connected to and that is printing: the phone's
   Devices tab should show Pause live and Resume dimmed; Pause should read "Paused." within a few
   seconds and the printer's own screen should agree. Resume takes it back.
2. **Bambu, stop**: the confirmation sheet, then "Stopped.". On an H2-series printer that is not in
   LAN-only mode with Developer Mode on, expect the refusal with the hint instead of silence — and
   note whether pause/stop are refused the same way a print command is (research §5, question 5).
3. **Four printers at once**: check that only the selected printer reports a live `print_status`,
   and whether the other cards show a stale idle (research §5, question 6).
4. **Snapmaker U1**: with the printer printing, the card should offer Pause and Stop from the
   printer's own Moonraker state. Verify the print really pauses, and that `transport` in the job
   result says `http` (the printer's own API) rather than `mqtt`.
5. **A print that is not running**: every button dimmed, with a reason.

## 6. Not done / follow-ups

- Cancelling a control command that is in flight (as with sends).
- `sm:<id>` printers: the id is allow-listed and the Moonraker call is one function keyed on the
  address, but resolving that id needs `feat/phone-snapmaker-lan`'s device list.
- The U1's MQTT fallback is untested: a mock cannot stand in for the printer's certificate.
- The event watcher (P4) that would push a pause to the phone rather than being polled for it.
