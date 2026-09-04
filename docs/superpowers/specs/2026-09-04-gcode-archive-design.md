# The G-code archive: every file that was sent, ready to send again (2026-09-04)

**The ask.** A Preferences > Ultra section with *Store G-Code Files* (on/off), a folder picker for
where they go, and *Maximum Retention* (a number, default 100). Every file the slicer uploads to any
printer - from the desktop or from the phone, upload or upload-and-print - is kept there, and the
phone can reprint any of them without the project being open. So the archive must remember **which
printer each file went to**, and enough of the send to replay it.

**What this document is.** The design around and beyond Stage 1 (the preferences, the archiving, the
retention and a listing endpoint), which is being built in parallel on `feat/gcode-archive`. Here:
the storage layout and sidecar schema, the retention rules, the phone's *Reprints* experience, the
Stage 2 API contract, what happens with several slicers open or none at all, the privacy line, a
phased plan, and a ranked list of suggestions to pick from - several of them lifted from
[u1hub](https://github.com/dlgambill/u1hub), which has been solving the same problem for a farm of
U1s for a year.

Branch `docs/gcode-archive-design`, cut from `feat/ultra-preferences` at `309ff76f8c`.

---

## 1. Goals

1. **Nothing sent is lost.** Every byte stream that leaves this PC for a printer is kept, exactly as
   sent, with a record of where it went, when, and what it was.
2. **A reprint needs neither the project nor the plate.** Tapping *Reprint* on the phone must work
   when the slicer holds an empty plater, and (later) when no slicer is running at all. The archived
   bytes are the source, not a re-slice.
3. **The replay is faithful.** The same printer, the same file name, the same toolhead / AMS mapping,
   the same options - or an explicit, visible deviation.
4. **It costs the user nothing to have on.** Bounded disk, no dialogs, no slowdown of a send, and no
   new network surface.
5. **It never leaks the model.** The archive holds the user's geometry. It stays behind the phone
   token and the instance API, never on a static path.

**Non-goals for this design.** Re-slicing an archived job (that needs the project, not the G-code);
a print queue / scheduler; editing an archived file; syncing archives between machines; a cloud.

---

## 2. How a send actually works here (the anchors this design has to fit)

Four paths put a file on a printer. They agree on nothing except that a local file gets uploaded.

| Path | Trigger | File that is uploaded | Where the file is | Printer identity |
|---|---|---|---|---|
| **Bambu** (network plugin) | `SelectMachineDialog` / `SendToPrinterDialog`; phone `RemoteSend` `kind=bambu` | the **gcode 3mf**, plus a config 3mf for cloud printers | the plate temp path with the extension **replaced**: `Plater::send_gcode` (`Plater.cpp:22302-22334`) does `_3mf_path = plate->get_tmp_gcode_path(); _3mf_path.replace_extension("3mf")` → `BBL::PrintParams::filename` / `config_filename` (`RemoteSend.cpp:358`). `.gcode.3mf` is the *upload* name, not the name on disk | `dev_id` (the printer's serial) from the `MachineObject`; the phone's `/api/printers` id **is** the dev_id |
| **Print host** (OctoPrint, Moonraker, Duet, …) | `Plater::send_gcode_legacy` (`Plater.cpp:22073`) → `PrintHostSendDialog` → `PrintHostJobQueue`; phone `RemoteSend` `kind=printhost` | plain `.gcode` - or the gcode 3mf when a Bambu preset sits behind a third-party host (`use_3mf`, `RemoteSend.cpp:585-597`) | `PrintHostUpload::source_path`; name in `upload_path`. The desktop queue makes its own temp copy (`BackgroundSlicingProcess::prepare_upload`, `:906-952`) and deletes it in `PrintHostJobQueue::priv::remove_source()` (`PrintHost.cpp:305`); the phone path uploads the live file in place | the preset's `print_host` URL; the phone's id is the literal string `"host"` |
| **Snapmaker U1 over the LAN** (the primary path) | phone `RemoteSend` `kind=snapmaker` | plain `.gcode` | `p->upload.source_path` = `plate->get_tmp_gcode_path()` **with no copy** (`RemoteSend.cpp:511`), named `p->lan_filename` (`:515-523`) | `sm:<serial>` from `/machine/system_info` → `product_info.serial_number`; the store is `<datadir>/hub/snapmaker_lan.json` |
| **Snapmaker over MQTT** ("connect") | the desktop's Flutter `WebPreprintDialog`, `SSWCP.cpp`; phone `RemoteSend` `kind=connect` | plain `.gcode` via `Moonraker::upload` with `print=false`, then `printer.print.start` over MQTT | same temp G-code; SSWCP may zip it next to the tmp file (`generate_zip_path`, `SSWCP.cpp:350-366`) | the literal string `"connect"` - one connection per PC |

Four facts fall out of that table, and the whole design leans on them.

**(a) The bytes only exist during the send, and two paths do not even copy them.**
`get_tmp_gcode_path()` is `<temp>/snapmaker_orca_model/<day>/<time>#<pid>#<model>/Metadata/.<pid>.<N>.gcode`
(`PartPlate.cpp:2928`, `Model.cpp:1056`), where `<N>` is a global counter, not the plate index - so
the name is not stable across sessions, and the whole backup directory is removed with the `Model`.
The Snapmaker-LAN and non-Bambu print-host phone paths hand that live file straight to the uploader
(`RemoteSend.cpp:511`, `:593`); the next slice invalidates it. **The archive must copy during the
send, at the hook, never lazily from a recorded path.**

**(b) The thumbnail can be recovered from the archived file, and that is the better route.** The
phone's live preview (`api_plate_thumbnail`, `RemoteAccess.cpp:613`) needs the plate loaded and a GL
context on the GUI thread, so it is useless for history - but the archived bytes carry their own
picture, and this codebase already parses both forms: a sliced `.gcode` has
`; THUMBNAIL_BLOCK_START` / `; thumbnail begin WxH` base64 blocks, parsed at `SSWCP.cpp:200-257`
(and again at `Config.cpp:1440-1485`); a gcode 3mf carries `Metadata/plate_<n>.png`
(`THUMBNAIL_FILE_FORMAT`, `bbs_3mf.hpp:23`), extractable with the miniz pattern already written in
`SliceCompare/Snapshot.cpp:179-220`. Extracting on a worker thread from bytes the archive already
holds works retroactively, needs no GL, and matches u1hub ("Snapmaker Orca embeds model previews in
every sliced file; the Hub extracts them"). Do that; keep the live render only as a fallback for a
file that turns out to carry no thumbnail.

**(c) A dry run produces a full result and transfers nothing** (`RemoteSend.cpp:681-686`, `:788-793`,
`:858-863`) - and the phone fires one on **every** multi-material Snapmaker send to get the mapping
proposal (`stream_center.html:2528`, `dry_run=1&force=1`). A hook that does not check
`result["dry_run"]` (or `Prepared::dry_run`) will archive a phantom record roughly once per real
send. This is the single easiest way to get this feature wrong.

**(d) There is no single funnel.** The one existing "after a send" hook in this fork, Spoolman's
`SpoolmanDialog::deduct_after_send_async()`, is wired at exactly two call sites -
`Plater::print_job_finished` (`Plater.cpp:22379`, the Bambu desktop path) and `SSWCP.cpp:2353` (the
MQTT path) - and it is **not** wired into `RemoteSend`, so a phone send deducts nothing today. That
is the shape of the trap: an "every send" feature hooked per-path ends up covering some of them.

**The funnel that does exist** covers the phone, and only the phone. `RemoteSend::Prepared`
(`RemoteSend.hpp:43-70`) already normalises all four kinds into one struct - `kind`, `mode`, `plate`,
`printer_id`, `printer_name`, `dry_run`, `params` (Bambu), `upload.source_path` /
`upload.upload_path` (host and LAN), `lan_filename`, `file_filaments`, `mapping`, `toolheads` - and
`RemoteSend::run(Prepared, Sink)` (`RemoteSend.cpp:916`) is the single function every phone send
passes through. One level up, `RemoteAccess::finish_job` (`RemoteAccess.cpp:1311`) sees all three
phone kinds with a `result` JSON that already carries `kind`, `mode`, `printer{id,name}`, `source`,
`filename`/`upload_path`, `size`, `uploaded`, `mapping` and `printer_state` - though for Bambu the
source path is only inside `result["params"]["filename"]`.

**Archive from `Prepared`, in `run()`'s success path**, where the source path, the dry-run flag and
the printer identity are all in hand and typed. Then give the three desktop entry points a thin
adapter that fills the same struct: `Plater::print_job_finished` (`Plater.cpp:22377`) and
`Plater::send_job_finished` (`:22402`) for Bambu, `PrintHostJobQueue::priv::perform_job`
(`PrintHost.cpp:322`) for the desktop print-host queue - **copying before `remove_source()` deletes
the temp file at `:316`** - and `SSWCP_MachineOption_Instance::sw_MachinePrintStart`
(`SSWCP.cpp:2331`) for the Device page, whose local file is `SSWCP::get_active_filename()`
(`SSWCP.cpp:7480`). Those are the same four places Spoolman should have been wired into. One writer,
one schema, five callers.

---

## 3. What u1hub already does, and what this fork's phone page does not

[u1hub](https://github.com/dlgambill/u1hub) ("U1 Print Hub", MIT, Node 22, read 2026-09-04) is a
local dashboard for a farm of U1s and other Klipper machines. It is the closest thing to a
specification of what a person wants once they have more than one printer, and several of its
answers are directly reusable. Read on 2026-09-04:
[README](https://github.com/dlgambill/u1hub/blob/main/README.md),
[docs/CHANGELOG.md](https://github.com/dlgambill/u1hub/blob/main/docs/CHANGELOG.md),
[server.js](https://github.com/dlgambill/u1hub/blob/main/server.js),
[gcode/README.txt](https://github.com/dlgambill/u1hub/blob/main/gcode/README.txt).

| u1hub has | This fork's phone page has | Gap |
|---|---|---|
| **A file library** - a folder of `.gcode` it holds (`gcode/`, pointed at "your Snapmaker Orca output folder" per `gcode/README.txt`), merged with each printer's onboard storage into one list with source pills (2.7, "the remote & files release") | nothing - a send starts from a *sliced plate* in a slicer window | **the whole archive**; without it there is no list to reprint from |
| **Thumbnails for every file**, extracted from the G-code's own embedded previews ("Snapmaker Orca embeds model previews in every sliced file; the Hub extracts them"), served from a cached path (`/api/thumb`, `/api/pthumb`) | plate thumbnails only, rendered live from the plater (`/api/plates/{i}/thumbnail.png`) | a picture per archived file |
| **Per-file filament memory keyed by content**: "On job completion it records which physical spools were loaded - keyed by file **content**, so the memory survives renames and folder moves, and correctly resets when you re-slice" (2.10). The key is `fileContentHash()`, `server.js:263` - sha256 of `size + first 1 MB + last 1 MB`, prefixed `ph1-` | the toolhead mapping is composed per send and forgotten | remember what a file printed with, and offer it back |
| **Loadout replay** - "Replaying a past loadout is now spool-first: for each spool the print used, pick which tray it's sitting in *today*", with empty trays unselectable and the historical slot only a hint, verified by re-reading the printer (2.10) | the mapping step pre-fills by nearest colour and nothing else | the *last time* row in the mapping step |
| **Start a file already on the printer** - `SDCARD_PRINT_FILE FILENAME="…"` after the mapping macros (`server.js`, `/api/print`), and copy printer→printer without touching disk (2.7) | every send uploads | a reprint of a file the printer still holds could skip the upload entirely |
| **Print history and lifetime statistics** - `/api/farm/history` and `/api/farm/stats`, read straight from each printer's Moonraker `/server/history/list` and `/server/history/totals` | the event watcher (`RemoteEvents`) sees transitions live and keeps a ring in the hub, but nothing is joined back to a file | outcome per archived file, and totals per printer |
| **A queue and a scheduler** - "The Dispatch tab schedules your queue across every printer, inside the hours you are actually home to swap plates", spool-clash detection, *push back one*, *take down* a printer (2.11, 2.20) | one send at a time per instance, no queue | out of scope here, but the archive is its prerequisite |
| **Colour matching across the library** - "The Match tab reads the colors loaded on each printer and lists the library files those colors can already produce, best match first, one tap from printing" | the per-send colour match only | *what can I print right now* over the archive |
| **Spool inventory** - RFID/QR identity, grams left, price, which machine holds it, and a shortfall report | Spoolman integration on the desktop (`spoolman_deduct`), not on the phone and not per file | see suggestion 12 |

Two u1hub design choices are worth copying verbatim, and one worth *not* copying:

- **Copy: key the memory on content, not on the file name.** A rename or a move must not lose the
  record, and a re-slice must reset it. u1hub's cheap variant (size + 1 MB head + 1 MB tail) is a
  good trade for a 400 MB file, but our archive holds its own copy of the bytes and hashes them once
  while it is already streaming them - so a full sha256 is nearly free here. Record both.
- **Copy: refuse rather than overwrite, and say why.** "The Hub never touches a file that is actively
  printing, never silently overwrites, honors the printer's own read-only flags … every refusal
  tells you exactly why" (2.7).
- **Do not copy: one merged list of "the library plus what is on each printer".** u1hub is the only
  thing talking to those printers, so it can own that list. Here a slicer instance comes and goes; a
  list that mixes archive records with a live directory read of four printers is a list that changes
  shape when a printer is asleep. Keep the archive its own list, and show *"still on the printer"* as
  a **badge on an archive row** (suggestion 7) rather than as extra rows.

---

## 4. Storage layout

### 4.1 The folder

The preference is a folder picker; the default is `<data_dir()>/archive`. (`data_dir()` is where
`hub/`, `hub/uploads`, `hub/saves` already live - `RemoteHub.cpp:108-111` - so this is the same
neighbourhood, on the same volume, and it survives an app reinstall.) A folder the user picks may be
on a NAS or an external drive; every write therefore has to tolerate a path that is slow, briefly
absent, or read-only, and a failure to archive must **never** fail the send. Archiving is a
side-effect, not a step.

Two notes for whoever writes the preferences. There is **no generic directory-picker helper** on the
Ultra page: the only one is `create_item_downloads` (`Preferences.cpp:997-1035`), hard-wired to
`download_path`; generalising it into `create_item_dir(parent, padding, param, caption)` is a
two-literal change and gives the archive folder the same look as the download folder. And
**AppConfig in this fork is multi-instance-aware** - several slicers share the file and additive
sections are unioned back in by `merge_shared_from_disk` (`AppConfig.cpp:799`, `:961`). A scalar key
(a bool, a path, a count) is safe there; a growing *list* would not be. That is another reason the
archive's own records live in the archive folder and not in AppConfig.

### 4.2 One directory per record

```
<archive>/
  20260904T142233Z-7f3a2c/          <- a record. The directory name is the record id.
      job.json                      <- the sidecar (schema below)
      print.gcode                   <- the file exactly as it was uploaded
      thumb.png                     <- the plate render, captured at send time
  20260904T151907Z-91be04/
      job.json
      print.gcode.3mf
      thumb.png
  .20260905T090012Z-3c1180.part/    <- a record being written; readers skip dot-names
  archive.json                      <- OPTIONAL cache of the listing. Rebuildable. Never the truth.
```

**Why a directory and not three sibling files.** The three parts arrive together or not at all, a
delete is one recursive remove, and the write can be made atomic without a lock: write everything
into `.<id>.part/`, then a single `rename()` to `<id>/`. A reader that ignores names beginning with
`.` can never see a half-written record, and two slicers writing at the same time cannot collide
because their ids differ. This is the same discipline as `SnapmakerLan::save_store`
(`SnapmakerLan.cpp:101` - temp file, then rename) generalised from one JSON to one record.

**The id.** `<UTC ISO basic timestamp>-<first 6 hex of the sha256>`, e.g.
`20260904T142233Z-7f3a2c`. Sortable by time with a plain string sort (so a listing does not have to
open every sidecar to order it), unique without a counter, safe in a URL path segment, and it says
something to a human browsing the folder. Collisions inside one second on the same bytes are the
same record, which is the right answer anyway (see dedupe, suggestion 8).

**The stored file keeps the extension it was sent with** - `print.gcode` or `print.gcode.3mf` - so
that a reprint can tell a Bambu 3mf from a plain G-code without reading the sidecar, and so that a
person double-clicking it in Explorer gets the right thing. The *name it was given to the printer*
lives in the sidecar, not in the file system, so a name a printer would not accept (or two records
of the same name) never becomes a file-system problem.

### 4.3 The sidecar, `job.json`

```json
{
  "schema": 1,
  "id": "20260904T142233Z-7f3a2c",
  "created": 1757001753000,
  "source": "phone",
  "instance": 21044,
  "app": "Snapmaker Orca Ultra 2.3.1",

  "mode": "print",

  "printer": {
    "id": "sm:8110025111600047BIY2",
    "kind": "snapmaker",
    "name": "U1 (workshop)",
    "model": "Snapmaker U1",
    "address": "10.0.0.108:80",
    "host_type": null
  },

  "file": {
    "sent_as": "bracket_plate_2.gcode",
    "stored": "print.gcode",
    "bytes": 4821330,
    "sha256": "9f2c…",
    "content_key": "ph1-4a1d…"
  },

  "plate": { "index": 2, "name": "plate_2" },
  "project": { "name": "bracket", "saved": true },

  "print": {
    "time_s": 8123,
    "filament_g": 41.2,
    "filament_mm3": 33230.0,
    "layer_height": 0.2,
    "nozzle_diameter": 0.4,
    "bed_type": "textured_plate"
  },

  "filaments": [
    { "index": 0, "type": "PLA", "sub_type": "PLA Basic", "vendor": "Polymaker",
      "color": "#1E88E5", "grams": 22.6, "toolhead": 2 },
    { "index": 1, "type": "PLA", "sub_type": "PLA Matte", "vendor": "Polymaker",
      "color": "#111111", "grams": 18.6, "toolhead": 1 }
  ],

  "send": {
    "mapping": "0:2,1:1",
    "ams_mapping": null,
    "options": { "bed_leveling": true, "flow_cali": true, "timelapse": false,
                 "vibration_cali": false, "use_ams": true }
  },

  "presets": {
    "printer": "Snapmaker U1 0.4 nozzle",
    "print": "0.20mm Standard @U1",
    "filament": ["Polymaker PLA Basic", "Polymaker PLA Matte"]
  },

  "outcome": { "state": "finished", "at": 1757009876000, "source": "events" },

  "pinned": false,
  "sent": [ { "at": 1757001753000, "printer": "sm:8110025111600047BIY2", "mode": "print" } ]
}
```

Field notes, and why each one is there rather than derived later:

- **`printer.id` is the identity the send API already uses**, verbatim: a Bambu `dev_id`, the literal
  `host` or `connect`, or `sm:<serial>` for a LAN U1. That is what
  `POST /api/plates/{i}/send?printer=` takes and what `GET /api/printers` returns, so a reprint can
  hand it straight back with no translation. `host` and `connect` are *not stable identities* - they
  mean "whatever this PC's printer preset points at" and "whatever the Device tab is connected to" -
  so `printer.address`, `printer.host_type`, `printer.name` and `printer.model` are recorded
  alongside, and a reprint to `host` warns when today's host address differs from the recorded one.
- **`file.sha256` is computed while the file is being copied**, in the same pass, so it costs one
  hash of bytes already in cache. `content_key` is u1hub's cheap head+tail key, kept because it lets
  us recognise *the file on the printer* (or in someone else's folder) without reading all of it.
- **`print.*` and `filaments[].grams`** come from `p->get_slice_result()->print_statistics` - the
  same numbers `/api/plates` already reports as `time_s`, `filament_g`, `filament_mm3`
  (`RemoteAccess.cpp:591-602`) - and, for a U1, from `Prepared::file_filaments`, which already
  carries index, colour, type and grams. Nothing new has to be computed.
- **`send.mapping` is stored in the exact wire form** (`"0:2,1:1"`) that
  `POST /api/plates/{i}/send&mapping=` accepts, and `send.ams_mapping` as the JSON string
  `PrintParams` carried. A replay is then a copy, not a re-derivation - and a re-derivation is where
  a subtle difference between "what we printed" and "what we replay" would hide.
- **`presets`** are for the human ("was this the draft profile?") and for a compatibility check when
  reprinting to a *different* printer. They are names, not values: the full config is inside the
  archived `.gcode`/`.3mf` anyway.
- **`outcome`** is filled in later, by the event watcher (§7). Absent means "we never learned".
- **`sent`** is an append-only list, so a file reprinted five times is one record with five entries
  rather than five copies of 40 MB.
- **No absolute paths anywhere.** `project.name` is the project's name; `project.saved` says whether
  it had ever been saved. The archive must not tell a phone where anything lives on the PC (§9).

### 4.4 The listing cache

`archive.json` at the top of the folder is a convenience: `{ "schema": 1, "records": [ …the sidecars
minus the bulky fields… ] }`. It is written after any change, best-effort, and **any reader that
finds it stale or missing falls back to a directory scan**. With a retention default of 100 records a
scan is ~100 small file reads, which is a few milliseconds warm - so the cache is an optimisation for
a NAS, never a source of truth. Deleting it must be harmless. (This is deliberately the opposite of
`snapmaker_lan.json`, which *is* the truth: that file has one writer's worth of state and can afford
last-writer-wins; an archive index cannot.)

---

## 5. Retention

**The rule Stage 1 implements:** keep the newest N complete records, N from *Maximum Retention*
(default 100); delete the oldest beyond it, at the end of a successful archive write. Deletion is a
rename to `.trash-<id>/` followed by a recursive remove, so a failure halfway leaves a dot-name that
the next pass finishes and no reader ever shows.

Three refinements, in the order they earn their keep:

1. **Pinned records are exempt and do not count.** A checkbox on the row, `"pinned": true` in the
   sidecar. Without it, the one file the user actually reprints monthly is guaranteed to be the one
   that ages out. This is the single highest-value retention feature and it is nearly free.
2. **A size cap as well as a count.** *"…or 20 GB, whichever comes first."* 100 records of a
   multi-colour 400 MB `.gcode` is 40 GB; 100 records of a keychain is 200 MB. A count alone is not a
   promise about disk. Evict oldest-first until both bounds hold.
3. **A per-printer cap.** *"Keep the newest 25 per printer."* On a three-printer farm a busy machine
   otherwise evicts the other two out of the list entirely. Only worth it once several printers are
   in regular use; the global cap covers the single-printer case.

**Two rules that are not optional.** A record whose file is currently printing (matched by
`outcome.state == "printing"`, or by the event watcher's live job name) is never evicted - u1hub's
"never touches a file that is actively printing". And eviction runs **after** a successful write, not
before, so a failure to write never costs the user a record they already had.

---

## 6. The phone: *Reprints*

`resources/web/orca/stream_center.html` has three tabs today - Streams, Prepare, Devices
(`stream_center.html:279`). The archive is a fourth: **Reprints**. It sits between Prepare and
Devices because that is the order of the story (slice → send → send again).

**The list.** Newest first, one row per record: the thumbnail at the left, the file name, the printer
it went to with its kind, how long ago, the time and grams, and a strip of colour swatches - one per
filament, in the file's own order. A pin, and an outcome dot when the event watcher knows one
(green finished, red failed, grey cancelled, none = unknown). Rows are cheap: the thumbnails are
~10 KB PNGs, and 100 rows is one JSON call plus 100 image requests the browser caches by id.

**Filters, in this order of usefulness:** printer (chips, the same shape as the Devices tab's
cards), then a text filter over the file and project name, then *pinned only*. u1hub's source pills
are exactly this and they earn their space.

**One tap: reprint to the same printer.** The row's primary button. It re-uploads the archived bytes
under the recorded name, and for a `mode=print` record replays the recorded mapping and options -
after the same confirmation `mode=print` requires today ("Start printing … now?"), and after the same
preflight the send sheet already does. What it must **not** do is silently print with a mapping that
no longer matches the machine, so between the tap and the start:

- **the toolheads are re-read.** For a U1 the phone already asks for a dry run before the mapping
  step; the archive reprint does the same, and shows the mapping step **pre-filled from the record**
  with a *"last time"* line under each row, falling back to the colour match when a toolhead's
  contents have changed. That is u1hub's spool-first replay, minus the spool registry: the historical
  toolhead is a hint, an empty toolhead cannot be silently accepted, and the colours that are
  actually loaded win.
- **the differences are named, not hidden.** A row's filament was PLA Matte and the toolhead now
  holds PETG → an amber line "T2 holds PETG, this file was sliced for PLA Matte". A toolhead is empty
  → the same warning the send sheet already shows, and `force=1` required. A colour is far from the
  recorded one → a swatch pair, recorded vs loaded. None of these block; all of them are visible
  before the confirm.

**Reprint to a different printer.** A secondary *Send to…* on the row, offering the online printers.
This is where the archive stops being a list and starts being useful on a farm - and where it can go
wrong, so it is gated by a compatibility check before the picker even shows a machine:

- **same kind, or refuse.** A `.gcode.3mf` cannot go to a Moonraker host and a plain `.gcode` cannot
  go through the Bambu plugin. Cross-kind is a hard refusal with the reason, not a warning.
- **same model, or `force=1`.** The send API already refuses a model mismatch with a 409 carrying
  both model names and an "anyway"; reuse it unchanged, with `presets.printer` as the recorded side.
- **enough toolheads.** A four-filament file cannot be mapped onto a single-extruder machine.
  u1hub's class guard is the right shape here: block a multi-colour job on a single-extruder printer
  with a 409 that the phone can override, and let a single-colour job onto anything.
- then the mapping step, pre-filled by colour match against the **destination** printer, with the
  recorded mapping shown as history.

**Delete.** A trash on the row, with one confirmation naming the file. It refuses while that file is
printing.

**What the phone never sees:** the archive folder's path, the PC's project paths, or the file itself.
The row shows the *name*, the thumbnail comes from an endpoint keyed by record id, and the bytes go
PC → printer without passing through the phone (a 400 MB download over a Tailscale link is not a
feature). If a *download this file* button is ever wanted it is a deliberate, separate decision.

---

## 7. The Stage 2 API contract

Per instance, under `/r/<token>/i/<pid>/api/...`, exactly as the send API. All of it also has to be
added to the hub's proxy allow-list (`instance_api_allowed`, `RemoteHub.cpp:2220`) and to the
manifest at `GET /api`, which `test_hardening.py` checks as a pair.

```
GET    /api/archive?printer=&limit=&since=&pinned=
GET    /api/archive/{id}
GET    /api/archive/{id}/thumbnail.png
POST   /api/archive/{id}/send
POST   /api/archive/{id}/pin        ?on=0|1
POST   /api/archive/{id}/delete
```

**`GET /api/archive`** → `{ "records": [ … ], "total": 137, "kept": 100, "bytes": 4102338911,
"dir_set": true }`. Each record is its sidecar minus `presets` and with `file.stored` dropped (a
phone has no use for a file name inside a folder it cannot see). `printer=` filters on
`printer.id`, `since=` is a `created` timestamp for cheap polling, `limit` defaults to 100.

**`GET /api/archive/{id}/thumbnail.png`** serves `thumb.png` with a long `Cache-Control` - the record
is immutable, so the id is a perfect cache key. 404 with an empty body when a record has no
thumbnail (a desktop-path send that was archived before the thumbnail hook existed, say), so the page
can fall back to a placeholder without a spinner.

**`POST /api/archive/{id}/send`** takes **the same parameters as `POST /api/plates/{i}/send`** and
means the same things - `printer`, `mode=upload|print`, `confirm=1`, `force=1`, `dry_run=1`, `name`,
`mapping`, and the Bambu options `bed_leveling` / `flow_cali` / `timelapse` / `vibration_cali` /
`use_ams`. The differences are only these:

- `printer` **defaults to the recorded one**, so a one-tap reprint is `POST …/send` with
  `mode=print&confirm=1` and nothing else.
- every option not given falls back to **the record**, then to the desktop's remembered default -
  where a plate send falls back to the desktop's default directly. That is the whole point: the
  record is the memory.
- there is no plate, so no "not sliced" / "slicer is slicing" 409. There is a new one: **410 Gone**
  when the record's file is missing (evicted or the folder moved).
- the same one-send-at-a-time lock, the same `{job, …}` answer, and the same `GET /api/jobs/{id}`
  progress - `RemoteSend::run()` does the work, given a `Prepared` filled from the record instead of
  from the plater. **This is the load-bearing design decision:** a reprint is not a second transport,
  it is the same `run()` with a different `prepare`.

**`dry_run=1` on a reprint returns the proposed mapping**, exactly as it does for a plate send, which
is how the phone fills the mapping step.

**No `DELETE`, deliberately.** The hub's proxy allow-list rejects every method but GET and POST at
its first line (`if (!get && !post) return false;`, `RemoteHub.cpp:2223`), so a `DELETE
/api/archive/{id}` would work against the instance directly and 404 through the phone's own path -
the worst kind of bug. `POST /api/archive/{id}/delete` it is, and the `/api/archive/` prefix needs
the same id-shaped segment validation the allow-list already applies to `/api/printers/<id>/control`
(a bounded character set, no encoded slash).

---

## 8. Several slicers, and no slicer at all

**Several instances share one folder.** That is the normal case here: the hub starts slicers on
demand and the phone drives whichever one it opened. The layout of §4.2 makes concurrent writers safe
without a lock file - distinct record ids, a `.part` directory, one `rename()` to publish - and
concurrent readers safe by construction, because a published record is never modified in place.

Three places still need a rule:

- **Retention is a shared decision.** Two instances each finishing a send at the same moment would
  both scan, both find 101 records, and both evict the oldest - possibly the same one twice
  (harmless) or two different ones (one record too few, mildly wrong). Take an advisory lock for the
  eviction pass only: create `<archive>/.lock` with exclusive-create semantics, carry the pid and a
  timestamp, remove it after, and treat a lock older than 60 s as stale. If the lock cannot be taken,
  skip eviction - the next send does it. Retention is a bound, not an invariant; being one record
  over for a minute costs nothing.
- **Mutating a published record** (`pinned`, `outcome`, an appended `sent` entry) is a read-modify-
  write and therefore *does* race. Do it the same way as a create: write `job.json.tmp` next to it,
  rename over `job.json`. A lost update to `pinned` or `outcome` is recoverable by the user or by the
  next event; nothing structural breaks.
- **`archive.json` is written last, best-effort, and never trusted.** A racing pair of writers can
  leave it describing neither state; the next reader that notices a mismatch between it and the
  directory rewrites it. This is exactly why it is a cache.

**With no slicer open**, the phone's Reprints tab has nowhere to ask. Two answers, and the second is
the right one:

- *Start a slicer.* The hub can already launch an instance (`/hub/new`), and the phone already opens
  one to slice. But a headless slicer takes seconds to start and holds a lot of memory just to read
  100 JSON files.
- **A hub-side reader.** The hub already reads shared state that instances write - `snapmaker_lan.json`,
  `streams.json`, `events.json` - and already serves the phone directly at `/r/<token>/…`. Give it
  `GET /r/<token>/archive` and `GET /r/<token>/archive/{id}/thumbnail.png`, implemented as a
  directory scan of the same folder. **Read-only, always.** Reprinting needs a slicer (it needs
  `RemoteSend::run`, the print-host classes, the Bambu plugin), so a tap on *Reprint* with no slicer
  open starts one - the same "open a slicer" flow the Prepare tab already has - and then posts to it.
  The hub therefore needs to know the archive folder: the instance writes it into `hub.json` (or a
  small `hub/archive.json` pointer) whenever the preference changes, so the hub never has to read
  AppConfig.

That split - **the hub can show it, only a slicer can send it** - keeps the hub free of the printer
stack, which is the property that has kept it small.

---

## 9. Privacy and security

- **The archive is the user's geometry.** A `.gcode.3mf` contains the model. Every path to it must be
  behind the phone token or the instance API. In particular it must never be reachable under a
  static prefix: the hub serves `/r/<token>/…` and proxies an allow-listed instance API, and the
  archive lives only there. No `file://`, no folder share, no "serve the archive directory".
- **No paths on the wire.** `file.stored` and the archive folder itself are stripped from every JSON
  the phone sees. A phone that learns `D:\Work\Clients\Acme\bracket.3mf` has learned something about
  the user that a printer control page has no business carrying, and a compromised phone token should
  not yield a map of the PC.
- **The id is the only handle**, and it is not a path. Validate `{id}` against the exact shape the
  writer produces (`[0-9]{8}T[0-9]{6}Z-[0-9a-f]{6}`) *before* it touches the file system, in the
  instance and in the hub's allow-list both. That closes traversal without relying on the
  file-system layer.
- **A user-picked folder can be anywhere**, including a synced Dropbox/OneDrive folder. Say so in the
  preference's tooltip: *"Anything here is a copy of your models. Pick a folder you are happy to have
  them in."* Do not try to be clever about it.
- **The bytes stay on the LAN.** A reprint streams PC → printer. Nothing about the archive gives the
  phone a way to pull a model file over the tunnel, and that stays true unless someone deliberately
  adds a download.
- **Retention is not deletion-proof and should not pretend to be.** An evicted record is removed, not
  shredded. If someone wants the archive to hold client work, that is a folder-permissions and
  disk-encryption conversation, not a slicer feature.

---

## 10. Phased plan

Effort is in ideal days for one person who knows this codebase, excluding the hardware pass.

| Stage | What | Effort |
|---|---|---|
| **1** *(in flight, `feat/gcode-archive`)* | The three Ultra preferences; archive on every send from `RemoteSend::Prepared`; the record layout of §4.2; sidecar v1; newest-N retention; `GET /api/archive` + manifest + allow-list; the gate | 2-3 d |
| **1b** | The four desktop entry points through the same writer (`Plater::print_job_finished`, `Plater::send_job_finished`, `PrintHostJobQueue::priv::perform_job` before `remove_source()`, `SSWCP::sw_MachinePrintStart`) - a small adapter that fills a `Prepared` from what those paths already hold. Without this the preference is called *store any gcode sent to any printer* and means *sent from the phone* | 1 d |
| **1c** | The thumbnail, extracted from the archived bytes on a worker thread (§2b): the `; thumbnail` block parser of `SSWCP.cpp:200` for `.gcode`, the miniz pattern of `Snapshot.cpp:179` for `Metadata/plate_*.png` in a 3mf. Retroactive, so it can also fill in records written before it existed | 1 d |
| **2** | Reprint: `POST /api/archive/{id}/send` built on `RemoteSend::run` with a `prepare_from_record`; `/thumbnail.png`; `/delete`; the **Reprints** tab with the list, filters, one-tap reprint to the same printer and the mapping step pre-filled from the record | 3-4 d |
| **3** | Outcome linking (`RemoteEvents` → the record, §7 of the events design), `pinned`, the size cap, dedupe by sha256 with a `sent[]` entry instead of a second copy | 2 d |
| **4** | Reprint to another printer with the compatibility gate and the destination mapping step; per-printer cap | 2 d |
| **5** | The hub-side reader: `GET /r/<token>/archive`, the folder pointer in `hub.json`, and "no slicer open → start one, then reprint" | 2 d |
| **6** *(optional)* | Skip the upload when the printer still holds the file; fleet statistics; the *what can I print now* match over the archive | 2-4 d each |

The gate follows the shape of `test_phone_send.py` / `test_phone_events.py`: an isolated data dir, a
mock print host, records written by hand into a temp archive folder, and every assertion about
*retention, id validation and the allow-list* made without a printer. Only the reprint-to-hardware
pass needs a real machine.

---

## 11. Suggestions, ranked

Ranked by value to a person with one to four printers and a phone. Each: what it is, why it is worth
it, what it costs, and where u1hub has already done it.

1. **Reprint from the archive, not from a plate.** Make the archived bytes the thing that gets sent -
   `POST /api/archive/{id}/send` builds a `RemoteSend::Prepared` from the record and calls the same
   `run()` every send already uses. *Why:* this is the entire feature; without it the archive is a
   backup folder. It also makes the phone genuinely useful with the slicer holding an empty plater,
   which is the state a slicer is in most of the time. *Cost:* 3-4 d including the phone tab; the
   transport already exists and is proven on hardware. *u1hub:* its whole file library works this way
   - `POST /api/print { file, printer, start, map }` uploads from the library and starts, with no
   slicer in the loop at all.

2. **Record the printer identity and the mapping in the exact wire form the send API takes.**
   `printer.id` is the `dev_id` / `host` / `connect` / `sm:<serial>` string; `send.mapping` is
   `"0:2,1:1"`; `send.ams_mapping` is the JSON string `PrintParams` carried. *Why:* a replay becomes
   a copy rather than a re-derivation, and re-derivation is exactly where "what we printed" and "what
   we replay" would quietly diverge - a bug that costs a 9-hour print to discover. It also means the
   sidecar is self-documenting against an API that is already specified and gated. *Cost:* free; it
   is a choice about which strings to write. *u1hub:* stores the mapping it sends and replays it as
   the pre-selection.

3. **Fill in the outcome from the event watcher.** `RemoteEvents` already sees `started | finished |
   failed | cancelled | paused | runout` per printer with the job's file name
   (`2026-09-04-phone-events-design.md` §2). Join the newest record for that printer whose
   `file.sent_as` matches the event's `job`, and write `outcome`. *Why:* a history where every row
   looks identical is a list; a history where three rows are red is a tool. It is also the cheapest
   possible source of "did that overnight print work", which is the question the phone gets opened
   for. *Cost:* ~1 d - the matching rule and one careful write; the watcher, the vocabulary and the
   hub ring are all built. *u1hub:* `/api/farm/history` reads Moonraker's own job history, and 2.10's
   filament memory is written "on job completion" for the same reason.

4. **Pin a record so retention cannot take it.** A pin on the row; pinned records are exempt from the
   count. *Why:* retention's failure mode is precise and annoying - the calibration cube, the printer
   part, the thing reprinted monthly is by definition old, and a newest-N rule deletes exactly it.
   One checkbox turns "a rolling window" into "a small library". *Cost:* half a day, most of it the
   phone control. *u1hub:* no direct analogue (its library is not evicted at all), which is itself
   the argument - an evicting library needs an escape hatch a permanent one does not.

5. **Reprint to a different printer, gated by a compatibility check.** Cross-kind refused, model
   mismatch behind `force=1`, multi-colour onto a single extruder refused with an override, then a
   mapping step matched against the destination. *Why:* the moment there are two machines, "print
   this on whichever is free" is the most-used thing on the page, and it is also the most dangerous -
   sending a `.gcode.3mf` to a Moonraker host or a four-colour file to a single-head machine fails
   late and messily. *Cost:* ~2 d, most of it the checks; the picker and the mapping step come from
   stage 2. *u1hub:* a "structural cross-class block" plus a class guard that returns 409 with
   `classWarning: true` and a plain-English question ("It will likely fail or print wrong. Send
   anyway?") - copy the wording as well as the mechanism.

6. **A size cap alongside the count, and a per-printer cap after it.** *Why:* 100 records is a
   promise about the list, not about the disk, and the difference between the two is two orders of
   magnitude depending on what the user prints. A folder on a NAS that silently grows to 40 GB is the
   kind of thing that is discovered at the worst moment. *Cost:* ~0.5 d for the size cap (the sizes
   are already in the sidecars), ~0.5 d for the per-printer cap. *u1hub:* no cap at all - its library
   is deliberately permanent - so this is ours to get right.

7. **Skip the upload when the printer still holds the file.** Before re-uploading, ask the printer
   whether a file of that name and size is already on it (`GET /server/files/metadata` for a U1,
   which `SnapmakerLan::metadata` at `SnapmakerLan.cpp:705` already calls after an upload) and, if
   so, go straight to the mapping macros and the start. **The transport is already there on both
   sides:** `SnapmakerLan::start_print(d, filename, error)` (`:885`) takes an arbitrary file name and
   no caller reaches it without an upload, so this is a route and a check, not a new client; and the
   Bambu side has `PrintJob`'s `from_sdcard_view` mode (`PrintJob.cpp:186, 245-247, 508-511`) which
   sets `params.dst_file` and calls `start_sdcard_print`, driven today only from
   `SelectMachineDialog` (`SelectMachine.cpp:2139-2160`). *Why:* a reprint the same evening becomes
   instant instead of a 400 MB upload, and the archive row can carry an *on the printer* badge that
   says something true about the machine. *Cost:* ~1 d for the U1 path; the Bambu path needs a
   storage listing exposed to the phone API (`PrinterFileSystem` / `MediaFilePanel` for Bambu,
   `sw_GetFileListPage` for MQTT Moonraker - neither is reachable from `RemoteAccess` today), so ~2 d
   more. *u1hub:* exactly this - upload, then macros, then `SDCARD_PRINT_FILE FILENAME="…"`, with the
   file list merged from every printer's own storage (2.7).

8. **Dedupe by content: the same bytes sent twice is one record with two `sent` entries.** Hash on
   the way in (free, the bytes are being copied anyway); if a record with that sha256 exists, append
   to `sent[]`, update `printer` if it went somewhere new, and skip the copy. *Why:* sending the same
   plate to two printers, or reprinting from the archive, otherwise burns two or three of the user's
   100 slots on identical files and pushes out genuinely different ones. *Cost:* ~0.5 d. *u1hub:*
   keys its filament memory on content precisely so it "survives renames and folder moves, and
   correctly resets when you re-slice" (`fileContentHash`, `server.js:263`).

9. **Warn against the recorded filament setup, and show it as history in the mapping step.** Per
   filament row: the recorded colour and material next to what that toolhead holds now, an amber note
   when the material differs, the existing hard warning when a toolhead is empty. *Why:* the record
   makes a claim about the world ("T2 had blue PLA") that is often stale by the time it is replayed,
   and a replay that silently uses today's toolhead for yesterday's colour is the failure this whole
   feature could cause. Showing both sides costs one line and removes the class of error entirely.
   *Cost:* ~0.5 d on top of the mapping step. *u1hub:* 2.10's spool-first replay - the historical
   slot is "just a *was T3* hint", empty trays cannot be selected, and the result is verified by
   re-reading the printer rather than assumed ("2/3 applied - T3 has no filament loaded").

10. **A hub-side archive reader so Reprints works with no slicer open.** `GET /r/<token>/archive`
    plus the thumbnail route, served by the hub from a directory scan, with the folder recorded in
    `hub.json`; reprinting still starts a slicer. *Why:* the phone is opened when the user is not at
    the PC, which is exactly when no slicer is running; "open the app to see your history" is the
    kind of friction that makes a feature go unused. *Cost:* ~2 d, and it is genuinely optional -
    stage 2 works without it. *u1hub:* the hub *is* the always-on process; this is the closest this
    architecture gets to that property without moving the printer stack out of the slicer.

11. **Fleet statistics from what the printers already keep.** A small card per printer: lifetime
    prints, hours, filament, and the last ten jobs. *Why:* it is the one thing a farm owner asks for
    that costs almost nothing here, because Moonraker keeps it: `/server/history/totals` and
    `/server/history/list`. It also cross-checks the archive - a print the archive never saw shows up
    in the printer's own history. *Cost:* ~1 d for the U1/Moonraker half; the Bambu half has no
    equivalent endpoint and would have to be accumulated from the event watcher, which is a different
    and larger job. *u1hub:* `/api/farm/stats` and `/api/farm/history` do exactly this, plus
    per-printer temperature trends from `/server/temperature_store`.

12. **Close the Spoolman gap while the hook is being written.** `SpoolmanDialog::deduct_after_send_async()`
    is called from the Bambu desktop path and the MQTT path only; a phone send deducts nothing. The
    archive writer is the funnel that should have existed. *Why:* the user has already turned that
    preference on and it is silently half-true - which is worse than off. *Cost:* an hour, if the
    archive hook goes in `RemoteSend::run`'s success path where it belongs. *u1hub:* its spool
    inventory decrements from the job it dispatched, because it dispatches every job.

13. **A queue, eventually - and only after the archive exists.** Several jobs lined up per printer,
    started as each finishes. *Why:* it is the natural end of this road and the thing that turns two
    printers into a farm. *Why not yet:* it needs the outcome watcher to be trustworthy, an archive
    to queue *from*, and a policy for what happens when the user is asleep and a job needs a plate
    swap - which is a product question, not an engineering one. *Cost:* a week, at least, and it
    changes the hub from a viewer into an actor. *u1hub:* the Dispatch tab, and its 2.11 and 2.20
    notes are a good list of the problems that appear once you have one - spool clashes, "push back
    one", adopting prints already running, and taking a printer out of service without unplugging it.

---

## 12. Open questions

1. **Can a Bambu printer be handed a previously exported gcode 3mf unchanged?** The plumbing says
   yes: `PrintPrepareData::is_from_plater` (`PrintJob.hpp:21`) already skips the plater export for a
   caller that supplies its own `_3mf_path`, and calibration is the existing user of it
   (`CalibUtils.cpp:1213`). What is unproven is whether a printer accepts a 3mf whose `PrintParams`
   (project name, ftp folder, task fields) were composed for a *different* send. Only hardware can
   answer that. Until it does, the safe scope for stage 2 is the Snapmaker LAN and print-host paths,
   with Bambu reprint behind the same "verify on hardware, start with upload only" discipline the
   send design used.
2. **Can this codebase list a Bambu printer's own storage from the phone API?** Not today.
   `SendJob` puts the file on the SD card (`start_send_gcode_to_sdcard`) and `PrintJob`'s
   `from_sdcard_view` mode can print from it, but the only browser is `MediaFilePanel` /
   `PrinterFileSystem` (a desktop panel over FTP+MQTT); `RemoteAccess` exposes no storage listing for
   any kind. That listing is the missing half of suggestion 7 on Bambu, and it is a bigger job than
   the U1 half.
3. **Where should the archive default to?** `<data_dir()>/archive` is the argument in §4.1;
   `Documents\Snapmaker Orca\Archive` is more discoverable and more likely to be backed up, and more
   likely to be inside a synced folder. A user decision, not an engineering one.
4. **Is *Maximum Retention* a count of records or of jobs per printer?** This design reads it as a
   global count of records (the simplest promise), with per-printer caps as a later refinement.
   Worth confirming before the phone shows a number.
5. **What should happen to an archived record when the user changes the folder?** Move, copy, or
   start empty and leave the old folder alone. "Leave it alone, start empty, and say so" is the
   honest default; moving 40 GB from a preference dialog is not.
6. **Does the U1 name a filament-runout sensor?** Open from the events design (§3) and it matters
   here too: a `runout` outcome on a record is much more useful than a bare `paused`.
7. **Does anything need the archive to survive an app *uninstall*?** If yes, `data_dir()` is the
   wrong default.

---

## 13. Sources

All read 2026-09-04.

- u1hub (dlgambill/u1hub), MIT: [repository](https://github.com/dlgambill/u1hub),
  [README.md](https://github.com/dlgambill/u1hub/blob/main/README.md),
  [docs/CHANGELOG.md](https://github.com/dlgambill/u1hub/blob/main/docs/CHANGELOG.md) (releases 2.5
  through 2.20),
  [server.js](https://github.com/dlgambill/u1hub/blob/main/server.js) (`fileContentHash` at :263,
  `POST /api/print` and its mapping macros, `/api/farm/history`, `/api/farm/stats`, `/api/thumb`),
  [gcode/README.txt](https://github.com/dlgambill/u1hub/blob/main/gcode/README.txt).
- Snapmaker's own U1 firmware forks (Klipper / Moonraker / Fluidd), announced
  [2025](https://www.snapmaker.com/blog/snapmaker-u1-firmware-now-on-github/) - the reason the LAN
  path is plain Moonraker HTTP.
- This fork: `docs/superpowers/specs/2026-09-03-phone-send-design.md` (the four send paths, the
  `/api/plates/{i}/send` contract, the U1 mapping macros, §9's `upload_name` work),
  `2026-09-03-phone-print-control-design.md`, `2026-09-04-phone-events-design.md` (the event
  vocabulary and the hub's ring), `2026-09-03-phone-mobile-capabilities-research.md`,
  `2026-09-02-remote-access-design.md`.
- Code (all in the `feat/ultra-preferences` tree; several of these files do not exist on
  `feat/support-sets-stage2`): `src/slic3r/GUI/RemoteSend.{hpp,cpp}` (`Prepared` at hpp:43,
  `prepare_bambu` :315, `prepare_snapmaker` :492, `prepare_host` :554, the dry-run early returns at
  :681/:788/:858, `run` :916, `export_name_for` :950),
  `RemoteAccess.cpp` (`/api/plates` statistics :591, `api_plate_thumbnail` :613, `api_printers` :950,
  `api_send` :1079, `finish_job` :1311),
  `RemoteHub.cpp` (`hub_dir` :108, `instance_api_allowed` :2220),
  `SnapmakerLan.{hpp,cpp}` (`Device` hpp:25, the shared store and its temp-file write :84/:101,
  `list_printers` :625, `upload` :673, `metadata` :705, `mapping_script` :846, `start_print` :885),
  `RemoteEvents.{hpp,cpp}` (`step` :92, `PrinterState::watched` hpp:35, the job-name-change rule
  :144), `PartPlate.cpp:2928` (`get_tmp_gcode_path`), `Model.cpp:1056` (the backup dir),
  `Plater.cpp` (`send_gcode` :22302, `send_gcode_legacy` :22073, `print_job_finished` :22377,
  `send_job_finished` :22402), `BackgroundSlicingProcess.cpp:906` (`prepare_upload`),
  `Utils/PrintHost.cpp` (`perform_job` :322, `remove_source` :305),
  `Jobs/PrintJob.cpp` (`from_sdcard_view` :186/:245/:508) and `Jobs/SendJob.cpp:277`,
  `SSWCP.cpp` (the `; thumbnail` parser :200, `generate_zip_path` :350, `sw_MachinePrintStart` :2331,
  `get_active_filename` :7480), `libslic3r/Format/bbs_3mf.hpp:23` (`THUMBNAIL_FILE_FORMAT`) and
  `SliceCompare/Snapshot.cpp:179` (the miniz extraction pattern),
  `Preferences.cpp` (`create_ultra_page` :1562, `create_item_downloads` :997),
  `AppConfig.cpp` (`set_defaults` :405, the multi-instance merge :799/:961),
  `SpoolmanDialog.cpp` (`deduct_after_send_async` :183) and its two call sites, `Plater.cpp:22379`
  and `SSWCP.cpp:2353`, `resources/web/orca/stream_center.html` (the three tabs :279, the mapping
  dry run :2528, `sendStart` :2563).
