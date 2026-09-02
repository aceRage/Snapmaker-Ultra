# Hidden service mode - how the phone-serving slicers run without a window

Date: 2026-09-02 · Branch: feat/ultra-preferences · Companion to the implementation plan in `docs/superpowers/plans/2026-09-02-hidden-service-mode.md` and the phone app design in `2026-09-01-phone-stream-access-design.md`.

## 1. What it is

The phone app drives full slicer instances on the PC (open a file, arrange, choose presets, edit process settings, slice, look at the toolpath preview). Before this work every one of those instances was a normal window on the desktop, stealing focus and cluttering the taskbar. In hidden service mode an instance runs with its main window never shown: it still has a live OpenGL context (thumbnails and preview frames render off-screen into a framebuffer), still slices, and still answers every phone route; it simply has no window until a person asks for one.

- **Hidden, not minimised.** A minimised window still paints and can be restored by accident; a hidden one is only reachable through the hub tray icon, the hub page and the phone.
- **The hub is the taskbar.** The tray icon's *Slicer windows* submenu lists every instance with *(hidden)* and *(needs attention)* markers and offers Show / Hide / Quit for each.
- **Phone-started slicers are hidden by default.** `POST /r/<token>/api/instances/open` spawns a hidden instance; add `?visible=1` to get a window. The hub page's *Open a new slicer window* stays visible; *Open a hidden slicer* is the hidden twin.
- **Closing a hub-managed window hides it** instead of quitting (the project keeps living for the phone). *File > Quit*, the tray's *Quit*, the hub page's *Quit* and `/api/quit` really quit.

## 2. Turning it on

| Way | Effect |
|---|---|
| Phone: *New* / *Open* on the Prepare tab | Always hidden (the hub sets `SNORCA_HIDDEN=1`). Use *Show on PC* in the instance bar to get the window. |
| Hub page or tray: *Open a hidden slicer* | Hidden. |
| Hub page or tray: *Open a new slicer window* | Visible. |
| Preferences > Ultra > Phone access > *Start hidden* (`start_hidden`) | Slicers started by hand - double-click, file association, command line - start hidden. Off by default. |
| `snapmaker-orca --hidden [file]` | Hidden, one launch. |
| `SNORCA_HIDDEN=1` / `0` in the environment | Forces either way; wins over the flag and the preference (this is what the hub uses). |

Precedence: environment > `--hidden` > preference. A hidden instance never shows the splash screen and never shows the main frame from `recreate_GUI` (language or theme change) either.

## 3. Getting a window back

- **Tray icon** > Slicer windows > *n · title (hidden)* > Show.
- **Hub page** (`http://127.0.0.1:13640/hub/`): Show / Hide per instance.
- **Phone**: *Show on PC* / *Hide on PC* in the Prepare tab's instance bar.
- **API**: `POST /api/window?show=1|0` on the instance (through the hub: `POST /r/<token>/i/<pid>/api/window?show=1`, or loopback-only `POST /hub/instances/<pid>/window?show=1`). `GET /api/window` reports `{hidden, iconized}`.

Showing raises the window to the foreground; hiding puts it away without touching the project.

## 4. What happens to dialogs nobody can answer

Every modal dialog in the process passes through a `wxModalDialogHook` installed before the app starts (`RemoteAccess::install_dialog_policy`). The policy has three modes (`RemoteAccess::dialog_mode()`):

| Mode | When | Rule |
|---|---|---|
| Interactive | a window is shown and no phone request is running | dialogs behave normally |
| Request | a phone request is executing on the GUI thread | affirmative answer (Yes / OK) for questions the request itself provoked, e.g. "the plate has unsliced changes, slice anyway?" |
| Background | hidden instance, nothing requested | the do-nothing answer (No / Cancel); destructive or input-needing dialogs (file, directory, colour, font, print dialogs, text entry) are cancelled and the instance raises attention |

Every auto-answer is logged (`/api/info` → `attention[]`, with the dialog class, title and answer) so the phone can show what was decided. Call sites that would block start-up or destroy work have explicit guards on top of the hook: the first-run wizard, the privacy prompt, the mandatory-update dialog, the network plug-in install and update prompts, the restore-unsaved-project prompt, the mid-slice memory warning (the slice is stopped rather than gambled), the TLS certificate-store question (accepted, not remembered), the "unsaved changes" dialog (transfer or discard per the request) and the system-preset save dialog (saves as *name - Custom*).

## 5. Attention

When a hidden instance meets something only a person at the PC can settle it **raises attention**: `needs_attention` becomes true with a reason and a kind (`dialog`, `timeout`, `manual`), the window is shown so the person sees it, the hub tray tooltip counts it, the tray submenu and the hub page mark the instance *(needs attention)*, and the phone's Prepare tab shows a red banner with the reason and a *Dismiss* button.

- `dialog`: a cancelled input-needing dialog.
- `timeout`: a phone request waited more than 15 s (60 s for slice) for the GUI thread; the request itself answers 503. This kind clears itself once a later request completes (the GUI heartbeat notices).
- `manual`: a guarded call site (wizard, mandatory update, plug-in install, restore prompt, memory warning).

Clearing: phone *Dismiss*, hub page *Dismiss* (`POST /hub/instances/<pid>/attention/clear`), or `POST /api/attention/clear` on the instance. Hiding the window again does not clear it.

## 6. Watchdog

A `wxTimer` heartbeat on the GUI thread stamps `/api/info` → `gui_stall_ms` and `modal_open`; `run_on_main` refuses to wait forever, so a stuck GUI thread degrades to 503s with a `timeout` attention instead of a hung phone page. `SNORCA_DEBUG_ROUTES=1` enables `/api/debug/sleep?ms=`, `/api/debug/modal` and `/api/debug/file` for exercising the policy (never on by default).

## 7. Files

| File | Role |
|---|---|
| `src/slic3r/GUI/RemoteAccess.cpp/.hpp` | per-instance loopback API, `hidden()`/`set_hidden()`, dialog policy, attention, heartbeat, `run_on_main` |
| `src/slic3r/GUI/RemoteHub.cpp` | hub process: tray menu, instance files, `/hub/*` routes, `spawn_slicer(file, hidden)` |
| `src/slic3r/GUI/GUI_App.cpp` | hidden decision, warm-up (`SNORCA_HIDDEN_WARMUP=none\|gl\|full`), start-up guards, `start_remote_access()` |
| `src/slic3r/GUI/GLCanvas3D.cpp` | `ensure_gl_ready()` (context + `init()` + one empty ImGui frame on a never-shown canvas), off-screen preview rendering |
| `src/slic3r/GUI/MainFrame.cpp` | close-to-hide for hub-managed instances, `request_quit(discard)` |
| `src/slic3r/GUI/Preferences.cpp` | Ultra > Phone access > *Start hidden* |
| `resources/web/orca/hub.html` | Show / Hide / Quit / Dismiss per instance, *Open a hidden slicer* |
| `resources/web/orca/stream_center.html` | instance bar: *Show on PC* / *Hide on PC*, attention banner |

## 8. Testing

Helpers live in `C:\Users\acesa\AppData\Local\Temp\snorca_hubtest\` (a short path: the session scratchpad exceeds MAX_PATH and the slicer then fails to load files). `run_hub_app.py [file] [--new] [--hidden|--visible]` starts an instance from the build tree with phone access on; `test_hidden.py` (Stage 1: registration, show/hide/quit), `test_stage2.py` (OpenGL on a never-shown window: thumbnails byte-identical to a visible instance, preview frames, idle CPU), `test_stage3.py` (dialog policy, attention, watchdog, synchronous slice) and `test_stage4.py` (fresh data directory, the preference and its precedence, hub page attention relay) are the stage gates; `test_preview2.py` is the visible-instance regression.
