# WebRTC video, brought into the tree (2026-09-12)

Branch `feat/webrtc-video-2`, from `feat/ultra-preferences` at `89bdf84170`.

Phase 2 (WebRTC camera video for the phone, with the relay as the fallback) was built and gated on
2026-09-02 as `feat/webrtc-video` — five commits — and then parked. The reason it was parked was
sound at the time: MSE worked, on the LAN and through Tailscale Serve, so the direct path bought
smoothness rather than function. What changed is that the hub has since grown a Phase 0b admin
listener, `settings.json` persistence, the Tailscale Serve card, the app push plane, relay-for-U1-on-
LAN and per-tile aspect reporting — and every month the branch sat, the merge got harder. This
brings it in, and adds a per-phone **Quality** setting alongside.

## 1. What changed versus the 2026-09-02 branch

The four code commits were cherry-picked unchanged where they still applied. What follows is what
had to be *different*, and why.

### 1.1 `WEBRTC_RELAY` is no longer a constant

The old branch shipped `var WEBRTC_RELAY = false;` in `stream_center.html` with a comment naming it
"the one line to flip". A hand-flipped constant is the wrong shape here: the page and the hub are
built from the same tree but *deployed* separately (the phone loads the page from whichever hub it
reaches), and a hub that could not get a media port must not advertise a mode that will never play.

So the hub reports it. `state_for_phone()` already emitted `"webrtc": webrtc_port > 0`; the page now
reads that (`remoteWebrtc`, via `webrtcOffered()`) instead of carrying its own answer. The practical
gain over flipping the constant to `true`: a hub whose 8555-8574 range is entirely taken reports
`false` and the phone hides the entry rather than offering a tile that stays black. The gate asserts
the constant is gone (`WEBRTC_RELAY not in center`) so it cannot creep back.

### 1.2 The global Video setting became the default for a per-tile picker

The 2026-09-02 branch added one global `Video: auto / WebRTC / relay` dropdown that set the mode for
every camera tile. Since then, `89bdf84170` added a **per-tile** mode picker — the badge in each
tile's corner, backed by `modePrefs` in `localStorage` — which is strictly more expressive.

Keeping both as peers would have meant two controls fighting over one URL parameter. Instead the
global setting is now the *default* that `autoStreamMode()` consults, and a single tile can still
override it. One list of modes (`streamModes()`), one source of truth for whether WebRTC is offered.

### 1.3 The fallback is bounded at 2.5 s, not 8 s

The old `rtctimeout` default was 8000 ms. The relayed stream plays from the first moment either way,
so this deadline only decides how long a dead peer connection keeps an ICE agent and a second decode
alive behind a picture the viewer can already see — but it is also how long a tile's badge lies about
which path it is on. On the LAN a working peer connection is up in well under a second, so 2.5 s only
ever expires on a path that was never going to work. The gate pins it to the 2-3 s band.

The old block also only recognised `mode=webrtc,mse`. The page now asks for `webrtc,mp4,mjpeg` (mp4
and mjpeg being what the iOS/ManagedMediaSource work settled on), so the give-up logic was rewritten
to treat "WebRTC plus any other mode" as the both-at-once case.

### 1.4 The tile says where it landed

New, not on the old branch. `player.html` posts `snorcaMode` to the parent once it knows which path
it settled on, using the same same-origin convention as the existing `snorcaAspect`. The tile's badge
then reads `Auto · WebRTC` or `Auto · MP4` — the truth, rather than the first mode that was asked for.
It relabels **in place** rather than rebuilding the cell: a rebuild would restart the stream it just
settled on.

### 1.5 Tailscale Serve chooses the relay by itself

WebRTC media is UDP. Through Tailscale directly or over DERP it is peer-to-peer and works; through
**Tailscale Serve** it cannot, because Serve is an HTTPS reverse proxy that forwards TCP to
`127.0.0.1` and has no way to carry UDP. The old branch knew this in prose but did nothing about it,
so a phone behind Serve on Auto would try WebRTC on every tile and eat the fallback timeout each time.

`serveOrigin()` detects it from the origin the page was loaded from (`https:` on a `.ts.net` host) and
Auto stays on the relay there. Choosing `Video: WebRTC` by hand still attempts it — a phone *on* the
tailnet rather than behind Serve does reach the media port, and the user forcing it is a legitimate
case. The Video setting's help text says which of the two applies from the current origin, so the
explanation is where the user meets it.

## 2. Conflict resolutions

Five conflicts across four files. Each was re-read rather than resolved by taking a side.

| Where | What collided | Resolution |
|---|---|---|
| `RemoteHub.cpp`, `start_go2rtc()` | Current tree had a long comment arguing *against* a WebRTC listener (`webrtc: listen: ""`, "deliberately"); branch replaced it with the port-binding config. | Took the branch's config, **rewrote the comment**. The old reasoning was not wrong, it was stale: it said no port was opened and no firewall was asked, and both now exist. The half that is still true — Serve cannot forward UDP — is kept and now points at §1.5 and the page code that acts on it. |
| `RemoteHub.cpp` (3 later commits) | — | Applied cleanly. The predicted admin-listener conflict landed only in the config hunk above; `firewall_state()` and `state_for_phone()` merged without collision. |
| `hub.html` | Branch changed the Tailscale toggle to `Turn off` / `dim`; current tree has `Disable` / `warn`. Branch also inserted `videoLine(body)`. | Kept the **current tree's** wording and class (the branch's was the older text, not an intentional change), took the `videoLine(body)` insertion. Purely additive otherwise. |
| `player.html` | Branch added the WebRTC give-up block; current tree added the `snorcaAspect` reporter. Same region, unrelated work. | Kept **both**, aspect reporter first. Then rewrote the give-up block per §1.3/§1.4. |
| `stream_center.html`, topbar | Branch had the old single-row topbar; current tree has the two-row `tbrow` layout. Branch inserted `#videobox`. | Kept the **two-row layout** and inserted `#videobox` before `#layoutbox`, where the branch had put it. Taking the branch's side here would have silently reverted the topbar-wrapping fix. |
| `stream_center.html`, tile src | Branch: `'&mode=' + remoteVideoMode()`; current tree: `'&mode=' + encodeURIComponent(streamMode(h.id))`. | Kept the **current tree's** per-tile call — see §1.2. The branch's global function survives as the default underneath it. |
| `2026-09-02-remote-access-design.md` | Branch added a Windows Firewall paragraph; current tree added a token-hygiene paragraph. | Kept **both**; they are unrelated. |

## 3. Quality (Auto / High / Medium / Low)

Persisted per phone (`snorca_remote_quality`), beside the Video setting, and expressed in go2rtc's
own terms: the phone asks for a **stream name**, so Medium and Low open `<name>_med` / `<name>_low`
and High opens the source stream unchanged.

### 3.1 There is no bundled ffmpeg, and that is the limit

go2rtc re-encodes by shelling out to an **ffmpeg binary** (its `ffmpeg:` source scheme); it carries no
codec of its own. Checked, and stated plainly because it decides what this feature can do:

- `resources/tools/go2rtc/` contains `go2rtc.exe` and `LICENSE-NOTE.txt`. Nothing else.
- No `ffmpeg.exe` anywhere in the build tree or in the installed live build.
- `ffmpeg` is not on this PC's `PATH`.
- `RemoteHub.cpp` contains no reference to ffmpeg; the only mention in `src/` is `MediaPlayCtrl.cpp`
  (the desktop Bambu live view), which is **not** repurposed — taking it would make a second feature
  depend on a binary that one already relies on.

So `quality_variants()` returns empty unless an ffmpeg is found (beside `go2rtc.exe`, or on `PATH`),
and the hub **reports what it actually registered** rather than registering streams go2rtc would fail
to start. A variant that 404s is a black tile, which is worse than not offering the step. The page
hides steps the hub cannot deliver, and an unavailable step falls back to the source name.

### 3.2 What works today, with no ffmpeg

- **The Bambu MJPEG relay drops frames.** `BambuCamRelay` forwards whole JPEGs byte-for-byte between
  `FF D8` and `FF D9` markers — there is no decoder to lower a quality factor with — but MJPEG has no
  inter-frame coding, so bitrate is very nearly linear in frame rate and dropping frames is a real,
  proportional saving. `?fps=` (Low 5, Medium 10, High every frame), paced on the wall clock because
  the printer's own rate is neither fixed nor known. The hub's `/bambu` route forwards the parameter,
  clamped at both ends since it arrives from the phone. This is the common camera on this fork, so it
  is the case that matters most.
- **The source-selection path is fully live.** Dropping an `ffmpeg.exe` beside `go2rtc.exe` lights up
  the transcoded variants with no page change and no rebuild.

### 3.3 What the transcode variants would need

An ffmpeg binary reachable by go2rtc. Then `variant_src()` registers, per source stream:

- `_med`: `ffmpeg:<name>#video=h264#hardware#width=1280`
- `_low`: `ffmpeg:<name>#video=h264#hardware#width=640#raw=-r 10`

pointed back at the stream the hub already registered, so a variant is a re-encode of *our* stream
rather than a second connection to the printer — the camera still sees exactly one consumer, which is
what `866e28267b` went to some trouble to achieve. `#hardware` lets go2rtc pick a GPU encoder
(dxva2/cuda here) and fall back to software itself. go2rtc starts the ffmpeg process lazily, only when
a viewer opens the variant, so an unused `_med`/`_low` costs nothing.

Bundling one would mean shipping an LGPL/GPL ffmpeg build and the licence obligations that carry — a
decision for the owner, not this branch.

### 3.4 Auto

Low on a Serve origin (§1.5 — the slow case this setting exists for), High on the home network,
Medium otherwise. A tile whose first seconds buffer steps down once by itself: `player.html` posts
`snorcaStall` after two `waiting` events inside 8 s (one is normal at start-up while the first
keyframe arrives, so one is deliberately not enough). An explicit choice is the user's and is never
overridden.

The badge names the step when it is not the source: `Auto · WebRTC · Low`.

## 4. Installer

`cmake/nsis/SnapmakerURLProtocols_install.nsh` / `_uninstall.nsh`, and the standalone `installer.nsi`,
gain a program-bound rule beside `EdgeSlicer` (TCP 13640) and `EdgeSlicer LAN discovery`
(UDP 2021,1990):

```
name="EdgeSlicer WebRTC video"  dir=in  action=allow
program="$INSTDIR\resources\tools\go2rtc\go2rtc.exe"
protocol=UDP|TCP  localport=8555-8574  profile=private,domain
```

A range rather than one port because the hub takes the first free one in 8555-8574; both protocols
because UDP is the media and TCP is go2rtc's ICE-TCP fallback for networks that drop UDP;
`private,domain` because a phone reaches this over the home network or the tailnet, never a public
one. Both rules share one name, so one delete removes them on uninstall.

**There is no portable-build or first-run-prompt path to cover.** Checked: the hub starts silently on
every launch (`GUI_App::start_remote_access`, unguarded), there is no `*_asked` / first-run config key
anywhere, and there is no portable-build concept in `src/` or `cmake/`. `RemoteHub.cpp`'s
`firewall_query()` is read-only by design — adding a rule needs administrator rights and doing it
silently would be wrong — so a zip or dev run still relies on Windows' own prompt, and the hub's
firewall note is what covers that case. That note is also why pre-creating the rule matters: if the
user dismisses Windows' prompt, Windows writes a **block** rule, which wins over any allow rule and is
the `blocked` state `firewall_query()` reports ahead of the others.

## 5. Gate

`test_webrtc.py` was refreshed rather than reused: the 2026-09-02 version read
`C:\Dev\SnapmakerOrcaNext\resources\web\orca` and assumed an already-running hub. It now **starts its
own hub** from the install it is given, on its own data dir, and quits it again — so it can run
against a scratch instance without colliding with anything.

What it covers beyond the original 33 checks: the admin-listener split (`/hub/*` answers on
`admin_port` and 404s on the phone-facing port), the quality variant reporting and per-host `qnames`,
the 2-3 s fallback bound, `snorcaMode` / `snorcaStall`, the absence of `WEBRTC_RELAY`, Serve-origin
detection, and that `/state`'s existing fields (`hosts`, `active`, `lan_url`, `remote_url`, `ips`) have
not moved — other gates read them.

Wired into `gate_all.sh` as a `webrtc` section and into `gate_smart.sh`'s file map:
`RemoteHub|BambuCamRelay|resources/web/orca/(stream_center|player)\.html → webrtc`.

## 6. Measurements and click-tests

<!-- MEASUREMENTS -->

## 7. Notes for the merge

- Nothing in `/state` was renamed. `webrtc`, `video_note`, `quality`, `quality_mjpeg` and the per-host
  `qnames` are additions; every field other gates read is untouched.
- `resources/web/orca/*.html` are CRLF in this repo. Edits must preserve that (a whole-file LF rewrite
  shows up as a total-file diff and loses review).
- The `feat/webrtc-video` branch can be deleted once this lands; nothing in it is unrepresented here
  except the two comments deliberately rewritten in §2.
