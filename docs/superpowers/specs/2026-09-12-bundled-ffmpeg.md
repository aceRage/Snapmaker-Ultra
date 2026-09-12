# A bundled ffmpeg, and the camera Quality steps it turns on (2026-09-12)

Branch `feat/bundled-ffmpeg`, from `feat/ultra-preferences` at `944457b8bb`.

`feat/webrtc-video-2` shipped the per-phone **Quality** setting (Auto / High / Medium / Low) with
half of it inert. Its §3.1 said so plainly: go2rtc re-encodes by shelling out to an **ffmpeg
binary** and carries no codec of its own, so the transcoded `_med` / `_low` stream variants were
registered only when the user happened to have an ffmpeg on `PATH`. On a stock install Quality was
reduced to the Bambu MJPEG frame-rate knob — real, but only for one camera type.

This bundles one. The owner's decision; what follows is which build, why that licence, and what had
to change in the hub to make go2rtc actually use it.

## 1. The build

| | |
|---|---|
| Source | [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds) |
| Release tag | `autobuild-2026-09-12-13-12` |
| Asset | `ffmpeg-N-126523-g884590dd4a-win64-lgpl.zip` |
| Asset sha256 | `f418bfa41340c636f193c1fcd9d48b1d65f833c90ed71bb8e8356671fa79b29e` |
| `ffmpeg.exe` sha256 | `ff2adedd74aca004452d611b27df7a4806087891c3102078654034b4861d9182` |
| `LICENSE.txt` sha256 | `da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768` |
| Version string | `N-126523-g884590dd4a-20260912` (ffmpeg commit `884590dd4a`) |
| Licence | **LGPL-3.0** (`--enable-version3`, no `--enable-gpl`) |
| Kind | static, x64, one file (128 MiB; 53 MiB compressed) |

Pinned to the **dated autobuild tag**, not to `latest`. BtbN's `latest` is a rolling tag whose
assets are replaced on every build, so pinning to it would pin nothing: the same URL would fetch a
different binary next week and the sha256 above would stop matching. The dated tag is immutable.

Staged (outside the repo, as with FlashNetwork and UltraNet) at:

```
C:\Users\acesa\AppData\Local\Temp\snorca_hubtest\ffmpeg_out\ship\
    ffmpeg.exe
    FFMPEG-LICENSE.txt        (the build's LICENSE.txt, renamed so it is unambiguous
                               once it sits beside go2rtc's own note)
```

A **static** build deliberately: a shared one would scatter a dozen `av*.dll` into
`resources/tools/go2rtc/`, where they would sit next to `go2rtc.exe` and be mistaken for its
dependencies. One file is also one thing to verify. The CMake rule still carries a `*.dll` glob so
a future shared build would work, but ships nothing extra today.

**Size, and the one thing worth the owner's attention.** The static exe is 128 MiB uncompressed and
**53 MiB deflate-compressed** (measured, not estimated). That is the largest single file we ship,
and it grows both packages by roughly that compressed figure. Two things to weigh if that matters:

* a **shared** LGPL build is smaller in total, at the cost of the DLL clutter above;
* we already ship only `ffmpeg.exe` — `ffplay`/`ffprobe` from the same zip are discarded, so 128 MiB
  *is* the ffmpeg-only figure. Trimming further would mean building ffmpeg ourselves with
  `--disable-everything` plus just what this feature touches (h264 in, libopenh264 out, mp4/rtsp),
  which typically lands under 20 MiB.

Neither is done here: correctness first, and the size is not a regression anyone has complained
about. Flagged so the choice is deliberate rather than inherited.

## 2. Licence reasoning

EdgeSlicer is **AGPL-3.0**. Two questions had to be answered, and the brief asked for the
conservative choice where one existed.

**Which ffmpeg licence.** A GPL-3.0 ffmpeg would have been *compatible* — the AGPL-3.0 and GPL-3.0
are explicitly compatible in both directions, and ffmpeg is invoked here as a separate process over
a pipe, which is not linking at all. So a GPL build would have been defensible. **LGPL is what we
ship anyway**, because it is strictly weaker and costs us nothing: nothing in this feature needs a
GPL-only component (see below), so taking the GPL build would have added copyleft obligations to the
package in exchange for nothing.

The practical consequence of LGPL-with-no-linking: we redistribute an **unmodified** binary as a
separate executable. The obligations we meet are the ones that attach to redistribution —

* the full LGPL-3.0 text ships beside the binary as `FFMPEG-LICENSE.txt`;
* `resources/tools/go2rtc/LICENSE-NOTE.txt` records the exact release tag, asset, ffmpeg commit,
  configure flags and where to get the corresponding source;
* `CopyrightsDialog::fill_entries()` lists **FFmpeg (LGPL build)** in the About box — and now also
  **go2rtc**, which was bundled since the camera relay landed but had never been listed there.

**The encoder, which is where the licence actually bites.** The LGPL build is configured
`--disable-libx264 --disable-libx265`: x264 and x265 are GPL, so no LGPL build can contain them.
The brief's fallback question was whether any LGPL build offers a usable H.264 encoder. It does:

```
libopenh264   OpenH264 H.264 / AVC (Cisco, BSD-2-Clause)   <- software, always available
h264_amf / h264_nvenc / h264_qsv / h264_mf / h264_vaapi ...  <- hardware, GPU-dependent
```

`libopenh264` is a real software H.264 encoder under a BSD-2-Clause licence, present in this build
and verified encoding (§5). **So no GPL fallback is needed and none was taken.**

## 3. What had to change in the hub

### 3.1 go2rtc's default h264 template is libx264, and would have failed

go2rtc 1.9.14's built-in `h264` template is:

```
-codec:v libx264 -g:v 30 -preset:v superfast -tune:v zerolatency -profile:v main -level:v 4.1
```

Every one of `libx264`, `-preset:v` and `-tune:v` is fatal against the bundled build — the codec
does not exist in it, and `libopenh264` has no `-preset`/`-tune` options at all. Left alone, every
variant would have died the moment a viewer opened it, and reached the phone as a black tile.

go2rtc lets the template be replaced, so `start_go2rtc()` writes our own:

```yaml
ffmpeg:
  bin: "<install>/resources/tools/go2rtc/ffmpeg.exe"
  h264: "-codec:v libopenh264 -profile:v constrained_baseline -rc_mode bitrate -bf 0"
```

`bin:` is set explicitly — the point of bundling is not to depend on what is on `PATH`, and an
ffmpeg on `PATH` could be any build with any flags. Naming our own exe makes the template above a
statement about a known binary.

The encoder settings, and the `-preset ultrafast -tune zerolatency` equivalents the brief asked for:

| Asked for | With libopenh264 | Why |
|---|---|---|
| `-tune zerolatency` | `-profile:v constrained_baseline`, `-bf 0` | What zerolatency mostly buys is *no B-frames*: nothing is held back to reorder, so a frame is emitted as soon as it is encoded. Constrained Baseline forbids B-frames by construction. It is also the profile every phone decoder handles and what WebRTC wants. |
| `-preset ultrafast` | (no equivalent, none needed) | libopenh264 has no speed/quality presets. It is already fast enough: §5 measures 4.7% of one core for 720p. |
| bitrate control | `-rc_mode bitrate` with `-b:v`/`-maxrate` | libopenh264 defaults to `quality` mode, which lets the bitrate wander — the opposite of what a Quality step is for. |

### 3.2 The RTSP listener had to come back on (loopback only)

**This is the bug that would have shipped a black tile**, and it was invisible in review: the config
looked right and the variants registered fine. Only *requesting* one showed it.

go2rtc pipes its `ffmpeg:` sources back through **its own RTSP listener**. The hub had
`rtsp: listen: ""` — correct while nothing needed it — and with it off every variant fails the
instant a viewer connects:

```
ERR internal/mp4/mp4.go:107 > error="streams: exec: rtsp module disabled"
```

So the listener is now bound **when, and only when, there is an ffmpeg to need it**, to
`127.0.0.1:<random free port>`:

* loopback, so it is not reachable from the LAN or the tailnet — the security property the empty
  listener was protecting is kept intact;
* a random free port rather than go2rtc's default 8554, so two hubs on one PC cannot collide;
* still `""` when there is no ffmpeg, because then nothing uses it.

`test_webrtc.py`'s old assertion ("rtsp and srtp listeners are still empty") was updated rather than
deleted: it now asserts srtp is empty and rtsp is **either empty or loopback-only**, which is the
property actually worth pinning.

### 3.2b A variant source may not contain a space

The second bug that would have shipped a black tile, and like §3.2 it was invisible in review.

go2rtc refuses a stream registration whose source string contains a space:

```
400 streams: source with spaces may be insecure
```

So the natural way to write the extra encoder arguments —
`#raw=-r 10 -b:v 600k -maxrate 600k -g:v 20` — registers **nothing at all**. The variant then does
not exist, the phone asks for a name that 404s, and the tile is black.

Nothing in the hub would have told anyone: the registration PUT is fire-and-forget on a detached
thread with no error path, so the 400 reaches no log and no user. It surfaced only by driving a
real go2rtc with the exact strings the hub writes.

Each argument is therefore its own `#raw=` segment, one token per segment:

```
ffmpeg:<name>#video=h264#width=854#raw=-r#raw=10#raw=-b:v#raw=600k#raw=-maxrate#raw=600k#raw=-g:v#raw=20
```

go2rtc accepts this and builds the identical ffmpeg command line. The gate asserts the absence of
spaces directly, so the readable-but-broken form cannot come back.

### 3.3 The variants, tuned

`variant_src()`, per the brief's targets:

| Variant | Resolution | Rate | Bitrate | Keyframe |
|---|---|---|---|---|
| `_med` | 1280x720 | 15 fps | 1.5 Mbps | `-g:v 30` = 2 s |
| `_low` | 854x480 | 10 fps | 0.6 Mbps | `-g:v 20` = 2 s |

written as one `#raw=` segment per token (§3.2b):

```
ffmpeg:<name>#video=h264#width=1280#raw=-r#raw=15#raw=-b:v#raw=1500k#raw=-maxrate#raw=1500k#raw=-g:v#raw=30
ffmpeg:<name>#video=h264#width=854#raw=-r#raw=10#raw=-b:v#raw=600k#raw=-maxrate#raw=600k#raw=-g:v#raw=20
```

Two changes from what `feat/webrtc-video-2` sketched. **854 rather than 640** for Low: Low is meant
to be watchable, and at 0.6 Mbps a real 16:9 480p costs little (`#width` alone keeps the aspect
ratio). And the keyframe interval is set per variant to ~2 s *of that variant's own frame rate* —
30 at 15 fps, 20 at 10 fps — so a joining viewer waits at most 2 s for its first picture without
spending bitrate on more IDRs than that.

Each variant re-encodes **the stream the hub already registered**, not a second connection to the
printer, so the camera still sees exactly one consumer — preserving what `866e28267b` went to
trouble to achieve.

### 3.4 Hardware encoding is off by default

`#hardware` (which `feat/webrtc-video-2` had sketched) is **not** in the strings above. go2rtc would
pick a GPU encoder, and where one works it is cheaper — but it fails in ways software encoding does
not: a headless or RDP session with no GPU, a driver that refuses a second encode session, an
encoder already busy with a game. And it fails as a *black tile*, not as an error the user can see.
Against a measured 4.7% of one core (§5), that is not a trade worth making by default.

**To enable it:** add `#hardware` to the two strings in `variant_src()` in `RemoteHub.cpp` and
rebuild. go2rtc then tries dxva2/cuda/qsv and falls back to software by itself. The bundled build
carries `h264_nvenc`, `h264_qsv`, `h264_amf`, `h264_mf` and `h264_vaapi`, so whichever GPU is
present is covered.

### 3.5 Idle cost is zero

go2rtc starts the ffmpeg process **lazily** — only when a viewer actually opens the variant — and
kills it when the last consumer goes. Verified directly rather than taken from the docs:

```
registered, nobody watching   ffmpeg.exe count = 0
viewer connected              ffmpeg.exe count = 2   (source + transcode)
1 s after viewer left         ffmpeg.exe count = 0
```

So an unwatched `_med`/`_low` costs nothing at all. The gate asserts all three.

## 4. Packaging

`CMakeLists.txt` gains `FFMPEG_BIN_DIR`, mirroring the `FLASHNETWORK_BIN_DIR` block beside it:

```
-DFFMPEG_BIN_DIR=C:/Users/acesa/AppData/Local/Temp/snorca_hubtest/ffmpeg_out/ship
```

It installs `ffmpeg.exe` into `<install>/resources/tools/go2rtc/` — beside `go2rtc.exe`, which is
where `ffmpeg_path()` looks first and what `start_go2rtc()` names in `ffmpeg: bin:` — with
`FFMPEG-LICENSE.txt` next to it. **Portable zip and NSIS installer both get it from this one rule**,
because the zip is the install tree and the installer is built from that same tree. Without the
option the build is unchanged and Quality falls back to the MJPEG knob, which is what a tree without
the staged binary (CI, another contributor) gets.

`ffmpeg_path()` still falls back to `PATH` when the bundled exe is missing, so an install that lost
it, or a platform we do not ship one for, degrades rather than breaks.

## 5. CPU cost, measured on this PC

**12th Gen Intel Core i7-12700K, 20 logical cores.** One Medium transcode (1280x720, 15 fps,
1.5 Mbps, libopenh264 software), sampled over a 20 s steady-state window with `-re` so it runs at
real time rather than as fast as it can:

```
cpu_seconds     = 0.94 over wall 20.01 s
% of one core   = 4.7
% of total CPU  = 0.23
```

So roughly **1/20th of one core per Medium viewer**, and zero when nobody is watching. Low costs
less again (half the frame rate, a third of the pixels). This is the number §3.4 weighs hardware
encoding against: the GPU path would save a fraction of a percent of this machine while adding a
class of failure that shows as a black tile.

## 6. Gate

`test_quality.py`, wired into `gate_all.sh` (as `quality`) and into `gate_smart.sh`'s map
(`RemoteHub.cpp` or `resources/tools/go2rtc` → `quality`). Self-contained: its own install, its own
data dir, its own synthetic source; it never touches the live hub, `%APPDATA%` or a printer. It
**skips** (not fails) on an install built without `-DFFMPEG_BIN_DIR`, the same way the webrtc gate
skips a build without WebRTC.

What it asserts:

1. the package ships `ffmpeg.exe` beside `go2rtc.exe`, with `FFMPEG-LICENSE.txt`, and the note
   records the build;
2. it is the LGPL build — `libopenh264` present, `libx264` **absent**, not configured
   `--enable-gpl`;
3. the generated `go2rtc.yaml` names the bundled exe in `ffmpeg: bin:`, carries a `h264:` template
   that uses libopenh264 and carries **no** x264-only `-preset`/`-tune`, and binds `rtsp` to a
   loopback port (§3.2);
4. `/state` reports both `med` and `low` variants;
5. a real pull of the low variant returns a stream whose **SPS says ≤ 854x480**, and is genuinely
   downscaled from the 1080p source — pulled as **MSE over the hub's own `/api/ws` tunnel**, the
   path the phone actually uses, not go2rtc's `/api/stream.mp4` (which `allow_paths` deliberately
   does not expose, so a gate using it would be testing a door the product keeps shut and would
   read the resulting 404 as a transcode failure);
   the variant sources it registers carry **no spaces**, asserted directly (§3.2b);
6. the transcode is lazy — 0 ffmpeg processes idle, ≥1 while a viewer is connected, back to 0
   within ~10 s of the viewer leaving.

`test_webrtc.py`'s rtsp assertion was updated as described in §3.2.

**Building this branch.** Nothing here changes the build's memory profile, but getting a complete
build on this PC took several attempts, for two reasons worth recording.

1. **Run the build detached.** Six builds ran on this machine today. A `cmake --build` launched
   through an agent's shell tool is killed with its whole msbuild process tree when that tool's
   timeout expires, and the signature is misleading: MSBuild reports **exit -1 with no error line
   at all**, part-way through `libslic3r`, looking exactly like a compiler crash. Launch it with
   PowerShell `Start-Process ... -RedirectStandardOutput ... -PassThru` instead, and poll the log.
   Note also that `-1` is **not** caught by `if errorlevel 1`, so a script testing only that falls
   through to `cmake --install` and fails there on a missing `EdgeSlicer.dll` - which reads as an
   install bug rather than a build that never finished. Test `if not "%ERRORLEVEL%"=="0"`, and
   check the exe exists before installing. A half-built tree also leaves `libslic3r.pdb` without
   `libslic3r.lib`, after which the GUI targets are silently skipped and the build "succeeds"
   having produced no exe.

2. **Take the build lock.** Wrap the launch in `sh <snorca_hubtest>/with_build_lock.sh <cmd>` so
   it does not contend with other agents' builds. Under contention this build also hit genuine
   `C3859`/`C1076` (PCH virtual-memory exhaustion) at `/m:4` and `/m:2`; the lock is what actually
   fixes that, `/m:1` only narrows the window.

3. **sccache as `CLToolExe` did not work here.** Configuring with
   `-DCMAKE_VS_GLOBALS="CLToolExe=sccache.exe;CLToolPath=<WinGet Links>;..."` makes MSBuild invoke
   sccache *as* the compiler, and sccache 0.17.0 then fails every single compile with
   `sccache: error: failed to execute compile / caused by: cannot find binary path` - it cannot
   locate the real `cl.exe` from that launch. The build fails immediately, in the dependencies,
   before any project of ours. Reverting needs `-UCMAKE_VS_GLOBALS`: simply dropping the flag from
   the configure line leaves the value sitting in `CMakeCache.txt` and it keeps applying.

4. **`cmake --install` wants all four paho static libs, which our targets do not build.** Building
   only `Snapmaker_Orca` + `Snapmaker_Orca_app_gui` produces `paho-mqtt3as-static.lib` alone, and
   the install stops with `file INSTALL cannot find .../paho-mqtt3a-static.lib` - then, once that
   is built, again on `paho-mqtt3c-static.lib`. Unrelated to this branch: it is the mqtt
   subproject's own install rule, which installs all four. Build them in one go and re-run the
   install:

   ```
   cmake --build . --config Release --target        paho-mqtt3a-static paho-mqtt3as-static paho-mqtt3c-static paho-mqtt3cs-static
   ```

**One trap worth knowing when gating this by hand.** A go2rtc started from the *worktree's*
`resources/tools/go2rtc/go2rtc.exe` holds that file open, and Windows will then fail the build's
install/copy step into that tree — silently, as a killed `cmd.exe` with no error in the log. Run
throwaway go2rtc instances from the **staged** copy or a scratch dir, never from the worktree, and
kill them when finished. `test_quality.py` itself is safe: it drives the hub's own go2rtc out of
the scratch *install*, and its teardown quits only the hub it started.

## 6b. The exec-source startup race (found by the gate, fixed)

The gate caught this and it would have shipped the feature broken: on a normal start the hub
reported `_med`/`_low` variants it had **failed to register**, so every Quality tile would have
gone black.

**A freshly started go2rtc answers its API before it will accept an *exec* source.** For roughly
the first five seconds a PUT of an `ffmpeg:` stream comes back

```
400 streams: source not supported
```

and so does `echo:` - the other exec scheme - while `rtsp:` is accepted immediately. Measured
against a hub-spawned go2rtc: refused at t+0, accepted from t+5 onward, and from then on
indefinitely.

The hub registers streams the moment go2rtc is up. The source stream (`rtsp:`, or the hub's own
`/relay/h264`) is not an exec source, so it always landed; the variants always fell inside the
window. `update_go2rtc_streams()` already retried - and correctly, since a 400 goes to `on_error`
rather than `on_complete`, leaving `ok` false - but three attempts at 1.5 s covers only 4.5 s and
never quite reached the far side. It is now **twelve attempts (~18 s)**, with a logged warning if
it still fails, because a missing variant is otherwise invisible until a viewer opens the tile.

**Two other explanations were tested first and ruled out**, each in isolation against the
hub-spawned process:

* **the environment block** - `CreateProcess` is called with `lpEnvironment = nullptr`, so go2rtc
  inherits ours. Dumped from the live child's PEB: 113 variables, with `SystemRoot`, `PATH`,
  `TEMP`, `TMP`, `ComSpec`, `windir` and `USERPROFILE` all present. Nothing missing that Go's
  `os/exec` needs;
* **the job object** - `ffmpeg:` is accepted under `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, with and
  without `JOB_OBJECT_LIMIT_BREAKAWAY_OK`, and with no job at all (tested by reproducing the hub's
  exact spawn: `CreateProcess` then `AssignProcessToJobObject`). So **no breakaway flag is needed**
  and go2rtc keeps dying with the hub, which is the property that flag would have weakened.

Also ruled out along the way: the source string, the `ffmpeg: bin:` / `h264:` block,
`local_auth` / `allow_paths` / credentials, the `webrtc:` listener and `ice_servers` (including on
a contended 8556), the rtsp loopback listener, registration order, the config file's location, the
working directory, the exe itself (install and worktree copies are byte-identical), and streams
persisted back into the config (go2rtc does write them, but the hub rewrites the file each start).

The gate retries the same way rather than racing the window, and now also asserts that **no
`ffmpeg.exe` from the install survives hub shutdown** - an ffmpeg that escaped the job object would
be an orphan holding the camera with nobody watching.

## 7. Click-tests (owner)

To be run on the emulator against an install of this branch:

1. **Quality Low shows a visibly smaller stream.** Open the Stream tab on the phone, set
   **Quality: Low**, and confirm the picture is visibly softer/smaller than **High** on the same
   tile — the tile is being served `<name>_low` at 854x480 rather than the source.
2. **The badge reads `Auto · MSE · Low`.** With **Video: Auto** and **Quality: Low** on a Serve
   origin, the tile's corner badge names all three: the mode it settled on and the quality step it
   is actually receiving.
3. **Idle costs nothing.** With no tile open, Task Manager shows no `ffmpeg.exe`; opening a Low
   tile starts one; closing it ends it within a few seconds.
