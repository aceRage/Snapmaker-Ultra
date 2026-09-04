# Phone: the printer event watcher (2026-09-04)

**Goal.** P4 of `2026-09-03-phone-mobile-capabilities-research.md` §4, built to §2.2's W1+W2 shape:
the slicer instance watches the printers it can already see and emits an event on a *transition*;
the hub takes those events, keeps them, shows a tray balloon and serves them to the phone. P5
(relay notifications) hangs off one function in the hub and nothing else.

Branch `feat/phone-events`, cut from `feat/ultra-preferences` at `b29202e160`.

## 1. Where the parts live

| Part | File | Why there |
|---|---|---|
| the watcher | `src/slic3r/GUI/RemoteEvents.{hpp,cpp}` (new) | the instance is the only place printer state already exists: a Bambu `MachineObject` fed by the plugin's MQTT push, a Snapmaker answering Moonraker over the LAN, a print host probed by the same code `/api/printers` uses |
| the tick | `RemoteAccess.cpp`, `GuiHeartbeat::Notify` | the Bambu half must run on the GUI thread anyway, and that timer is already there |
| this instance's own ring | `GET /api/events?since=` | debugging one printer, and the manifest / allow-list pair `test_hardening.py` enforces |
| the transition rule under test | `POST /api/debug/events` (`SNORCA_DEBUG_ROUTES=1`) | pure function in, events out; no printer, no clock, no network |
| delivery | `RemoteHub.cpp`: `POST /hub/event`, `HubServer::accept_event`, the ring, `events.json`, the balloon, `GET /hub/events`, `GET /r/<token>/events` | the hub outlives every instance and merges all of them |
| the phone | `resources/web/orca/stream_center.html` | a badge on the Devices tab and the event list at the top of it |

## 2. The event contract (shared with P5)

```json
{ "id": 7, "time": 1757001234567, "instance": 21044,
  "printer": { "id": "sm:8110025111600047BIY2", "name": "U1", "kind": "snapmaker" },
  "kind": "started", "severity": "info",
  "title": "U1 started printing", "text": "U1 started a print • cube.gcode.",
  "code": "0300020003", "job": "cube.gcode" }
```

`kind` is one of `started | finished | failed | cancelled | paused | resumed | runout | error`;
`severity` one of `info | warning | error`; `printer.kind` one of
`bambu | snapmaker | printhost | connect`; `code` and `job` are optional. `id` and `time` are the
hub's — an instance never sends them, and `accept_event` is where they are assigned.

**Routes**

| Route | Gate | Answer |
|---|---|---|
| `POST /hub/event` | loopback peer + loopback `Host` + not cross-site + `X-Hub-Secret`, `Content-Type: application/json` | the stored event (so the sender can see the id it got) |
| `GET /hub/events?since=<id>` | the same | `{"events": [...], "last_id": n}` |
| `GET /r/<token>/events?since=<id>` | the phone token, exactly as `/r/<token>/state` | the same shape |
| `GET /api/events?since=<id>` (instance) | the hub's proxy allow-list | `{"events": [...], "last_id": n}`, `local_id` per event |

## 3. Decisions

- **Edge-triggered, from a snapshot diff, in a pure function.** `RemoteEvents::step(Memory&, const
  Snapshot&, cooldown_ms)` takes the previous memory and the snapshot just taken and returns the
  events; `now.at` is the only clock it sees. Everything hard — the "already printing when the
  slicer opened" case, reconnect flapping, the cooldown — is decided there, and a Python test
  drives it through `/api/debug/events` with snapshots written by hand.
- **One vocabulary.** Bambu's `print_status` (`RUNNING`/`PAUSE`/`FINISH`/`FAILED`/`SLICING`) and
  Klipper's `print_stats.state` (`printing`/`paused`/`complete`/`error`/`cancelled`/`standby`) are
  normalised into `idle | preparing | printing | paused | finished | failed | cancelled` before the
  rule sees them, so the rule has one set of words and each printer family has one small mapper.
- **Nothing is invented for a printer nobody can see.** A `PrinterState` carries `watched`, and in
  LAN mode only the *connected* Bambu printer has it (`DeviceManager::set_selected_machine`
  disconnects the previous one, research §2.1). An unwatched or offline printer produces no event,
  and neither does the first snapshot of any printer: a state must have been seen in a watched,
  online snapshot before a change from it counts. That is §2.2's "hold for two polls", cheaper.
- **Filament runout is its own kind.** Bambu stage 6 is "Paused due to filament runout"
  (`DeviceManager.cpp:52`); a pause with that stage becomes `runout` rather than `paused`. Klipper
  has no equivalent this fork can name — the U1's `filament_switch_sensor` name is still unknown
  (research §5 question 4), so a U1 runout arrives as a plain `paused` until hardware answers that.
- **The cooldown is per printer, kind and cause.** The key is
  `<printer>|<kind>|<code or job>` with a three-minute window: an error that keeps being reported
  is one event, a reconnect that re-announces the same job is one event, and starting a *different*
  file inside the window is still its own event. A suppressed event does not refresh the key, so a
  condition that really lasts reappears once the window has passed.
- **The GUI thread does the cheap half only.** The heartbeat reads the `MachineObject`s and the
  preset's print host (both GUI-thread state) and hands a snapshot to a worker; the worker does the
  network (`SnapmakerLan::status`, `RemoteControl::describe_hosts`), the diff and the POST. One
  poll is in flight at a time, every 5 s.
- **Delivery is fire and forget.** `RemoteHub::post_event` is one loopback POST with a one-second
  connect timeout. With no hub running it fails and the event stays in the instance's own ring.
- **The hub owns the sequence.** Ids come from the hub, so two instances reporting at once cannot
  collide, and `events.json` carries `last_id` so a restart continues rather than reusing ids — a
  reused id would hide an event from a phone for good.
- **The balloon is the tray's own.** `HubTaskBarIcon` is a `wxTaskBarIcon`, so this is
  `ShowBalloon` (Shell_NotifyIcon's `NIF_INFO`), marshalled to the GUI thread. It fires for
  `warning` and `error`, and for `finished` — the one piece of good news worth interrupting for.
- **Everything from a printer is clipped.** `accept_event` normalises the kind, the severity and
  the printer kind against closed sets and truncates every string, so an HMS message or a file name
  cannot become a wall of text in the hub's memory, its state file or a notification.
- **P5's seam.** `HubServer::accept_event(json&)` is the single point where an event enters the
  hub, and it carries one comment line saying where the relay call goes. No destination, setting or
  sender is built here.

## 4. What the phone shows

The Devices tab grew a red badge with the number of events newer than the highest id this phone has
looked at (`snorca_remote_events_seen` in `localStorage`), and, at the top of the tab, the last
eight events as compact lines — a severity dot, the title, the sentence and how long ago. Looking
at the tab is what marks them read. The feed is polled with `since=<newest held>` on the page's own
cadence: every 5 s while the Devices tab is open, every 15 s otherwise so the badge is right from
any tab. A hub on a fresh data dir starts its ids at 1 again, and the page trusts the hub's
`last_id` over its own remembered one so nothing is hidden for ever.

## 5. Verified (2026-09-04, gate `snorca_hubtest\test_phone_events.py`, 72 checks, all green)

Run by `gate_events.sh` against this branch's build installed to `inst_events`, on the isolated
`dd_events` data dir, with `mock_printhost.py` on 127.0.0.1:18089 standing in for a U1. No real
printer was ever commanded.

- **A, the rule alone** (through `POST /api/debug/events`, no printer, no clock): the first
  snapshot of a printer only seeds it; `idle -> printing` is `started`, `printing -> printing`
  nothing, `printing -> paused` `paused`, `paused -> printing` `resumed` (not `started`),
  `printing -> finished` `finished`; stage 6 makes the pause a `runout`; a failure carrying a new
  code is **one** event, not `failed` *and* `error`, and a further code while already failed is an
  `error` on its own; an unwatched printer and an offline one produce nothing while the watched one
  beside them does; the same file started again inside the cooldown makes no event, a different
  file does, and past the window the same file does again.
- **B, the pair**: `GET /api/events?since={id}` is in the manifest and proxied; `/api/debug/events`
  is in neither, and the instance answers it only on loopback.
- **C, the live pipeline**: the mock starts printing and a `started` event reaches `/hub/events`
  with a hub-assigned increasing id and unix-ms time, the reporting instance's pid, the printer's
  id/name/kind, severity `info`, a title, a sentence, the job — and no field outside the contract;
  `/r/<token>/events` returns byte-identical objects; going idle gives `finished`.
- **D**: ids increase and never repeat, `last_id` tracks them, `since=` filters on both routes.
- **E**: against the running watcher, the same file restarted inside the window makes no second
  `started` while a different file does; and a printer forgotten and taken back *while printing*
  announces nothing (the seed rule, live).
- **F**: the ring is mirrored to `events.json`, and a hub quit and restarted still serves every
  event with the id sequence carrying on.
- **G**: `/hub/events` and `/hub/event` refuse a missing or wrong secret (403), a non-loopback
  `Host` and a cross-site fetch (404), a non-JSON body (415) and a broken one (400); a forged post
  without the secret stores nothing; `/r/<wrong token>/events` and a bare `/events` are 404; and
  the events carry no secret, no data dir and no address.

`test_security_lan.py` pointed at this data dir passes, as do `test_phone_control_lan.py`,
`test_phone_snapmaker_lan.py --real=` and `test_phone_ui.py` on the same build.

In a browser at 375x812 against the same hub: the Devices tab carried a red **3** while the Streams
tab was showing, and opening it listed the three events newest-first ("Mock U1 started printing /
Mock U1 started a print - ev_two.gcode / 3m ago", and so on) above the printer cards, clearing the
badge and leaving `snorca_remote_events_seen=3` in `localStorage`.

## 6. For the user to verify on hardware

Only the Snapmaker path can be proven without a printer. The Bambu half needs one:

1. **A print started from the PC** on the connected Bambu printer: a `started` event within ~5 s,
   with `subtask_name` as the job.
2. **Finish**: a `finished` event and a tray balloon on the PC.
3. **Pause and resume** from the printer's own screen (not from the phone): `paused` then
   `resumed`, and the pause's text should carry `get_curr_stage()` ("Printing was paused by the
   user").
4. **Filament runout**: check that the pause arrives as `runout`, not `paused` — this is the one
   place the fork's stage index (6) is being trusted, and it is the event most worth getting right.
5. **An HMS error / `print_error`**: an `error` event with the printer's own text, and no second
   one for the same code within three minutes.
6. **Stop from the printer**: which of `FINISH` / `FAILED` a Bambu abort really lands on, and
   therefore whether it reads as `finished` or `failed` (nothing here maps a Bambu abort to
   `cancelled`; if it lands on `FINISH` the event will say "finished", which is wrong and is a
   one-line fix once the answer is known).
7. **Four printers at once**: only the connected one should ever produce events; the other three
   must stay silent (research question 6).
8. **A U1 runout**: confirm it arrives as a plain `paused` and note the Klipper object name so the
   `runout` kind can be extended to it (research question 4).

## 7. Not done

- No relay, destination or setting: that is P5, and it hooks into `accept_event`.
- No service worker and no Web Push (P6, P7).
- A U1's filament-runout sensor is not read (its Klipper object name is unknown), so a U1 runout is
  reported as a pause.
- Progress milestones (25 / 50 / 75 %) are not emitted; the research lists them as opt-in.
