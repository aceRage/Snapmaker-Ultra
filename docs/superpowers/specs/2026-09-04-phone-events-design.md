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

## 5. Verified

Gate `snorca_hubtest\test_phone_events.py`, run by `gate_events.sh` against this branch's build on
the isolated `dd_events` data dir with `mock_printhost.py` standing in for a U1. Sections: the
transition rule on hand-written snapshots (starts, pauses, runout, resume, finish, failure with an
HMS code, cancelled, the unwatched/offline rule, the cooldown in and out of its window); the
manifest / allow-list pair and that `/api/debug/events` is in neither; the live pipeline (the mock
starts printing, a `started` event reaches `/hub/events` and `/r/<token>/events` with every contract
field and nothing else); ids and `since=`; the cooldown against the running watcher; the ring
across a hub restart; and the gating of both new routes.

## 6. For the user to verify on hardware

The Bambu half cannot be proven without a printer — see the checklist in the branch's report.

## 7. Not done

- No relay, destination or setting: that is P5, and it hooks into `accept_event`.
- No service worker and no Web Push (P6, P7).
- A U1's filament-runout sensor is not read (its Klipper object name is unknown), so a U1 runout is
  reported as a pause.
- Progress milestones (25 / 50 / 75 %) are not emitted; the research lists them as opt-in.
