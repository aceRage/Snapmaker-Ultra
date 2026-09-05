# Rebranding Snapmaker Orca Ultra (2026-09-04)

**The trigger.** [`2026-09-04-ultra1-app-plan.md` §R7](2026-09-04-ultra1-app-plan.md) says, of the
planned companion app: *"The app cannot be called 'Snapmaker' anything. Pick the neutral name in
phase 0 and use it everywhere from the first commit."* The user's question follows from it: if the
app needs a neutral name, **what does the slicer itself need?**

**What this document is.** The trademark and licence facts with sources; a complete inventory of
where the current brand lives in this repo with paths and counts; three options with a
recommendation; a phased migration plan with effort; and the risks. **Documentation only - no code
is changed on this branch.**

Branch `docs/rebrand-plan`, cut from `feat/ultra-preferences` at `ef81b7c194`. Codebase facts carry
`file:line` anchors against that commit. External facts carry a URL and the date they were read;
anything that could not be confirmed from a primary source is marked **UNVERIFIED** and must not be
planned against.

**This is not legal advice.** Nothing below is a legal opinion, and none of the name candidates has
had a clearance search. Section 3.4 says what a lawyer would actually have to do.

---

## The short version

1. **The licence is not the problem.** AGPL-3.0 lets us fork, modify and redistribute, and Snapmaker
   says so themselves. **The name is a separate question the licence does not answer** - AGPL-3.0
   §7(e) expressly carves trademarks out of the grant (§1.1).
2. **Snapmaker's own Terms of Use draw exactly that line** and are the single most load-bearing
   external fact here: modify the code under the open-source licence, but *"You may not use our name
   or logo for commercial promotion without authorization"* (§1.2). They hold a registered US word
   mark that covers software design and development.
3. **The app icon is Snapmaker's logo.** `resources/images/Snapmaker_Orca_192px.png` and its
   twenty-two siblings are Snapmaker's "S" glyph on a black rounded square. That is their mark, used
   as our icon, and it is precisely what App Store 4.1(c) names (§1.4, §2.7).
4. **Rebranding the slicer is not optional if the app ships**, and the cleanest reading is that it is
   not really optional either way (§3).
5. **The rename is 282 files, of which 141 are currently byte-identical to Snapmaker upstream** - so
   it doubles the fork's permanent conflict surface in exactly the files most likely to be touched
   upstream (§4.3, §5.3).
6. **"Ultra" and "Ultra1" are both bad names** and should be dropped rather than promoted: "Ultra1"
   abbreviates to U1, which is Snapmaker's flagship printer (§3.2). Of 32 candidates swept, three
   survive: **Halyard**, **Skerry**, **Alidade** (§3.3). Take two to counsel, not one.
7. **There is a bigger exposure than the name, and a rename does not touch it.** The fork sends
   `BBL-Slicer/02.03.00.01` as its User-Agent to bambulab.com's sign-in
   (`src/slic3r/GUI/Widgets/WebView.cpp:274-281`) - it presents itself to Bambu's servers as Bambu
   Studio. That is structurally the behaviour Bambu Lab acted against in April 2026 (§1.5, §5.1).
   **This deserves a decision before the naming one.**
8. **Crash reports go to Snapmaker's Sentry project**, hard-coded, on by default on Windows and
   macOS (`src/sentry_wrapper/SentryWrapper.cpp:88`). That is a bug regardless of naming (§2.9).

---

## 1. The legal facts

Every URL in this section was read **2026-09-04**. Confidence notes are the researcher's, not a
lawyer's.

### 1.1 The licence chain, and what survives a rename

| Project | Licence | Source |
|---|---|---|
| Slic3r (Ranellucci) | AGPL-3.0 | via PrusaSlicer lineage |
| PrusaSlicer (Prusa Research) | AGPL-3.0 | stated in Bambu Studio's README |
| Bambu Studio (Bambu Lab) | AGPL-3.0 | [github.com/bambulab/BambuStudio](https://github.com/bambulab/BambuStudio) |
| OrcaSlicer (SoftFever) | AGPL-3.0 | [LICENSE.txt](https://raw.githubusercontent.com/SoftFever/OrcaSlicer/main/LICENSE.txt) |
| Snapmaker Orca (Snapmaker) | AGPL-3.0 | [github.com/Snapmaker/OrcaSlicer](https://github.com/Snapmaker/OrcaSlicer) |
| **This fork** | **AGPL-3.0** | `LICENSE.txt`, `README.md:131` |

**What AGPL-3.0 requires of us no matter what we call ourselves**
([gnu.org/licenses/agpl-3.0.en.html](https://www.gnu.org/licenses/agpl-3.0.en.html)):

- **§5(a)** - the work must carry *"prominent notices stating that you modified it"*, with a date.
- **§5(b)** - and *"prominent notices stating that it is released under this License"*.
- **§13** - a modified version must *"prominently offer all users interacting with it remotely"* the
  Corresponding Source, at no charge, from a network server.
- **§7(e)** - additional terms may decline *"to grant rights under trademark law for use of some
  trade names, trademarks, or service marks"*.

**§7(e) is the crux.** The AGPL is a copyright licence. It grants no trademark rights, and it never
did. "The code is AGPL" is not an answer to "may we call it Snapmaker Orca". These are two separate
permissions and we currently hold only the first.

**§13 is the one the phone plane already engages, and the app will engage harder.** The hub serves
`/r/<token>/` to a browser and, under the Ultra1 plan, to a native app. §13 applies *"if your version
supports such interaction"* - it does. The obligation is discharged by the source being public at
`aceRage/Snapmaker-Ultra`, but the *offer* should be visible from the served pages, not only from the
desktop About dialog. **This is a gap today:** `resources/web/orca/hub.html` and
`resources/web/orca/stream_center.html` carry no licence line and no source link. See §4.6.

**What we already do right.** `src/slic3r/GUI/AboutDialog.cpp:289-294` names the full chain -
Snapmaker Orca ← OrcaSlicer (SoftFever) ← Bambu Studio (Bambu Lab) ← PrusaSlicer (Prusa Research) ←
Slic3r (Ranellucci), plus SuperSlicer - and states the AGPL-3.0 licence. `README.md:131` repeats it.
**None of that changes under any rebrand.** A rename replaces *our* product name in those sentences
and leaves every upstream attribution exactly as it is. Removing or thinning them would be a licence
breach; renaming without touching them is not.

### 1.2 Snapmaker's position - the decisive source

[Snapmaker Terms of Use](https://www.snapmaker.com/terms-of-use), last updated 2026-06-10; entities
Shenzhen Snapmaker Technologies Co., Ltd. and SNAPMAKER HK LIMITED. Three operative passages:

- *"You may not use our name or logo for commercial promotion without authorization."*
- *"Snapmaker reserves all intellectual property rights in its products or services."*
- On open source: Snapmaker Luban and Snapmaker Orca *"contain customized development based on open
  source projects ... which are subject to their respective open source license agreements"*, and
  *"Under the premise of complying with relevant open source licenses, you may modify the
  corresponding code."*

Read together, Snapmaker states the same split the AGPL does: **the code is yours to modify; the name
and the logo are not yours to use.** That is as close to an on-point answer as a public document is
going to give.

**Registered marks.** USPTO TESS and EUIPO eSearch are JavaScript applications that could not be
fetched; the table below comes from Justia search extracts and must be re-verified on
[tsdr.uspto.gov](https://tsdr.uspto.gov) before anyone relies on it.

| Mark | Serial | Reg. | Registered | Relevance |
|---|---|---|---|---|
| SNAPMAKER (stylised) | 87581397 | 5778443 | 2019-06-18 | 3D printers, machine tools |
| **SNAPMAKER (word mark)** | 79307451 | **6599728** | 2021-12-28 | **covers "design and development of computer hardware and software"** |
| SNAPMAKER ARTISAN | 90407061 | - | application | - |
| SNAPMAKER LAVA (EUIPO) | 018363411 | - | 2021-05-18 | classes 7, 9, 17, 42 |

Sources: [87581397](https://trademarks.justia.com/875/81/snapmaker-87581397.html),
[79307451](https://trademarks.justia.com/793/07/snapmaker-79307451.html),
[SNAPMAKER LAVA](https://www.trademarkelite.com/europe/trademark/trademark-detail/018363411/SNAPMAKER-LAVA).
**Confidence: medium-high** for the two US registrations; **UNVERIFIED** whether a bare "SNAPMAKER"
word mark is registered in the EU. **The standard-character word mark 6599728 covering software
design and development is the one that matters to a slicer and to an app.**

No Snapmaker trademark policy, brand-guidelines document or press kit was found publicly. **Absence
of evidence only** - one may exist behind a partner portal.

> **A note on how this was researched.** The Snapmaker Terms of Use page also carries a clause
> prohibiting *"the use of any artificial intelligence/AI tools to crawl, copy, download, or
> exploit content published on the platform."* That page was fetched once to answer this question.
> It is a site term, not an instruction that binds this document, but you should know it is there
> before anyone builds automated collection against snapmaker.com.

### 1.3 Bambu Lab

Bambu Lab's Terms of Use returned **HTTP 403** to automated fetching, for both the `/en/` and
`/en-eu/` paths. **The primary source could not be read and must be read manually in a browser.**
Secondary sources indicate Bambu treats its trademark and logo as "Licensed Materials" available only
in forms Bambu provides, and its affiliate terms bar implying official status.

Bambu Studio's repository carries **no** trademark or fork-naming clause beyond the AGPL itself.

### 1.4 App store policy - why the app forces the question

[Apple App Store Review Guidelines](https://developer.apple.com/app-store/review/guidelines/):

- **4.1(c)**: *"You cannot use another developer's icon, brand, or product name in your app's icon or
  name, without approval from the developer."*
- **5.2.1**: *"Don't use protected third-party material such as trademarks, copyrighted works, or
  patented ideas in your app without permission, and don't include misleading, false, or copycat
  representations, names, or metadata in your app bundle or developer name."*
- **2.3.7**: *"don't try to pack any of your metadata with trademarked terms, popular app names..."*

[Google Play Impersonation policy](https://support.google.com/googleplay/android-developer/answer/9888374):

- *"We don't allow apps that mislead users by impersonating someone else (for example, another
  developer, company, entity) or another app. Don't imply that your app is related to or authorized
  by someone that it isn't."*
- Listed violations include icons and titles that falsely imply a relationship to another company.

[Google Play IP policy](https://support.google.com/googleplay/android-developer/answer/9888072):
*"We don't allow apps that infringe on others' trademarks"*, and an app using another party's marks
*"in a way that is likely to cause confusion"* may be suspended.

**Descriptive / compatibility use.** No Apple or Google *review* policy was found that blesses
"works with &lt;third-party brand&gt;" in an app title. The nearest published articulation of the
nominative pattern is each company's rules about *its own* marks:

- [Apple's trademark guidelines](https://www.apple.com/legal/intellectual-property/guidelinesfor3rdparties.html)
  permit a word mark *"in a referential phrase such as 'runs on,' 'for use with,' 'for,' or
  'compatible with'"*, provided the mark is **less prominent than the product name** and the claim
  is true.
- [Android Brand Guidelines](https://developer.android.com/distribute/marketing-tools/brand-guidelines)
  say "Android" *"cannot be used in names of applications"* - use **"for Android"** instead; their
  worked example is ✅ "MediaPlayer for Android", ❌ "Android MediaPlayer".

**Extrapolating that to Snapmaker's and Bambu's marks is inference, not policy.** But it is the
pattern option (c) in §3.1 is built on, and it is the pattern Apple and Google themselves publish, so
it is the strongest available footing.

### 1.5 How other forks handled it - and the one that got a cease and desist

**OrcaSlicer's own rename.** OrcaSlicer was "BambuStudio-SoftFever" until **v1.5.0, 2023-03-17**. The
release note says, in full: *"Application name change ;)"*
([release](https://github.com/SoftFever/OrcaSlicer/releases/tag/v1.5.0)). **The reason is
UNVERIFIED.** Commentary assumes trademark caution; SoftFever never said so. Do not cite this as a
trademark precedent - cite it only as the fact that the most successful fork in this lineage dropped
the upstream vendor's name from its own.

**Vendor forks of OrcaSlicer:**

| Vendor | Product name | Keeps "Orca"? |
|---|---|---|
| Snapmaker | Snapmaker Orca | **yes** |
| FlashForge | Orca-Flashforge | **yes** |
| Elegoo | **ElegooSlicer** | no |
| Anycubic | **Anycubic Slicer Next** | no |
| Creality | **Creality Print** | no |

Sources: the repos plus [printago.io](https://printago.io/blog/orca-slicer-forks-compared) and
[simplyprint.io](https://simplyprint.io/articles/orcaslicer-forks-compared). Note the shape: **each
vendor puts its *own* name on the product.** None puts another vendor's name on it. That is the
pattern we currently break - we are not Snapmaker, and our product is called Snapmaker Orca.

Whether SoftFever objects to vendors using "Orca" is **UNVERIFIED** - no statement either way was
found, and two vendors ship "Orca" products uncontested.

**The Bambu Lab episode - April/May 2026.** A developer published an AGPL fork named
`OrcaSlicer-bambulab`; Bambu contacted him privately; the repo came down. Bambu's 2026-05-07 post
**conceded that AGPL forks of Bambu Studio are permitted** and narrowed its objection to
*impersonation* - specifically that the fork *"quietly introduces itself as official Bambu Studio"*
via its HTTP User-Agent.

Sources: [consumerrights.wiki](https://consumerrights.wiki/w/Bambu_Lab_cease_and_desist_against_OrcaSlicer_fork_developer),
[Tom's Hardware](https://www.tomshardware.com/3d-printing/developer-re-enables-3d-printer-features-that-bambu-lab-disabled-firm-promptly-threatens-legal-action-orcaslicer-bambulab-project-now-shuttered),
[GamersNexus](https://gamersnexus.net/fk-you-bambu-lab). **Confidence: medium** - the primary
documents (the C&D, Bambu's post) were not reachable and everything here is paraphrase.

**Read that last clause again against `src/slic3r/GUI/Widgets/WebView.cpp:274-281`.** See §5.1.

---

## 2. Where the brand lives in this repo

Anchors are against `ef81b7c194`. Counts are from `git grep` over tracked files.

**Headline counts**

| Measure | Count |
|---|---|
| Files containing `Snapmaker_Orca`, `snapmaker-orca`, `Snapmaker Orca` or `SnapmakerOrca` | **282** |
| ...of which are currently **byte-identical to `snapmaker-upstream/main`** | **141** |
| ...of which the fork has already modified anyway | 141 |
| Files whose **path** carries the brand (need `git mv`) | **54** |
| `Snapmaker_Orca` occurrences | 764 in 159 files |
| `Snapmaker Orca` occurrences | 2 970 in 95 files (2 519 of them in 20 `.po` files) |
| `snapmaker-orca` occurrences | 110 in 53 files |
| `Snapmaker-Ultra` occurrences (the half-rebrand already done) | 47 in 17 files |

The 141/141 split is the number that matters for §5.3: **half the rename lands in files we have never
touched**, and each one becomes a permanent conflict against upstream.

### 2.1 The two sources of truth for the name

The product name is defined **twice**, independently, and they must be kept in step:

| Where | Lines | Consumed by |
|---|---|---|
| `src/common_func/common_func.hpp:6-7` | `#define SLIC3R_APP_NAME "Snapmaker Orca"` / `#define SLIC3R_APP_KEY "Snapmaker_Orca"` | **all C++ code** |
| `version.inc:4-5` | `set(SLIC3R_APP_NAME ...)` / `set(SLIC3R_APP_KEY ...)` | **`configure_file` only** - the `.rc.in`, `Info.plist.in`, AppImage scripts |

A third: `src/libslic3r/libslic3r.h:5-7` defines `SLIC3R_APP_FULL_NAME "Snapmaker Orca"`,
`GCODEVIEWER_APP_NAME "Snapmaker_Orca G-code Viewer"` and
`GCODEVIEWER_APP_KEY "Snapmaker_OrcaGcodeViewer"`.

**Collapsing these three into one is a prerequisite of a clean rename**, and is worth doing on its
own merits.

### 2.2 The executable, targets and packaging

| Item | Value | Where |
|---|---|---|
| CMake project | `Snapmaker_Orca` | `CMakeLists.txt:57` |
| Native sub-project | `Snapmaker_Orca-native` | `src/CMakeLists.txt:2` |
| Library / exe target | `Snapmaker_Orca` | `src/CMakeLists.txt:128,131` |
| GUI wrapper target | `Snapmaker_Orca_app_gui` | `src/CMakeLists.txt:197` |
| **Output name** | **`snapmaker-orca`** (`.exe`) | `src/CMakeLists.txt:141,204` |
| Profile validator | `Snapmaker_Orca_profile_validator` | `src/CMakeLists.txt:102-115` |
| Helper function | `Snapmaker_Orca_copy_dlls` | `CMakeLists.txt:764` |
| VS startup project | `Snapmaker_Orca_app_gui` | `CMakeLists.txt:882` |
| CPack package name / vendor | **`Snapmaker-Ultra`** | `CMakeLists.txt:937-938` |
| Installer filename | `Snapmaker-Ultra_Windows_Installer_V<ver>.exe` | `CMakeLists.txt:945` |
| Package description | "Snapmaker Orca is an open source slicer for FDM printers" | `CMakeLists.txt:946` |
| Homepage URL | `https://github.com/Snapmaker/OrcaSlicer` | `CMakeLists.txt:947` |
| Package icon | `resources/images/Snapmaker_Orca.ico` | `CMakeLists.txt:949` |
| Installed icon name | `$INSTDIR\snapmaker-orca.exe` | `CMakeLists.txt:952` |
| Desktop shortcut | `Snapmaker-Ultra.lnk` → `snapmaker-orca.exe` | `CMakeLists.txt:1034` |
| Uninstall registry key | `Snapmaker-Ultra` | `CMakeLists.txt:1043` |
| WiX upgrade GUID | `058245e8-20e0-4a95-9ab7-1acfe17ad511` | `CMakeLists.txt:1048` |

**Note the existing half-rebrand.** The *package* is already `Snapmaker-Ultra`; the *application* is
still `Snapmaker Orca`. `README.md:5` says so in as many words: *"the app still reports itself as
Snapmaker Orca 2.3.6.x"*. This is the awkward middle state a full rebrand resolves.

`installer.nsi` (the older standalone script, superseded by CPack but still present) hardcodes
`PRODUCT_NAME "Snapmaker Orca"`, `PRODUCT_PUBLISHER "Snapmaker"`, `PRODUCT_WEB_SITE`
`https://github.com/Snapmaker/OrcaSlicer`, install dir `$PROGRAMFILES64\Snapmaker_Orca`,
`VIAddVersionKey "CompanyName" "Snapmaker"` and `"LegalCopyright" "Copyright (C) Snapmaker"`
(`installer.nsi:6-46`). **`CompanyName`/`LegalCopyright` naming Snapmaker on a binary Snapmaker did
not build is the most straightforwardly wrong string in the repo** and should be corrected under any
option, including "do nothing".

### 2.3 The data directory - the migration problem

`src/slic3r/GUI/GUI_App.cpp:2192` calls `SetAppName(SLIC3R_APP_KEY)`; line 2221 then takes
`wxStandardPaths::Get().GetUserDataDir()`. So:

```
SLIC3R_APP_KEY = "Snapmaker_Orca"
  → Windows  %APPDATA%\Snapmaker_Orca
  → macOS    ~/Library/Application Support/Snapmaker_Orca
  → Linux    $XDG_CONFIG_HOME/Snapmaker_Orca        (GUI_App.cpp:2231)
```

and the config file inside it is `SLIC3R_APP_KEY ".conf"` (`src/libslic3r/AppConfig.cpp:1600-1604`),
i.e. `Snapmaker_Orca.conf`.

**Changing `SLIC3R_APP_KEY` moves the data directory and renames the config file. Everything below
lives there.** Measured on the user's real data dir, 2026-09-04:

| Subtree | Size | What breaks if it is not migrated |
|---|---|---|
| **`hub/`** | **432 MB** | the whole phone plane - see below |
| `ota/` | 54 MB | update payloads; re-downloadable |
| `log/` | 39 MB | history only |
| `web/` | 36 MB | flutter/web assets; regenerated |
| `system/` | 23 MB | vendor profile snapshot; regenerated |
| `user/` | 5.9 MB | **the user's presets** - printer, filament, process |
| `plugins/` | 4.7 MB | `bambu_networking.dll` / `BambuSource.dll`; re-fetchable but a nuisance |
| `printers/` | 43 KB | per-printer state |
| `Snapmaker_Orca.conf` | 244 KB | **all preferences, recent files, window state** |
| `user_backup-v2.2.1` … `-v2.3.6` | - | nine historical preset backups |
| `ultranet/`, `hms/`, `SVG/`, `FlashNetwork/` | - | cloud-login state, HMS strings, scratch |
| **Total** | **603 MB** | |

**Inside `hub/`, the things that cannot simply be regenerated:**

| File | Holds | Consequence of loss |
|---|---|---|
| `hub/settings.json` | `token`, `old_tokens`, `webpush` (the **P-256 VAPID key pair** and every subscription), `notify` (Pushover/ntfy/webhook destinations), `allowed_logins`, `remote_on` | **Every phone bookmark 404s** (new token). **Every Web Push subscription is dead** - a new VAPID key invalidates them all and each phone must re-subscribe by hand. Every relay destination is gone. The tailnet allow-list is gone, so remote access silently stops. |
| `hub/snapmaker_lan.json` | LAN printer list | re-add each printer by address |
| `hub/snapmaker_keys.json` | per-printer keys, three entries today | re-pair each printer |
| `hub/streams.json` | camera hosts + active set | re-add every camera |
| `hub/events.json` | event ring + `last_id` | history only |
| `hub/saves/`, `hub/uploads/` | **the G-code archive**, the bulk of the 432 MB | the Reprints list ([`2026-09-04-gcode-archive-design.md`](2026-09-04-gcode-archive-design.md)) empties |
| `hub/go2rtc.yaml` | generated per run | regenerated |
| `hub/hub.json` | per-run secret and port; `shutdown()` deletes it | nothing |

**The VAPID pair is the sharpest edge.** `src/slic3r/GUI/WebPush.cpp` owns it in `settings.json`; a
subscription is bound to the key that created it. Lose the key and every installed home-screen web
app stops receiving push **silently** - no error, just nothing. Any migration must move
`hub/settings.json` or explicitly warn.

**Absolute paths embedded in the config.** `Snapmaker_Orca.conf` contains **14 lines** with literal
`...\AppData\Roaming\Snapmaker_Orca\...` paths - the recent-project list, `last_backup_path`,
`settings_folder`, upload paths under `hub\uploads\`. A **copy** leaves them pointing at the old
directory (which still exists, so they resolve, but edits diverge). A **move** leaves them dangling.
Either way the recent-files list needs rewriting or accepting as lossy. This is a small, real,
easy-to-forget task.

**We already have the pattern to build on.** `src/slic3r/GUI/PresetMirror.cpp:67-92`
(`find_bambu_user_dir`) walks `bfs::path(Slic3r::data_dir()).parent_path()` to find a sibling
vendor's data directory under `%APPDATA%`. A migration reads the *old* sibling the same way. The
comment at `PresetMirror.cpp:69` even spells out the assumption: *"data_dir() is
%APPDATA%\Snapmaker_Orca; BambuStudio is a sibling under %APPDATA%"* - a line that must itself be
updated.

### 2.4 Windows installer, registry and file associations

| Item | Value | Where |
|---|---|---|
| URL scheme registered by the installer | `snapmaker-ultra` → `URL:Snapmaker-Ultra` | `cmake/nsis/SnapmakerURLProtocols_install.nsh:8-11` |
| ...removed on uninstall | same key only | `cmake/nsis/SnapmakerURLProtocols_uninstall.nsh:4` |
| URL schemes registered **at runtime** | `Snapmaker_Orca`, `snapmaker-orca` | `src/slic3r/GUI/GUI_App.cpp:2835-2836` |
| Runtime registry root | `HKCU\Software\Classes\<scheme>\shell\open\command` | `GUI_App.cpp:7648-7649` |
| Accepted URL prefixes | `Snapmaker_Orca://open`, `snapmaker-orca://open`, `orcaslicer://open` | `src/libslic3r/Utils.hpp:233-235` |
| File-association ProgID | **`" Orca.Slicer.1"`** - note the leading space | `GUI_App.cpp:369` |
| Associated extensions | `3mf`, `stl`, `step`/`stp`, `gcode` | `GUI_App.cpp:2827-2839` |
| Preinstall guard checks for | `snapmaker-orca.exe` **and** legacy `Snapmaker_Orca.exe` | `CMakeLists.txt:966-971` |

Two notes. **First**, the NSIS script and the runtime disagree: the installer registers
`snapmaker-ultra` while the running app registers `Snapmaker_Orca` and `snapmaker-orca` in HKCU -
which is exactly the collision with an official Snapmaker Orca install that the NSIS comment says it
is avoiding. **Second**, `" Orca.Slicer.1"` with the leading space is inherited from OrcaSlicer and
is almost certainly a bug; a rename is the natural moment to fix it, but doing so re-associates
files and needs a migration of its own.

### 2.5 macOS bundle

`src/dev-utils/platform/osx/Info.plist.in`:

| Key | Value |
|---|---|
| `CFBundleExecutable`, `CFBundleName` | `@SLIC3R_APP_KEY@` → `Snapmaker_Orca` |
| **`CFBundleIdentifier`** | **`com.snapmaker.snapmaker-orca`** |
| `CFBundleIconFile` | `images/Snapmaker_Orca.icns` |
| `CFBundleGetInfoString` | `@SLIC3R_APP_NAME@ Copyright © 2024-2026 **Snapmaker**. All rights reserved.` |
| `CFBundleURLName` | `Snapmaker Orca Open URL` |
| `CFBundleURLSchemes` | `Snapmaker_Orcaopen`, `Snapmaker_Orca`, `snapmaker-orca` |

Also `CMakeLists.txt:147` sets `CMAKE_XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER` to
`com.snapmaker.snapmaker-orca`, and `src/CMakeLists.txt:108-110` gives the profile validator
`com.Snapmaker.Snapmaker_Orca.profile-validator` and `MACOSX_BUNDLE_COPYRIGHT "Copyright © 2024-2026
Snapmaker. All rights reserved."`.

**`com.snapmaker.*` is a reverse-DNS identifier claiming snapmaker.com.** We do not control that
domain. Under a rebrand it must become a domain we do control, and **changing a bundle id is
one-way** - macOS treats the result as a different application (keychain items, TCC/privacy grants,
Gatekeeper assessment and any notarisation history all reset).

### 2.6 Linux packaging

| Item | Value | Where |
|---|---|---|
| **Flatpak app-id** | **`io.github.Snapmaker.Snapmaker_Orca`** | `scripts/flatpak/io.github.Snapmaker.Snapmaker_Orca.yml:1` |
| Metainfo id / launchable / desktop-id | same | `...metainfo.xml:3-6` |
| `<name>` / `<developer_name>` | `Snapmaker_Orca` / **`Snapmaker`** | `...metainfo.xml:8-10` |
| homepage / help / bugtracker | `github.com/Snapmaker/OrcaSlicer[/wiki|/issues]` | `...metainfo.xml:11-13` |
| D-Bus talk-name | `io.github.Snapmaker.Snapmaker_Orca.InstanceCheck.*` | `...yml:19` |
| Desktop entry | `Name=Snapmaker Orca`, `Icon=Snapmaker_Orca`, `Exec=snapmaker-orca %U`, `StartupWMClass=snapmaker-orca` | `src/dev-utils/platform/unix/Snapmaker_Orca.desktop` |
| FHS resources dir | `${DATAROOTDIR}/Snapmaker_Orca` | `CMakeLists.txt:915` |
| Icon install | `Snapmaker_Orca_{32,128,192}px.png` → `Snapmaker_Orca.png` | `CMakeLists.txt:921-922` |
| AppImage name | `@SLIC3R_APP_KEY@_Linux_V@Snapmaker_VERSION@.AppImage` | `build_appimage.sh.in:7` |
| pixbuf cache dir | `$XDG_CACHE_HOME/@SLIC3R_APP_KEY@` | `build_linux_image.sh.in:317` |

**`io.github.Snapmaker.*` asserts we are the GitHub organisation `Snapmaker`.** We are not. For a
Flatpak this is not cosmetic: the reverse-DNS id is meant to be a namespace the publisher controls,
and Flathub enforces it. `io.github.aceRage.<Name>` would be correct. **A Flatpak app-id change is a
hard break** - it is a different application to the system, installed alongside rather than over the
old one, with its own `~/.var/app/<id>/` sandbox home. Users must migrate by hand or by a shipped
script.

D-Bus single-instance names are inconsistent already: `src/slic3r/GUI/InstanceCheck.cpp:240,554,592`
use `com.snapmaker.snapmaker-orca.InstanceCheck...` while `:243,613` use
`/com/softfever3d/Snapmaker_Orca/InstanceCheck/...` - **the object path still says SoftFever.**

### 2.7 Icons, logos and the splash

`resources/images/` holds **23 files** whose names carry the brand:

```
Snapmaker_Orca.ico  Snapmaker_Orca.icns  Snapmaker_Orca.png
Snapmaker_Orca-mac_128px.png  Snapmaker_Orca-mac_256px.ico
Snapmaker_OrcaTitle.ico  Snapmaker_OrcaTitle.png
Snapmaker_Orca_{32,64,128,154,180,192,512}px*.png
Snapmaker_Orca_192px_{grayscale,transparent}.png
Snapmaker_Orca_512px_maskable.png  Snapmaker_Orca_154_title.png
Snapmaker_Orca_about.svg  Snapmaker_Orca_gradient{,_circle,_narrow}.png
Snapmaker_Orca_gray.png
```

plus unbranded-by-name but brand-carrying `splash_logo.svg`, `splash_logo_dark.svg`,
`splash_app_icon.svg`, `studio_logo.svg`.

**These are Snapmaker's logo.** `Snapmaker_Orca_192px.png` is Snapmaker's "S" glyph, white on a black
rounded square, with a blue BETA banner. It is the Windows taskbar icon, the tray icon, the About
dialog logo, the installer icon, the macOS `.icns`, the Linux hicolor icon, the PWA maskable icon and
the phone home-screen icon. **This single asset family is the strongest item in the whole inventory:**
it is a registered mark used as our application identity, and Apple 4.1(c) names icons first
(*"another developer's icon, brand, or product name in your app's icon or name"*).

Code references: 41 sites use `Snapmaker_OrcaTitle.ico` as the dialog icon (`SetIcon(...)` across
`AboutDialog.cpp`, `BindDialog.cpp`, `CreatePresetsDialog.cpp`, `Calibration.cpp`,
`AmsMappingPopup.cpp` and others); `AboutDialog.cpp:21` loads `Snapmaker_Orca_192px.png`;
`AboutDialog.cpp:236` loads the `Snapmaker_Orca_about` SVG; `RemoteHub.cpp:2896-2897` loads
`Snapmaker_Orca.ico` with a `Snapmaker_Orca_128px.png` fallback for the tray.

### 2.8 The hub, the tray and the web pages

| Surface | Current string | Where |
|---|---|---|
| Tray tooltip | `Snapmaker-Ultra Hub` | `RemoteHub.cpp:2823` |
| Tray balloon title | `Snapmaker-Ultra Hub` | `RemoteHub.cpp:2813` |
| Tray menu header | `Snapmaker-Ultra Hub: N slicer windows open` | `RemoteHub.cpp:2835` |
| Hub process app name | `Snapmaker-Ultra Hub` | `RemoteHub.cpp:2888` |
| Tray icon | `Snapmaker_Orca.ico` | `RemoteHub.cpp:2896` |
| Hub page `<title>` and `<h1>` | **`Ultra Hub`** | `resources/web/orca/hub.html:6,57` |
| Phone page `<title>` | `Stream` | `stream_center.html:16` |
| **PWA manifest `name`** | **`Snapmaker Orca`** | `RemoteHub.cpp:2297` |
| **PWA manifest `short_name`** | **`Orca`** | `RemoteHub.cpp:2298` |
| PWA description | "The cameras, printers and prints on the PC in your workshop." | `RemoteHub.cpp:2299` |
| Main window title | `<project> - Snapmaker Orca` | `Plater.cpp:16831` |
| About dialog title | `About <SLIC3R_APP_FULL_NAME>` | `MainFrame.cpp:2492,3089` |
| About version line | `Snapmaker Orca <version>` | `AboutDialog.cpp:245` |

**The manifest name is the sharpest one.** `RemoteHub.cpp:2296-2304` generates the web manifest in
C++, and a phone that installs the page to its home screen gets an icon labelled **"Orca"** from a
manifest whose `name` is **"Snapmaker Orca"**. That is already an installed app carrying Snapmaker's
name and logo - it simply arrives via Safari rather than the App Store. **Guideline 4.1(c) is about
the App Store and does not reach a home-screen web app, but Snapmaker's Terms of Use do.**

`resources/web/orca/hub.html` is already titled "Ultra Hub" and `RemoteHub.cpp` already says
"Snapmaker-Ultra Hub" - so the hub is *half* renamed and the phone manifest is not renamed at all.
The three should agree.

The 14 occurrences of "Snapmaker" in `stream_center.html` were checked individually: **all of them
refer to the printer hardware** ("A Snapmaker has four toolheads", "No Snapmaker found yet. Add one
by its address below", the `'Snapmaker / direct stream URL'` option label). **These are correct and
must not change** - see §2.11.

### 2.9 Crash reporting - a bug, not a branding question

`src/sentry_wrapper/SentryWrapper.cpp:88` hard-codes:

```
https://282935326eecb9758e7f84a2ad3ae0ab@o4508125599563776.ingest.us.sentry.io/4510425163956224
```

`SentryWrapper.cpp:258` sets environment `"Release"`; `:272` sets the release to `Snapmaker_VERSION`
(`"2.3.6"`). `CMakeLists.txt:112-118` makes `SLIC3R_SENTRY` default **ON on Windows and macOS**.

So **every crash in every Ultra build is being reported into Snapmaker's Sentry organisation, tagged
as release 2.3.6, indistinguishable from a crash in their own product.** Two problems at once: we
send our users' crash data to a third party who did not agree to receive it, and we pollute
Snapmaker's triage with faults in code they did not write. **This should be fixed whichever naming
option is chosen** - either point it at our own Sentry project, or default `SLIC3R_SENTRY` to OFF.
Cost: under an hour. It is the highest ratio of risk removed to effort spent anywhere in this
document.

### 2.10 Localisation

| Item | Count |
|---|---|
| `.po` files (`localization/i18n/<lang>/Snapmaker_Orca_<lang>.po`) | 20 |
| Template `localization/i18n/Snapmaker_Orca.pot` | 1 |
| `Snapmaker Orca` occurrences across the 20 `.po` files | **2 519** |
| ...in the `.pot` | 69, of which 39 are `msgid` |
| Files needing `git mv` | 21 |

Build glue: `CMakeLists.txt:693` writes `${BBL_L18N_DIR}/Snapmaker_Orca.pot`; `:702-708` glob
`*/Snapmaker_Orca*.po` and `msgmerge` them; `:716-720` compile to
`${L10N_DIR}/${po_dir}/Snapmaker_Orca.mo`. Also `scripts/run_gettext.sh`, `scripts/run_gettext.bat`,
`scripts/HintsToPot.py` and `.github/workflows/check_locale.yml`.

**This is bulk, not difficulty.** The `msgid`s change, so every translation of a brand-bearing string
goes fuzzy and needs re-merging; `msgmerge` handles it, but 39 strings × 20 languages come back
untranslated until someone fixes them. Budget it as churn, not as engineering.

`resources/data/hints.ini` carries the name in user-visible hint text (*"Did you know that Snapmaker
Orca supports chamber temperature?"* and similar) - a handful of lines, also translated.

### 2.11 What must NOT change

**`resources/profiles/Snapmaker.json` and `resources/profiles/Snapmaker/` stay exactly as they are.**

- `resources/profiles/Snapmaker.json` declares `"name": "Snapmaker"`, `"description": "Snapmaker
  configurations"` - **19 machine models, 100 machines, 328 filaments**, across **647 files** under
  `resources/profiles/Snapmaker/`.
- That "Snapmaker" is **the printer manufacturer's name**, exactly as `BBL.json`, `Creality.json`,
  `Anycubic.json`, `Elegoo.json` and thirty-odd others name theirs. It is a factual statement about
  whose printer a profile drives. It is nominative use, it is correct, and changing it would break
  every user's preset inheritance and every profile the app ships.
- **Renaming it would be the actual error.** A slicer that supports Snapmaker printers must say
  "Snapmaker" in its printer list. The rebrand is about *our product's* name, not about erasing the
  word.

Similarly untouched:

- **The network agent and Snapmaker cloud login.** `SSWCP.cpp`, `RemoteSnapmaker.cpp`,
  `SnapmakerLan.cpp`, `WebSMUserLoginDialog.cpp` and the endpoints they call - `id.snapmaker.com`,
  `api.snapmaker.com`, `public.resource.snapmaker.com` and the `.cn` mirrors (about 45 URL
  occurrences). These legitimately talk to Snapmaker's services on the user's behalf, with the user's
  own account. That is compatibility, not branding.
- **Every reference in `stream_center.html` to Snapmaker printers** (§2.8).
- **Every upstream attribution** in `AboutDialog.cpp:281-294` and `README.md:131` (§1.1).
- `MIN_FIRM_VER` and the "firmware version of SnapmakerU1" notice (`AboutDialog.cpp:288`).

**The one open question in this category is the `SM-Slicer` User-Agent** (§5.1), which is neither
clearly branding nor clearly compatibility.

### 2.12 Everything else, briefly

- **Source filenames**: `src/Snapmaker_Orca.cpp` (the entry point, ~6 400 lines),
  `src/Snapmaker_Orca.hpp`, `src/Snapmaker_Orca_app_msvc.cpp`,
  `src/dev-utils/Snapmaker_Orca_profile_validator.cpp`,
  `src/dev-utils/platform/msw/Snapmaker_Orca{.rc.in,.manifest.in,-gcodeviewer.rc.in}`.
- **G-code and 3MF identity**: `GCode.cpp:2275` writes `; generated by <name> on <date>`;
  `3mf.cpp:2564` writes `<metadata name="Application">SLIC3R_APP_KEY-SLIC3R_VERSION</metadata>`.
  Reading back is via `GCodeProcessor::Producers` (`GCodeProcessor.cpp:618-624`), which already
  accepts `SLIC3R_APP_NAME`, `"generated by Snapmaker Orca"`, `"generated by Snapmaker_Orca"`,
  `"generated by BambuStudio"` and `"BambuStudio"`. `Config.cpp:1398-1399` already keeps a
  `compat_snapmaker_gcode_header` next to the `SLIC3R_APP_NAME` one. **A rename adds one entry to
  each list; the compatibility mechanism already exists.** Files written by the old build keep
  working, and third-party tools that sniff for "Snapmaker Orca" keep matching old files.
- **Version-check URL**: `AppConfig.cpp:41` still points at
  `api.github.com/repos/Snapmaker/OrcaSlicer/releases/latest` - **the fork checks Snapmaker's
  releases for its own updates.** Independent of naming, this is wrong and should point at
  `aceRage/<repo>`.
- **Temp files**: `%TEMP%\snapmaker_orca_model\...` (seen in the live config's `last_backup_path`);
  `BackgroundSlicingProcess.cpp:910` uses `"." SLIC3R_APP_KEY ".upload.%%%%-..."`.
- **CI and templates**: `.github/workflows/{build_all,build_deps,build_orca,check_locale}.yml`,
  `.github/ISSUE_TEMPLATE/{bug_report,feature_request}.yml`.
- **Docs**: `README.md`, `AGENTS.md`, `CLAUDE.md`, `SECURITY.md`, `doc/Home.md`,
  `doc/developer-reference/*`, `docs/*`, `.superpowers/sdd/*`, plus the build scripts
  (`build_release_vs2022.bat`, `build_release_macos.sh`, `build_linux.sh`, `build_flatpak.sh`,
  `build_release.bat`, `scripts/sign_and_package.sh`, `scripts/orca_cli.py`).
- **`deps/`**: ~30 files mention `Snapmaker_Orca` in **comments only**. No rename needed; leave them.
- **Repo URLs**: 11 occurrences of `github.com/aceRage/Snapmaker-Ultra`.

---

## 3. The options

### 3.1 The three shapes

**(a) Rename only the companion app; leave the slicer as "Snapmaker Orca".**

- *Cost*: near zero for the slicer. The app gets a neutral name in its own repo.
- *Why it fails*: the app is not a separate product. It pairs to *this* hub, renders *this* page, and
  its App Store listing has to describe what it does - which means naming the desktop application it
  connects to. A listing that says "companion for Snapmaker Orca" walks straight back into 4.1(c) and
  into Google's *"don't imply that your app is related to or authorized by someone that it isn't"*.
  It also leaves the desktop side unchanged: Snapmaker's logo as our icon, `com.snapmaker.*` as our
  bundle id, `io.github.Snapmaker.*` as our Flatpak id, and Snapmaker named as `CompanyName` on
  binaries they did not build. **The option does not solve the problem it is scoped to solve.**
- *When it would be right*: only if the app is abandoned (§6, R8 of the app plan) **and** the user
  accepts the desktop-side facts as they are.

**(b) Rebrand the slicer to a neutral name.**

- The product gets its own name, its own icon, its own reverse-DNS namespace, its own Sentry project,
  its own installer identity. The app takes the same name plus a suffix.
- *Cost*: §4. Roughly **12-18 working days** across five phases, most of it mechanical, with one
  genuinely delicate piece (the data-dir migration, §4.4).
- *This is the recommendation.*

**(c) Hybrid: neutral product name plus descriptive compatibility wording.**

- Not really an alternative to (b) - it is **(b) plus a rule about the tagline.** The product is
  "&lt;Name&gt;"; the strapline is "*a slicer for Snapmaker and Bambu Lab printers*". The vendor
  names appear only in a referential phrase, never in the product name, never in the icon, never
  larger than our own name.
- This follows the nominative pattern Apple and Google publish for their own marks (§1.4): a
  referential phrase, the third-party mark less prominent than the product name, and a claim that is
  true. It is also simply accurate - the profiles ship, the printers work.
- **Adopt (b) and (c) together.**

### 3.2 Why "Ultra" and "Ultra1" should be dropped

The working names are the weakest candidates on the list.

- **"Ultra1" abbreviates to U1, which is Snapmaker's flagship printer** - a four-toolhead
  toolchanger, >$20.6 M on Kickstarter from >20 000 backers, retailing from early 2026
  ([snapmaker.com/en/snapmaker-u1](https://www.snapmaker.com/en/snapmaker-u1);
  [Fabbaloo](https://www.fabbaloo.com/news/snapmaker-launches-u1-affordable-toolchanger-3d-printer-aimed-at-reducing-filament-waste)).
  Renaming *away* from Snapmaker and landing on their product's SKU is the worst available outcome.
- **"Snapmaker Ultra" reads as a Snapmaker product tier.** No such Snapmaker product exists
  (**UNVERIFIED negative** - absence of evidence), which makes it worse, not better: it is precisely
  the *"imply that your app is related to or authorized by someone that it isn't"* pattern. The
  current repo name `aceRage/Snapmaker-Ultra` has this property.
- **"Ultra" alone is unregistrable-weak and unsearchable.** It is a laudatory adjective. Live uses
  include `exhibitionist-digital/ultra` (a Deno/React framework holding the bare name on GitHub),
  ULTRA MOBILE (reg. 6257205, telecom, apps on both stores), ultra.io (game distribution), and the
  UltraEdit family of software marks.
- **"Ultra" is a standard 3D-printer tier suffix** (Anycubic Photon Ultra, Photon Mono 4 Ultra), so
  it reads as a hardware qualifier, not a software brand.
- **"Ultra One" already exists in 3D printing twice**: MakerGear Ultra One and U3DS UltraOne - direct
  hits in our own sector, the worst place for a collision.

**"Ultra" can survive as internal vocabulary** - the `Ultra:` code comments, the `-ultra` release tag
suffix, the "Ultra preferences" tab - without being the public product name. Nothing forces a
same-day purge of internal identifiers.

### 3.3 Candidate names

A collision sweep was run over 32 candidates: GitHub, App Store, Google Play, plain-web trademark
search, domain, and generic-word risk. **Neither official register was machine-readable**
(`tmsearch.uspto.gov` and EUIPO eSearch are JavaScript applications; `uspto.report`, Trademarkia and
direct Justia pages returned 403), so **every trademark line below is plain web search only. "Not
found" means "I did not find one", never "clear".**

**The shortlist - three to take to counsel, in order.**

| # | Name | Verdict | The case | The snag |
|---|---|---|---|---|
| **1** | **Halyard** | likely clear | The rope that raises a sail: arbitrary as applied to slicing software, which is what makes a mark strong. **No app on either store.** No software mark found. GitHub use is weak and stale - `spinnaker/halyard` (316★) is **archived**, `Merck/Halyard` (114★) is a semantic triplestore last touched 2023. The metaphor also fits - a hub that hoists a print to a printer. | **O&M Halyard** is a large medical-supply brand with ~139 filings. Counsel must confirm classes 9/42 are open and that they do not police outside medical. |
| **2** | **Skerry** | likely clear | A small rocky sea islet. **Nothing on GitHub above 14★** (`SeCherkasov/SkerrySSH`); nothing in 3D printing, CAD, graphics or dev tooling. `skerry.com` redirects to a **GoDaddy for-sale page**; `skerry.app` is **unregistered**. Two syllables, easy to say, no descriptive overlap. | `getskerry.com` is a live European B2B service brand (explicitly "not software", so likely class 35). One small indie iOS app, a UK sea-state monitor ([id6783832327](https://apps.apple.com/fr/app/skerry/id6783832327)). |
| **3** | **Alidade** | likely clear | The sighting arm of an astrolabe. Top GitHub hit is [the-engine-room/alidade](https://github.com/the-engine-room/alidade) at **17★**; everything else is 1★. **No software-tooling incumbent** and nothing in our category. | [Alidade Partners](https://alidade.com) (banking consultancy) holds the `.com`; an indie Minecraft-map app on the App Store ([id6741483898](https://apps.apple.com/us/app/alidade/id6741483898)). Readers may hesitate on the pronunciation. |

**Two more, if breadth is wanted - but both are compromised:**

| Name | Verdict | Why it is not in the top three |
|---|---|---|
| **Sextant** | needs counsel | Genuinely crowded as a developer-tool name - repos at 573★, 176★, 161★, 92★, 68★ - plus many App Store navigation apps and two consultancies. |
| **Capstan** | needs counsel | Repos at ~650★ and ~300★, CapstanAG apps on the stores, and **"capstan drive" is a real motion-control mechanism**, which edges the name toward descriptive in a product that moves toolheads. |

**Notable rejections, with the reason - so nobody re-proposes them:**

*Fatal inside 3D printing:*

- **Marlin** - [marlinfw.org](https://marlinfw.org/) is *the* 3D-printer firmware; a slicer called Marlin reads as a firmware fork.
- **Fathom** - [FATHOM Advanced Manufacturing](https://fathommfg.com/) is a large additive-manufacturing service bureau that also ships its own quoting and AM-analytics software. Same industry, same buyer.
- **Belay** - [Annex-Engineering/Belay](https://github.com/Annex-Engineering/belay) (296★) is **an FFF 3D-printer sensor with a Klipper module**. Plus two other software Belays.
- **Windlass** - tiny GitHub footprint, but [Annex-Engineering/windlass](https://github.com/Annex-Engineering/windlass) is a Rust Klipper host implementation - our lane, and the same org as Belay.
- **Spool / Spoolworks** - [Spoolman](https://github.com/Donkie/Spoolman) (2.8k★) is the incumbent filament manager; plus OpenSpool (762★), SpoolEase (545★) and E3D's shipping **spoolWorks** filament line.
- **Forge / ForgeSlicer** - [forgeslicer.com](https://forgeslicer.com/) is a live browser tool advertising one-click hand-off to OrcaSlicer, Bambu Studio and PrusaSlicer. Same users, same workflow. Plus Autodesk and Laravel Forge marks.
- **Nozzle** - a printer part. Descriptive, unownable here.

*Dominant software incumbent:*

- **Kestrel** - the default ASP.NET Core web server.
- **Lodestar** - ChainSafe's Ethereum consensus client (~1.4k★); LodeStar Technology holds 15 marks.
- **Trellis** - [microsoft/TRELLIS](https://github.com/microsoft/TRELLIS) (13.6k★) and TRELLIS.2 (11k★) are the leading structured-3D-generation models. Adjacent enough that search would be unwinnable.
- **Yawl** - YAWL is an established open-source workflow language.
- **Ketch** - ketch.com is a funded data-privacy software company.
- **Tackle** - Tackle.io raised a $100 M Series C.
- **Anvil** - three separate 1k★+ dev-tool projects, plus anvil.works and useanvil.com.
- **Vellum** - **Ashlar-Vellum is a CAD/3D-modelling vendor** (mark 75208086); CAD → slicer is related goods. Plus a 2023 class-9 registration and three store apps.
- **Astrolabe** - ASTROLABE® is a *registered* mark for astrology **software**. Four syllables too.
- **Bowline** - bowline.app is a live SaaS; Bowline.net holds US software marks; a 632★ Ruby GUI framework.
- **Quarterdeck** - the historic PC brand (DESQview, QEMM), *and* a live 2025-26 GitHub cluster of tray apps, dashboards and control decks. **Converging on our exact product metaphor.**
- **Binnacle** - a live Mexican B2B SaaS shipping under the exact name on both stores.
- **Kiln** - both stores are full of pottery-kiln controller apps (KilnLink, Bartlett KilnAid, TAP Kiln Control), which is *structurally our pitch* aimed at an adjacent hobby. Plus Kiln-AI (5k★), kiln.fi, kiln.com.
- **Meridian** - google/meridian (1.5k★), multiple registered software marks, every good domain gone.
- **Cirrus** - Cirrus Logic asserts "Cirrus" as a registered mark; a $B-scale IC vendor in consumer electronics.
- **Bellwether** - no dominant owner, but several small software firms and banking apps on both stores.

*Too generic to own:* **Lathe**, **Sounding**, **Ultra** (§3.2).

**The pattern worth noticing.** Every name that survived is **arbitrary** - a word with no connection
to slicing, printing or making. Every name that failed did so either because it *described* the
domain (nozzle, spool, lathe, kiln, forge) and was therefore already taken by five people, or because
it was a good enough software name that someone got there first. **Arbitrary is not a stylistic
preference; it is the only category with room left in it.**

### 3.4 What a lawyer would actually have to do

This document cannot clear a name. Before anything is filed, published or shipped under a new name:

1. A **knockout search** on the shortlist - identical and near-identical marks in the relevant
   classes, likely **Nice class 9** (software) and **class 42** (SaaS / software design).
2. A **full clearance search** on the survivor: common-law use, design marks, foreign registers in
   the jurisdictions where the app would be listed, and phonetic/visual near-misses.
3. An opinion on whether the **descriptive-use tagline** in option (c) is defensible in each of those
   jurisdictions.
4. A decision on whether to **file** at all. For a free open-source project, an unregistered mark
   plus a clean clearance may be enough; registration buys enforceability we may never need.

**Clear two names, not one.** A single-candidate clearance that fails costs the whole cycle.

---

## 4. The phased plan

Effort is developer-days for one person who knows the codebase. It excludes the legal work in §3.4
and the design work for a new logo.

### Phase 0 - Decide (0.5 d + external latency)

Pick the name; get the clearance started; register the GitHub org or account name, the domain and the
App Store / Play Console name reservations **before** any commit uses the name. **Nothing else starts
until this returns.**

Deliverables: the name; the reverse-DNS namespace (`<tld>.<domain>.<name>`); the Flatpak id
(`io.github.<account>.<Name>`); the repo name.

### Phase 1 - One source of truth, and the free wins (1-1.5 d)

Do this **before** the rename; it is worth doing even if the rename never happens.

1. **Collapse the three name definitions into one.** `version.inc:4-5`,
   `src/common_func/common_func.hpp:6-7` and `src/libslic3r/libslic3r.h:5-7` become a single
   `configure_file`-generated header consumed by both C++ and CMake. *0.5 d.*
2. **Fix the Sentry DSN** (§2.9) - our own project, or `SLIC3R_SENTRY` default OFF. *0.5 h.*
3. **Fix the version-check URL** (`AppConfig.cpp:41`) to point at our own releases. *0.5 h.*
4. **Fix `installer.nsi`'s `CompanyName` / `LegalCopyright`**, which name Snapmaker on our binaries.
   *0.5 h.*
5. **Reconcile the URL schemes** - the installer registers `snapmaker-ultra`, the runtime registers
   `Snapmaker_Orca` and `snapmaker-orca` in HKCU (§2.4). Pick one. *0.5 d.*

**These five are independently shippable and reduce real exposure without any naming decision.**

### Phase 2 - Identity and assets (3-4 d)

1. **New logo and icon family.** 23 raster/vector files (§2.7) plus `splash_logo{,_dark}.svg`,
   `splash_app_icon.svg`, `studio_logo.svg`. Needs a designer or a competent generative pass, then
   `.ico` / `.icns` / maskable-PNG derivation at every size. *2 d, mostly not engineering.*
2. **Rename the string constants** - the single header from phase 1. *0.5 d.*
3. **Rename the CMake targets and outputs** (§2.2), the source files (`git mv`, 8 files), the
   `.rc.in` / `.manifest.in` / `Info.plist.in` templates, the `.desktop` file. *1 d.*
4. **Add the new G-code / 3MF producer strings** to `GCodeProcessor::Producers` and
   `Config.cpp:1398-1399`, **keeping every existing entry**. *0.5 h.*
5. **Rename the 21 localisation files**, regenerate the `.pot`, `msgmerge` the 20 `.po` files, update
   the four CMake glob sites and the CI workflow (§2.10). *0.5 d + translator churn.*
6. **Hub, tray and web strings** (§2.8) - including the PWA `name` and `short_name` at
   `RemoteHub.cpp:2297-2298`, which is the one a phone user actually sees. *0.5 d.*

### Phase 3 - The data-directory migration (2-3 d) — the delicate one

**Requirement: a user who upgrades in place loses nothing and is not asked anything they cannot
answer.**

**Recommended shape: copy, not move; once; with a marker.**

On first start under the new key, if the new data dir does not exist and the old sibling does
(`bfs::path(data_dir()).parent_path() / "Snapmaker_Orca"` - the `PresetMirror.cpp:67-92` pattern,
§2.3):

1. **Copy** the whole tree. Write `migrated_from` into the new `.conf` and a
   `.migrated-to-<newkey>` marker into the **old** dir so a downgrade can tell what happened.
2. **Rewrite** `Snapmaker_Orca.conf` → `<NewKey>.conf`, and rewrite the **14 lines** carrying
   `...\Roaming\Snapmaker_Orca\...` to the new path (§2.3).
3. **Show one dialog afterwards**, not before: what moved, where the old copy still is, and that it
   is safe to delete. Never delete the old directory automatically.

**Copy, not move, because:**

| | Copy | Move |
|---|---|---|
| Old build still works | **yes** - the real rollback | no |
| Disk cost | **+603 MB** (432 MB of it `hub/`) | none |
| Stale absolute paths | resolve, to the old copy | dangle |
| User can compare | yes | no |

**Offer to skip the bulk.** `hub/saves/` and `hub/uploads/` are 432 MB of G-code archive and by far
the largest cost. Offer "copy settings only" vs "copy everything"; **`hub/settings.json` is never
optional.**

**Both directions.** A user who installs the new build, dislikes it and reinstalls the old one finds
their old data dir untouched - that is the whole point of copy-not-move. But **changes made in the
new build do not flow back**. Say so in the dialog. Do not attempt a two-way sync.

**What breaks if this phase is done badly** (§2.3): every phone bookmark 404s, every Web Push
subscription dies **silently**, the Pushover/ntfy/webhook destinations vanish, the tailnet allow-list
empties so remote access stops, every camera and LAN printer must be re-added, and the Reprints
archive empties. **`hub/settings.json` alone accounts for four of those seven.**

**Gate script** (`test_rebrand_migration.py`, following the `snorca_hubtest` convention): build a
synthetic old data dir with a known token, a known VAPID pair, three LAN printers, two cameras and a
saves entry; run the new build against a fresh parent; assert the new dir exists, the token and VAPID
key are byte-identical, the printer/camera/saves counts match, the `.conf` was renamed, the 14
absolute paths were rewritten, the old dir is untouched, and a **second** start does not re-migrate.

### Phase 4 - Installers and platform identity (3-4 d)

| Task | Effort | Note |
|---|---|---|
| Windows: CPack name, registry key, shortcut, URL scheme, preinstall guard (§2.2, §2.4) | 1 d | keep the guard's legacy `Snapmaker_Orca.exe` check |
| Windows upgrade path from `Snapmaker-Ultra` | 0.5 d | the preinstall guard reads `...\Uninstall\Snapmaker-Ultra`; it must read the **old** key, offer the uninstall, then install under the new one |
| Windows: **new WiX/upgrade GUID** | 0.5 h | a new product needs a new GUID |
| macOS bundle id `com.snapmaker.snapmaker-orca` → ours | 0.5 d | **one-way**; keychain, TCC grants and notarisation history all reset |
| macOS: `.icns`, `CFBundleURLName`, `CFBundleURLSchemes`, copyright strings | 0.5 d | |
| Flatpak id `io.github.Snapmaker.Snapmaker_Orca` → `io.github.<account>.<Name>` | 1 d | **hard break**: new `~/.var/app/<id>/` sandbox home, installs alongside not over |
| Flatpak: rename both files, metainfo `<name>`/`<developer_name>`/URLs, D-Bus talk-name | 0.5 d | |
| AppImage name, FHS resources dir, icon install, pixbuf cache dir | 0.5 d | |
| File-association ProgID (and fixing the leading space in `" Orca.Slicer.1"`) | 0.5 d | re-associates files; needs its own migration note |

**Flatpak deserves a callout.** Because the app-id changes, the Flatpak sandbox home changes too, and
the phase-3 migration **will not find the old data** unless the new manifest grants read access to
the old `~/.var/app/io.github.Snapmaker.Snapmaker_Orca/` path. Add that filesystem permission for one
release, then drop it.

### Phase 5 - Repo, docs and release (1-2 d)

1. **Rename the GitHub repo.** GitHub redirects the old URL - web, git, and API - **until someone
   creates a new repo with the old name**. That is the failure mode to watch. Keep `aceRage` as the
   owner so the redirect is under our control.
2. **Update the 11 `github.com/aceRage/Snapmaker-Ultra` occurrences**, `CPACK_PACKAGE_HOMEPAGE_URL`
   (which currently points at *Snapmaker's* repo), the flatpak metainfo URLs, and the issue
   templates.
3. **Rewrite `README.md`** - new name, the §3.1(c) descriptive strapline, and **the attribution
   paragraph at `README.md:131` kept intact** with our own name substituted for ours only.
4. **Update `AGENTS.md`, `CLAUDE.md`, `SECURITY.md`, `doc/`, `docs/`** and the build scripts.
5. **One release that does nothing else.** The rebrand release ships the rename and the migration and
   no features, so that any breakage has exactly one cause.
6. **A pinned migration note** covering: the new data dir, the phone link and Web Push re-subscribe
   if migration was skipped, the Flatpak side-by-side install, and the macOS "this is a new app to
   the system" consequences.

### Effort summary

| Phase | Days |
|---|---|
| 0 Decide | 0.5 + external |
| 1 One source of truth, free wins | 1-1.5 |
| 2 Identity and assets | 3-4 |
| 3 Data-dir migration | 2-3 |
| 4 Installers and platform identity | 3-4 |
| 5 Repo, docs, release | 1-2 |
| **Total** | **11-15 d** + legal + design + translator churn |

---

## 5. Risks

### 5.1 R1 - The User-Agent, which no rename fixes (**the top risk**)

`src/slic3r/GUI/Widgets/WebView.cpp:274-281`, reached from
`src/slic3r/GUI/WebUserLoginDialog.cpp:95-98`:

```cpp
// Ultra P4: bambulab.com/sign-in version-gates its login flavor on the BBL-Slicer
wxString ua_ver = (brand_tag == "BBL-Slicer") ? wxString("02.03.00.01") : wxString(SLIC3R_VERSION);
```

and `src/slic3r/GUI/Widgets/WebView.hpp:9-12`:

```
// The Bambu account login must use "BBL-Slicer" or bambulab.com's sign-in won't run the slicer
// login flow (it just redirects to the marketing home and never posts the token back).
```

**The fork identifies itself to Bambu Lab's authentication servers as `BBL-Slicer/02.03.00.01`, i.e.
as Bambu Studio 2.3.0.1.** Per the reporting in §1.5, that is the *specific* behaviour Bambu Lab
narrowed its April 2026 objection down to: the fork *"quietly introduces itself as official Bambu
Studio"*. The developer in that episode had the same justification we do - the flow does not work
otherwise - and the repository came down anyway.

**A rename does not touch this.** A newly-renamed product still sending `BBL-Slicer` to
bambulab.com is arguably *worse*, because the mismatch between what we call ourselves and what we
tell their servers becomes deliberate rather than inherited.

**This needs its own decision, ahead of the naming one.** The options are roughly: send our own
User-Agent and accept that Bambu account login breaks; keep it and accept the exposure; or drop the
Bambu cloud login and keep only the LAN path, which is the part users actually rely on. **Out of
scope for this document to decide - in scope to refuse to leave unsaid.**

The `SM-Slicer` tag (`WebSMUserLoginDialog.cpp:67`, `WebView.cpp:302,407`) is the milder version of
the same question against Snapmaker's servers, where at least the code is genuinely Snapmaker-derived.

### 5.2 R2 - User confusion and lost discoverability

- Existing users search for "Snapmaker Ultra" and find nothing. Mitigation: the GitHub redirect
  (phase 5.1), a pinned release note, and the §3.1(c) strapline carrying the searchable words.
- **The name is the *only* thing that made the fork findable.** "Snapmaker Orca fork" is how someone
  discovers this project. A neutral name loses that, permanently, and the strapline only partly
  recovers it. **This is the genuine cost of the recommendation and it should not be minimised.**
- A user with both the official Snapmaker Orca and ours installed currently distinguishes them by the
  splash saying "Ultra version" and by two identical icons. After the rebrand they are visibly
  different products - **an improvement**, and the strongest user-facing argument for doing it.

### 5.3 R3 - Upstream merge friction

**282 files carry the brand; 141 of them are currently byte-identical to `snapmaker-upstream/main`.**
Renaming converts those 141 into permanently-diverged files. Where they land:

| Area | Files |
|---|---|
| `src/slic3r/` | 40 |
| `localization/i18n/` | 21 |
| `src/libslic3r/` | 8 |
| `src/dev-utils/` | 6 |
| `resources/web/` | 5 |
| `.github/workflows/` | 4 |
| everything else | 57 |

The fork already diverges in 4 753 files from the merge base (367 of them under `src/`), so **the
rename increases the diverged-file count by roughly 3 %** - but it does so in a *concentrated* way, in
`src/slic3r/GUI/` and `src/libslic3r/`, which is exactly where upstream changes land.

**Mitigations:**
- Phase 1's single-source-of-truth header means most C++ sites reference a constant rather than a
  literal, so a future upstream change to those files merges cleanly.
- Localisation conflicts are mechanical - regenerate rather than merge.
- Do the rename in **one commit per category** (strings, targets, files, localisation, packaging) so
  a future `git log --follow` and any `rerere` cache stay useful.
- Accept that `AboutDialog.cpp`, `GUI_App.cpp`, `MainFrame.cpp`, `Plater.cpp` and `RemoteHub.cpp`
  become slightly harder to merge. They are already the most-forked files in the tree.

### 5.4 R4 - Migration failure

Covered in §4.4. The specific silent failure - **Web Push dying with no error** because the VAPID
pair did not come across - is the one that will not be reported as a bug, it will be reported as
"notifications just stopped". The gate script must assert VAPID byte-equality.

### 5.5 R5 - AGPL attribution must survive

The rename must not touch: `AboutDialog.cpp:281-294` (the full upstream chain), the AGPL-3.0 statement
at `AboutDialog.cpp:294` and `CopyrightsDialog` at `:151-156`, `LICENSE.txt`, and `README.md:131`.
Only *our* product name changes inside those sentences. **Additionally**, phase 5 should add the §13
network-source offer to `hub.html` and `stream_center.html` (§1.1), which is missing today - a
one-line footer with a link to the repo.

### 5.6 R6 - Doing half of it

The current state - package `Snapmaker-Ultra`, application "Snapmaker Orca", hub page "Ultra Hub",
PWA manifest "Snapmaker Orca"/"Orca", icon = Snapmaker's logo - **is the worst configuration**,
because it has the costs of both and the benefits of neither. If the answer is "not now", the honest
version of "not now" is to do **phase 1 only** (§4.2) and leave the name alone, rather than to
continue drifting.

---

## 6. Open questions for the user

1. **The Bambu User-Agent (§5.1).** Keep it, change it, or drop the Bambu cloud login? This is the
   largest exposure in the repo and it is orthogonal to the name. **Decide this first.**
2. **Is the companion app actually happening?** The app plan's own R8 asks whether *"the honest
   answer is that Pushover plus the installed web app is enough."* If the app is dropped, option (a)
   becomes arguable - but the icon, the bundle id, the Flatpak id and the `CompanyName` strings are
   still wrong, and phase 1 is still worth doing.
3. **Which name**, and is there budget for a real clearance search (§3.4)? Clear two, not one.
4. **Who draws the logo?** Phase 2 is 2 days of design work this document cannot do.
5. **Copy or move for the data dir**, and is the 603 MB duplicate acceptable? The recommendation is
   copy, with an option to skip the 432 MB archive.
6. **Do we keep "Ultra" internally?** The recommendation is yes - code comments, the preferences tab,
   the release-tag suffix - and no in the product name.
7. **Should the Flatpak carry a one-release read permission** on the old sandbox home so migration
   works there (§4.5)?
8. **Verification still owed** before anyone relies on §1: read Bambu Lab's Terms of Use manually
   (403 to automated fetch), and re-check the Snapmaker marks on tsdr.uspto.gov and the EUIPO
   register directly. **The trademark table in §1.2 is from search extracts, not the primary
   register.**
