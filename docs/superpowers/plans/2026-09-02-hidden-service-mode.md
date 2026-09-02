# Hidden service mode — phased implementation plan

Date: 2026-09-02 · Branch: feat/ultra-preferences · Status: draft for review (three Opus research passes, read-only, consolidated by the session that runs the phone work)

## 1. Goal

Slicer instances that serve the phone web app run **hidden by default**: no window, no splash, no taskbar button. The PC user can make any instance visible on demand from the hub's tray icon (and the hub page), closing the window only hides it again, and quitting stays explicit (tray, hub page, `POST /api/quit`, File ▸ Quit on a shown window). Everything the phone does today keeps going through the desktop's own code paths (Plater, Tabs, GCodeViewer), so behaviour stays identical to the desktop; only visibility, OpenGL initialisation and dialogs change.

Not in scope: a wx-free headless server (that would mean reimplementing the settings editor, preset selection, object transforms and the preview renderer; the headless CLI roadmap covers slicing only).

## 2. Stages and gates

Each phase is testable on its own build. Do not start the next phase until its gate holds.

| Stage | Delivers | Gate (must all pass) |
|---|---|---|
| 1 — Hidden launch, registration, show/hide | `--hidden` / `SNORCA_HIDDEN`, no splash or Show, API + hub registration independent of the Stream tab, `POST /api/window`, `POST /api/quit`, tray submenu and hub-page buttons, close-to-hide, hub spawns hidden for phone-initiated opens | Hidden instance appears in the hub list with `hidden:true`; `/api/info`, `/presets`, `/settings/process`, `/plates`, `/plates/{i}/layout`, `/objects/transform` answer; Show/Hide/Quit work from tray and hub page; visible launch unchanged. Rendering routes may still fail. |
| 2 — OpenGL on a never-shown window | `GLCanvas3D::ensure_gl_ready()`, warm-up in `post_init`, `set_current_panel` no longer bails, make-current guards at the out-of-render GL sites, route guards | On a hidden instance: every plate thumbnail is non-blank and pixel-identical to a shown instance; slice from the phone; preview info/PNG at several layers, views and zooms; re-slice; second plate; after Show the desktop view is consistent; `test_preview.py` and `test_preview2.py` pass. |
| 3 — Dialogs, blocking and attention | `wxModalDialogHook` policy (Request / Background / Interactive modes), call-site guards for destructive or unhookable dialogs, GUI heartbeat + modal-depth watchdog, `needs_attention` in `/api/info` and the hub list, phone banner, `POST /api/attention/clear` | Every induced dialog in the table answers itself with the expected default and is logged; the memory guard, restore prompt, wizard and force-upgrade never run unattended; a stalled GUI thread or a let-through modal shows the window and the phone banner; the shown-window regression suite is byte-identical to today. |
| 4 — Defaults and polish | Preferences toggle, hub defaults, docs, memory notes, phone "Show on PC" button | Fresh install: phone-initiated opens are hidden by default; a PC-launched slicer is visible; documentation in `docs/` and the hub page explain both. |

## 3. Shared contract (names the three phases must agree on)

The drafts were written independently; use these names when implementing:

- **Launched hidden**: `GUI_App::is_hub_managed()` (Phase 1, `m_hub_managed`). Phase 2's placeholder `is_hidden_instance()` and Phase 3's static `RemoteAccess::hidden()` both mean *this instance is hub-managed and its main frame is currently not shown*; implement Phase 3's `hidden()` as `RemoteAccess::get().hidden()` (the Phase 1 instance method backed by `m_hidden`, which `set_hidden()` keeps in sync with the frame).
- **Show the window**: one code path, `RemoteAccess::api_window(show=1)` (Phase 1). Phase 3's `RemoteAccess::show_window(reason)` calls the same Show/Raise/`set_hidden(false)` sequence on the GUI thread and adds a `note_attention` entry.
- **Registration**: `GUI_App::start_remote_access()` at the end of `on_init_inner` (Phase 1 change 5d/5f) replaces the StreamPanel constructor. This resolves Phase 3's risk 8. The dialog policy hook (Phase 3 change 5) is installed even earlier, at the top of `GUI_Run`.
- **Order at startup**: `on_init_inner` (hidden flag → no splash → MainFrame → no Show → `start_remote_access()`) → first idle → `post_init` (Phase 2 warm-up at the top, then the normal input-file load) → `Plater::priv::update` → `reload_scene` builds the GLVolumes the thumbnails need.
- **Modes** (Phase 3): `Interactive` = window shown; `Request` = inside `run_on_main` (existing `AutoConfirmScope`, affirmative answers); `Background` = hidden and idle (do-nothing answers). A window shown by the watchdog switches to `Interactive`, which is intended.
- **Instance file** `<datadir>/hub/instances/<pid>.json` carries `hidden`, `title`, `path` (Phase 1) and later `needs_attention` (Phase 3 via `/api/info` probing); the tray menu reads the file, never probes.

## 4. Decisions taken in this plan

- Hidden, not minimised: a minimised window still paints and is less predictable; the tray submenu is the "taskbar" entry point. Reconsider only if the user insists on a taskbar button.
- Close-to-hide applies to hub-managed instances only; a normally launched slicer keeps closing as today.
- Phone-initiated opens spawn hidden (`POST /r/<t>/api/instances/open`, `?visible=1` opts out); the tray's "Open a new slicer window" stays visible and gains "Open a new hidden slicer".
- Affirmative defaults only while a phone request is executing; background prompts take the do-nothing branch; destructive or input-needing dialogs are never auto-answered and instead raise attention.
- The slice request will dispatch its toolbar event synchronously inside `run_on_main` so the pre-slice confirms run under the request scope (Phase 3 change 6); keep `wxPostEvent` as the fallback if re-entrancy bites.
- Test files, batch files and projects used for hidden-instance testing live at `C:\Users\acesa\AppData\Local\Temp\snorca_hubtest\` (the session scratchpad path exceeds MAX_PATH).

## 5. Known runtime unknowns to settle first (cheap experiments)

1. `wglMakeCurrent` on the never-shown canvas DC and FBO rendering without a visible window (Phase 2 R1/R2): a Debug build with `HAS_GLSAFE` plus one hidden thumbnail compared byte-for-byte with a shown one.
2. Foreground activation from the hub (Phase 1 risk 1): `AllowSetForegroundWindow(pid)` in the hub before the request; fall back to `AttachThreadInput` if the window only flashes.
3. `wxModalDialogHook` coverage in the vendored wx (Phase 3 test D) - SETTLED 2026-09-02 by inspecting `deps/build/dep_wxWidgets-prefix/src/dep_wxWidgets/src`: `WX_HOOK_MODAL_DIALOG()` is present in `msw/dialog.cpp` (`wxDialog::ShowModal`, the base every fork dialog and `wxTextEntryDialog` end in), `msw/msgdlg.cpp`, `msw/richmsgdlg.cpp`, `msw/filedlg.cpp`, `msw/dirdlg.cpp`, `msw/colordlg.cpp`, `msw/fontdlg.cpp` and `msw/printdlg.cpp`. Every modal in the process reaches the hook; test D becomes a one-time confirmation, not a gate.
4. Idle-loop cost of a bound but never-painting canvas (Phase 2 R5): CPU at idle after warm-up.

## Phase 1 — Hidden launch, registration, show/hide

### Findings

**Startup / where the window is shown**

- `src/slic3r/GUI/GUI_App.cpp:1259-1263` — `GUI_App::GUI_App()` calls `init_app_config()` in the constructor ("app config initializes early because it is used in instance checking"), so `app_config` is already valid at the first line of `on_init_inner()`.
- `src/slic3r/GUI/GUI_App.cpp:2463-2472` — `GUI_App::OnInit()` is a try/catch wrapper around `on_init_inner()`; nothing else.
- `src/slic3r/GUI/GUI_App.cpp:2738-2760` — the splash screen block: `SplashScreen* scrn = nullptr; if (app_config->get("show_splash_screen") == "true") { … scrn = new SplashScreen(…, wxSPLASH_TIMEOUT, 1500, pos); wxYield(); scrn->SetText(…); }`. `scrn` is never destroyed explicitly (timeout style); it is the only splash creation site.
- `src/slic3r/GUI/GUI_App.cpp:3009` — `mainframe = new MainFrame();` (the Plater is created inside, see below), `:3031` `SetTopWindow(mainframe)`, `:3054` `mainframe->topbar()->SaveNormalRect()`, `:3056` **`mainframe->Show(true)`** — the one and only startup Show.
- `src/slic3r/GUI/GUI_App.cpp:3071-3120` — app-level `wxEVT_IDLE` handler; on the first idle it sets `m_post_initialized` and calls `post_init()`. Idle events fire with no visible window (the hub process is proof: a windowless `wxApp` with a `wxTimer`, `RemoteHub.cpp:1319-1366`).
- `src/slic3r/GUI/GUI_App.cpp:3122` — `m_initialized = true;` then `:3147-3149` `return true`.
- `src/slic3r/GUI/GUI_App.cpp:3909-3953` — `recreate_GUI()` builds a second `MainFrame` and calls `mainframe->Show(true)` at `:3953` (language change / GUI rebuild). Phase 1 must honour hidden mode here too or a hidden instance pops a window on a language change.
- `src/slic3r/GUI/GUI_Utils.cpp:129-149` — `on_window_geometry()` on Windows calls the callback **immediately** (not on `wxEVT_SHOW`), so window geometry restore/sanitize is unaffected by never showing the frame.
- `src/slic3r/GUI/MainFrame.cpp:236-237` — `MainFrame::MainFrame() : DPIFrame(NULL, …, BORDERLESS_FRAME_STYLE, "mainframe")`. A `wxFrame` is not shown until `Show()`, so simply skipping `:3056` yields no window **and no taskbar button** on Windows; no style change is needed.
- `src/slic3r/GUI/MainFrame.cpp:711` — `wxGetApp().persist_window_geometry(this, true)`, which (`GUI_App.cpp:4192-4211`) binds a second `wxEVT_CLOSE_WINDOW` handler on the mainframe that saves geometry and calls `event.Skip()`. Bound *after* the main close handler, so wx runs it **first**, then falls through — a veto in the main handler still works.

**Existing flag / env style to copy**

- `src/libslic3r/PrintConfig.cpp:9244-9261` — `CLIMiscConfigDef` defines `hub` (coBool), `hub_token` (coString), `hub_phone` (coBool), each with `cli_params` and a default value.
- `src/Snapmaker_Orca.cpp:1276-1283` — right after `set_temporary_dir(temp_path)`: `if (const ConfigOptionBool* hub = m_config.opt<ConfigOptionBool>("hub"); hub && hub->value) return RemoteHub::run_server(m_config.opt_string("hub_token"), m_config.opt_bool("hub_phone"));`
- `src/Snapmaker_Orca.cpp:1398-1402,1426` — the GUI path fills `GUI_InitParams params` (`src/slic3r/GUI/GUI_Init.hpp:11-26`) and calls `GUI_Run(params)`.
- `src/slic3r/GUI/GUI_Init.cpp:51-56` — `SNORCA_NEW_INSTANCE` env is read with `std::getenv` and forces `gui_single_instance_setting = false` before `instance_check`.
- `src/slic3r/GUI/StreamPanel.cpp:48-57` — `SNORCA_PHONE_ACCESS` is read with `wxGetEnv` and, if set, overrides the saved `stream_phone_access` / `stream_phone_token` app_config keys without writing them.
- `src/libslic3r/AppConfig.cpp:341-348` — the house style for a new default: `if (get("key").empty()) set_bool("key", false);`.

**StreamPanel: what has to move**

- `src/slic3r/GUI/MainFrame.cpp:1275-1277` — `m_stream = new StreamPanel(m_tabpanel); … m_tabpanel->AddPage(…)` inside `MainFrame::init_tabpanel()` (`:1137`), which the ctor calls at `:331`. **Construction is eager**, not lazy on first tab show — so today `RemoteAccess::start()` already happens before `mainframe->Show(true)`.
- `src/slic3r/GUI/StreamPanel.cpp:31-33` — `m_browser = WebView::CreateWebView(this, url); if (m_browser == nullptr) return;` — an **early return before** `RemoteAccess::start()`. If WebView2 is missing/fails, the instance never registers today. This alone justifies the move.
- `src/slic3r/GUI/StreamPanel.cpp:44` — `RemoteAccess::get().start();`
- `src/slic3r/GUI/StreamPanel.cpp:46-60` — reads `SNORCA_PHONE_ACCESS` / `stream_phone_access` + `stream_phone_token`, then `std::thread([token, phone]{ RemoteHub::ensure_running(token, phone); }).detach();`
- `src/slic3r/GUI/StreamPanel.cpp:66-77` — `hub_start` → `ensure_running("", false)` → `window.__hubReady(go2rtc_port, relay_port)`. Purely a reply to a message the page sends; **nothing to do when the tab never opens**.
- `src/slic3r/GUI/StreamPanel.cpp:78-82` — `stream_state:` → `RemoteHub::post_state(state)`; the page pushes the camera list. When the tab never opens no state is pushed, but `HubServer::start()` (`RemoteHub.cpp:1213`) does `m_state = read_file(streams_json_path())`, i.e. the hub restores the last camera list from `<datadir>/hub/streams.json`. **Skippable in hidden mode.**
- `src/slic3r/GUI/MainFrame.cpp:1261-1265` — `m_plater = new Plater(this, this); wxGetApp().plater_ = m_plater;` happens *before* `:1275`, so the Plater exists both at the current call site and at any later point in `on_init_inner`.

**Close / quit**

- `src/slic3r/GUI/MainFrame.cpp:491-546` — the `wxEVT_CLOSE_WINDOW` handler: gizmo-editing veto (`:493-498`), `m_plater->close_with_confirm(check)` veto (`:511-515`), `check_print_host_queue()` veto (`:516-519`), then `m_plater->reset(); this->shutdown(); … event.Skip();`. **Every veto path is guarded by `event.CanVeto()`**, so `Close(true)` skips all confirmations.
- `src/slic3r/GUI/MainFrame.cpp:1015-1089` — `MainFrame::shutdown()`; `:1049` `this->Show(false)`; `:1084` `wxGetApp().shutdown(isRecreate)`.
- `src/slic3r/GUI/MainFrame.cpp:2735-2739` — File ▸ Quit → `[this](wxCommandEvent&) { Close(false); }` (editor); `:3443-3444` the same for the G-code viewer. `:627` Ctrl+Q posts a `wxCloseEvent` to the same handler.
- `src/slic3r/GUI/MainFrame.cpp:4518` — `SettingsDialog` already does close-to-hide: `Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { this->Hide(); });` — the in-tree precedent.
- `src/slic3r/GUI/Plater.cpp:20453-20489` — `close_with_confirm()` shows a `MessageDialog`; `src/slic3r/GUI/MsgDialog.cpp:363-371` auto-answers **Yes** while `RemoteAccess::auto_confirm()` is set → it will call `save_project()`.
- `src/slic3r/GUI/Plater.cpp:18172-18184` — `save_project(false)` with an empty project filename falls into `p->get_export_file(FT_3MF)`, i.e. **a modal file dialog** — it would hang a hidden instance. `RemoteAccess.cpp:1197-1213` already solves this for `/api/project/open` by giving an unnamed dirty project a name under `RemoteHub::saves_dir()` first; the quit route must do the same.
- `src/slic3r/GUI/GUI_App.cpp:2474-2480` — `GUI_App::OnExit()` calls `RemoteAccess::get().stop()`, which removes `<instances>/<pid>.json` (`RemoteAccess.cpp:163-176`). Nothing else to unregister.

**Hub internals**

- `src/slic3r/GUI/RemoteHub.cpp:624-632` — `struct Instance { long pid; int port; long long started; std::string title, path; bool slicing; bool alive; }`.
- `src/slic3r/GUI/RemoteHub.cpp:911-940` — `instances(bool probe)`: scans `<datadir>/hub/instances/*.json` for `pid`/`port`/`started`, drops files whose pid is dead, sorts by start time; with `probe=true` it fans out `probe_instance()` on one thread each and joins.
- `src/slic3r/GUI/RemoteHub.cpp:891-907` — `probe_instance()` does a synchronous `GET http://127.0.0.1:<port>/api/info` (connect 1 s / total 3 s) and copies `title`, `path`, `slicing`, and validates `pid`.
- `src/slic3r/GUI/RemoteHub.cpp:942-959` — `instances_json()` = `{"instances":[{id,index,pid,title,path,slicing}]}` — served both to the hub page (`/hub/instances`) and to the phone (`/r/<t>/api/instances`).
- `src/slic3r/GUI/RemoteHub.cpp:961-976` — `snapshot()` uses the **cheap** `instances(false)`; the tray timer (`:1345-1346`, every 5 s) calls it, so nothing on the tray path does HTTP today.
- `src/slic3r/GUI/RemoteHub.cpp:996-1003` — `spawn_slicer(file)`: `args = { current_exe() }` + optional file, `spawn_process(args, { {"SNORCA_NEW_INSTANCE","1"} }, /*hide_console*/ false, /*job*/ nullptr)`.
- `src/slic3r/GUI/RemoteHub.cpp:1006-1041` — `handle_hub()`: `GET /hub/info`, `GET /hub/` (+ `/hub/index.html`), `GET /hub/qrcode.js`, `GET /hub/instances`, `POST /hub/new`, `POST /hub/state`, `POST /hub/phone`, `POST /hub/quit`.
- `src/slic3r/GUI/RemoteHub.cpp:1152-1156` — `/hub/*` is **loopback-only** (`if (!peer.is_loopback()) 404`), i.e. the PC owns these routes; the phone can never reach them.
- `src/slic3r/GUI/RemoteHub.cpp:1060-1073` — phone `GET /api/instances` and `POST /api/instances/open` (spool the upload, then `spawn_slicer(path)`).
- `src/slic3r/GUI/RemoteHub.cpp:1076-1117` — `/i/<pid>/…`: looks the pid up in `instances(false)`, `POST /i/<pid>/open` does an `Http::post(base + "/api/project/open")` round trip; `/i/<pid>/api/...` is spliced through `tunnel()`.
- `src/slic3r/GUI/RemoteHub.cpp:1264-1316` — `HubTaskBarIcon`: `enum { ID_STATUS = wxID_HIGHEST + 100, ID_PHONE, ID_PAGE, ID_NEW, ID_QUIT }`, per-ID `Bind(wxEVT_MENU, …, ID)` in the ctor, `CreatePopupMenu()` rebuilds the menu from `snapshot()` on every click, `refresh()` updates the tooltip.
- `src/slic3r/GUI/RemoteHub.cpp:1319-1366` — `HubApp`: `SetExitOnFrameDelete(false)`, server on its own thread, `wxTimer` every 5 s calling `m_icon->refresh()`.
- `src/slic3r/GUI/RemoteHub.cpp:366-438` — `spawn_process(args, env, hide_console, job)`: sets/restores env vars around `CreateProcessW`, uses `CREATE_BREAKAWAY_FROM_JOB` so children outlive the hub.
- `resources/web/orca/hub.html:76-86` — `renderSlicers(list)` renders `index · title (slicing)` + path per instance, no buttons; `:87-90` `refresh()` polls `/hub/info` + `/hub/instances` every 3 s; `:91` the "Open a new slicer window" button posts `/hub/new`.

**Instance API internals**

- `src/slic3r/GUI/RemoteAccess.cpp:114-124` — `run_on_main(fn, timeout_ms = 15000)`: `CallAfter` + `std::promise`, wraps `fn` in an `AutoConfirmScope`, returns false on timeout.
- `src/slic3r/GUI/RemoteAccess.cpp:141-161` — `start()`: loopback `tcp::acceptor` on port 0, accept thread, `write_instance_file()`.
- `src/slic3r/GUI/RemoteAccess.cpp:185-196` — `write_instance_file()` writes `{pid, port, started, version}` (called with `m_mutex` held).
- `src/slic3r/GUI/RemoteAccess.cpp:1156-1169` — `api_info()`: `{pid, port, title, path, slicing, version}`, explicitly "cheap (no GUI thread)".
- `src/slic3r/GUI/RemoteAccess.cpp:1239-1335` — `handle_api()`: a JSON route manifest for `/api` (`:1244-1274`), then a flat `if (path == "…" && method == "…")` chain; unknown → 404 `json_error("no such route; see /api")`.
- `src/slic3r/GUI/RemoteAccess.cpp:1392-1408` — `serve()` accepts only `/api…` paths and maps `ApiResponse::status` through a fixed status-text table (200/400/404/409/413/503/500 — **no 202**).
- `src/slic3r/GUI/Plater.cpp:16802-16817, 16861` — `RemoteAccess::note_project()` is called from `Plater::priv::set_project_name()` and `set_project_filename()`, on the GUI thread; it is the only source of `title`/`path`.

---

### Changes

#### 1. `src/libslic3r/PrintConfig.cpp` — `CLIMiscConfigDef::CLIMiscConfigDef()` (after the `hub_phone` block, `:9257-9261`)

Add the launch flag next to the hub options:

```cpp
    // Ultra: start without a window (the hub shows it on demand) — see RemoteHub.hpp
    def = this->add("hidden", coBool);
    def->label = L("Start hidden");
    def->tooltip = L("Start the slicer without a window and without a taskbar button; the hub's tray menu can show it later.");
    def->cli_params = "option";
    def->set_default_value(new ConfigOptionBool(false));
```

#### 2. `src/slic3r/GUI/GUI_Init.hpp` — `struct GUI_InitParams` (`:11-26`)

```cpp
    bool                        input_gcode { false };
    // Ultra: --hidden / SNORCA_HIDDEN: no splash, no main-frame Show; the hub owns visibility.
    bool                        start_hidden { false };
```

#### 3. `src/Snapmaker_Orca.cpp` — `CLI::run`, GUI branch (`:1398-1402`)

```cpp
        Slic3r::GUI::GUI_InitParams params;
        params.argc = argc;
        params.argv = argv;
        params.load_configs = load_configs;
        params.extra_config = std::move(m_extra_config);
        params.start_hidden = m_config.opt_bool("hidden");
```

#### 4. `src/slic3r/GUI/GUI_App.hpp` — new state + accessors

Next to `bool m_is_closing {false};` (`:312`):

```cpp
    // Ultra: this instance was started hidden (--hidden / SNORCA_HIDDEN / app_config
    // "start_hidden"). It has no window until the hub shows it, closing hides it again,
    // and only an explicit quit (tray, hub page, POST /api/quit, File > Quit) ends it.
    bool m_hub_managed { false };
```

Public, next to `is_recreating_gui()` (`:423`):

```cpp
    bool is_hub_managed() const { return m_hub_managed; }
```

#### 5. `src/slic3r/GUI/GUI_App.cpp` — `on_init_inner()`

**(a) decide hidden mode** — insert right after `StartupProfiler profiler("GUI_App::on_init_inner");` (`:2551`); `app_config` is already valid (finding above):

```cpp
    // Ultra: hidden launch. Precedence: SNORCA_HIDDEN (1/0, lets a test or the hub force
    // either way) > --hidden on the command line > app_config "start_hidden".
    {
        wxString env;
        if (wxGetEnv("SNORCA_HIDDEN", &env) && !env.empty())
            m_hub_managed = env != "0";
        else if (init_params && init_params->start_hidden)
            m_hub_managed = true;
        else
            m_hub_managed = app_config->get_bool("start_hidden");
        if (m_hub_managed)
            BOOST_LOG_TRIVIAL(info) << "starting hidden: no splash, no window until the hub shows it";
    }
```

**(b) skip the splash** — `:2739`:

```cpp
    if (!m_hub_managed && app_config->get("show_splash_screen") == "true") {
```

**(c) skip the Show** — `:3053-3058`:

```cpp
##ifdef __WINDOWS__
    mainframe->topbar()->SaveNormalRect();
##endif
    if (!m_hub_managed) {
        mainframe->Show(true);
        BOOST_LOG_TRIVIAL(info) << "main frame firstly shown";
    }
    profiler.mark("mainframe->Show");
```

**(d) register with the hub** — insert immediately after `m_initialized = true;` (`:3122`), replacing what StreamPanel used to do:

```cpp
    // Ultra: this instance's loopback API + the hub. Used to live in the Stream tab's
    // constructor; a hidden instance never builds that tab, and the WebView can fail.
    start_remote_access();
```

**(e) `recreate_GUI()`** — `:3953`:

```cpp
    if (!m_hub_managed)
        mainframe->Show(true);
```

**(f) new method `GUI_App::start_remote_access()`** (declare in `GUI_App.hpp` next to `machine_find()`), verbatim from `StreamPanel.cpp:44-60`:

```cpp
void GUI_App::start_remote_access()
{
    RemoteAccess::get().set_hidden(m_hub_managed); // before start(): goes into <pid>.json
    RemoteAccess::get().start();

    wxString    env_token;
    std::string token;
    bool        phone = false;
    if (wxGetEnv("SNORCA_PHONE_ACCESS", &env_token) && !env_token.empty()) {
        token = env_token.ToStdString();
        phone = true;
    } else if (app_config->get("stream_phone_access") == "1") {
        token = app_config->get("stream_phone_token");
        phone = true;
    }
    // Off the GUI thread: spawning the hub takes a moment.
    std::thread([token, phone]() { RemoteHub::ensure_running(token, phone); }).detach();
}
```

Includes: `GUI_App.cpp:4` already has `RemoteAccess.hpp`, `:40` `<thread>`, `:72` `<wx/utils.h>` (for `wxGetEnv`) — only **`#include "RemoteHub.hpp"`** has to be added.

#### 6. `src/slic3r/GUI/StreamPanel.cpp` — ctor (`:43-60`)

Delete lines 43-60 (the `RemoteAccess::get().start()` call, the token/phone block and the `ensure_running` thread). Leave `OnScriptMessage` untouched — `hub_start`, `stream_state:` and `remote_on/off/info` still work when the tab is opened, and `ensure_running` is idempotent. Add a one-line comment pointing at `GUI_App::start_remote_access()`.

#### 7. `src/slic3r/GUI/RemoteAccess.hpp`

```cpp
    void start();
    void stop();
    int  port();

    // Ultra: window visibility, mirrored into <pid>.json and /api/info so the hub can
    // list and toggle it. GUI thread (or before start()).
    void set_hidden(bool hidden);
    bool hidden();
```

and in the private route list, next to `api_info()`:

```cpp
    ApiResponse api_info();
    ApiResponse api_window(const std::string& show);   // "" = query only, "1"/"0" = set
    ApiResponse api_quit(bool discard);
```

plus `bool m_hidden { false };` next to `m_slicing`.

#### 8. `src/slic3r/GUI/RemoteAccess.cpp`

**`write_instance_file()` (`:185-196`)** — add what the hub's *cheap* (non-probing) listing needs, so the tray menu never has to do HTTP:

```cpp
    j["version"] = std::string(SLIC3R_VERSION);
    j["hidden"]  = m_hidden;
    j["title"]   = m_title;
    j["path"]    = m_path;
```

**`note_project()` (`:198-203`)** — rewrite the file so the tray menu label follows the project:

```cpp
void RemoteAccess::note_project(const std::string& title, const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_title = title;
    m_path  = path;
    if (m_on) write_instance_file();
}
```

**new `set_hidden()` / `hidden()`** (after `port()`, `:178-182`):

```cpp
void RemoteAccess::set_hidden(bool hidden)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_hidden == hidden && m_on) return;
    m_hidden = hidden;
    if (m_on) write_instance_file();
}

bool RemoteAccess::hidden()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_hidden;
}
```

**`api_info()` (`:1156-1169`)** — add one field:

```cpp
    j["slicing"] = m_slicing;
    j["hidden"]  = m_hidden;
```

**new `api_window()`** — put it next to `api_info()`:

```cpp
// Show or hide this instance's window. GET/POST without `show` only reports.
RemoteAccess::ApiResponse RemoteAccess::api_window(const std::string& show)
{
    ApiResponse r;
    const int want = show.empty() ? -1 : (show == "0" || show == "false" ? 0 : 1);
    auto out = std::make_shared<nlohmann::json>();
    bool ok = run_on_main([out, want]() {
        MainFrame* mf = wxGetApp().mainframe;
        if (mf == nullptr) return;
        if (want == 1) {
            if (mf->IsIconized()) mf->Iconize(false);
            mf->Show(true);
            mf->Raise();
##ifdef _WIN32
            ::SetForegroundWindow(mf->GetHandle()); // the hub called AllowSetForegroundWindow(pid)
##endif
        } else if (want == 0) {
            mf->Hide();
        }
        const bool hidden = !mf->IsShown();
        RemoteAccess::get().set_hidden(hidden);
        (*out)["hidden"]   = hidden;
        (*out)["iconized"] = mf->IsIconized();
    }, 5000);
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (out->empty()) { r.status = 503; r.body = json_error("no main window"); return r; }
    r.body = out->dump();
    return r;
}
```

**new `api_quit()`** — answer *before* the frame tears the process down, and never let the auto-confirmed save pop a file dialog:

```cpp
// End this instance. discard=1 skips the unsaved-project handling entirely; otherwise the
// usual close path runs and the auto-confirm Yes saves — so an unnamed dirty project first
// gets a name under <datadir>/hub/saves, exactly as api_project_open does.
RemoteAccess::ApiResponse RemoteAccess::api_quit(bool discard)
{
    ApiResponse r;
    auto result = std::make_shared<std::pair<int, std::string>>(500, "not run");
    bool ok = run_on_main([result, discard]() {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr || wxGetApp().mainframe == nullptr) { *result = { 500, "no main window" }; return; }
        if (!discard && plater->is_background_process_slicing()) { *result = { 409, "slicing in progress" }; return; }
        if (!discard && plater->is_project_dirty() && plater->get_project_filename(".3mf").IsEmpty() &&
            !plater->model().objects.empty()) {
            boost::system::error_code ig;
            fs::create_directories(RemoteHub::saves_dir(), ig);
            std::time_t t = std::time(nullptr);
            std::tm     tm {};
##ifdef _WIN32
            localtime_s(&tm, &t);
##else
            localtime_r(&t, &tm);
##endif
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
            plater->set_project_filename(wxString::FromUTF8(
                (fs::path(RemoteHub::saves_dir()) / (std::string("Untitled_") + stamp + ".3mf")).string()));
        }
        *result = { 200, "" };
    }, 15000);
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (result->first != 200) { r.status = result->first; r.body = json_error(result->second); return r; }
    // The close runs after this response has been written.
    wxGetApp().CallAfter([discard]() {
        if (wxGetApp().mainframe) wxGetApp().mainframe->request_quit(discard);
    });
    r.body = "{\"ok\":true}";
    return r;
}
```

**`handle_api()` (`:1275-1276`)** — two routes and two manifest lines:

```cpp
    if (path == "/info" && method == "GET")
        return api_info();
    if (path == "/window" && (method == "GET" || method == "POST")) {
        std::string show = query_param(query, "show");
        if (show.empty()) show = query_param(body, "show");
        return api_window(method == "GET" ? std::string() : show);
    }
    if (path == "/quit" && method == "POST") {
        std::string d = query_param(query, "discard");
        if (d.empty()) d = query_param(body, "discard");
        return api_quit(d == "1" || d == "true");
    }
```

manifest (`:1250`, after the `/api/info` line):

```cpp
    { {"method", "GET"},  {"path", "/api/window"},   {"description", "is this instance's window shown? {hidden, iconized}"} },
    { {"method", "POST"}, {"path", "/api/window?show=1|0"}, {"description", "show or hide this instance's window"} },
    { {"method", "POST"}, {"path", "/api/quit[?discard=1]"}, {"description", "close this instance; without discard the unsaved project is saved (an unnamed one under <datadir>/hub/saves)"} },
```

#### 9. `src/slic3r/GUI/MainFrame.hpp` / `.cpp` — close-to-hide + explicit quit

**MainFrame.hpp** (public):

```cpp
    // Ultra: the only way a hub-managed (hidden-launch) instance really closes.
    // discard = skip the unsaved-project handling (Close(true)).
    void request_quit(bool discard = false);
private:
    bool m_quit_requested { false };
```

**MainFrame.cpp** — new method next to `shutdown()` (`:1015`):

```cpp
void MainFrame::request_quit(bool discard)
{
    m_quit_requested = true;   // consumed by the next close attempt
    Close(discard);            // discard -> CanVeto()==false -> no prompts at all
}
```

**MainFrame.cpp:491** — first thing in the `wxEVT_CLOSE_WINDOW` lambda, before the gizmo check:

```cpp
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": mainframe received close_widow event";
        // Ultra: a hub-managed instance closes to the hub's tray menu, not to the grave.
        // Only request_quit() (tray, hub page, POST /api/quit, File > Quit) really ends it.
        const bool quit_requested = m_quit_requested;
        m_quit_requested = false;
        if (wxGetApp().is_hub_managed() && !quit_requested && event.CanVeto()) {
            event.Veto();
            Hide();
            RemoteAccess::get().set_hidden(true);
            BOOST_LOG_TRIVIAL(info) << "hub-managed instance: close hid the window, still running";
            return;
        }
        …existing gizmo / close_with_confirm / print-host-queue checks…
```

Add `#include "RemoteAccess.hpp"` to MainFrame.cpp.

**MainFrame.cpp:2735-2739 and :3443-3444** — File ▸ Quit must still be able to end a visible hub-managed window:

```cpp
            [this](wxCommandEvent&) { request_quit(false); }, …
```

(`request_quit(false)` == the old `Close(false)` for a normal instance, so no behaviour change there.)

#### 10. `src/slic3r/GUI/RemoteHub.cpp` — hub side

**`struct Instance` (`:624-632`)** — `bool hidden { false };`

**`instances(bool probe)` (`:919-924`)** — read the new fields from the file so the *cheap* path is complete:

```cpp
            inst.started = j.value("started", 0LL);
            inst.hidden  = j.value("hidden", false);
            inst.title   = j.value("title", "");
            inst.path    = j.value("path", "");
```

**`probe_instance()` (`:900-902`)** — `inst.hidden = j.value("hidden", inst.hidden);`

**`instances_json()` (`:949-956`)** — `ji["hidden"] = inst.hidden;`

**`spawn_slicer()` (`:996-1003`)** — a second argument, defaulting to hidden-off so existing call sites keep their meaning:

```cpp
long HubServer::spawn_slicer(const std::string& file, bool hidden)
{
    std::vector<std::string> args = { current_exe() };
    if (!file.empty()) args.push_back(file);
    std::vector<std::pair<std::string, std::string>> env { { "SNORCA_NEW_INSTANCE", "1" } };
    env.emplace_back("SNORCA_HIDDEN", hidden ? "1" : "0");   // explicit either way
    const long pid = spawn_process(args, env, false, nullptr);
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: new " << (hidden ? "hidden" : "visible")
                            << " instance pid " << pid << (file.empty() ? std::string() : " for " + file);
    return pid;
}
```

(declare `long spawn_slicer(const std::string& file, bool hidden = false);` at `:653`)

**new instance-control helpers on `HubServer`** (private, used by the tray and by `handle_hub`; they do HTTP, so never call them on the GUI thread):

```cpp
// One round trip to an instance's loopback API. Returns the status and the body.
std::pair<int, std::string> HubServer::instance_post(long pid, const std::string& sub)
{
    Instance target;
    for (const Instance& i : instances(false))
        if (i.pid == pid) target = i;
    if (!target.alive) return { 404, json_error("no such slicer instance") };
##ifdef _WIN32
    ::AllowSetForegroundWindow((DWORD) pid); // let the instance raise itself
##endif
    int         status = 502;
    std::string body   = json_error("the slicer did not answer");
    Http::post("http://127.0.0.1:" + std::to_string(target.port) + sub)
        .timeout_connect(2).timeout_max(120)
        .on_complete([&](std::string b, unsigned s) { status = (int) s; body = b; })
        .on_error([&](std::string b, std::string e, unsigned s) { status = s ? (int) s : 502; body = b.empty() ? json_error(e) : b; })
        .perform_sync();
    return { status, body };
}
bool HubServer::instance_window(long pid, bool show) { return instance_post(pid, std::string("/api/window?show=") + (show ? "1" : "0")).first == 200; }
bool HubServer::instance_quit(long pid, bool discard) { return instance_post(pid, std::string("/api/quit") + (discard ? "?discard=1" : "")).first == 200; }
```

**`handle_hub()` (`:1016-1021`)** — extend `/hub/new` and add the per-instance routes (loopback-only by construction, `:1152-1156`):

```cpp
    } else if (r.path == "/hub/new" && r.method == "POST") {
        const long pid = spawn_slicer("", query_param(r.query, "hidden") == "1");
        …unchanged…
    } else if (r.path.compare(0, 16, "/hub/instances/") == 0 && r.method == "POST") {
        // /hub/instances/<pid>/window?show=1|0 and /hub/instances/<pid>/quit[?discard=1]
        const std::string rest  = r.path.substr(15);
        const size_t      slash = rest.find('/');
        const long        pid   = std::atol(rest.substr(0, slash).c_str());
        const std::string sub   = slash == std::string::npos ? "" : rest.substr(slash);
        if (sub == "/window") {
            auto res = instance_post(pid, std::string("/api/window?show=") + (query_param(r.query, "show") == "0" ? "0" : "1"));
            respond_json(client, res.first, res.second);
        } else if (sub == "/quit") {
            auto res = instance_post(pid, std::string("/api/quit") + (query_param(r.query, "discard") == "1" ? "?discard=1" : ""));
            respond_json(client, res.first, res.second);
        } else {
            respond_json(client, 404, json_error("no such hub route"));
        }
    } else if (…
```

**`handle_phone()` (`:1064-1073`)** — a phone-initiated open spawns hidden unless it asks otherwise:

```cpp
        const long pid = spawn_slicer(path, query_param(r.query, "visible") != "1");
```

and the manifest line at `:1052` becomes `"…; starts a new (hidden) slicer instance with it; ?visible=1 opens a window"`.

**`HubTaskBarIcon` (`:1264-1316`)** — ids, the range binding, the submenu:

```cpp
    enum { ID_STATUS = wxID_HIGHEST + 100, ID_PHONE, ID_PAGE, ID_NEW, ID_NEW_HIDDEN, ID_QUIT,
           ID_INST_FIRST = wxID_HIGHEST + 200, ID_INST_PER = 3, ID_INST_MAX = 32,
           ID_INST_LAST  = ID_INST_FIRST + ID_INST_PER * ID_INST_MAX };
```

in the ctor, next to the existing `Bind`s:

```cpp
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_server.spawn_slicer("", true); }, ID_NEW_HIDDEN);
        Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
            const int n = (e.GetId() - ID_INST_FIRST) / ID_INST_PER, what = (e.GetId() - ID_INST_FIRST) % ID_INST_PER;
            if (n < 0 || n >= (int) m_menu_pids.size()) return;
            const long   pid = m_menu_pids[n];
            HubServer*   s   = &m_server;
            // HTTP: never on the tray's (GUI) thread.
            std::thread([s, pid, what]() {
                if (what == 2) s->instance_quit(pid, false);
                else           s->instance_window(pid, what == 0);
            }).detach();
        }, ID_INST_FIRST, ID_INST_LAST);
```

`CreatePopupMenu()` — after "Open a new slicer window":

```cpp
        menu->Append(ID_NEW, "Open a new slicer window");
        menu->Append(ID_NEW_HIDDEN, "Open a new hidden slicer");

        // Cheap: pid/title/hidden all come from <datadir>/hub/instances/<pid>.json.
        m_menu_pids.clear();
        auto* subs = new wxMenu;
        int   n    = 0;
        for (const Instance& inst : m_server.instances(false)) {
            if (n >= ID_INST_MAX) break;
            const int base = ID_INST_FIRST + n * ID_INST_PER;
            auto*     one  = new wxMenu;
            one->Append(base + 0, "Show window")->Enable(inst.hidden);
            one->Append(base + 1, "Hide window")->Enable(!inst.hidden);
            one->AppendSeparator();
            one->Append(base + 2, "Quit this window");
            subs->AppendSubMenu(one, wxString::Format("%d \xC2\xB7 %s%s", n + 1,
                inst.title.empty() ? "Untitled" : wxString::FromUTF8(inst.title),
                inst.hidden ? "  (hidden)" : ""));
            m_menu_pids.push_back(inst.pid);
            ++n;
        }
        if (n == 0) subs->Append(wxID_ANY, "No slicer is running")->Enable(false);
        menu->AppendSubMenu(subs, "Slicer windows");
```

member: `std::vector<long> m_menu_pids;`

Also make the tooltip say how many are hidden (`refresh()`, `:1284-1291`) — `Snapshot` gains `int hidden { 0 };`, filled in `snapshot()` (`:974`) from the same `instances(false)` walk.

#### 11. `resources/web/orca/hub.html`

`renderSlicers()` (`:76-86`) — add the per-instance controls:

```js
    function renderSlicers(list) {
        var body = document.getElementById('slicerbody');
        body.innerHTML = '';
        if (!list.length) { body.appendChild(el('div', 'note', 'No slicer window is open.')); return; }
        list.forEach(function(i) {
            var d = el('div', 'inst');
            d.appendChild(el('div', 't', i.index + ' \u00b7 ' + (i.title || 'Untitled') +
                (i.hidden ? ' (hidden)' : '') + (i.slicing ? ' (slicing)' : '')));
            d.appendChild(el('div', 'm', i.path || 'not saved yet'));
            var b = el('span', 'btn' + (i.hidden ? '' : ' dim'), i.hidden ? 'Show' : 'Hide');
            b.onclick = function() { post('/hub/instances/' + i.pid + '/window?show=' + (i.hidden ? '1' : '0')).then(refresh); };
            d.appendChild(b);
            var q = el('span', 'btn warn', 'Quit');
            q.onclick = function() {
                if (!confirm('Quit slicer window ' + i.index + '? Unsaved work is saved first.')) return;
                post('/hub/instances/' + i.pid + '/quit').then(function() { setTimeout(refresh, 2000); });
            };
            d.appendChild(q);
            body.appendChild(d);
        });
    }
```

and next to the "Open a new slicer window" button (`:35`, handler `:91`):

```html
  <span class="btn dim" id="newhidden">Open a hidden slicer</span>
```
```js
    document.getElementById('newhidden').onclick = function() { post('/hub/new?hidden=1').then(function() { setTimeout(refresh, 3000); }); };
```

`post()` at `:45` already sends POST and parses JSON; the new routes answer JSON, so no change there. Note `resources/web/orca/hub.html` is copied into the build tree by `cmake --install`.

---

### Hub / API surface added

**Instance API (loopback, `RemoteAccess`)**

| Route | Meaning |
|---|---|
| `GET /api/window` | `{hidden, iconized}` — no side effect |
| `POST /api/window?show=1` | show + de-iconize + raise; returns `{hidden:false, iconized}` |
| `POST /api/window?show=0` | hide; returns `{hidden:true, …}` |
| `POST /api/quit` | 200 `{ok:true}` and then close; 409 while slicing; unnamed dirty project is first named under `<datadir>/hub/saves` so the auto-confirmed save cannot open a file dialog |
| `POST /api/quit?discard=1` | `Close(true)` — no prompts, no save |

`GET /api/info` gains `"hidden": bool`. Both new routes are listed in the `/api` manifest.

**Instance file `<datadir>/hub/instances/<pid>.json`** gains `"hidden"`, `"title"`, `"path"` (on top of `pid`, `port`, `started`, `version`), rewritten by `set_hidden()` and `note_project()`. This is what makes the tray menu probe-free.

**Hub routes (loopback-only `/hub/…`, i.e. the PC, never the phone)**

| Route | Meaning |
|---|---|
| `POST /hub/instances/<pid>/window?show=1\|0` | proxied to that instance's `/api/window` (with `AllowSetForegroundWindow(pid)` first on Windows) |
| `POST /hub/instances/<pid>/quit[?discard=1]` | proxied to that instance's `/api/quit` |
| `POST /hub/new?hidden=1` | spawn a hidden instance (default stays visible) |

`GET /hub/instances` and the phone's `GET /r/<t>/api/instances` gain `"hidden"` per entry.

**Phone route change**: `POST /r/<t>/api/instances/open` now spawns a **hidden** instance; `?visible=1` restores the old behaviour.

**Tray menu** (`HubTaskBarIcon::CreatePopupMenu`)

```
Snapmaker-Ultra Hub: 2 slicer windows open (1 hidden)   [disabled]
────────────────────────────────────────
[x] Phone access
    Open hub page (QR code, link, slicers)
    Open a new slicer window
    Open a new hidden slicer
    Slicer windows ▸  1 · Bracket  (hidden) ▸  Show window / Hide window / ─── / Quit this window
                      2 · Untitled            ▸  Show window / Hide window / ─── / Quit this window
────────────────────────────────────────
    Quit hub (stops phone access and camera relays)
```

**Hub page**: each instance row gains `(hidden)` in its title, a `Show`/`Hide` button and a `Quit` button; a new "Open a hidden slicer" button next to "Open a new slicer window".

---

### Test plan for this phase

Helpers live in the session scratchpad (`…/548a2e04-…/scratchpad/`): `run_hub_app.py <file> [--new]` starts `C:\Dev\SnapmakerOrca\build\Snapmaker_Orca\snapmaker-orca.exe` detached with `SNORCA_PHONE_ACCESS=testtoken12345`; `wait_for_hub.py [N] [--untitled]` polls `http://127.0.0.1:13640/hub/info` + `/r/<token>/api/instances` until N instances are listed; `test_layout.py`, `test_presets.py`, `test_settings.py` (in `…/67c5db31-…/scratchpad/`) exercise the routes through `http://10.0.0.131:13640/r/<token>/…`. Build with `build_app.bat`, deploy with `cmake --install C:\Dev\SnapmakerOrca\build --config Release` (this is what copies `hub.html` into the build tree).

1. **Build + deploy.** `build_app.bat`, then `cmake --install … --config Release`. Confirm `resources/web/orca/hub.html` in the build tree matches the source (`cmp`, as `deploy_and_test2.sh` does).
2. **Clean slate.** `curl -s -X POST http://127.0.0.1:13640/hub/quit`, then `Get-Process snapmaker-orca | Stop-Process -Force`.
3. **Visible instance is unchanged.** `python run_hub_app.py <3mf>` → splash appears, a window appears, a taskbar button appears; `python wait_for_hub.py 1` succeeds; `GET /hub/instances` shows `"hidden": false`. This proves the move of `RemoteAccess::start()` out of `StreamPanel` did not regress registration — and crucially, **without ever opening the Stream tab** (which is where it used to happen).
4. **Hidden launch.** Start a second instance with `SNORCA_HIDDEN=1 SNORCA_NEW_INSTANCE=1` (add the two env vars to `run_hub_app.py`, or set them in the shell before calling the exe). Check: **no splash, no window, no taskbar button**, the process is alive in Task Manager, and `python wait_for_hub.py 2` still succeeds. `GET http://127.0.0.1:13640/hub/instances` must list it with `"hidden": true` and a non-empty `"title"` (the title comes from the instance file, so this also proves `note_project()` rewrites it).
5. **Non-rendering routes against the hidden instance.** With `pid` = the hidden instance's id, against `http://10.0.0.131:13640/r/testtoken12345/i/<pid>/api`:
   `GET /info` (has `hidden:true`), `GET /presets`, `GET /settings/process`, `GET /plates`, `GET /plates/0/layout`, `POST /objects/transform` (move / rotate / restore). `test_layout.py`, `test_presets.py` and `test_settings.py` do exactly this once pointed at that pid. Expected: all 200, same payloads as from the visible instance.
   Rendering routes are expected to fail here in Phase 1: `GET /plates/0/thumbnail.png`, `/plates/0/preview.png`, `POST /slice` — record what they return (probably 503/500), that is Phase 2's input.
6. **Show from the tray.** Tray icon ▸ Slicer windows ▸ `2 · <title> (hidden)` ▸ **Show window**. The window must appear and come to the front; `GET /hub/instances` flips to `"hidden": false`; the taskbar button appears.
7. **Hide from the tray.** Same submenu ▸ **Hide window**. Window and taskbar button disappear, `"hidden": true` again, and step 5's routes still answer.
8. **Show/Hide from the hub page.** `http://127.0.0.1:13640/hub/` — the row shows `(hidden)`, the `Show`/`Hide` button toggles it and the label follows within one 3 s refresh.
9. **Close-to-hide.** Show the hidden instance, then click the window's ✕. Expect: window gone, **process still alive**, `"hidden": true`, and `GET /i/<pid>/api/info` still answers. Repeat with a dirty project — **no save prompt** must appear (the close is vetoed before `close_with_confirm`).
10. **Explicit quit.** Tray ▸ Slicer windows ▸ that instance ▸ **Quit this window** (and/or `POST /hub/instances/<pid>/quit`). Expect: HTTP 200 first, then the process exits, `<datadir>/hub/instances/<pid>.json` disappears, and the instance drops off `/hub/instances` within a few seconds. With a dirty unnamed project, a `Untitled_<stamp>.3mf` must appear under `<datadir>/hub/saves` (never a file dialog). Also check `POST /api/quit?discard=1` exits with no save.
11. **File ▸ Quit on a shown hub-managed window** ends the process (does not hide).
12. **Phone-initiated open spawns hidden.** From the LAN page `http://10.0.0.131:13640/r/testtoken12345/`, upload a model with "New". Expect: no window appears on the PC, a new instance shows up in `/hub/instances` with `"hidden": true` and the uploaded project's title. `POST /r/<t>/api/instances/open?visible=1` must still open a window.
13. **Tray "Open a new slicer window"** still opens a *visible* one; **"Open a new hidden slicer"** and `POST /hub/new?hidden=1` open hidden ones.
14. **Nothing left behind.** Quit every instance, then the hub (`POST /hub/quit`); `<datadir>/hub/instances/` must be empty and no `snapmaker-orca.exe` left.

**What an automated script checks** (a `test_hidden.py` next to the other helpers):

- start hidden with `env["SNORCA_HIDDEN"]="1"`, `env["SNORCA_NEW_INSTANCE"]="1"`; `wait_for_hub.py N` returns 0 within 120 s → registration works without the Stream tab;
- `GET /hub/instances` contains the new pid with `hidden == True` and a non-empty `title`;
- each of `/info`, `/presets`, `/settings/process`, `/plates`, `/plates/0/layout` returns 200 with the expected top-level keys, and `POST /objects/transform` moves an object and can be restored;
- `POST /hub/instances/<pid>/window?show=1` → 200 and `hidden` flips to `False` in `/hub/instances` within 3 s; `?show=0` flips it back;
- `POST /hub/instances/<pid>/quit` → 200, then the pid is gone from `/hub/instances` within 15 s and `psutil.pid_exists(pid)` is False;
- a visible control instance (no `SNORCA_HIDDEN`) reports `hidden == False` — guards against the flag leaking through `app_config`.

---

### Risks / open questions

- **Foreground stealing (Windows).** `Show()+Raise()` from a background process usually only flashes the taskbar button. The plan calls `::AllowSetForegroundWindow(pid)` in the hub right before the request (the hub *is* the foreground process when the user clicks the tray) and `::SetForegroundWindow` in the instance. If it still only flashes, the fallback is the `AttachThreadInput` dance or a brief `SetWindowPos(HWND_TOPMOST)` toggle — decide after step 6.
- **First run in hidden mode.** `GUI_App::post_init` → `config_wizard_startup()` (`GUI_App.cpp:1151, 7325-7346`) runs `run_wizard()` when `!m_app_conf_exists || preset_bundle->printers.only_default_printers()`, which would raise a modal `GuideFrame` from a window-less instance and hang it. Phase 1 does **not** change this (the test datadir is configured); **Phase 3 (dialog policy) owns it**. A one-line stopgap if it ever bites: `if (m_hub_managed) { /* skip the wizard, log it */ }` around `:1151`.
- **Other dialogs that can appear out of a hidden instance** — all Phase 3, listed for completeness: `m_updateDialog->Raise()/Show()` on `EVT_SLIC3R_VERSION_ONLINE` (`GUI_App.cpp:2817-2818`), `DownloadDialog` on `EVT_ENTER_FORCE_UPGRADE` which then calls `mainframe->Close(true)` (`:2830-2851`), `check_updates()` → `mainframe->Close()` on incompatible presets (`:7354`), the TLS-cert `RichMessageDialog` at `:2654-2666` (fires *before* the frame exists, so it is a startup hang risk even today), the config-corrupted `show_error` at `:3129`, and the "did you know" hint notification (`:1109-1111`, harmless).
- **OpenGL / `post_init`.** `GUI_App.cpp:1063` gates GL init on `plater_->canvas3D()->get_wxglcanvas()->IsShownOnScreen()`, which is false while hidden — it logs "Found glcontext not ready, postpone the init" and skips. Expected in Phase 1; it means `thumbnail.png`, `preview.png` and probably `POST /slice` fail until Phase 2. Open question for Phase 2: does the GL context initialise correctly the *first* time a hidden instance is shown, or does something have to re-trigger `make_current_for_postinit()`?
- **`api_project_open` calls `mainframe->select_tab(MainFrame::tp3DEditor)`** (`RemoteAccess.cpp:1228`) on a hidden frame. Needs a check in step 12 — if `select_tab` touches the GL canvas it may be the first Phase 2 casualty rather than a Phase 1 one.
- **Camera list in hidden mode.** No instance pushes `stream_state` unless the Stream tab is opened, so the hub falls back to `<datadir>/hub/streams.json` from a previous session (`RemoteHub.cpp:1213`). Acceptable; a PC that has *never* opened the Stream tab has no camera list on the phone. Not a Phase 1 goal.
- **Tray menu id range.** 32 instances max (`ID_INST_MAX`); beyond that the extra instances just don't appear in the submenu. Fine, but worth a log line.
- **`instances(false)` freshness.** The tray menu now reads `hidden`/`title` from the instance file rather than probing. A crashed instance leaves a stale file until the next `pid_alive` sweep (`RemoteHub.cpp:925-929`), which every listing already does — so no new staleness, but the title can be up to one `note_project()` behind (it is written synchronously, so effectively never).
- **`m_quit_requested` and vetoes.** If the user cancels the save prompt during an explicit quit, the flag has already been consumed and the *next* window close hides again instead of re-asking. That is the intended behaviour, but call it out in the commit message.
- **Single-instance forwarding.** With `app.single_instance=true` and only a hidden instance running, launching the exe from Explorer forwards the file to the hidden instance (`GUI_Init.cpp:56`, `InstanceCheck.cpp:490-527`) and **nothing appears on screen** — `handle_message` never raises the frame. Phase 1 should either leave it (documented) or, better, one line in the `EVT_LOAD_MODEL_OTHER_INSTANCE` handler: if `is_hub_managed()` and hidden, show the window. Decide before implementing; it is a two-line change and it prevents a very confusing "the app doesn't start" report.
- **`start_hidden` app_config key** is read but never written by any UI in Phase 1 (it defaults to false and does not need an `AppConfig::set_defaults` entry — `get_bool` on a missing key is false). Exposing it in Preferences belongs to Phase 3.

## Phase 2 — OpenGL on a never-shown window

Goal: every rendering / OpenGL-dependent API path works on an instance whose `MainFrame` was
never `Show()`n. Phase 1 supplies the launch flag and a predicate; this plan calls it
`wxGetApp().is_hidden_instance()` — substitute Phase 1's real name.

### Findings (bullets with file:line)

#### 1. The wxGLContext, the HWNDs and the pixel format — all exist before any window is shown

- Three `wxGLCanvas` widgets are created in the `Plater::priv` constructor, all children of one
  `panel_3d`: `src/slic3r/GUI/Plater.cpp:10727-10732` (View3D, Preview, AssembleView), each via
  `OpenGLManager::create_wxglcanvas` (`src/slic3r/GUI/GUI_Preview.cpp:59`, `:257`, `:805`).
  These are the only `wxGLCanvas` instances in the app (`create_wxglcanvas` has exactly those
  three call sites) — nothing creates a GL canvas later, so nothing can steal the current
  context after start-up.
- `OpenGLManager::create_wxglcanvas` (`src/slic3r/GUI/OpenGLManager.cpp:337-367`) builds the
  attribute list (RGBA, double buffer, 8/8/8/8, depth 24, stencil 8, 4x MSAA) at `:339-355`,
  runs `detect_multisample` once (`:357-361`, `:369-382`, which calls
  `wxGLCanvas::IsDisplaySupported`), and constructs the canvas at `:366` with `wxDefaultSize`.
- **The HWND and the pixel format are created at construction, not at show time.**
  `wxGLCanvas::CreateWindow` (`deps/build/dep_wxWidgets-prefix/src/dep_wxWidgets/src/msw/glcanvas.cpp:713-746`)
  calls `MSWCreate(... WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN ...)` with the
  `CS_OWNDC` window class and then `m_hDC = ::GetDC(GetHwnd())` — a permanent private DC.
  `wxGLCanvas::Create` (`.../src/msw/glcanvas.cpp:765-800`) then calls
  `FindMatchingPixelFormat` + `::SetPixelFormat(m_hDC, pixelFormat, &pfd)` at `:790`.
  None of this consults visibility.
- The single shared `wxGLContext` is created on first use, against the View3D canvas:
  `OpenGLManager::init_glcontext` (`src/slic3r/GUI/OpenGLManager.cpp:322-335`, `m_context = new
  wxGLContext(&canvas)` at `:325`), reached from `GUI_App::init_glcontext`
  (`src/slic3r/GUI/GUI_App.cpp:2088-2091`) which is called from `View3D::init`
  (`src/slic3r/GUI/GUI_Preview.cpp:64`), `Preview::init` (`:262`) and `AssembleView::init`
  (`:810`). `wxGLContext`'s ctor calls `wglCreateContextAttribsARB` / `wglCreateContext` on
  `win->GetHDC()` (`.../src/msw/glcanvas.cpp:592-624`) — again no visibility requirement.
  All three canvases share this one context, which is legal because they share one pixel format.
- `GLCanvas3D::_set_current()` is `m_context != nullptr && m_canvas->SetCurrent(*m_context)`
  (`src/slic3r/GUI/GLCanvas3D.cpp:7200-7203`). That resolves to
  `wxGLCanvasBase::SetCurrent(const wxGLContext&)`
  (`.../src/common/glcmn.cpp:79-88`), whose own comment is decisive:
  *"although on MSW it works even if the window is still hidden, it doesn't work in other ports"* —
  it then does `wxASSERT_MSG( IsShown(), ...)` and forwards to `wxGLContext::SetCurrent`
  (`.../src/msw/glcanvas.cpp:645-655`), i.e. plain `wglMakeCurrent(win.GetHDC(), m_glContext)`.
  **Certain from the code: on Windows the make-current is a `wglMakeCurrent` on an existing,
  pixel-format-configured DC and does not depend on the window being on screen.**
- That `wxASSERT_MSG` is live even in a release wx build (`wxDEBUG_LEVEL` defaults to 1
  regardless of `NDEBUG`: `.../include/wx/debug.h:42-44`; `include/wx/msw/setup.h:84-93` leaves it
  undefined; `wxTheAssertHandler = wxDefaultAssertHandler` at `.../src/common/appbase.cpp:1188`).
  It is harmless here because it tests the **canvas's own** `m_isShown`
  (`.../include/wx/window.h:650`, `virtual bool IsShown() const { return m_isShown; }`), which is
  `true` from construction and is never cleared: `Plater::priv::set_current_panel` hides the
  *panels* (`src/slic3r/GUI/Plater.cpp:15091-15093`) and `MainFrame` hides the *Plater*
  (`src/slic3r/GUI/MainFrame.cpp:1263`), never the `wxGLCanvas`. If it ever became a problem,
  `m_context->SetCurrent(*m_canvas)` (the `wxGLContext` overload) bypasses the assert entirely.
- `IsShownOnScreen()` is the one that walks parents:
  `.../src/common/wincmn.cpp:1243-1250` — `IsShown() && (IsTopLevel() || !GetParent() ||
  GetParent()->IsShownOnScreen())`. So on a hidden `MainFrame` every child answers
  `IsShownOnScreen() == false` and `IsShown() == true`. Every audit item below turns on this
  distinction.
- **Precedent inside this repo that the offscreen path works on a never-shown window:** the CLI
  thumbnail renderer creates a GLFW window with `glfwWindowHint(GLFW_VISIBLE, false)`
  (`src/Snapmaker_Orca.cpp:5746`), calls `opengl_mgr.init_gl(false)` (`:5771-5772`) and then
  `GLCanvas3D::render_thumbnail_framebuffer` (`:5875`) with no wxApp at all. The hook for it is
  `OpenGLManager::get_active_shader()` (`src/slic3r/GUI/OpenGLManager.hpp:101-103`), used by
  `GLModel::render` (`src/slic3r/GUI/GLModel.cpp:602-604`).

#### 2. What `GLCanvas3D::init()` needs (context: yes; size or visibility: no)

`bool GLCanvas3D::init()` — `src/slic3r/GUI/GLCanvas3D.cpp:1221-1277`:

- `:1225-1226` bails if `m_canvas == nullptr || m_context == nullptr` (both are non-null from
  construction).
- `:1229` `on_change_color_mode(...)` → `Bed3D`/`GCodeViewer`/`ImGuiWrapper` colour mode,
  `NotificationManager`, `DailyTips`, both `IMSlider`s, `PartPlateList`, and
  `_init_select_plate_toolbar()` (`:1299`) which loads four SVGs into GL textures
  (`:7033-7046`). Requires `plater()`, `notification_manager` and `partplate_list` to exist —
  they do by the time `post_init` runs (`GUI_App::post_init` is invoked from the idle handler at
  `src/slic3r/GUI/GUI_App.cpp:3105`, after `plater_->init_notification_manager()` at `:3033`).
- `:1233-1243` raw state: `glClearColor/glClearDepth/glDepthFunc/glEnable(GL_DEPTH_TEST,
  GL_CULL_FACE, GL_BLEND, GL_MULTISAMPLE)/glBlendFunc`.
- `:1246-1247` `m_layers_editing.init()` (textures), `:1250` `m_gizmos.init()` (gizmo textures),
  `:1255` `_init_toolbars()` (`:6857-6884` → main / assemble / return / separator / select-plate /
  collapse toolbars, each loading PNG or SVG into GL textures), `:1259` `m_selection.init()`.
- `:1274` `m_initialized = true`.
- **Nothing in `init()` reads `get_canvas_size()`, `IsShown()` or `IsShownOnScreen()`.** It needs
  exactly one thing: a current GL context with GLEW initialised (the texture loaders call
  GLEW-dispatched entry points).
- `wxGetApp().init_opengl()` (`src/slic3r/GUI/GUI_App.cpp:2093-2101`) → `OpenGLManager::init_gl`
  (`src/slic3r/GUI/OpenGLManager.cpp:243-320`): `glewInit()` at `:247`, compressed-texture and
  **framebuffer-type detection** at `:254-270` (`s_framebuffers_type` stays `Unknown` until this
  runs), version check `:272`, `m_shaders_manager.init()` (compiles every shader) at `:288`.
  It **must be called with a context already current** — which is why `render()` orders it
  `_set_current()` then `init_opengl()` (`:1994`).
- ImGui: `ImGuiWrapper::new_frame` (`src/slic3r/GUI/ImGuiWrapper.cpp:517-529`) lazily calls
  `init_font(true)` which does `glGenTextures` (`:2787`), so it needs a context but no window.
  `ImGui::NewFrame` asserts `DisplaySize.x >= 0 && DisplaySize.y >= 0`
  (`deps_src/imgui/imgui.cpp:7188`) and `ImGuiIO`'s default `DisplaySize` is `(-1,-1)`; that
  assert is plain `assert()` (`deps_src/imgui/imconfig.h:19-20` leaves `IM_ASSERT` at its
  default), so it is compiled out in Release and fires in Debug. `set_display_size`
  (`:438-442`) is the only thing that sets it, and it is normally called from `GLCanvas3D::_resize`
  (`:7212-7213`) — which only runs on `wxEVT_SIZE` / `_refresh_if_shown_on_screen`. **On a hidden
  instance `DisplaySize` is never set unless we set it.** `render_draw_data` returns early when
  the framebuffer size is 0 (`:2967-2971`), so a stray render is safe.
  The offscreen paths never call `new_frame()` at all (see §6), so ImGui is only a warm-up
  concern, not a per-request one.

#### 3. GL calls that happen OUTSIDE a render pass (they rely on "some context is current")

`glsafe()` only checks errors in debug builds (`HAS_GLSAFE` is defined only when `NDEBUG` is
absent: `src/slic3r/GUI/3DScene.hpp:20-34`; the checker `assert(false)`s at
`src/slic3r/GUI/3DScene.cpp:39-65`). In Release a GL call with no current context is silently
dropped; a **GLEW-dispatched** call before `glewInit()` is a null function-pointer call, i.e. a
crash. So "warm up before anything else" is a correctness *and* a stability requirement.

| Site | file:line | Context guaranteed today on a hidden instance? |
|---|---|---|
| `GCodeViewer::load` → `load_toolpaths` `glGenBuffers`/`glBufferData` | `src/slic3r/GUI/GCodeViewer.cpp:990`, `:1079`, `:2129`, `:2919`, `:3074` | **No.** Reached from `GLCanvas3D::load_gcode_preview` which never makes anything current. |
| `GLCanvas3D::load_gcode_preview` (calls `m_gcode_viewer.init()` + `load()` + `refresh()`) | `src/slic3r/GUI/GLCanvas3D.cpp:3093-3117` | **No `_set_current()` anywhere in it.** |
| `GCodeViewer::reset` → `VBuffer/IBuffer::reset` `glDeleteBuffers` | `src/slic3r/GUI/GCodeViewer.cpp:1318`, `:131-170` | **No.** Entry point is `GLCanvas3D::reset_gcode_toolpaths()`, an inline in `src/slic3r/GUI/GLCanvas3D.hpp:786`, called from `Plater::priv::reset_gcode_toolpaths` (`src/slic3r/GUI/Plater.cpp:17054`), `Plater::reslice` (`:21669`, `:21738`) and `Plater::select_plate` (`:24347`, `:24403`). |
| `Preview::load_shells` → `GLCanvas3D::load_shells` → `GCodeViewer::load_shells` | `src/slic3r/GUI/GUI_Preview.cpp:383-386`, `src/slic3r/GUI/GLCanvas3D.cpp:3079-3085`, `src/slic3r/GUI/GCodeViewer.cpp:3139` | Guarded by `if (m_initialized)` — so on a hidden instance it is a **silent no-op**: no shells ever load. Shells themselves are `GLVolume`s (CPU `init_from`), uploaded lazily at render. |
| `GLCanvas3D::reload_scene` (builds the `GLVolume`s the plate thumbnails draw) | `src/slic3r/GUI/GLCanvas3D.cpp:2507-2515` | `if (!m_initialized) return;` at `:2511-2512`, then `_set_current()` at `:2515`. **On a hidden instance it returns immediately → `m_volumes` stays empty → every plate thumbnail is empty.** `m_reload_delayed` (`:2569`) is computed from `m_canvas->IsShown()`, which is `true`, so that path is not the problem. |
| `Plater::update_all_plate_thumbnails` → `GLCanvas3D::render_thumbnail` | `src/slic3r/GUI/Plater.cpp:19584-19597`, `src/slic3r/GUI/GLCanvas3D.cpp:2246/2258/2269/2308` | **No.** None of the four `render_thumbnail` overloads calls `_set_current()` or `init_opengl()`. They call `wxGetApp().get_shader("thumbnail")` (null until `init_gl()` runs) and switch on `OpenGLManager::get_framebuffers_type()` (`Unknown` until `init_gl()` runs → falls through to `render_thumbnail_legacy`). |
| `IMToolbarItem::generate_texture` (`glGenTextures`/`glTexImage2D`) and `~IMToolbarItem` (`glDeleteTextures`) | `src/slic3r/GUI/IMToolbar.cpp:23-45`, `:17-20` | **No.** Called from `GLCanvas3D::_update_imgui_select_plate_toolbar` (`src/slic3r/GUI/GLCanvas3D.cpp:7059-7085`, `generate_texture()` at `:7077`), itself called from `update_plate_thumbnails()` (`:2367-2370`, reached from `Plater::force_update_all_plate_thumbnails`, `src/slic3r/GUI/Plater.cpp:19611-19620`), from `on_idle` (`:3254`) and from `on_set_focus` (`:4858`). |
| Toolbar / gizmo / layers-editing texture loads | `src/slic3r/GUI/GLCanvas3D.cpp:1246-1259`, `:6857-6884`, `:7033-7046` | Only run inside `init()`, so they inherit whatever `init()`'s caller made current. |
| `Plater::priv::on_process_completed` | `src/slic3r/GUI/Plater.cpp:15696` | Touches GL through `update_fff_scene()` (`:14307-14314` → `preview->reload_print()` + two `reload_scene(true)`), `update_fff_scene_only_shells()` (`:14318-14331`), and `preview->reload_print(false)` at `:15939`. All of these bottom out in the rows above. **No make-current on the way.** |
| `GLModel` GPU upload | `src/slic3r/GUI/GLModel.cpp:608-612`, `:742-775` | Lazy — `send_to_gpu()` runs from inside `GLModel::render()`, i.e. always inside a render pass. **No change needed.** |
| `GLCanvas3D::render_gcode_preview_image` | `src/slic3r/GUI/GLCanvas3D.cpp:6643-6651` | **Yes — this one is already self-sufficient**: `if (!_set_current() \|\| !wxGetApp().init_opengl() \|\| (!is_initialized() && !init()))` at `:6651`. It is the model for everything else. |

**Key consequence of the shared context:** `wglMakeCurrent` binds `(HDC, HGLRC)` to the *calling
thread*, and all of this runs on the GUI thread (`RemoteAccess::run_on_main`,
`src/slic3r/GUI/RemoteAccess.cpp:114-124`, marshals every request there). Once **one**
successful `_set_current()` has happened on the GUI thread, the context stays current for the
life of that thread; later `SetCurrent` calls only swap which canvas' *default* framebuffer is
the target, which is irrelevant to FBO work. So a one-shot warm-up plus cheap defensive
make-currents at the few entry points is sufficient — no per-call save/restore is needed.

#### 4. The actual blockers (in the order the phone hits them)

1. **`Plater::priv::set_current_panel` bails on the very first call** —
   `src/slic3r/GUI/Plater.cpp:15070-15076`:
   ```cpp
   wxPanel* old_panel = current_panel;
   if (!old_panel) {
       panel->Show();
       if (!panel->IsShownOnScreen())
           return;            // <-- hidden frame: always taken
   }
   ```
   `current_panel` therefore stays `nullptr` forever, which cascades:
   `is_preview_shown()` (`:10230`, `current_panel == preview`) is permanently false, so
   `ensure_preview_loaded`'s `select_view_3D("Preview")` never takes effect
   (`src/slic3r/GUI/RemoteAccess.cpp:357`); `do_reslice()`'s
   `if (!preview->get_canvas3d()->is_initialized()) preview->get_canvas3d()->render(true);`
   (`:15003-15006`) never runs; `bind_event_handlers()` (`:15137`, `:15165`) is never called;
   `Plater::priv::update`'s `if (current_panel && q->is_preview_shown())
   q->force_update_all_plate_thumbnails()` (`:11192-11194`) never fires. Note the side effect
   that `panel->Show()` *has* already run before the return, so both View3D and Preview end up
   `IsShown()` — accidental and fragile, but it is why `Preview::IsShown()` is currently true.
2. **Nothing ever initialises GL.** `GLCanvas3D::render` returns at
   `src/slic3r/GUI/GLCanvas3D.cpp:1994` (`!_is_shown_on_screen()`), `on_paint`
   (`:4839-4845`, the normal first-init trigger) never fires because there is no `WM_PAINT` for a
   hidden window, `on_idle` (`:3243-3246`) returns on `!m_initialized` *and* is not even bound,
   and `GUI_App::post_init`'s warm-up block is doubly unreachable:
   `src/slic3r/GUI/GUI_App.cpp:1063` guards it with
   `plater_->canvas3D()->get_wxglcanvas()->IsShownOnScreen() && ...make_current_for_postinit()`,
   **and** the whole block sits inside `if (!switch_to_3d)` (`:1054`) — `switch_to_3d` is set
   `true` whenever an input file was passed (`:1017`, `:1022`), which is exactly how
   `run_hub_app.py` starts the app. So even fixing the `IsShownOnScreen` half is not enough.
3. Result today: no shaders (`get_shader()` returns `nullptr`), `s_framebuffers_type ==
   Unknown` → `render_thumbnail` falls to `render_thumbnail_legacy`
   (`src/slic3r/GUI/GLCanvas3D.cpp:6780-6792`), which clamps `w`/`h` by the canvas size and hands
   back a tiny or invalid `ThumbnailData`; `m_volumes` is empty anyway.
4. **A public make-current already exists:** `GLCanvas3D::make_current_for_postinit()`
   (`src/slic3r/GUI/GLCanvas3D.cpp:1970-1972`, declared `src/slic3r/GUI/GLCanvas3D.hpp:1187`) is
   nothing but a public wrapper around the private `_set_current()`; `init()` is already public
   (`GLCanvas3D.hpp:736`). The new helper below folds those two plus `init_opengl()` and the
   ImGui display size into one call.

#### 5. Zero-size / not-shown audit of the phone's paths

| # | Site | file:line | Verdict |
|---|---|---|---|
| 5.1 | `Preview::load_print_as_fff`'s `if (IsShown())` around the whole G-code load | `src/slic3r/GUI/GUI_Preview.cpp:734` | **No change needed.** `IsShown()` is the panel's own flag (`wx/window.h:650`), not `IsShownOnScreen()`; once change #1 lets `set_current_panel` complete, `preview->Show()` (`Plater.cpp:15090`) makes this true whenever the Preview panel is the current panel — exactly the desktop semantics. Do **not** "fix" it to something weaker: when View3D is current the Preview panel is deliberately hidden (`Plater.cpp:15091-15093`) and skipping the load is correct. |
| 5.2 | `Preview::refresh_print`'s `if (!IsShown()) return;` | `src/slic3r/GUI/GUI_Preview.cpp:375-376` | **No change needed**, same reasoning. `ensure_preview_loaded` already calls `select_view_3D("Preview")` (`RemoteAccess.cpp:357`) before `refresh_print()` (`:361`). |
| 5.3 | `Preview::reload_print`'s `if (!IsShown())` early-out | `src/slic3r/GUI/GUI_Preview.cpp:339-347` | Inside `#ifdef __linux__`. **No change needed on Windows.** |
| 5.4 | `Plater::priv::set_current_panel` `IsShownOnScreen()` early return | `src/slic3r/GUI/Plater.cpp:15074` | **Must change** (see #1 in Changes). |
| 5.5 | `GLCanvas3D::get_canvas_size()` on a never-laid-out canvas | `src/slic3r/GUI/GLCanvas3D.cpp:4883-4900` | Returns `m_canvas->GetSize()`. On MSW a child created with `wxDefaultSize` is created **20x20**, not 0x0 (`.../include/wx/window.h:1806-1807`, used by `wxWindowMSW::MSWCreate`, `.../src/msw/window.cpp:3963-3964`); it may be the real layout size if the AUI manager laid out the hidden frame. Either way it is only consulted by `render()` (clamped to ≥10 at `:2024-2025`), by `render_thumbnail_legacy` (`:6783-6789`) and by imgui sizing. **No change needed**, but the warm-up must publish a sane imgui display size (see #2 in Changes) and we should log the observed size once. |
| 5.6 | `Camera::set_viewport` / `apply_viewport` / `apply_projection` | `src/slic3r/GUI/Camera.cpp:225-233`, `:235-275`, `assert(left != right && bottom != top && near_z != far_z)` at `:279` | The assert can only trip on a **zero**-sized viewport. Both offscreen paths set their own: `render_thumbnail_internal` uses `thumbnail_data.width/height` (`GLCanvas3D.cpp:6257-6258`, 512x512), `render_gcode_preview_image` uses the requested `w/h` clamped to 64..2048 (`RemoteAccess.cpp:437-438`, `GLCanvas3D.cpp:6698`). **No change needed.** |
| 5.7 | `IMSlider` mutation from the preview.png route (`SetSelectionSpan`, `set_as_dirty`, `SetSliderValues`, `SetMaxValue`) | `src/slic3r/GUI/IMSlider.cpp:194-216`, called from `RemoteAccess.cpp:459-465` and `Preview::update_layers_slider` (`GUI_Preview.cpp:558-653`) | Pure data + a dirty flag; the slider only touches GL/imgui inside `GCodeViewer::render_slider`, which never runs offscreen. **No change needed.** |
| 5.8 | `Preview::update_layers_slider` reads `m_gcode_result->print_statistics.modes.front()` | `src/slic3r/GUI/GUI_Preview.cpp:615` | Unrelated to visibility; already reached only when `zs` is non-empty (`:782-783`). **No change needed.** |
| 5.9 | `Plater::priv::update` → `force_update_all_plate_thumbnails()` gated on `current_panel && is_preview_shown()` | `src/slic3r/GUI/Plater.cpp:11192-11194` | Starts working once change #1 lands. It reaches `update_plate_thumbnails()` → `_update_imgui_select_plate_toolbar()` → raw `glGenTextures`, so it needs the guard in change #4. |
| 5.10 | `Plater::select_plate`'s synchronous `wxGetApp().plater()->canvas3D()->render()` | `src/slic3r/GUI/Plater.cpp:24311-24312` | `render()` no-ops on a hidden canvas. Harmless — the caller does not consume a result. **No change needed.** |
| 5.11 | `MainFrame::select_tab`'s `m_plater->canvas3D()->render()` for `tp3DEditor` | `src/slic3r/GUI/MainFrame.cpp:3823-3824` | Same; no-op. **No change needed.** |
| 5.12 | `GUI_App::post_init` GL warm-up block | `src/slic3r/GUI/GUI_App.cpp:1054-1090` | Unreachable for a hidden instance for two independent reasons (§4.2). Leave it alone for the desktop path; add a separate hidden-instance warm-up (change #3). |
| 5.13 | `camera.requires_zoom_to_bed` / `requires_zoom_to_plate` / `requires_zoom_to_volumes` | set e.g. `src/slic3r/GUI/Plater.cpp:15234`; consumed only in `GLCanvas3D::render`, `src/slic3r/GUI/GLCanvas3D.cpp:2027-2043` | They simply accumulate while hidden and are honoured by the first real render after the window is shown — which is the behaviour we want. The offscreen paths build their own `Camera` (`GLCanvas3D.cpp:6254`, `:6694`) and never read them. **No change needed.** |
| 5.14 | `GLCanvas3D::on_idle` after warm-up | `src/slic3r/GUI/GLCanvas3D.cpp:3243-3287` | Today it is never bound (see §4.1); after change #1 it will be. It returns before rendering (`_refresh_if_shown_on_screen()` at `:3274` no-ops when hidden, `m_dirty = false` at `:3286`) but it *does* call `_update_imgui_select_plate_toolbar()` at `:3254` and `update_notifications()` at `:3257` on every idle. Covered by change #4; measure the idle cost (Risks). |
| 5.15 | `GLCanvas3D::request_extra_frame` / `set_as_dirty` used by the preview.png route (`RemoteAccess.cpp:463-464`) | `src/slic3r/GUI/GLCanvas3D.hpp:1101`, `GLCanvas3D.cpp:1327-1330` | Flag setters only. **No change needed.** |
| 5.16 | `Plater::priv::update_preview_bottom_toolbar` | `src/slic3r/GUI/Plater.cpp:17042-17045` | Empty body. **No change needed.** |

#### 6. The Prepare-tab plate thumbnails are pure FBO renders

- `Plater::update_all_plate_thumbnails(force)` (`src/slic3r/GUI/Plater.cpp:19584-19597`) loops the
  plates and calls `get_view3D_canvas3D()->render_thumbnail(plate->thumbnail_data, 512, 512,
  params, Camera::EType::Ortho)` plus the `ban_light` variant; `RemoteAccess::api_plate_thumbnail`
  (`src/slic3r/GUI/RemoteAccess.cpp:313-338`) calls the same and re-renders the current plate
  itself at `:326`.
- The overloads at `src/slic3r/GUI/GLCanvas3D.cpp:2269-2299` and `:2308-2340` pick the shader
  (`"thumbnail"`, or `"flat"` for picking) and dispatch on
  `OpenGLManager::get_framebuffers_type()` to `render_thumbnail_framebuffer` (`:6428`),
  `..._ext` (`:6537`) or `..._legacy` (`:6780`).
- `render_thumbnail_framebuffer` and `render_thumbnail_internal` are **`static`**
  (`src/slic3r/GUI/GLCanvas3D.hpp:948-959`) — they cannot touch the canvas at all. The framebuffer
  variant builds its own FBO + colour renderbuffer/texture + depth renderbuffer, checks
  `glCheckFramebufferStatus`, renders and `glReadPixels` (`:6428-6535`).
  `render_thumbnail_internal` builds its own `Camera`, sets its viewport from
  `thumbnail_data.width/height` (`:6255-6258`), computes the view, bails out if the shader is null
  (`:6317-6320`), and draws `vol->simple_render(...)` per visible `GLVolume` (`:6383-6412`).
  **No `SwapBuffers`, no imgui, no canvas size, no default framebuffer.**
- What they need: (a) a current context, (b) `init_gl()` to have run — for the GLEW framebuffer
  entry points, for `s_framebuffers_type != Unknown`, and for `get_shader("thumbnail")` to be
  non-null — and (c) a populated `m_volumes` on the View3D canvas, which means `reload_scene`
  must have run with `m_initialized == true`.
- `render_thumbnail_legacy` (`:6780-6805`) is the only visibility-sensitive variant (it clamps to
  the canvas size and reads the default framebuffer). It is unreachable once `init_gl()` has run
  on any machine that supports FBOs — which is every machine that can run this app.

---

### Changes

All line numbers are pre-change.

#### 1. `Plater::priv::set_current_panel` — do not bail on a hidden instance
**File** `src/slic3r/GUI/Plater.cpp`, function `Plater::priv::set_current_panel`, at `:15070-15076`.

```cpp
    wxPanel* old_panel = current_panel;
//#if BBL_HAS_FIRST_PAGE
    if (!old_panel) {
        //BBS: only switch to the first panel when visible
        panel->Show();
        // Ultra: an instance that serves the phone is never shown on screen, so this test would
        // never pass and current_panel would stay null forever — is_preview_shown(), the
        // do_reslice() branch below and bind_event_handlers() would all be dead. The panels are
        // Show()n/Hide()n normally either way, so Preview::IsShown() keeps its usual meaning.
        if (!panel->IsShownOnScreen() && !wxGetApp().is_hidden_instance())
            return;
    }
```
This single change unblocks `is_preview_shown()`, `Preview::refresh_print()`,
`Preview::load_print_as_fff`'s `if (IsShown())`, the `do_reslice()` path (including
`preview->get_canvas3d()->render(true)` at `:15005`, which becomes a no-op that the warm-up in
change #3 makes redundant), and `Plater::priv::update`'s thumbnail refresh at `:11192`.
Side effect: `bind_event_handlers()` starts running, so `on_idle` becomes live — change #4 covers
the GL it touches.

#### 2. New public helper `GLCanvas3D::ensure_gl_ready()`
**File** `src/slic3r/GUI/GLCanvas3D.hpp`, public section next to `init()` (`:736`) and
`make_current_for_postinit()` (`:1187` — that one is private-adjacent; keep it, this is its
public superset):

```cpp
    // Ultra: make this canvas' GL context current and finish the one-time GL init, with no
    // dependency on the canvas ever having been shown on screen. render() does the same work
    // (GLCanvas3D.cpp:1994-1998) but refuses to run when !_is_shown_on_screen(); an instance
    // that serves the phone is never shown, so every offscreen path calls this first.
    // Returns false if OpenGL is unusable (then the caller must fail the request, not draw).
    bool ensure_gl_ready();
```

**File** `src/slic3r/GUI/GLCanvas3D.cpp`, next to `make_current_for_postinit` (`:1970`):

```cpp
bool GLCanvas3D::ensure_gl_ready()
{
    if (m_canvas == nullptr || m_context == nullptr)
        return false;
    // wglMakeCurrent on this canvas' own DC. The HWND and its pixel format exist from
    // construction (wxGLCanvas::CreateWindow / ::SetPixelFormat), so a hidden window is fine
    // on MSW; wx says so itself in wxGLCanvasBase::SetCurrent().
    if (!_set_current())
        return false;
    // glewInit + framebuffer-type detection + shader compilation. Must come after the
    // make-current, and must have happened before ANY GLEW-dispatched call anywhere.
    if (!wxGetApp().init_opengl())
        return false;
    if (!m_initialized) {
        // ImGui::NewFrame() asserts on the default DisplaySize of (-1,-1), and a never-laid-out
        // canvas is 20x20 (wx default child size), so publish something usable once. The
        // offscreen paths never call new_frame(), but init() -> on_change_color_mode() ->
        // ImGuiWrapper::on_change_color_mode() and any later idle work assume a sane io.
        const Size cnv = get_canvas_size();
        wxGetApp().imgui()->set_display_size(std::max(10.0f, float(cnv.get_width())),
                                             std::max(10.0f, float(cnv.get_height())));
        if (!init())
            return false;
    }
    return true;
}
```
(If the debug-build `wxASSERT_MSG(IsShown())` in `wxGLCanvasBase::SetCurrent` ever becomes a
problem, change `_set_current()`'s body at `:7200-7203` to
`m_context->SetCurrent(*m_canvas)` — the `wxGLContext` overload, `.../src/msw/glcanvas.cpp:645`,
which has no assert.)

**File** `src/slic3r/GUI/Plater.hpp` / `Plater.cpp` — one convenience wrapper:

```cpp
bool Plater::ensure_gl_ready()
{
    // View3D last, so the 3D canvas' drawable is the one left current — that is the canvas
    // whose m_volumes the plate thumbnails draw.
    const bool pv = p->preview->get_canvas3d()->ensure_gl_ready();
    const bool v3 = p->view3D->get_canvas3d()->ensure_gl_ready();
    return pv && v3;
}
```

#### 3. Warm up at the top of `GUI_App::post_init`
**File** `src/slic3r/GUI/GUI_App.cpp`, function `GUI_App::post_init`, immediately after the
`initialized()` check at `:1002-1005` and **before** the input-file handling at `:1010`
(the existing block at `:1054-1090` stays exactly as it is for the desktop path — it is inside
`if (!switch_to_3d)` and behind `IsShownOnScreen()`, so it cannot serve us):

```cpp
    // Ultra: an instance launched to serve the phone is never shown, so it gets no WM_PAINT and
    // GLCanvas3D::render() — the usual trigger for init_opengl()/init() — never runs. Do that
    // work here, before any project is loaded: without it there are no shaders, no GLEW entry
    // points, no framebuffer type, and reload_scene() bails on !m_initialized so there are no
    // GLVolumes for the plate thumbnails to draw.
    if (is_hidden_instance() && plater_ != nullptr) {
        if (!plater_->ensure_gl_ready())
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": hidden instance: OpenGL warm-up FAILED; "
                                        "thumbnail and preview routes will return errors";
        else
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": hidden instance: OpenGL ready, view3D canvas "
                                    << plater_->get_view3D_canvas3D()->get_canvas_size().get_width() << "x"
                                    << plater_->get_view3D_canvas3D()->get_canvas_size().get_height();
        // Give the Plater a current panel so is_preview_shown()/select_view_3D() work from here on.
        mainframe->select_tab(size_t(MainFrame::tp3DEditor));
        plater_->select_view_3D("3D");
    }
```
Order matters: warming up *before* `load_files` (`:1030-1044`) means the normal
`load_files → Plater::priv::update() → view3D->reload_scene()` chain (`Plater.cpp:11187`) builds
the `GLVolume`s with no extra forced reload. If a later refactor moves the warm-up after the
load, add `plater_->get_view3D_canvas3D()->reload_scene(true, true);` right after it.

#### 4. Make-current at the GL sites that run outside a render pass
Each is one line; all are cheap (`wglMakeCurrent` with the already-current pair is a no-op in the
driver) and all also protect the *shown* app against ordering bugs.

**4a.** `src/slic3r/GUI/GLCanvas3D.cpp:2269` and `:2308` — both non-delegating
`render_thumbnail` overloads, first statement:
```cpp
    // Ultra: the thumbnail FBO path is reached from Plater::update_all_plate_thumbnails and from
    // the phone API without any render pass around it; nothing else makes a context current.
    if (!ensure_gl_ready())
        return;
```
This covers `Plater::update_all_plate_thumbnails` (`Plater.cpp:19584`),
`force_update_all_plate_thumbnails` and every 3MF-export caller in one place.

**4b.** `src/slic3r/GUI/GLCanvas3D.cpp:3093` `load_gcode_preview`, before
`m_gcode_viewer.init(...)`:
```cpp
    // GCodeViewer::init()/load() create GL buffers and textures directly (GCodeViewer.cpp:2919,
    // :3074); this is called from Preview::load_print_as_fff, outside any render pass.
    if (!ensure_gl_ready())
        return;
```

**4c.** `src/slic3r/GUI/GLCanvas3D.hpp:786` — move `reset_gcode_toolpaths()` out of line into
`GLCanvas3D.cpp`:
```cpp
void GLCanvas3D::reset_gcode_toolpaths()
{
    // GCodeViewer::reset() -> VBuffer/IBuffer::reset() -> glDeleteBuffers (GCodeViewer.cpp:135,
    // :147, :166). Deleting against the wrong/absent context silently leaks VRAM.
    _set_current();
    m_gcode_viewer.reset();
}
```

**4d.** `src/slic3r/GUI/GLCanvas3D.cpp:7059` `_update_imgui_select_plate_toolbar`, right after the
`is_enabled()/is_render_finish` early-out:
```cpp
    // del_all_item() -> glDeleteTextures and generate_texture() -> glGenTextures/glTexImage2D
    // (IMToolbar.cpp:17-20, :23-45). Reached from on_idle() and from update_plate_thumbnails().
    if (!ensure_gl_ready())
        return false;
```

#### 5. Route guards in `RemoteAccess.cpp`
Belt-and-braces on top of 4a/4b, so a failed warm-up produces a clean HTTP error instead of a
blank PNG.

**5a.** `src/slic3r/GUI/RemoteAccess.cpp:313` `api_plate_thumbnail`, inside the `run_on_main`
lambda before `plater->update_all_plate_thumbnails(false)` (`:321`) — add a
`std::shared_ptr<std::string> err` alongside `data` and:
```cpp
        if (!plater->get_view3D_canvas3D()->ensure_gl_ready()) { *err = "the 3D view is not ready"; return; }
```
then after the wait: `if (!err->empty()) { r.status = 503; r.body = json_error(*err); return r; }`.

**5b.** `src/slic3r/GUI/RemoteAccess.cpp:343` `ensure_preview_loaded`, after the plate/slice
checks and **before** `plater->select_view_3D("Preview")` (`:357`):
```cpp
    // The G-code viewer's buffers are built by Preview::load_print_as_fff -> load_gcode_preview
    // with no render pass around it, and GLCanvas3D::load_shells is a no-op until the preview
    // canvas is initialised (GLCanvas3D.cpp:3081).
    if (!plater->get_preview_canvas3D()->ensure_gl_ready())
        return "the preview is not ready";
```

**5c.** `src/slic3r/GUI/RemoteAccess.cpp:445` `api_plate_preview_png`: no change strictly needed —
`render_gcode_preview_image` already does `_set_current() + init_opengl() + init()` at
`GLCanvas3D.cpp:6651` — but `ensure_preview_loaded` (5b) now runs first and covers the
toolpath-loading half, which is where the real gap was.

`api_slice` (`:693`) needs nothing new: it posts `EVT_GLTOOLBAR_SLICE_PLATE`
(`:708`) → `Plater::priv::on_action_slice_plate` (`Plater.cpp:16025`) → `q->reslice()` +
`select_view_3D("Preview")` (`:16047`), and on completion `on_process_completed` (`:15696`) →
`update_fff_scene()` (`:15865`) / `preview->reload_print(false)` (`:15939`) — all of which now
funnel through 1, 4b, 4c and 5b.

#### 6. Optional: keep `make_current_for_postinit()` but express it via the new helper
`src/slic3r/GUI/GLCanvas3D.cpp:1970-1972` stays as-is (it is the desktop `post_init` path); no
behaviour change. Do not delete it — `GUI_App.cpp:1063` still calls it.

---

### Test plan for this phase

Scratchpad helpers (already present in this session's scratchpad directory):
`run_hub_app.py` (launches `build/Snapmaker_Orca/snapmaker-orca.exe` detached with
`SNORCA_PHONE_ACCESS=testtoken12345`, opening `hubtest/h2d_copy.3mf`; `--new` adds
`SNORCA_NEW_INSTANCE=1`), `wait_for_hub.py [N]` (polls `http://127.0.0.1:13640/hub/info` and
`/api/instances` until N instances are listed), `test_preview.py [plate]` (preview info + PNG
frames at several views/layers, slices first if needed), `test_preview2.py [plate]` (zoom/pan
frames + the `/preview/status` route). Every instance route is reachable through the hub as
`http://10.0.0.131:13640/r/<token>/i/<pid>/api/...` — the two test scripts already resolve
`<token>` from `/hub/info` and `<pid>` from `/api/instances`, so they work unchanged against a
hidden instance.

1. **Launch hidden.** `python run_hub_app.py --new` with Phase 1's hidden flag/env set, then
   `python wait_for_hub.py 1`. Confirm with Task Manager / `tasklist` that the process exists and
   with `Get-Process ... | %{$_.MainWindowHandle}` that it has **no** main window. Grep the log for
   `hidden instance: OpenGL ready, view3D canvas WxH` — record the reported W and H (item R4).
2. **Thumbnails for every plate.** For `i` in `0..plates-1`:
   `GET /i/<pid>/api/plates/<i>/thumbnail.png`. Each must be HTTP 200, `image/png`, 512x512, and
   **non-blank** — check with `struct.unpack(">II", b[16:24])` for size and a quick alpha/pixel
   histogram (a fully transparent or single-colour PNG is the exact signature of "no shaders /
   no volumes / no context"). Compare byte-for-byte against thumbnails from the same project on a
   normally-shown instance; they should be pixel-identical (same static FBO code, same Ortho
   camera, no canvas-size input).
3. **Slice from the phone.** `POST /i/<pid>/api/slice?plate=0`, then poll
   `/api/jobs/<job>` to `state != running`. Expect success, no assertion dialog, no crash.
   Watch the log for `OpenGL error in ...` (Debug build) — see item R1.
4. **Preview info + PNG.** `python test_preview.py 0`: `/plates/0/preview` must report
   `sliced: true`, a non-empty `layers` array and a defined `box`; then the six
   view/layer frames (`front` at top/mid/0, `rear`, `left`, `right`) must all be 200 with
   plausible byte sizes. Then `python test_preview2.py 0` for `zoom=1/2/4/99` and the
   `cx`/`cy` pans, checking `X-Preview-Zoom` and that `z2 != fit` and `z2_tl != z2`.
   Open the written `pv_*.png` / `pv2_*.png` and confirm toolpaths **and** the grey bed slab are
   visible (the slab proves the `"flat"` shader compiled, i.e. `init_gl()` ran).
5. **Layer slider round-trip.** `GET /plates/0/preview` after step 4 and confirm `range` moved to
   the last requested layer (proves `IMSlider::SetSelectionSpan` + `set_layers_z_range` took
   effect with no render).
6. **Re-slice.** Change something (`POST /api/object/transform?obj=0&rz=15`, or just
   `POST /api/slice?plate=0` again), wait for the job, and confirm `/plates/0/preview` reports a
   **new** `result_id` and that `preview.png` differs from the previous frame. This exercises
   `on_process_completed → update_fff_scene → preview->reload_print` and
   `reset_gcode_toolpaths` (`glDeleteBuffers`) on a hidden instance.
7. **Second plate.** Repeat 2/3/4 for `plate=1` (add one if the test project has a single plate
   via the plates API). This exercises `Plater::select_plate` (`Plater.cpp:24305`) including its
   `is_preview_shown()` branches, which only behave correctly once change #1 lands.
8. **Thumbnail invalidation.** After the transform in step 6, re-fetch every plate thumbnail and
   confirm the changed plate's image actually changed (proves `invalid_all_plate_thumbnails` +
   the re-render at `RemoteAccess.cpp:326` still work).
9. **Show the window.** Trigger Phase 1's show path. Then confirm on the desktop that: the 3D
   view paints correctly on the first frame (toolbars, gizmos and bed textures all present — they
   were created during the warm-up, not during the first paint); switching to Preview shows the
   same toolpaths and the same layer range the phone last requested; the plate thumbnails in the
   Preview plate list are the same images the phone got; and the camera zoom-to-bed/plate happens
   on that first frame (`GLCanvas3D.cpp:2027-2043`). Then slice again from the GUI and re-run
   `test_preview.py` to confirm the phone and the desktop stay consistent.
10. **Leak / stability soak.** Loop steps 2 and 4 ~200 times against the hidden instance while
    watching the process's GPU memory and handle count (Task Manager → GPU memory column, or
    `Get-Process | Select GDIHandles,Handles`). Flat is pass; a steady climb points at the
    `glDeleteBuffers`/`glDeleteTextures` sites in change #4 running against the wrong context.
11. **Regression on the normal path.** Launch without the hidden flag and confirm nothing changed:
    start-up time (the `StartupProfiler` marks in `GUI_App::post_init`), first-paint appearance,
    slicing, plate thumbnails, and that the phone API still works from a shown instance.

---

### Risks / must-verify-at-runtime list

- **R1 — Does `wglMakeCurrent` on the hidden canvas' DC actually succeed on this box?**
  The code says it should (wx's own comment, `.../src/common/glcmn.cpp:81-84`) and the CLI already
  renders thumbnails against an invisible GLFW window (`src/Snapmaker_Orca.cpp:5746`), but the DC
  of a never-shown window on a particular driver is the one thing only a run can settle.
  *Quickest experiment:* build **Debug** (so `HAS_GLSAFE` is on, `src/slic3r/GUI/3DScene.hpp:20-22`,
  and every GL error `assert(false)`s at `src/slic3r/GUI/3DScene.cpp:64`) and run step 1-2 of the
  test plan. If `ensure_gl_ready()` returns true and the thumbnail is non-blank, the answer is yes.
  A cheaper smoke test: log `::wglGetCurrentContext()` and `::glGetString(GL_RENDERER)` right after
  `_set_current()` in `ensure_gl_ready()`.
- **R2 — Does the driver clip or refuse FBO rendering when no window is visible?**
  Nothing in the spec allows it (FBOs are independent of the drawable), and the CLI path is
  evidence against it, but some Optimus/hybrid setups behave oddly with a hidden window's DC.
  *Quickest experiment:* compare a hidden-instance thumbnail byte-for-byte with a shown-instance
  one (test plan step 2). Any difference beyond zero means the drawable is leaking into the FBO.
- **R3 — GPU selection.** On a laptop with switchable graphics, a hidden window may land on the
  integrated GPU while the shown app gets the discrete one, changing `GL_MAX_SAMPLES` and thus the
  multisample branches (`GLCanvas3D.cpp:6435-6440`, `:6656`).
  *Quickest experiment:* log `OpenGLManager::get_gl_info().get_renderer()` from `ensure_gl_ready()`
  and compare hidden vs shown.
- **R4 — What size are the canvases on a hidden frame?**
  Code-level answer is 20x20 (`wx/window.h:1806-1807`) unless the AUI manager laid out the hidden
  frame, in which case it is the real layout size. It only matters for imgui's `DisplaySize` and
  for `render_thumbnail_legacy`. *Quickest experiment:* the log line already added in change #3.
- **R5 — `on_idle` cost after change #1.** Binding the event handlers makes `on_idle` run on every
  idle for a canvas that will never paint; it calls `_update_imgui_select_plate_toolbar()` and
  `update_notifications()` each time. *Quickest experiment:* after step 1, watch the process's CPU
  for 60 s at idle. If it is not ~0%, add an early
  `if (wxGetApp().is_hidden_instance()) return;` at the top of `GLCanvas3D::on_idle`
  (`src/slic3r/GUI/GLCanvas3D.cpp:3243`) — safe, since nothing it does is needed while hidden.
- **R6 — Does `set_current_panel` have other side effects that assume a visible frame?**
  It calls `update_sidebar(true)` (`Plater.cpp:15095`), camera view load/save (`:15098-15112`),
  gizmo state resets (`:15165-15170`) and `current_panel->SetFocusFromKbd()` (`:15238`). None reads
  visibility, but `SetFocusFromKbd` on a hidden frame is untested here.
  *Quickest experiment:* step 1 + step 3; a focus problem shows up as an immediate hang or as
  keyboard focus being stolen from the foreground app.
- **R7 — Does the app still get idle events, i.e. does `post_init` ever run, with no visible
  window?** wx's idle processing is driven by the empty message queue, not by visibility, and
  `mainframe` still exists so the main loop stays alive — but Phase 1's show/hide mechanics could
  change that. *Quickest experiment:* the `hidden instance: OpenGL ready` log line from change #3
  is itself the probe; if it never appears, `post_init` is not running and the warm-up must move to
  the end of `on_init_inner` (after `SetTopWindow`, `GUI_App.cpp:3031`) or to the first API request.
- **R8 — A modal dialog on the hidden GUI thread deadlocks `run_on_main`.**
  `run_on_main` times out at 15 s/30 s/60 s (`RemoteAccess.cpp:114`, `:328`, `:407`, `:468`) and
  `AutoConfirmScope` (`:110-112`) suppresses some prompts, but `init_gl()` itself pops
  `wxMessageBox` on an old OpenGL version or a shader compile failure
  (`OpenGLManager.cpp:281`, `:294`) — invisible and unclosable on a hidden instance.
  *Fix if confirmed:* call `m_opengl_mgr.init_gl(/*popup_error=*/false)` from a hidden instance;
  `GUI_App::init_opengl` (`GUI_App.cpp:2093`) currently hard-codes the defaulted `true`.
  *Quickest experiment:* temporarily force the version check to fail and confirm the process does
  not wedge.
- **R9 — `detect_multisample` clears the current context.**
  `wxGLCanvas::IsDisplaySupported` ends with `::wglMakeCurrent(NULL, NULL)`
  (`.../src/msw/glcanvas.cpp:1166`), so anything that creates a `wxGLCanvas` *after* the warm-up
  would silently unbind the context. Today all three canvases are built in the `Plater::priv` ctor
  (`Plater.cpp:10727-10732`) and `create_wxglcanvas` has no other call site, so this is currently
  a non-issue — but it is a landmine for anyone adding a fourth GL canvas.
  *Quickest experiment:* a one-line `assert(::wglGetCurrentContext() != nullptr)` at the top of
  `render_thumbnail` in a Debug build.
- **R10 — Blank-but-valid PNGs are the silent failure mode.** Every check in the current routes
  (`data->is_valid()` at `RemoteAccess.cpp:333`, `:475`) passes for an all-zero image. Make the
  tests assert on pixel content, not just on HTTP 200 and dimensions.

## Phase 3 — Dialogs, blocking and attention

Goal: a hidden instance never waits on a human. Every modal reachable from a phone request or
from a background event answers itself with a safe default and is logged; anything that cannot be
answered safely shows the main window and reports `needs_attention` so the phone can say so.

Assumed Phase 1 contract (consumed here, not built here):
`RemoteAccess::hidden()` — this instance was launched hidden and `mainframe->IsShown()` is false;
`RemoteAccess::show_window(reason)` — GUI thread, `mainframe->Show(true) + Raise()`, clears hidden.

---

### Findings

#### Structural facts (the choke points)

* `run_on_main` — `src/slic3r/GUI/RemoteAccess.cpp:114`. `wxGetApp().CallAfter` + `std::promise`,
  default 15 s. The lambda is wrapped in `RemoteAccess::AutoConfirmScope` (`RemoteAccess.cpp:117`),
  so `auto_confirm()` (`RemoteAccess.cpp:110`) is true **only for the duration of that lambda**.
  19 call sites (`grep -c "run_on_main(" RemoteAccess.cpp` = 19).
* **A modal does not stop the event loop.** wx runs a nested loop inside `ShowModal()`, so queued
  `CallAfter`s and `wxTimer`s still fire. Consequences: (a) other API requests keep working while a
  modal is up; (b) the request that *caused* the modal never returns → 503 and half-done work;
  (c) a GUI-thread timer is a valid watchdog even while a modal is up.
* `DPIAware<P>::ShowModal` — `src/slic3r/GUI/GUI_Utils.hpp:224`. `DPIDialog` is
  `DPIAware<wxDialog>` (`GUI_Utils.hpp:322`). Every fork dialog derives from it (MsgDialog and all
  its subclasses, UnsavedChangesDialog, SavePresetDialog, GuideFrame, SelectMachineDialog…), so
  this one method covers all of them. It pushes/pops `dialogStack`
  (`GUI_Utils.hpp:100`, `GUI_Utils.cpp:503`) — an existing "which dialogs are modal right now" list,
  already used at `GUI_App.cpp:2596` (end-session abort) and `GUI_App.cpp:4045` (dedup).
* **`wxModalDialogHook` is available** in the vendored wx —
  `deps/build/OrcaSlicer_dep/usr/local/include/wx/modalhook.h:26`, with
  `WX_HOOK_MODAL_DIALOG()` at `modalhook.h:98`. `Enter(wxDialog*)` returning anything but
  `wxID_NONE` makes `ShowModal()` return that value **without showing the dialog**, and the macro
  sits at the top of every wx `ShowModal()` implementation — including the native
  `wxMessageDialog`, `wxFileDialog`, `wxDirDialog`, `wxColourDialog`, `wxTextEntryDialog`.
  This is the single narrowest interception point for the whole process and is what Phase 3 should
  use; the existing per-class overrides stay for dialogs whose *result is read from members*.
* Style/buttons are recoverable generically: `wxMessageDialogBase::GetMessageDialogStyle()`
  (`wx/msgdlg.h:157`; `wxMessageDialogBase : public wxDialog`, `msgdlg.h:27`), and for MsgDialog
  subclasses the buttons are real children with `wxID_OK/YES/NO/CANCEL`
  (`MsgDialog::apply_style`, `MsgDialog.cpp:194`; `MsgDialog::get_button`, `MsgDialog.cpp:190`), so
  `dlg->FindWindow(wxID_YES)` works.
* `wxProgressDialog`-alikes are **not** hooked: `Slic3r::GUI::ProgressDialog : public wxDialog`
  (`src/slic3r/GUI/Widgets/ProgressDialog.hpp:27`) is driven with `Show()/Update()`, not
  `ShowModal()`. `wxPD_APP_MODAL` disables other windows but does not block the caller. Two live
  uses on API paths: `Plater.cpp:11549` (load_files "Loading…") and `Plater.cpp:18262`
  ("Importing Model"). Non-blocking, but they put a stray window on a hidden instance's desktop.

#### Already covered by `AutoConfirmScope` (grep `auto_confirm`)

| Site | What it does today |
|---|---|
| `GUI.cpp:243` (`show_error`) | Not shown; text handed to `RemoteAccess::note_error` and returned by the request. |
| `MsgDialog.cpp:365` (`MessageDialog::ShowModal`) | Returns `wxID_YES` if a Yes button exists, else `wxID_OK`. |
| `MsgDialog.cpp:388` (`RichMessageDialog::ShowModal`) | Same. |
| `MsgDialog.cpp:656` (`ErrorDialog::ShowModal`) | Returns `wxID_OK`. |
| `UnsavedChangesDialog.cpp:828` | `Transfer` if that button is offered, else `Discard`; returns `wxID_OK`. |

Gaps in today's coverage, all real:

1. **The slice path escapes the scope.** `api_slice` (`RemoteAccess.cpp:693`) ends in
   `wxPostEvent(plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE))` (`RemoteAccess.cpp:708`) and
   returns. The handler `Plater::priv::on_action_slice_plate` (`Plater.cpp:16025`) runs **after**
   the scope is destroyed, so `guard_before_slice_plate` (`Plater.cpp:16039` → `Plater.cpp:23392`),
   the temp-mixing confirm (`Plater.cpp:23434`), the mid-slice memory warning
   (`Plater.cpp:21757`) and the completion error (`Plater.cpp:15780` → `GUI.cpp:249`) all run with
   `auto_confirm() == false` and will block.
2. **Only `MessageDialog` / `RichMessageDialog` / `ErrorDialog` are patched.** `MsgDialog`
   subclasses that are not one of those three — `InfoDialog` (`MsgDialog.hpp:353`), `DownloadDialog`
   (`MsgDialog.hpp:367`), `MsgUpdateConfig` (`UpdateDialogs.cpp:40`), `MsgDataIncompatible`
   (`UpdateDialogs.cpp:261`, `:326`), `WarningDialog` (`MsgDialog.hpp:120`) — and every non-MsgDialog
   `DPIDialog` (`SavePresetDialog.hpp:27`, `WebGuideDialog.hpp:38`, `DeleteConfirmDialog`,
   `Newer3mfVersionDialog`, `NetworkErrorDialog`) are untouched.
3. **Native wx dialogs are untouched:** `wxMessageBox` (10 live sites), `wxMessageDialog` (12),
   `wxFileDialog` (36), `wxDirDialog` (6), `wxTextEntryDialog` (5), `wxColourDialog` (4).
4. **Affirmative is the wrong default for background events.** `wxID_YES` is right when the phone
   asked for the action; for an unprompted background prompt the safe answer is the do-nothing one.

#### API-reachable modals

`POST /api/project/open` (`RemoteAccess.cpp:1175`):

* `Plater::save_project(false)` (`Plater.cpp:18172`) → on failure `MessageDialog` wxOK,
  `Plater.cpp:18185`. Covered (returns OK → route reports "could not save the current project").
* `Plater::save_project(true)` would open a **`wxFileDialog`** at `Plater.cpp:12907`
  (`Plater::priv::get_export_file`, `Plater.cpp:12860`). `api_project_open` avoids it by calling
  `set_project_filename` first (`RemoteAccess.cpp:1212`) — keep that invariant.
* `Plater::load_project` (`Plater.cpp:18046`) → `close_with_confirm(check)` (`Plater.cpp:18065` →
  `Plater.cpp:20453`) → `MessageDialog` wxYES_NO_CANCEL "unsaved changes, save it before continue?"
  at `Plater.cpp:20458`; the `check` lambda (`Plater.cpp:18052`) calls
  `GUI_App::check_and_save_current_preset_changes` (`GUI_App.cpp:6378`) →
  `UnsavedChangesDialog::ShowModal` (`GUI_App.cpp:6388`). Both covered today.
  `"<loadall>"` skips `determine_load_type` (`Plater.cpp:18100`), so no load-type prompt.
* `Plater::priv::load_files` (`Plater.cpp:11511`), reached by both modes:
  `ProgressDialog` wxPD_APP_MODAL (`Plater.cpp:11549`, non-blocking);
  modified-G-code warning (`Plater.cpp:12019`); customized-preset warning (`Plater.cpp:12032`);
  colour dialog (`Plater.cpp:12208`); STEP non-UTF8 name (`Plater.cpp:12234`); step-mesh dialog
  (`Plater.cpp:12247`); "Objects with zero volume removed" (`Plater.cpp:12339`); "too small /
  in meters?" (`Plater.cpp:12346`); "multi-part object detected" (`Plater.cpp:12386`);
  "load as single object with multiple parts?" (`Plater.cpp:12497`); "file contains no geometry"
  (`Plater.cpp:12640`); "object too large, scale it down?" (`Plater.cpp:12693` wxYES-only and
  `Plater.cpp:12705` wxYES_NO, in `load_model_objects`).
  All are `MessageDialog` → covered, and `wxID_YES` is the wanted answer for each.
* `Plater::validate_current_plate` (`Plater.cpp:24488`) — notifications only, no modal. Good.

`POST /api/slice` (`RemoteAccess.cpp:693`) — **not covered today** (see gap 1):

* `Plater::confirm_filament_temp_mixing_before_slice` — `MessageDialog` wxOK|wxCANCEL,
  `Plater.cpp:23434`/`:23438`; `…_before_slice_all` at `Plater.cpp:23473`/`:23477`.
* mid-slice memory guard — `RichMessageDialog` wxYES_NO, `Plater.cpp:21757`, registered at
  `Plater.cpp:21743`. **Worst case in the codebase**: the callback runs on the *slicing worker*,
  does `CallAfter` + `future.get()` (`Plater.cpp:21785`), so the worker parks until a human clicks.
* slicing warnings after export begins — `MessageDialog` wxOK, `Plater.cpp:15689`
  (`Plater::priv::warnings_dialog`), called from `on_export_began` (`Plater.cpp:15607`).
* critical slicing error — `show_error` (`Plater.cpp:15780`) → `ErrorDialog` via `CallAfter`
  (`GUI.cpp:249`).

`POST /api/presets/select` (`RemoteAccess.cpp:842`): uses `Tab::select_preset(name,false,"",true)`
(force), so `Tab::may_discard_current_dirty_preset` (`Tab.cpp:5834`) →
`UnsavedChangesDialog` (`Tab.cpp:5838`/`:5846`) is bypassed; dirty options are captured and
re-applied (`capture_dirty`, `RemoteAccess.cpp:828`). Still reachable:
`Sidebar::update_nozzle_settings` `RichMessageDialog` (`Plater.cpp:9558`),
`TabPrinter::build_unregular_pages` nozzle-diameter prompts (`Tab.cpp:4789`, `:4899`),
`Tab::validate_custom_gcode` (`Tab.cpp:3523`), `InfoDialog` "G-code flavor is switched"
(`Tab.cpp:4339`). All `MessageDialog`/`InfoDialog`.
`Plater::priv::on_select_preset` has a **null-deref bug** at `Plater.cpp:15411-15416`
(`MessageDialog dlg(this->sidebar, _L(""), _L(""))`, then dereferences the null `preset`) — the API
path avoids it by resolving models itself (`RemoteAccess.cpp:858`).

`POST /api/settings/process` (`RemoteAccess.cpp:1070`) → `Tab::load_config` →
`Tab::on_value_change` (`Tab.cpp:1461`): prime-tower / timelapse / precise-Z prompts at
`Tab.cpp:1570, 1584, 1595, 1621, 1637, 1656, 1717, 1736, 1765, 1789, 1799, 1822, 1847, 1857`;
`Tab::show_timelapse_warning_dialog` (`Tab.cpp:1951`). All `MessageDialog`/`RichMessageDialog`,
covered, and Yes/OK is the right answer (the phone asked for the change).

`POST /api/settings/process/save` (`RemoteAccess.cpp:1135`) — **hole**:
`tab->save_preset(edited.is_system ? std::string() : edited.name)` (`RemoteAccess.cpp:1144`) →
`Tab::save_preset` (`Tab.cpp:6199`). The fork's silent `"<name> - Custom"` shortcut
(`Tab.cpp:6216-6220`) is real but **conditional on `app_config auto_shadow_system_presets == "true"`**
(default set at `AppConfig.cpp:434-435`, user-togglable at `Preferences.cpp:1587`). With it off and
a system preset edited, `name` stays empty and `SavePresetDialog` is constructed and shown modally
(`Tab.cpp:6223`, `:6225`) — a dialog with a **text field**, which no default can answer.

`POST /api/objects/transform`, `GET /api/plates*`, `/api/jobs`, `/api/info`,
`/api/presets/filament_color`, `/api/presets/filament_add` (`Sidebar::add_filament`,
`Plater.cpp:8218`): no modals found.

#### Background modals (no API request in flight)

*Slicing / export*
* `Plater.cpp:21757` — memory warning, blocks the slicing worker (above).
* `Plater.cpp:15780` → `GUI.cpp:249` — `ErrorDialog` on critical slicing error.
* `Plater.cpp:15689` — post-slice warnings list (`on_export_began`, `Plater.cpp:15607`).
* `Plater.cpp:10391` — `Plater::priv::show_delayed_error_message` replays a postponed error from
  `on_activate` / `update_after_undo_redo`; `Plater.cpp:25309` replays it after a popup menu.

*Auto-save / recovery*
* `MainFrame.cpp:602-615` — autosave `wxTimer` → `Plater::save_project(false)` → on failure
  `MessageDialog` wxOK at `Plater.cpp:18185`, then the timer stops itself. The only timer-raised
  modal.
* `Plater.cpp:11110` — `EVT_RESTORE_PROJECT` handler (bound `Plater.cpp:11109`, posted from
  `Plater::trigger_restore_project`, `Plater.cpp:20492`, called at `GUI_App.cpp:1101` and
  `GUI_App.cpp:3965`): "Previous unsaved project detected, do you want to restore it?"
  wxYES_NO|wxYES_DEFAULT. **The prompt is shown before `skip_confirm` is read**, and answering No
  deletes the backup directory (`Plater.cpp:11120-11123`).

*Update / version*
* `GUI_App.cpp:2818` — `UpdateVersionDialog` shown with `Raise()+Show()` — **non-modal**, so the
  hook never sees it; on a hidden instance it is a stray window.
* `GUI_App.cpp:2837` — `DownloadDialog` (force upgrade). **Every branch of the switch calls
  `mainframe->Close(true)`** (`GUI_App.cpp:2840-2852`): auto-answering kills a phone-serving instance.
* `GUI_App.cpp:2858, 2867, 2873, 2879, 2885` — `InfoDialog` (newest version / generic dialog /
  no preset update / no web-resource update / server fail).
* `PresetUpdater.cpp:2055` (`config_update`), `:2137` (`on_update_notification_confirm`),
  `:2168` (`do_printer_config_update`), `:2317` (`load_flutter_web`) — `MsgUpdateConfig`
  (`UpdateDialogs.cpp:40`), all reached from background HTTP completions.
  `PresetUpdater.cpp:2347` — restart prompt, leads to `schedule_recreate_gui_when_no_modal`
  (`GUI_App.cpp:3899`) and a modal `ProgressDialog` at `GUI_App.cpp:3925`.
  `UpdateDialogs.cpp:261`/`:326` — `MsgDataIncompatible`; `R_INCOMPAT_EXIT` closes the frame
  (`GUI_App.cpp:7353`).
* `GUI_App.cpp:1157-1161` in `post_init` (`GUI_App.cpp:1001`) starts all of the above.

*Network plugin / login / devices*
* `GUI_App.cpp:1830` — agent `set_on_server_connected_fn` (`return_code == 5`) → `MessageDialog`
  "Login information expired" via `CallAfter`.
* `GUI_App.cpp:4818` / `:4832` — `GUI_App::on_http_error` (`GUI_App.cpp:4792`): version-too-low,
  and HTTP 401 → logout + "Login information expired" (guarded by `m_show_http_errpr_msgdlg`).
* `GUI_App.cpp:5422` — `push_notification(UNS_WARNING_CONFIRM)` → `MessageDialog` from `CallAfter`.
* `GUI_App.cpp:5566` — `sync_preset` cloud-limit `MessageDialog` from the sync worker.
* `GUI_App.cpp:1778` (`updating_bambu_networking`, called from `post_init` at `GUI_App.cpp:1119`)
  and `GUI_App.cpp:4051` (`ShowDownNetPluginDlg`, from `Plater.cpp:16462`) — `DownloadProgressDialog`.
* `Plater.cpp:16473`/`:16475` — `UpdatePluginDialog` (`update_plugin_when_launch`, `Plater.cpp:16466`); `GUI_App.cpp:6262` — `InputIpAddressDialog` via
  `EVT_SHOW_IP_DIALOG`; `GUI_App.cpp:6934` — `PingCodeBindDialog`; `BindDialog.cpp:855` — a
  `MessageDialog` raised **from a destructor**.
* `DeviceManager.cpp:5039` — `SecondaryCheckDialog` raised straight out of MQTT JSON parsing
  (`DeviceManager.cpp:3341`); `StatusPanel.cpp:2191`, `:2224` — printer-error dialogs. All use
  `on_show()` = `Show()+Raise()` → **non-modal, not hooked**, but they open windows.

*First run*
* `GUI_App.cpp:1151` → `config_wizard_startup` (`GUI_App.cpp:7325`) → `run_wizard`
  (`GUI_App.cpp:7055`) → `GuideFrame::run` → `ShowModal` (`WebGuideDialog.cpp:908`). Fires whenever
  `!m_app_conf_exists || printers.only_default_printers()` (`GUI_App.cpp:7331`).
* `GUI_App.cpp:2660` — TLS cert-store `RichMessageDialog`; **not answering Yes aborts app init**.
* `GUI_App.cpp:2147` — `wxMessageBox` for the WebView2 runtime, raised **from the `GUI_App`
  constructor** (`GUI_App.cpp:1268`), before any frame exists.

*Crash / fatal*
* `GUI_App.cpp:952` and `:961` — `wxMessageBox` then `std::terminate()`
  (`generic_exception_handle`, `GUI_App.cpp:915`), reached from `OnExceptionInMainLoop`
  (`GUI_App.cpp:6667`) and from the slicing worker via `OnUnhandledException`
  (`BackgroundSlicingProcess.cpp:473`, `:484`).
* `GUI_App.cpp:965` — `wxLogError`; wxLogGui flushes it as a message box later.
* `GUI_App.cpp:6115` — "Switching language failed" `wxMessageBox`;
  `GUI_Init.cpp:107`, `:110` — GUI-init failure `wxMessageBox`.

*Print start* (phone cannot reach these yet — listed for completeness)
`Plater.cpp:16152/16164/16178/16231/16257` (`SelectMachineDialog`, `SendToPrinterDialog`,
`SendMultiMachinePage`), `SelectMachine.cpp:1721`/`:1898` (`ConfirmBeforeSendDialog`),
`SelectMachine.cpp:756` (`AmsReplaceMaterialDialog`), `Plater.cpp:22248` ("Is the printer ready?"),
`Plater.cpp:22161`/`:22238` (`PrintHostSendDialog`), the AMS mapping popup
(`AmsMappingPopup.cpp`, used at `SelectMachine.cpp:3337`) which is a non-modal `PopupWindow`,
and the calibration wizards (`CalibrationWizard.cpp:564…1503`).

---

### Dialog policy table

Modes: **R** = a phone/agent request is executing (`auto_confirm()`); **B** = hidden, background
(no request); **I** = interactive (window shown) — always let it show.

| Dialog / call site | file:line | blocks? | default while hidden | intercept point | notes |
|---|---|---|---|---|---|
| `MessageDialog::ShowModal` (generic) | `MsgDialog.cpp:363` | yes | R: Yes→OK · B: No→Cancel→OK | keep override, delegate to `RemoteAccess::modal_answer` | 375 uses across `src/slic3r` |
| `RichMessageDialog::ShowModal` | `MsgDialog.cpp:386` | yes | same | keep override (must still set the DSA checkbox) | |
| `ErrorDialog::ShowModal` | `MsgDialog.cpp:654` | yes | OK (dismiss) | keep override; also `note_attention` | |
| `show_error` | `GUI.cpp:239` | yes (via CallAfter) | not shown; text → `note_error` | extend the `auto_confirm()` test at `GUI.cpp:243` to `dialog_mode()!=I` | today only R |
| `show_info` / `warning_catcher` | `GUI.cpp:266`, `:279` | yes | OK | inherited from `MessageDialog` | |
| every other `DPIDialog` subclass | `GUI_Utils.hpp:224` | yes | per class table | **`wxModalDialogHook::Enter`** | catches `InfoDialog`, `WarningDialog`, `DownloadDialog`, `MsgUpdateConfig`, `NetworkErrorDialog`… |
| native `wxMessageDialog` / `wxMessageBox` | `GUI_App.cpp:952,961,2147,6115`, `GUI_Init.cpp:107,110` | yes | style-derived (`GetMessageDialogStyle`) | `wxModalDialogHook::Enter` | `952/961` still `std::terminate()` after |
| `wxFileDialog` / `wxDirDialog` | `Plater.cpp:12907,14439,14605,20815,20913`, `GUI_App.cpp:4221` | yes | **`wxID_CANCEL` + raise attention** | hook, class-name rule | cannot be defaulted: would read/overwrite an arbitrary path |
| `wxTextEntryDialog` / `wxColourDialog` | 5 / 4 sites | yes | `wxID_CANCEL` + attention | hook, class-name rule | needs a value |
| `UnsavedChangesDialog::ShowModal` | `UnsavedChangesDialog.cpp:823` (`inline int UnsavedChangesDialog::ShowModal()`) | yes | Transfer if offered, else Discard | keep override (sets `m_exit_action`) | add logging; unchanged semantics |
| `SavePresetDialog` | `Tab.cpp:6223`/`:6225` | yes | never construct it | fix `api_process_save` (`RemoteAccess.cpp:1144`) to pass an explicit name; hook returns Cancel as backstop | `auto_shadow_system_presets` can be off (`Preferences.cpp:1587`) |
| `Tab::delete_preset` confirm | `Tab.cpp:6436` | yes | **`wxID_NO`** | call-site guard | generic rule would answer Yes and delete a preset |
| `DeleteConfirmDialog` (printer + its presets) | `Tab.cpp:6360` | yes | `wxID_CANCEL` + attention | hook, class-name rule | destructive |
| `Plater::reset_with_confirm` | `Plater.cpp:20441` | yes | **`wxID_CANCEL`** | call-site guard | "All objects will be removed" |
| `close_with_confirm` "save before continue?" | `Plater.cpp:20458` | yes | R: Yes (save) · B: Yes | leave to `MessageDialog` override | `api_project_open` depends on Yes; honours `save_project_choise` |
| `guard_before_slice_plate` temp-mixing | `Plater.cpp:23434`, `:23473` | yes | OK (continue) | `MessageDialog` override — needs the scope extension (Change 6) | |
| mid-slice memory warning | `Plater.cpp:21757` | yes, **and blocks the slicing worker** | **`wxID_NO` (stop) + attention** | call-site guard at `Plater.cpp:21743` before constructing the dialog | Yes risks OOM-killing the process and every later request |
| post-slice warnings list | `Plater.cpp:15689` | yes | OK | `MessageDialog` override | |
| critical slicing error | `Plater.cpp:15780` | yes | not shown; → `note_error` + attention | `GUI.cpp:243` extension | |
| autosave failure | `Plater.cpp:18185` (timer `MainFrame.cpp:611`) | yes | OK + attention | `MessageDialog` override + `note_attention` | timer stops itself afterwards |
| restore-project prompt | `Plater.cpp:11110` | yes | **skip the handler entirely** + attention | call-site guard at `Plater.cpp:11109` | No deletes the backup dir (`Plater.cpp:11122`) |
| `ProgressDialog` "Loading…" / "Importing Model" | `Plater.cpp:11549`, `:18262` | no (pumps) | do not create | call-site guard | stray window on a hidden desktop |
| force-upgrade `DownloadDialog` | `GUI_App.cpp:2837` | yes | **skip the handler** + attention | call-site guard at `GUI_App.cpp:2825` | every answer closes the main frame |
| `UpdateVersionDialog` | `GUI_App.cpp:2818` | no (`Show()`) | do not show | call-site guard | not hookable |
| `InfoDialog` × 5 (version / config / server) | `GUI_App.cpp:2858,2867,2873,2879,2885` | yes | OK | hook | |
| `MsgUpdateConfig` | `PresetUpdater.cpp:2055,2137,2168,2317` | yes | **`wxID_CANCEL`** | hook, class-name rule | Yes rewrites the preset library under the phone |
| web-resource restart prompt | `PresetUpdater.cpp:2347` | yes | `wxID_CANCEL` | `MessageDialog` override in B mode (No/Cancel first) | |
| `MsgDataIncompatible` | `UpdateDialogs.cpp:261`, `:326` | yes | `wxID_CANCEL` + attention | hook, class-name rule | `R_INCOMPAT_EXIT` closes the frame |
| `GuideFrame` (first-run wizard) | `WebGuideDialog.cpp:908` | yes | **skip + attention + show window** | call-site guard at `GUI_App.cpp:1151`; hook as backstop | a printer-less instance cannot serve anyway |
| TLS cert-store prompt | `GUI_App.cpp:2660` | yes | **`wxID_YES`** (No aborts init) | explicit class rule or call-site guard | one of the few where Yes is mandatory |
| WebView2 runtime `wxMessageBox` | `GUI_App.cpp:2147` | yes | `wxID_NO` + attention | hook (fires before the frame exists) | |
| `DownloadProgressDialog` (net plugin) | `GUI_App.cpp:1778`, `:4051` | yes | skip + attention | call-site guards | |
| login-expired / http-error boxes | `GUI_App.cpp:1830`, `:4818`, `:4832` | yes | OK (dismiss) + attention | `MessageDialog` override | |
| `push_notification(UNS_WARNING_CONFIRM)` | `GUI_App.cpp:5422` | yes | OK | `MessageDialog` override | |
| preset-sync cloud limit | `GUI_App.cpp:5566` | yes | OK | `MessageDialog` override | one-shot |
| `InputIpAddressDialog`, `PingCodeBindDialog`, `BindMachineDialog`, `UnBindMachineDialog` | `GUI_App.cpp:6262`, `:6934`, `SelectMachinePop.cpp:559,708,736` | yes | `wxID_CANCEL` + attention | hook, class-name rule | need typed input |
| `SecondaryCheckDialog` / `PrintErrorDialog` | `DeviceManager.cpp:5039`, `StatusPanel.cpp:2191`, `:2224` | no (`Show()`) | do not show | guard in `SecondaryCheckDialog::on_show` (`ReleaseNote.cpp:776`) | raised from MQTT parsing |
| `SelectMachineDialog` / `SendToPrinterDialog` / `ConfirmBeforeSendDialog` / AMS | `Plater.cpp:16152,16178,16231`, `SelectMachine.cpp:1721,756` | yes | `wxID_CANCEL` + attention | hook, class-name rule | phone cannot reach these yet |
| calibration wizards | `CalibrationWizard.cpp:564…1503` | yes | `wxID_CANCEL` + attention | hook | |
| fatal `wxMessageBox` + `std::terminate` | `GUI_App.cpp:952`, `:961` | yes | dismiss, then it terminates anyway | hook + `raise_attention` before answering | attention is written to `<datadir>/hub/instances/<pid>.json` so the hub can report the crash |

---

### Changes

#### 1. `src/slic3r/GUI/RemoteAccess.hpp` — the policy and attention API

```cpp
    // ---- Phase 1 (window state) ----
    static bool hidden();                                 // launched hidden and not shown
    static void show_window(const std::string& reason);   // GUI thread; clears hidden

    // ---- Phase 3 (dialog policy + attention) ----
    // Interactive: a person is at the PC, dialogs show normally.
    // Request:     a phone/agent request is running -> take the affirmative branch.
    // Background:  hidden, nothing asked for this -> take the do-nothing branch.
    enum class Mode { Interactive, Request, Background };
    static Mode dialog_mode();

    // Installed once, before the GUI_App is constructed (GUI_Init.cpp).
    static void install_dialog_policy();

    struct Attention { long long time; std::string dialog; std::string answered; };

    void note_attention(const std::string& dialog, const std::string& answered); // any thread
    void raise_attention(const std::string& reason);                            // any thread
    void clear_attention(const char* why);                                      // any thread
    bool needs_attention(std::string* reason = nullptr);
    void note_gui_tick(int modal_depth);   // GUI thread heartbeat
    long long gui_stall_ms();              // any thread
private:
    std::deque<Attention> m_attention;     // ring, 50 entries
    bool        m_needs_attention { false };
    std::string m_attention_reason;
    long long   m_attention_since { 0 };
    long long   m_gui_tick_ms { 0 };
    int         m_modal_depth { 0 };
    int         m_requests_done { 0 };     // completions since the flag was raised
```

#### 2. `src/slic3r/GUI/RemoteAccess.cpp` — mode, hook, defaults

Replace the bare depth counter at `RemoteAccess.cpp:109-112` with:

```cpp
static int s_auto_confirm_depth = 0; // GUI thread only
bool RemoteAccess::auto_confirm() { return s_auto_confirm_depth > 0; }
RemoteAccess::AutoConfirmScope::AutoConfirmScope() { ++s_auto_confirm_depth; }
RemoteAccess::AutoConfirmScope::~AutoConfirmScope() { --s_auto_confirm_depth; }

RemoteAccess::Mode RemoteAccess::dialog_mode()
{
    if (s_auto_confirm_depth > 0) return Mode::Request;
    if (hidden())                 return Mode::Background;
    return Mode::Interactive;
}
```

Policy helpers and the hook (new, after `json_error`):

```cpp
##include <wx/modalhook.h>
##include <wx/msgdlg.h>

namespace {

// Dialogs whose affirmative answer destroys work, needs typed input, or takes the instance
// down. Matched on the wx class name so no translated text is involved.
struct ClassRule { const char* cls; int answer; bool human; };
static const ClassRule k_class_rules[] = {
    // needs typed input / a chosen path -> cancel and ask for a human
    { "wxFileDialog",            wxID_CANCEL, true }, { "wxDirDialog",           wxID_CANCEL, true },
    { "wxTextEntryDialog",       wxID_CANCEL, true }, { "wxColourDialog",        wxID_CANCEL, true },
    { "wxFontDialog",            wxID_CANCEL, true }, { "SavePresetDialog",      wxID_CANCEL, true },
    { "GuideFrame",              wxID_CANCEL, true }, { "ConfigWizard",          wxID_CANCEL, true },
    { "InputIpAddressDialog",    wxID_CANCEL, true }, { "PingCodeBindDialog",    wxID_CANCEL, true },
    { "BindMachineDialog",       wxID_CANCEL, true }, { "UnBindMachineDialog",   wxID_CANCEL, true },
    { "ConnectPrinterDialog",    wxID_CANCEL, true }, { "EditDevNameDialog",     wxID_CANCEL, true },
    // destructive or takes the app down -> decline, quietly
    { "DeleteConfirmDialog",     wxID_CANCEL, false }, { "DownloadDialog",       wxID_CANCEL, false },
    { "DownloadProgressDialog",  wxID_CANCEL, false }, { "UpdateVersionDialog",  wxID_CANCEL, false },
    { "UpdatePluginDialog",      wxID_CANCEL, false }, { "MsgUpdateConfig",      wxID_CANCEL, false },
    { "MsgUpdateSlic3r",         wxID_CANCEL, false }, { "MsgDataIncompatible",  wxID_CANCEL, true  },
    // print / calibration: never start a print unattended
    { "SelectMachineDialog",     wxID_CANCEL, false }, { "SendToPrinterDialog",  wxID_CANCEL, false },
    { "SendMultiMachinePage",    wxID_CANCEL, false }, { "ConfirmBeforeSendDialog", wxID_CANCEL, false },
    { "PrintHostSendDialog",     wxID_CANCEL, false }, { "CaliHistoryDialog",    wxID_CANCEL, false },
};

static bool has_btn(wxDialog* d, int id) { return d->FindWindow(id) != nullptr; }

static long style_of(wxDialog* d)
{
    if (auto* m = dynamic_cast<wxMessageDialogBase*>(d)) return m->GetMessageDialogStyle();
    long s = 0; // MsgDialog builds real child buttons with these ids (MsgDialog::apply_style)
    if (has_btn(d, wxID_OK))     s |= wxOK;
    if (has_btn(d, wxID_YES))    s |= wxYES;
    if (has_btn(d, wxID_NO))     s |= wxNO;
    if (has_btn(d, wxID_CANCEL)) s |= wxCANCEL;
    return s;
}

// affirmative: the phone asked for this, carry it out. Otherwise: do nothing.
static int default_answer(long s, bool affirmative)
{
    if (affirmative) {
        if (s & wxYES) return wxID_YES;
        if (s & wxOK)  return wxID_OK;
    } else {
        if (s & wxNO)     return wxID_NO;
        if (s & wxCANCEL) return wxID_CANCEL;
        if (s & wxOK)     return wxID_OK;   // a pure acknowledgement
    }
    if (s & wxCANCEL) return wxID_CANCEL;
    if (s & wxNO)     return wxID_NO;
    if (s & wxOK)     return wxID_OK;
    return wxID_CANCEL;
}

static const char* answer_name(int id)
{
    switch (id) { case wxID_YES: return "yes"; case wxID_NO: return "no";
                  case wxID_OK: return "ok";   default: return "cancel"; }
}

int g_modal_depth = 0; // GUI thread only: modals we let through

class DialogPolicyHook : public wxModalDialogHook
{
protected:
    int Enter(wxDialog* dlg) override
    {
        const std::string cls   = dlg->GetClassInfo()->GetClassName().ToStdString();
        const std::string title = dlg->GetTitle().ToUTF8().data();
        if (RemoteAccess::dialog_mode() == RemoteAccess::Mode::Interactive) {
            ++g_modal_depth;            // somebody is looking; let it show
            return wxID_NONE;
        }
        const ClassRule* rule = nullptr;
        for (const ClassRule& r : k_class_rules)
            if (cls == r.cls) { rule = &r; break; }
        const bool affirmative = RemoteAccess::dialog_mode() == RemoteAccess::Mode::Request;
        const int  answer      = rule ? rule->answer : default_answer(style_of(dlg), affirmative);

        RemoteAccess::get().note_attention(cls + (title.empty() ? "" : " \"" + title + "\""),
                                           answer_name(answer));
        BOOST_LOG_TRIVIAL(warning) << "hidden-mode dialog answered " << answer_name(answer)
                                   << ": " << cls << " \"" << title << "\"";
        if (rule && rule->human)
            RemoteAccess::get().raise_attention(cls + " needs someone at the PC");
        return answer;              // ShowModal() returns this without showing anything
    }
    void Exit(wxDialog*) override { if (g_modal_depth > 0) --g_modal_depth; }
};
static DialogPolicyHook s_dialog_hook;

} // namespace

void RemoteAccess::install_dialog_policy() { s_dialog_hook.Register(); }
```

Attention bookkeeping (mutex-protected, callable from the HTTP threads):

```cpp
static long long now_ms()
{ return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count(); }

void RemoteAccess::note_attention(const std::string& dialog, const std::string& answered)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_attention.push_back({ (long long) std::time(nullptr), dialog, answered });
    if (m_attention.size() > 50) m_attention.pop_front();
}

void RemoteAccess::raise_attention(const std::string& reason)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_needs_attention && m_attention_reason == reason) return;
        m_needs_attention  = true;
        m_attention_reason = reason;
        m_attention_since  = (long long) std::time(nullptr);
        m_requests_done    = 0;
    }
    BOOST_LOG_TRIVIAL(warning) << "RemoteAccess: needs attention: " << reason;
    // Put the window in front of the PC user so the problem can be resolved.
    if (hidden())
        wxGetApp().CallAfter([reason]() { RemoteAccess::show_window(reason); });
}

void RemoteAccess::clear_attention(const char* why)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_needs_attention) return;
    m_needs_attention = false;
    m_attention_reason.clear();
    BOOST_LOG_TRIVIAL(info) << "RemoteAccess: attention cleared (" << why << ")";
}
```

#### 3. `src/slic3r/GUI/RemoteAccess.cpp` — the watchdog

Two independent signals, because a modal does **not** stop the event loop:

* **modal depth** (`g_modal_depth > 0`) — a dialog got through the policy (a `Show()`-only dialog,
  or one the hook could not answer). The heartbeat still runs, so this is detectable.
* **GUI stall** (`now - m_gui_tick_ms`) — the loop is not pumping at all (a synchronous
  `perform_sync`, a long load, `stop_queue`). `CallAfter` will not run either, so nothing can be
  shown; only reporting is possible.

```cpp
class GuiHeartbeat : public wxTimer
{
public:
    void Notify() override
    {
        RemoteAccess& ra = RemoteAccess::get();
        ra.note_gui_tick(g_modal_depth);
        if (g_modal_depth > 0 && RemoteAccess::hidden())
            ra.raise_attention("a dialog on the PC is waiting for an answer");
        else if (g_modal_depth == 0 && ra.gui_stall_ms() < 3000)
            ra.maybe_clear_attention();   // clears once a request has completed since
    }
};
static GuiHeartbeat s_heartbeat;
```

Start it from `RemoteAccess::start()` (`RemoteAccess.cpp:141`, GUI thread) with
`s_heartbeat.Start(1000)`; stop it in `RemoteAccess::stop()` (`RemoteAccess.cpp:163`).

`note_gui_tick` stores `now_ms()` and the depth under the mutex; `gui_stall_ms()` returns
`now_ms() - m_gui_tick_ms`. `maybe_clear_attention()` clears only when `m_requests_done > 0`, so a
long slice that produced a single 503 does not flap the banner.

Make `run_on_main` report timeouts (`RemoteAccess.cpp:114`):

```cpp
static bool run_on_main(std::function<void()> fn, int timeout_ms = 15000, const char* what = "a request")
{
    auto done = std::make_shared<std::promise<void>>();
    auto fut  = done->get_future();
    wxGetApp().CallAfter([done, fn]() {
        RemoteAccess::AutoConfirmScope auto_yes;
        try { fn(); } catch (...) {}
        done->set_value();
    });
    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready) {
        RemoteAccess::get().note_request_done();
        return true;
    }
    RemoteAccess::get().raise_attention(std::string(what) + " did not finish on the PC within "
                                        + std::to_string(timeout_ms / 1000) + "s");
    return false;
}
```

Pass a `what` at the interesting call sites (`api_plate_thumbnail` "a thumbnail render",
`api_project_open` "opening a project", `api_process_save` "saving the preset"…).

#### 4. `src/slic3r/GUI/RemoteAccess.cpp:1156` — `/api/info`

```cpp
    j["hidden"]           = hidden();
    j["needs_attention"]  = m_needs_attention;
    j["attention_reason"] = m_attention_reason;
    j["attention_since"]  = m_attention_since;
    j["modal_open"]       = m_modal_depth;
    j["gui_stall_ms"]     = now_ms() - m_gui_tick_ms;
    j["attention"]        = nlohmann::json::array();
    for (const Attention& a : m_attention)
        j["attention"].push_back({ {"time", a.time}, {"dialog", a.dialog}, {"answered", a.answered} });
```

Add one route in `handle_api` (`RemoteAccess.cpp:1239`), next to `/info`:

```cpp
    if (path == "/attention/clear" && method == "POST") {
        clear_attention("dismissed from the phone");
        return api_info();
    }
```

…and a matching manifest line in the `routes` array (`RemoteAccess.cpp:1248`).

#### 5. `src/slic3r/GUI/GUI_Init.cpp:32` — install the policy first

Put `RemoteAccess::install_dialog_policy();` at the top of `GUI_Run`, **before**
`new GUI::GUI_App()` — the WebView2 `wxMessageBox` (`GUI_App.cpp:2147`) fires from the constructor
(`GUI_App.cpp:1268`), and `on_init_inner`'s TLS prompt (`GUI_App.cpp:2660`) fires long before
`RemoteAccess::start()` runs (`StreamPanel.cpp:44`).

#### 6. Extend the auto-confirm window across the posted slice

`api_slice` (`RemoteAccess.cpp:693`) posts an event and returns, so `on_action_slice_plate`
(`Plater.cpp:16025`) runs unguarded. Two options; take both:

* In `api_slice`, replace `wxPostEvent(...)` with a direct call of the same handler through
  `plater->GetEventHandler()->ProcessEvent(SimpleEvent(...))` **inside** the `run_on_main` lambda,
  so `guard_before_slice_plate` runs under `AutoConfirmScope`. (`reslice()` only *starts* the
  background process, so this does not lengthen the request materially — but raise that route's
  timeout to 60 s.)
* Regardless, hidden mode covers the asynchronous remainder (`on_process_completed`, the memory
  guard) through `Mode::Background`.

#### 7. Call-site guards (where the generic default is wrong or the dialog is not hookable)

| # | file:function | change |
|---|---|---|
| 7a | `Plater.cpp:21743` `Plater::reslice` memory-guard lambda | before constructing the `RichMessageDialog`: `if (RemoteAccess::dialog_mode() != RemoteAccess::Mode::Interactive) { RemoteAccess::get().note_attention("Memory Usage Warning", "no"); RemoteAccess::get().raise_attention("slicing stopped: the PC ran out of memory"); promise->set_value(false); return; }` — Yes would risk OOM-killing the process. |
| 7b | `Plater.cpp:11109` `EVT_RESTORE_PROJECT` handler | first line: `if (RemoteAccess::hidden()) { RemoteAccess::get().raise_attention("an unsaved project from a previous session is waiting"); return; }` — do **not** prompt and do **not** delete the backup (`Plater.cpp:11122`). |
| 7c | `Tab.cpp:6436` `Tab::delete_preset` | `if (RemoteAccess::dialog_mode() != Mode::Interactive) return;` before the confirm. |
| 7d | `Plater.cpp:20441` `Plater::reset_with_confirm` | same guard: never wipe the plate unattended. |
| 7e | `GUI_App.cpp:2825` `EVT_ENTER_FORCE_UPGRADE` | `if (RemoteAccess::hidden()) { raise_attention("a mandatory update is waiting"); return; }` — every answer calls `mainframe->Close(true)`. |
| 7f | `GUI_App.cpp:2818` `EVT_SLIC3R_VERSION_ONLINE` | skip `Raise()/Show()` while hidden (non-modal, so the hook never sees it). |
| 7g | `GUI_App.cpp:1119` `post_init` / `GUI_App.cpp:4043` `ShowDownNetPluginDlg` | skip while hidden + `raise_attention("the network plug-in needs installing")`. |
| 7h | `GUI_App.cpp:1151` `config_wizard_startup` | skip while hidden + `raise_attention("this instance has no printer configured")`; a printer-less instance cannot serve the phone. |
| 7i | `GUI_App.cpp:2660` TLS cert-store prompt | while hidden answer **Yes** explicitly (the generic Background rule would pick No and abort init). |
| 7j | `RemoteAccess.cpp:1144` `api_process_save` | `tab->save_preset(edited.is_system ? edited.name + " - Custom" : edited.name);` so `SavePresetDialog` is never constructed even with `auto_shadow_system_presets` off. |
| 7k | `Plater.cpp:11549` and `Plater.cpp:18262` `ProgressDialog` | while hidden, construct with `wxPD_AUTO_HIDE` only and never `Show()` — or skip the object entirely and keep the `dlg_cont = true` path. |
| 7l | `ReleaseNote.cpp:776` `SecondaryCheckDialog::on_show` (and `StatusPanel.cpp:2191`, `:2224`) | `if (RemoteAccess::hidden()) { log + note_attention; return; }` — `Show()`-only, not hookable. |
| 7m | `GUI.cpp:243` `show_error` | widen the test from `RemoteAccess::auto_confirm()` to `RemoteAccess::dialog_mode() != Mode::Interactive`, and add `note_attention("show_error", "not shown")`. |

#### 8. `src/slic3r/GUI/RemoteHub.cpp` — carry attention to the phone

* `struct Instance` (`RemoteHub.cpp:624`): add `bool hidden{false}; bool needs_attention{false};
  std::string attention_reason;`
* `HubServer::probe_instance` (`RemoteHub.cpp:891`): read them from `/api/info`
  (`inst.needs_attention = j.value("needs_attention", false);` etc.).
* `HubServer::instances_json` (`RemoteHub.cpp:942`): emit `ji["hidden"]`,
  `ji["needs_attention"]`, `ji["attention_reason"]`.
* `HubTray::refresh` (`RemoteHub.cpp:1284`) and `CreatePopupMenu` (`RemoteHub.cpp:1293`): append
  `" — 1 needs attention"` to the tooltip / status item so the PC user sees it without the phone.

#### 9. `resources/web/orca/stream_center.html` — the banner

In `refreshInstances` (`stream_center.html:1033`) the instance list already arrives every poll; in
`renderInstBar` (`stream_center.html:1048`), after the `<select>`:

```js
    var att = instances.filter(function(i) { return i.needs_attention; });
    if (att.length) {
        var b = el('div', 'note err',
            'The slicer needs attention on the PC' +
            (att[0].attention_reason ? ': ' + att[0].attention_reason : '') +
            (att.length > 1 ? ' (+' + (att.length - 1) + ' more)' : ''));
        var dismiss = el('span', 'btn dim', 'Dismiss');
        dismiss.onclick = function() {
            fetch(REMOTE + 'i/' + att[0].id + '/api/attention/clear', { method: 'POST' })
                .then(function() { refreshInstances(); });
        };
        b.appendChild(dismiss);
        bar.appendChild(b);
    }
```

Also mark the option text in the `instances.forEach` loop (`stream_center.html:1056`) with
`(i.needs_attention ? ' (needs attention)' : '')` so a non-selected instance is visible too.

#### 10. How attention clears

* the heartbeat clears it once `g_modal_depth == 0`, `gui_stall_ms() < 3000` **and** at least one
  request has completed since it was raised (`note_request_done`, Change 3);
* `POST /api/attention/clear` from the phone banner;
* Phase 1's `/api/window hide` re-enters hidden mode and starts clean;
* the ring buffer in `attention[]` is **not** cleared — it is the log of what was auto-answered.

---

### Test plan for this phase

Helpers in the scratchpad: `run_hub_app.py <file>` (starts the build-tree slicer detached with
`SNORCA_PHONE_ACCESS=testtoken12345`; `--new` sets `SNORCA_NEW_INSTANCE=1`), `wait_for_hub.py [n]`,
`test_hub.py {list|import|load|new|reject}`, `test_presets.py`, `test_settings.py`,
`hubtest/h2d_copy.3mf`. Phase 3 needs one addition: **`test_attention.py`** — polls
`/i/<pid>/api/info` and prints `hidden / needs_attention / attention_reason / modal_open /
gui_stall_ms` and the `attention[]` tail.

Baseline for every case: start hidden (`run_hub_app.py --hidden hubtest/h2d_copy.3mf`),
`wait_for_hub.py 1`, confirm `GET /api/info` answers in < 1 s and `hidden:true`,
`needs_attention:false`.

**A. No hang, logged answer, correct default** — after each induced dialog assert: the route
returns non-503 within its timeout, `attention[]` gained an entry with the expected `answered`,
`needs_attention` matches the table, and `snapmaker-orca.log` has the
`hidden-mode dialog answered …` line.

| Case | How to induce | Expect |
|---|---|---|
| load-project unsaved-changes | move an object via `POST /api/objects/transform`, then `test_hub.py load` | `MessageDialog` answered `yes` (saved), `UnsavedChangesDialog` `transfer`/`discard`; new project loaded |
| object-too-large | import an STL scaled ×50 (`test_hub.py import`) | `MessageDialog "Object too large"` answered `yes`, object scaled |
| multi-part import | import a multi-object STL | `"Multi-part object detected"` answered `yes` |
| no-geometry file | import an empty/garbage `.stl` | `"The file does not contain any geometry data."` answered `ok`, route returns 500 "nothing was imported" |
| temp-mixing guard | set two filament slots to incompatible temps, `POST /api/slice?plate=0` | `"Confirm slicing"` answered `ok`; job reaches `done` |
| slice error | corrupt a plate so `Print::validate` fails, then slice | `show_error` not shown, `/api/jobs/<id>` `state:error` with the text, `attention[]` entry `show_error / not shown` |
| memory guard (7a) | build with a forced low-memory threshold, or call the guard callback from a test hook | slice ends `cancelled`, `needs_attention:true` reason "slicing stopped: the PC ran out of memory", window appears |
| save a system preset with `auto_shadow_system_presets=false` | set the pref false in `Snapmaker_Orca.conf`, `test_settings.py` then `POST /api/settings/process/save` | 200, preset `"<name> - Custom"` created, **no** `SavePresetDialog` in `attention[]` |
| preset select with dirty process | `test_presets.py` after a settings change | 200, no `UnsavedChangesDialog` entry (force path), modifications re-applied |
| restore-project prompt (7b) | kill the process mid-edit, restart hidden | no prompt, `needs_attention:true` "an unsaved project from a previous session is waiting", backup dir still present |
| autosave failure | open a project on a path made read-only, wait one autosave interval | `"Failed to save the project"` answered `ok`, `attention[]` entry, timer stopped |
| config update | point `PresetUpdater` at a fixture with a pending package | `MsgUpdateConfig` answered `cancel`, presets untouched |
| first-run wizard (7h) | start hidden with an empty `--datadir` | no wizard, `needs_attention:true` "this instance has no printer configured", window shown |
| file chooser | force `save_project(true)` (clear the project filename, then `test_hub.py load`) | `wxFileDialog` answered `cancel`, `needs_attention:true`, window shown, route returns 500 not 503 |

**B. Genuinely blocking case → window + banner**
1. Add a debug-only route or a keyboard hook that calls `wxSleep(30)` on the GUI thread.
   Then `GET /api/plates` → 503 within 15 s; `/api/info` (served off the GUI thread) still answers
   and shows `needs_attention:true`, reason "a request did not finish on the PC within 15s",
   `gui_stall_ms > 15000`. Phone page shows the red banner within one poll.
2. Add a debug-only route that opens a raw `wxDialog::ShowModal()` the hook is told to let through.
   Expect: `modal_open:1`, the main window appears (`hidden` flips to false), banner shows
   "a dialog on the PC is waiting for an answer". Dismiss the dialog on the PC → within ~2 s
   `needs_attention:false` and the banner disappears; `attention[]` keeps the record.
3. With the window shown by the watchdog, open a normal dialog on the PC (e.g. Preferences) and
   confirm it is **not** auto-answered (`dialog_mode() == Interactive`).

**C. Regression** — with the window shown from the start (no `--hidden`), run the whole existing
suite (`test_hub.py list|import|load|new|reject`, `test_presets.py`, `test_settings.py`,
`test_preview.py`) and confirm behaviour is byte-identical to today: `Mode::Request` reproduces the
current affirmative answers.

**D. Coverage check for the hook** — one-off: build with a temporary `Enter()` that only logs, then
trigger a `wxMessageBox`, a `wxFileDialog`, a `MessageDialog`, an `InfoDialog` and a `GuideFrame`
and confirm all five reach the hook. This validates that the vendored
`Orca-deps-wxWidgets` carries `WX_HOOK_MODAL_DIALOG()` in every `ShowModal()` implementation.

---

### Risks / open questions

1. **Hook coverage is a build-time assumption.** `WX_HOOK_MODAL_DIALOG()` is declared
   (`wx/modalhook.h:98`) but the sources are fetched by `deps/wxWidgets/wxWidgets.cmake:26` from
   `SoftFever/Orca-deps-wxWidgets`; verify with test D before relying on it. If a dialog type is
   missing the macro, fall back to that class's own `ShowModal` override.
2. **`Show()`-only windows are invisible to the hook**: `UpdateVersionDialog` (`GUI_App.cpp:2818`),
   `SecondaryCheckDialog` (`ReleaseNote.cpp:776`), `PrintErrorDialog` (`StatusPanel.cpp:2191`),
   `ProgressDialog` (`Plater.cpp:11549`, `:18262`). They do not block but they do open windows on a
   hidden desktop; only the call-site guards (7f, 7k, 7l) handle them. There is no generic trap.
3. **Class-name matching.** `wxClassInfo` names come from `wxIMPLEMENT_DYNAMIC_CLASS`; a class that
   uses only `wxDECLARE_NO_COPY_CLASS` reports its base's name. Check each entry in `k_class_rules`
   at implementation time and prefer `dynamic_cast` for classes we own. Never match translated text.
4. **A truly wedged GUI thread cannot be rescued.** When the loop is not pumping, `CallAfter` never
   runs, so `show_window` cannot execute. `/api/info` still reports `gui_stall_ms`, so the phone can
   say "not responding" — but the PC user has to act. Consider a Win32 `PostMessage(hwnd, WM_…)`
   escape hatch, or leave it to the hub's tray ("this window is not responding").
5. **Modal over a hidden parent.** On MSW a modal whose parent is hidden still appears as a
   top-level window with no owner behind it. When the watchdog shows the main frame while a modal is
   up, the frame may land *behind* the modal; call `Raise()` on the top of `dialogStack`
   (`GUI_Utils.hpp:100`) after showing the frame.
6. **`Mode::Request` inside `Mode::Background`.** A phone request executing on a hidden instance
   reports `Request`, i.e. the affirmative policy — correct, but it means a *background* dialog that
   happens to fire inside a `run_on_main` lambda also gets the affirmative answer. Acceptable; noted
   because it makes the two policies non-orthogonal.
7. **Change 6 (direct dispatch instead of `wxPostEvent`)** changes the slice route's timing and
   could re-enter the plater. If it proves risky, keep `wxPostEvent` and rely on hidden mode alone —
   but then a *shown* instance driven from the phone still blocks on the temp-mixing prompt.
8. **`RemoteAccess::start()` runs from `StreamPanel`'s constructor** (`StreamPanel.cpp:44`), i.e.
   only once that tab is built. Phase 1 must guarantee a hidden instance still constructs it, or the
   loopback API (and the heartbeat) never start. The dialog hook is installed earlier (Change 5) and
   is independent of this.
9. **`std::terminate()` after the fatal boxes** (`GUI_App.cpp:954`, `:962`): the attention entry must
   be flushed to disk before the box, otherwise the hub only sees the pid disappear. Write a
   `crash` marker into `<datadir>/hub/instances/<pid>.json` from `raise_attention` when the reason
   starts with "fatal".
10. **Open question — `auto_shadow_system_presets`.** Change 7j hard-codes `" - Custom"` in
    `api_process_save`. Should the API instead force the pref on for hidden instances, or expose the
    target name in the request body?
11. **Open question — should `Mode::Background` also apply when the window is shown but the user is
    away?** Current design says no: visible window ⇒ `Interactive`. That keeps the PC user's own
    clicks untouched but means a shown-by-watchdog instance stops auto-answering, which is the point.

## Phase 4 — Defaults and polish

### Changes
1. **Preferences** (`src/slic3r/GUI/Preferences.cpp`): a checkbox "Start hidden when launched for the phone" bound to app_config `start_hidden` (read in Phase 1 change 5a; default false). The hub always passes `SNORCA_HIDDEN` explicitly, so this only affects manual launches.
2. **Hub page** (`resources/web/orca/hub.html`): explain the two buttons ("Open a new slicer window" vs "Open a hidden slicer"), show the attention state per instance (Phase 3 change 8), and offer "Show" next to any instance flagged `needs_attention`.
3. **Phone page** (`resources/web/orca/stream_center.html`): a "Show on PC" chip in the instance bar (`POST /r/<t>/i/<pid>/api/window?show=1` goes through the hub's `/i/<pid>/api` splice; decide whether the phone may show a window — default yes, it is the user's own PC).
4. **Docs**: a short section in `docs/` (next to the phone-stream-access design) describing hidden mode, the tray menu, `/api/window`, `/api/quit`, `/api/attention/clear`, the dialog policy and the attention banner; update the `/api` manifest strings.
5. **Memory notes** for the working sessions: the contract in §3, the MAX_PATH rule, and the test recipe (`run_hub_app.py --hidden`, `wait_for_hub.py`, `test_hidden.py`, `test_attention.py`).

### Test plan
- Fresh `--datadir`: first launch is visible and runs the wizard; once configured, a phone-initiated open is hidden; a PC-launched slicer is visible; Preferences toggle flips the manual-launch default.
- All Phase 1–3 scripts pass on the final build; the shown-window regression suite is unchanged.

## Appendix — test helpers

Scratchpad helpers referenced by the phase test plans (copy them to a short path before running anything through cmd.exe): `run_hub_app.py <file> [--new] [--hidden]` (add `SNORCA_HIDDEN=1` when `--hidden`), `wait_for_hub.py [N]`, `test_layout.py`, `test_presets.py`, `test_settings.py`, `test_hub.py {list|import|load|new|reject}`, `test_preview.py [plate]`, `test_preview2.py [plate]`; to be written: `test_hidden.py` (Phase 1 gate), `test_attention.py` (Phase 3). Hub: `http://127.0.0.1:13640/hub/info`; phone page: `http://10.0.0.131:13640/r/<token>/`; instance API through the hub: `http://10.0.0.131:13640/r/<token>/i/<pid>/api/...`.
