# Ultra1: a companion app for iOS and Android (2026-09-04)

**The ask.** A real app - one the person installs from an icon, not from a Share sheet - that
receives print notifications by itself, plays the cameras, drives the printers, and eventually hosts
the Reprints list. The user's words: *"long term we may need to create our own app that can just
directly receive the notifications for itself."*

**What this document is.** The requirements, the architecture options with a recommendation, the API
the app would use, an MVP, a phased plan with effort, the costs, and the risks. **Documentation
only - no app code is written on this branch.**

Branch `docs/ultra1-app-plan`, cut from `feat/ultra-preferences` at `eadc7ec8ed`. Codebase facts
carry `file:line` anchors against that commit. Platform facts carry a URL and the date they were
read; anything that could not be confirmed from a primary source is marked **UNVERIFIED** and must
not be planned against.

**It reverses a recommendation.** [`2026-09-03-phone-mobile-capabilities-research.md`
§1.4](2026-09-03-phone-mobile-capabilities-research.md) rejected both a native app (option D) and a
thin native shell (option E), on the grounds that *"everything valuable the phone does here -
cameras, Prepare, Send, Devices - is already a web page that the hub serves and that the fork must
keep working anyway. A native app would be a second front end for the same API, and its only real
advantage (push without the home-screen gate on iOS) is available to the web app once it is
installed to the home screen."* That was correct when the choice was *app instead of page*.
Section 3 recommends a shape where it is not a second front end at all - the page stays the front
end and the app supplies exactly the four things a page structurally cannot have - and section 6
keeps the original objection alive as the top maintenance risk.

---

## 0. Where things stand today (the ground the app is built on)

**The hub.** `snapmaker-orca.exe --hub` (`src/slic3r/GUI/RemoteHub.cpp`, ~2750 lines) is a tray
process with **two** listeners: a main one on `0.0.0.0:13640` (first free of 13640-13659) serving
the phone plane, and an ephemeral **loopback-only** admin listener carrying `/hub/*` and
`/relay/h264` and nothing else (`RemoteHub.cpp:1493`, `:1534`, split enforced at `:2516-2517`). Any
peer that is not RFC1918, loopback or link-local is dropped before routing (`is_private_v4`,
`:154`, checked `:2491`).

**The phone plane.** Everything the phone touches is under `/r/<token>/`. The token is 14 symbols
from a 32-character confusable-free alphabet drawn from `std::random_device` with rejection sampling
(`random_token`, `:233-246`), compared in constant time (`ct_equal`, `:223`), stored in
`<datadir>/hub/settings.json` (not `hub.json`, which `shutdown()` deletes), and replaced only by
`POST /hub/newlink`. The last three retired tokens are remembered solely to answer a *"this link was
replaced"* 404 page (`:2461`, `:2584-2589`); they authenticate nothing.

**Remote access.** The hub drives the user's own Tailscale: `tailscale serve --bg --https=443
http://127.0.0.1:<port>` (`:1966`). Requests arriving that way are loopback peers carrying
`Tailscale-User-Login`, checked against an allow-list (`:2519-2527`). So a remote visitor needs
**both** an allow-listed tailnet login **and** the path token. Because Serve is pinned to the main
port and `/hub/*` lives on the admin port, Serve structurally cannot front the control plane.

**Notifications, as built.** `RemoteEvents` in each slicer instance diffs a 5-second printer
snapshot into typed events and POSTs them to `POST /hub/event`; `HubServer::accept_event`
(`:1403-1458`) normalises, numbers and rings them, then fans out to `RemoteNotify::deliver` (a
queue plus one worker thread) and `WebPush::deliver`. Three relay destination types shipped in P5 -
**ntfy**, **Pushover** and a generic **webhook**. In practice the user has settled on **Pushover**
as the interim relay because the ntfy iOS client is unmaintained (its own maintainer calls it
*"bare bones and quite frankly a little buggy"*, quoted in the capabilities research §2.3c).
**Web Push** shipped in P7: the hub owns a P-256 VAPID pair in `settings.json`, encrypts a ~200-byte
JSON payload per RFC 8291 (`aes128gcm`) with OpenSSL 1.1.1 primitives, and POSTs it to the opaque
subscription endpoint with an RFC 8292 `Authorization: vapid t=..., k=...` header
(`src/slic3r/GUI/WebPush.cpp:416-472`, `:601-619`).

**Two things about Web Push that the app exists to fix.**

1. **It only works on the Tailscale origin.** `PushManager` needs a secure context, so the plain
   `http://<lan-ip>:13640/r/<token>/` link cannot have it at all; the page hides the control there
   rather than offering a button that cannot work (`stream_center.html:1527`). A LAN-only user has
   no push today.
2. **On iOS it needs the home-screen dance.** `Notification` and `PushManager` do not exist in a
   Safari tab on iOS in *any* browser, because they are all WebKit; Web Push has worked only inside
   a Home Screen web app since iOS 16.4 ([webkit.org, 2023-02-16](https://webkit.org/blog/13878/web-push-for-web-apps-on-ios-and-ipados/),
   read 2026-09-04). The page says so at the point of failure, which is the best a page can do.

**The phone page.** `resources/web/orca/stream_center.html`, 3006 lines, one self-contained file,
three tabs - **Streams** (camera wall, go2rtc MSE in an iframe for RTSP/ONVIF/U1-relay, an MJPEG
`<img>` for Bambu P1/A1), **Prepare** (instances, file load, presets, plates, slice, send) and
**Devices** (push control, event feed with an unread badge, printer cards with Pause/Resume/Stop,
Snapmaker LAN add/remove). Everything is polling; there is no SSE and no page-level WebSocket. A
fourth tab, **Reprints**, is designed in [`2026-09-04-gcode-archive-design.md`
§6](2026-09-04-gcode-archive-design.md) and not yet built.

---

## 1. Goals and non-goals

### 1.1 What the app adds that the page cannot have

| # | Capability | Why the page cannot | What the app does |
|---|---|---|---|
| **G1** | **Push on every origin, with no install ritual** | Push needs a secure context and, on iOS, a Home Screen web app | APNs / FCM deliver to the *app*, not to an origin. Push works when the person is on the LAN link, on the tailnet link, or nowhere near either |
| **G2** | **Notifications whose content the push vendor cannot read** | Web Push already has this; **relays do not** - Pushover and ntfy see the whole message | The hub encrypts the payload to a key the app generated (the same RFC 8291 code path already in `WebPush.cpp`) and an iOS Notification Service Extension / an Android `FirebaseMessagingService` decrypts it on the device before it is shown |
| **G3** | **Background reliability** | A page is only alive when it is open; a service worker only wakes for a push | The app holds its own state, reconnects on foreground, refreshes on `BGAppRefreshTask` / `WorkManager`, and survives a hub that was unreachable |
| **G4** | **Offline history** | With the PC off the page is a blank screen and a "lost contact" line (`stream_center.html:415`) | The last known printer states, the event history and the Reprints list render from a local store with an explicit "as of 14:02, PC unreachable" line |
| **G5** | **One-tap pairing and automatic LAN/tailnet switching** | A bookmark is one URL; the person keeps two and picks by hand | Scan the QR the hub page already draws, store *both* URLs in one profile, race them on every launch, use whichever answers |
| **G6** | **Actions on the notification itself** | Web Push actions on iOS are limited and unauthenticated back to us | *Pause* / *Stop* buttons on the notification, executed by the app against `POST /api/printers/{id}/control` with the stored token |
| **G7** | **Widgets and Live Activities** | No web equivalent exists | A home-screen widget with each printer's percent and ETA; a Live Activity / Android Live Update for the running print |
| **G8** | **A place for Reprints to live well** | It will work in the page; it will work better with a native list, local thumbnails and a share sheet | The archive list cached on the device, thumbnails cached by immutable record id |

**The single strongest one is G1.** Everything else is comfort. G1 is the difference between "the
user gets a filament-runout alert" and "the user gets one only if their phone happens to be on the
tailnet origin and they performed a Share-sheet ritual that iOS gives no way to prompt for."

### 1.2 Non-goals

- **Not a replacement for the phone page.** The page is the front end for Streams, Prepare, Devices
  and Reprints, and stays so. If the app duplicated it, every hub feature would need shipping twice
  and the fork would slow down. Section 3 is chosen precisely to avoid that.
- **No cloud account, no sign-up, no server the project runs.** The app's identity is the phone
  token plus a random device id it generates on first launch. Nothing else.
- **No telemetry, no analytics, no crash reporting to a third party.**
- **No slicing on the phone**, no re-slice of an archived job, no print queue or scheduler (all
  explicitly out of scope in the archive design too).
- **No model files on the phone.** A reprint streams PC -> printer. A 400 MB G-code download over a
  Tailscale link is not a feature.
- **No VPN of our own.** iOS and Android run one VPN at a time
  ([tailscale.com FAQ](https://tailscale.com/docs/reference/faq/other-vpns), read 2026-09-04), and
  the user relies on Tailscale's. Remote access stays the Tailscale app's job.
- **No manufacturer branding.** The app is not called Snapmaker anything and does not carry
  Snapmaker's or Bambu's icon (App Store guidelines 4.1(c) and 5.2.1 - see §6.7).
- **Not, in the first year, a public App Store product.** §5 ships to the user's own devices. §5.9
  is the separate, expensive decision to publish.

---

## 2. Requirements

Grouped must / should / could. Each is written so a gate script or a hardware checklist item can say
yes or no. Gate scripts follow the shape of `snorca_hubtest\test_phone_webpush.py` (an *independent*
implementation to compare against, isolated data dir, no printer needed).

### 2.1 Must

| # | Requirement | How it is tested |
|---|---|---|
| **M1** | The app pairs from the QR the hub page already draws, with no typing. Scanning stores a profile holding the LAN URL, the tailnet URL when remote access is on, the hub's name and its version | Point the app at a mock hub serving `GET /r/<token>/pair`; assert the profile matches; assert a QR of the wrong shape is refused with a readable message |
| **M2** | On launch the app races the profile's URLs and uses the first that answers `GET /r/<token>/state` within 2 s; it never asks the person which network they are on | Mock hub reachable on one URL only, then the other, then neither; assert selection and the offline banner |
| **M3** | The app registers a push token with the hub at `POST /r/<token>/push/device`, and re-registers on every cold launch (tokens rotate and no platform reliably tells us) | Register, restart, assert one row not two (idempotent on token); assert the hub's device list shows it masked |
| **M4** | An event accepted by `HubServer::accept_event` reaches the app as a notification within 30 s with the app killed, on iOS and on Android | `POST /hub/apppush/test` against a mock APNs/FCM in the gate; on hardware, the checklist in §5.10 |
| **M5** | Push works when the phone is **not** on the LAN and **not** on the tailnet - i.e. over the phone's own cellular data with the PC unreachable | Hardware checklist. This is G1 and the whole reason for the app |
| **M6** | The notification's **content** is not readable by Apple or Google: the payload on the wire is `aes128gcm` ciphertext keyed to the device, decrypted on device before display | Gate compares the hub's ciphertext against an independent Python RFC 8291 implementation (`mock_push.py` already does this for Web Push); assert the cleartext appears nowhere in the HTTP body |
| **M7** | Tapping a notification opens the app on the right thing: the Devices tab of the right hub profile, the right printer card | UI test with a synthetic payload per `kind` |
| **M8** | Streams, Prepare, Devices and (later) Reprints are all reachable and fully functional in the app, at parity with the page in a browser | Manual parity checklist per tab; the page is unmodified, so parity is structural |
| **M9** | Cameras play: go2rtc MSE for RTSP/ONVIF/U1-relay hosts, MJPEG for Bambu P1/A1, on both platforms | Hardware checklist against the user's H2D, X1C, P1/A1 and three U1s |
| **M10** | Nothing but the phone token leaves the LAN. No account, no server run by the project, no analytics, no third-party SDK that phones home | Static check of the app's network destinations; the only outbound hosts are the hub's two URLs and the platform's own push registration |
| **M11** | Push credentials (the APNs `.p8`, the FCM service account JSON) never leave the PC, never appear in any response under `/r/<token>/`, and are masked to `****`+last-4 on `/hub/*` exactly as `RemoteNotify::masked_json` already does | Gate asserts the key bytes appear in no response body on either listener; asserts a masked value posted back keeps the stored one (`take_secret`, `RemoteNotify.cpp:106`) |
| **M12** | A device token that the platform reports as dead is pruned, and a live one is never pruned by a transient failure | Gate: APNs `410 Unregistered` and FCM `UNREGISTERED`/`NOT_FOUND` prune; `503` and `429` retry then keep - the same rules `WebPush.cpp:627-636` already applies to `404`/`410` |
| **M13** | Rotating the phone token (`POST /hub/newlink`) does not leave the app silently broken: it detects the replaced-link 404 and prompts to re-pair | Gate rotates the token and asserts the app-facing route answers the replaced-link page; UI test asserts the prompt |
| **M14** | The app is useful with the PC off: last known printer states, the event history and an explicit staleness line | UI test with the mock hub stopped |
| **M15** | Every new hub route added for the app is on the phone plane under `/r/<token>/` or on the loopback admin plane under `/hub/*`, with the same gates as its neighbours, and every new *instance* route is added to **both** `RemoteAccess::handle_api`'s manifest and `instance_api_allowed` | `test_hardening.py` already diffs those two lists and fails when they drift |

### 2.2 Should

| # | Requirement | How it is tested |
|---|---|---|
| **S1** | Notification actions: *Pause* and *Stop* on a `runout` / `error` / `paused` notification, executed against `POST /api/printers/{id}/control` with the same `confirm=1`-for-stop rule the page uses | UI test firing each action against a mock instance; assert `stop` still requires an explicit confirm step |
| **S2** | A single `GET /r/<token>/summary` call gives every printer's state, percent and ETA, so a widget or a background refresh is one request rather than a fan-out across instances | Gate asserts the shape and that it needs no instance to be open for LAN Snapmakers |
| **S3** | The app supports more than one hub profile (a second PC) and shows which one a notification came from | UI test with two profiles and two device registrations |
| **S4** | Android can receive push **without Google Play Services**, via UnifiedPush, using the hub's existing Web Push sender unchanged | Gate registers a UnifiedPush-shaped endpoint through the existing `POST /r/<token>/push/subscription` and asserts the same `aes128gcm` body is sent |
| **S5** | Background refresh keeps the widget and the summary within 15 minutes of the truth when the phone can reach the hub | Instrumented test on device |
| **S6** | The app degrades honestly when the platform's push permission is denied: it says push is off, offers to open Settings, and falls back to foreground polling | UI test with permission denied |
| **S7** | The event history is de-duplicated against the hub's monotonic `id`, and a hub on a fresh data dir (ids restart at 1) does not hide events for ever - the same rule `stream_center.html:1396-1399` already applies | Gate replays an id reset |
| **S8** | Reprints: the archive list and its thumbnails are cached locally and readable offline; a reprint is initiated through the instance API, never by downloading the file to the phone | Gate against the archive Stage 2 API once it exists |

### 2.3 Could

| # | Requirement | How it is tested |
|---|---|---|
| **C1** | Live Activity (iOS) / Live Update (Android 16 `ProgressStyle`) for the running print, updated by push | Hardware; note the iOS 8-hour ceiling in §6.6 |
| **C2** | Home-screen widgets showing percent and ETA per printer | Hardware |
| **C3** | Native WebRTC camera playback against go2rtc's `/api/webrtc`, instead of MSE in the WebView | Latency comparison against the MSE path; the unmerged `feat/webrtc-video` branch already has the hub side and a 33-check gate |
| **C4** | Universal Links / App Links so the tailnet URL opens the app instead of Safari - the hub can serve `apple-app-site-association` and `assetlinks.json` over the Tailscale HTTPS origin | Gate asserts the files are served with the right content type and only on the remote origin |
| **C5** | A share extension: send a `.3mf`/`.stl` from Files or a browser straight into `POST /r/<token>/api/instances/open`, which already exists and already takes a raw body with `X-File-Name` | UI test |
| **C6** | Apple Watch complication / Wear tile | - |
| **C7** | Local network discovery of the hub by mDNS, so pairing works without a QR | Would need the hub to advertise a Bonjour service it does not advertise today, and would trip the iOS local-network prompt (§6.4). Low value next to a QR that already exists |

---

## 3. Architecture options

Five candidates, judged on six axes that actually decide this: push, video, iOS local-network
permission, Tailscale coexistence, build and distribution cost, and the standing objection that a
second front end is a permanent burden.

### 3.1 The axes, established once

**Push - the fixed points.**

- A native iOS app receives notifications through **APNs device tokens** only. *"At launch time,
  your app communicates with APNs and receives its device token, which you then forward to your
  provider server"*
  ([Apple, Registering your app with APNs](https://developer.apple.com/documentation/usernotifications/registering-your-app-with-apns),
  read 2026-09-04). Web Push subscriptions are minted by `PushManager.subscribe()` and delivered to
  a Service Worker; Apple documents Web Push for *"Home Screen web apps in iOS 16.4 or later and
  Webpages in Safari 16"*
  ([Apple, Sending web push notifications in web apps and browsers](https://developer.apple.com/documentation/usernotifications/sending-web-push-notifications-in-web-apps-and-browsers),
  read 2026-09-04). **The two mechanisms are disjoint: a native iOS app cannot consume a
  `web.push.apple.com` endpoint.** Apple never says this in one sentence, so treat the negative as
  inferred rather than quoted - but the documented mechanisms admit no path.
- APNs provider auth, the modern form: a **`.p8` text file**, a 10-character **Key ID**, a
  10-character **Team ID**, and an **ES256** JWT - *"APNs supports only the ES256 algorithm"*. The
  `iat` *"must be no more than one hour from the current time"*, and Apple asks you to *"Refresh
  your token no more than once every 20 minutes and no less than once every 60 minutes"*
  ([Apple, Establishing a token-based connection to APNs](https://developer.apple.com/documentation/usernotifications/establishing-a-token-based-connection-to-apns),
  read 2026-09-04). *"You can use one token to distribute notifications for all or a subset of your
  company's apps"*; team-scoped keys are limited to *"a maximum of two keys"* per environment.
- APNs delivery is **HTTP/2** to `https://api.push.apple.com:443` (or `api.sandbox.push.apple.com`,
  or port 2197), path `/3/device/<device_token>`, with required `apns-topic` and `apns-push-type`
  headers and optional `apns-priority`, `apns-expiration`, `apns-collapse-id` (*"must not exceed 64
  bytes"*). Payload limit **4 KB / 4096 bytes**
  ([Apple, Sending notification requests to APNs](https://developer.apple.com/documentation/usernotifications/sending-notification-requests-to-apns),
  read 2026-09-04). Our encrypted event is ~250 bytes base64, so the cap is not a constraint.
- **FCM HTTP v1** is `POST https://fcm.googleapis.com/v1/projects/{projectId}/messages:send` with
  an OAuth 2.0 bearer token minted from a **service-account JSON key**, scope
  `https://www.googleapis.com/auth/firebase.messaging`
  ([Firebase, Authorize send requests](https://firebase.google.com/docs/cloud-messaging/auth-server),
  read 2026-09-04). A **Firebase project is required** - the project id is in the URL. The legacy
  server-key API *"was deprecated on June 20, 2023"* with a migration window ending 06/21/2024 and
  *"decommissioning of the APIs in June 2024"*
  ([Firebase FAQ](https://firebase.google.com/support/faq), read 2026-09-04; the migration guide at
  `firebase.google.com/docs/cloud-messaging/migrate-v1` returns **HTTP 404** as of that date, so the
  often-quoted "July 22, 2024" shutdown date is **UNVERIFIED**).
- FCM on the device *"require[s] devices running Android 6.0 or higher that also have the Google
  Play Store app installed"*, but *"you are not limited to deploying your Android apps through
  Google Play Store"*
  ([Firebase, Set up an Android client](https://firebase.google.com/docs/cloud-messaging/android/client),
  read 2026-09-04). So a sideloaded APK receives FCM fine on a normal phone, and not at all on a
  de-Googled one.
- **UnifiedPush** is the de-Googled answer, and it is unusually well aligned with what this hub
  already sends: its Android spec (AND_3.1.0) makes the endpoint *"the URL of the push resource as
  defined by RFC8030"*, requires the body to be *"an encrypted content that follows RFC8291"* with
  *"header must be `aes128gcm`"*, and supports VAPID (RFC 8292)
  ([unifiedpush.org spec](https://unifiedpush.org/developers/spec/android/) and
  [intro](https://unifiedpush.org/developers/intro/), read 2026-09-04). That is byte-for-byte what
  `WebPush.cpp` already emits. **There is no iOS support** - iOS appears nowhere on unifiedpush.org.
  And no primary page promises drop-in compatibility with an arbitrary Web Push sender, so treat
  "our sender works unchanged" as design intent to be proven by the gate, not a guarantee.

**Video.** go2rtc 1.9.14 (bundled at `resources/tools/go2rtc/go2rtc.exe`) can output **WebRTC, MP4/
MSE, HLS, MJPEG, RTSP, RTMP** and ranks latency *WebRTC "best", MSE "medium", HLS "bad"*
([go2rtc README](https://github.com/AlexxIT/go2rtc), read 2026-09-04). The hub serves only
`/stream.html`, `video-stream.js`, `video-rtc.js` and the `/api/ws` tunnel today, and both call
sites force `mode=mse` (`RemoteHub.cpp:78-80`; `stream_center.html:598`). A WebRTC branch
(`feat/webrtc-video`) is complete with a 33-check gate and **kept unmerged by decision** - the
remote-access design records it as *"same bytes as MSE, about 0.4 s slower start from the PC."*
iPhone Safari has no `MediaSource`, only `ManagedMediaSource`, which the bundled go2rtc player
already handles; and **iOS suspends WebRTC and WebSockets when Safari is backgrounded or the screen
locks**, so any player must re-signal on `visibilitychange` (remote-access design, line 5). Bambu
P1/A1 is a plain MJPEG `<img>` through `BambuCamRelay`; the Snapmaker U1 is raw H.264 from
`/webcam/stream.h264` re-served from the first SPS into go2rtc (`relay_h264`, `RemoteHub.cpp:909`).

**iOS local network privacy - the axis that decides more than it looks like it should.** The
permission arrived in **iOS 14** (and macOS 15), and the operations that trigger it are *"outgoing
TCP connection, UDP unicast send, UDP multicast send, UDP broadcast send"* - so a plain
`http://192.168.1.50:13640/...` from native networking code prompts on first use. But the technote
lists explicit exceptions, and the first one is decisive: **traffic from `WKWebView`,
`SFSafariViewController` and Safari does not require local network access**
([Apple TN3179, revision 2026-02-17](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy),
read 2026-09-04). `NSLocalNetworkUsageDescription` is required for native access;
`NSBonjourServices` additionally for Bonjour.

Separately - and this is a live trap - **App Transport Security stopped allowing IP-address
connections by default in iOS 17**: *"In iOS 17, iPadOS 17, and macOS 14, ATS no longer allows
connections to IP addresses by default. Add individual IP addresses and classless inter-domain
routing (CIDR) ranges in the `NSExceptionDomains`"*
([Apple, NSAllowsLocalNetworking](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking),
read 2026-09-04). `NSAllowsLocalNetworking` alone is not the right lever for `192.168.1.50` any
more; Apple's DTS engineer's 2017 answer that the key *"has no effect on IP address loads"*
([developer.apple.com/forums/thread/66417](https://developer.apple.com/forums/thread/66417), post
June 2017, read 2026-09-04) predates the change and describes the iOS 10-16 era when such loads were
simply allowed. **Both facts point the same way: over the tailnet URL neither problem exists** - it
is HTTPS to a public DNS name through a system VPN - which is another reason the app should prefer
the remote URL when it is available.

**Tailscale coexistence.** The Tailscale apps install a VPN configuration
([tailscale.com/kb/1020](https://tailscale.com/kb/1020/install-ios), read 2026-09-04), and *"iOS and
Android enforce a limit of running only one VPN at a time"*
([Tailscale FAQ, using other VPNs](https://tailscale.com/docs/reference/faq/other-vpns), read
2026-09-04). That limit is about *other VPN apps*: an ordinary app coexists with Tailscale fine,
because Tailscale is a system-wide tunnel and any app's traffic to a `100.x` address or a MagicDNS
name routes through it. What we must not do is ship our own NetworkExtension. Embedding Tailscale is
not an option either: `tsnet` *"is a library that lets you embed Tailscale inside a **Go**
program"* ([Tailscale, tsnet](https://tailscale.com/docs/features/tsnet), last validated 2026-07-24,
read 2026-09-04) and there is no official Swift or Kotlin SDK. **Ultra1 depends on the Tailscale app
for remote access, exactly as the web page does today.** MagicDNS name resolution for third-party
apps on iOS is **UNVERIFIED**; prefer storing the `100.x` address alongside the name.

**Build and distribution cost.**

- Apple Developer Program: **"99 USD per membership year"**
  ([Apple](https://developer.apple.com/programs/whats-included/), read 2026-09-04). The Enterprise
  Program is **"299 USD per membership year"** and requires *"100 or more employees"* and in-house-
  only distribution ([Apple](https://developer.apple.com/programs/enterprise/), read 2026-09-04) -
  not applicable.
- A **free "Personal Team"** registers *"up to 10 App IDs, which expire after 7 days"*, *"up to 3
  devices"*, and provisioning profiles that *"expire 7 days from issuance"*
  ([Apple, compare memberships](https://developer.apple.com/support/compare-memberships/), read
  2026-09-04). App Store Connect and TestFlight are marked unavailable to free registrants on the
  same page. Whether a Personal Team can use the Push Notifications capability at all is **not
  stated by Apple anywhere I could find** - the comparison table has no row for it - so the
  universally repeated "no" is **UNVERIFIED from a primary source**. Plan on the paid program.
- TestFlight: **100 internal testers**, **10,000 external testers**, and *"You can test a build for
  up to 90 days"*; external testing requires Beta App Review on the first build (*"A review is
  required only for the first build"*), internal testing does not
  ([Apple, TestFlight overview](https://developer.apple.com/help/app-store-connect/test-a-beta-version/testflight-overview)
  and [add internal testers](https://developer.apple.com/help/app-store-connect/test-a-beta-version/add-internal-testers),
  read 2026-09-04).
- Google Play: *"There is a US$25 one-time registration fee"*
  ([Google](https://support.google.com/googleplay/android-developer/answer/6112435), read
  2026-09-04). Personal accounts created after 2023-11-13 must run closed testing with a *"minimum
  of 12 testers who have been opted in continuously for at least 14 days"* before applying for
  production ([Google](https://support.google.com/googleplay/android-developer/answer/14151465),
  read 2026-09-04). **Sideloading an APK avoids all of this** and still receives FCM.
- Expo's push service itself is free (*"There is no cost associated with sending notifications
  through Expo push notification service"*, 600/s per project) but *"A paid Apple Developer Account
  is required to generate credentials"*
  ([Expo push FAQ](https://docs.expo.dev/push-notifications/faq/) and
  [setup](https://docs.expo.dev/push-notifications/push-notifications-setup/), read 2026-09-04). EAS
  Build: Free $0/mo with "15 Android and 15 iOS builds" (period **UNVERIFIED**), Starter $19/mo,
  Production $199/mo ([expo.dev/pricing](https://expo.dev/pricing), read 2026-09-04).

### 3.2 The five options

#### A. Thin native shell around the existing page (Capacitor, or a hand-written WKWebView/WebView)

The app is a chrome-less browser pinned to one hub, plus a native layer that owns push, pairing,
storage and notification handling. The page inside is `stream_center.html`, unmodified.

- **Push.** `@capacitor/push-notifications` covers APNs and FCM; on Android it needs a Firebase
  project and `google-services.json`; on Android 13+ it uses the runtime permission. Documented
  limitation: *"iOS does not support silent push notifications"* in that plugin, which *"recommends
  using native code solutions for handling these types of notifications"*
  ([Capacitor docs](https://capacitorjs.com/docs/apis/push-notifications), read 2026-09-04). We do
  not need silent push - our notifications are user-visible by design - but the Notification Service
  Extension for M6 is native code either way, so a Capacitor project will carry a small Swift target
  and a small Kotlin service.
- **Video.** Free. The page's MSE-in-an-iframe path already works in WebKit, which is what WKWebView
  is.
- **iOS local network.** The best of the five: **WKWebView traffic is exempt from the local-network
  prompt** (TN3179). The LAN URL works with no permission dialog. Native calls we add (the summary
  route for a widget) do prompt - so keep the native networking surface small and prefer the tailnet
  URL for it.
- **Cleartext to the LAN.** `server.url` + `server.cleartext` is how Capacitor points a build at an
  external URL, and its own docs frame that as a live-reload feature and warn against it in
  production ([Capacitor config](https://capacitorjs.com/docs/config), read 2026-09-04). We are not
  shipping a fixed `server.url` - the URL is per-user and per-pairing - so this is a hand-written
  `WKWebView.load` / `WebView.loadUrl` rather than the config key, plus an ATS `NSExceptionDomains`
  entry covering the private ranges (§6.4).
- **Cost.** Weeks. One codebase, one UI, and every future hub feature (Reprints, a fifth tab, a new
  control) arrives with a page update and **no app release**.
- **Risk.** App Store guideline **4.2 Minimum Functionality**: *"Your app should include features,
  content, and UI that elevate it beyond a repackaged website"*
  ([App Store Review Guidelines](https://developer.apple.com/app-store/review/guidelines/), read
  2026-09-04). A pure wrapper would be refused. This matters only if we submit (§5.9), and the
  native layer of §3.3 is exactly the answer.

#### B. React Native

Latest stable **0.87** ([reactnative.dev/versions](https://reactnative.dev/versions), read
2026-09-04). Push is well trodden: `@react-native-firebase/messaging` covers FCM and iOS-via-APNs
(and requires the New Architecture from v26,
[rnfirebase.io](https://rnfirebase.io/messaging/usage), read 2026-09-04); **Notifee** is the display
layer and is explicit that *"Notifee is a local notifications library and does not integrate with
any 3rd party messaging services"* ([notifee.app](https://notifee.app/react-native/docs/overview),
read 2026-09-04) - it renders, FCM/APNs delivers. Video: `react-native-webrtc` (Android/iOS/tvOS,
bundling WebRTC M124, [GitHub](https://github.com/react-native-webrtc/react-native-webrtc), read
2026-09-04). **Cost: you rewrite 3006 lines of working UI, and re-write each new hub feature.** The
web page still has to keep working for anyone without the app, so the UI is now maintained twice.

#### C. Flutter

Latest stable **3.47.0** ([docs.flutter.dev](https://docs.flutter.dev/release/release-notes), page
last updated 2026-08-12, read 2026-09-04). `firebase_messaging` 16.6.0 and `flutter_webrtc` 1.6.1
are both current and broad ([pub.dev](https://pub.dev/packages/firebase_messaging),
[pub.dev](https://pub.dev/packages/flutter_webrtc), read 2026-09-04). There is a curious argument in
its favour here: the fork already *ships* a Flutter bundle - the Snapmaker Device page - so Flutter
is already in the product. But the fork does not *author* it, so no skill is actually reused. Same
fatal cost as B: a second UI, in a third language, for the same API.

#### D. Kotlin Multiplatform (+ Compose Multiplatform)

Android, iOS, desktop and server targets are all **Stable**
([kotlinlang.org](https://kotlinlang.org/docs/multiplatform/supported-platforms.html), read
2026-09-04); Compose Multiplatform for iOS reached Stable in 1.8.0 (May 2025 - **search-sourced,
body UNVERIFIED**). The most attractive *engineering* option: one shared model layer, native UI
where it matters, the smallest runtime. Also the smallest talent pool, the most build machinery, and
still a second front end.

#### E. Native SwiftUI + Jetpack Compose

Best platform fidelity by definition - Live Activities, widgets, App Intents, local-network handling
and background modes all first-class, no plugin between us and the platform. Also the highest
permanent cost: every screen twice, for ever, on a fork maintained by one person alongside a 500k-
line C++ slicer.

### 3.3 Recommendation: **A, with a real native layer** - "shell-first, native where it matters"

**Build Ultra1 as a thin native shell (option A) whose native side owns pairing, push, storage,
notifications, widgets and offline state, and whose main surface is the hub's own page.** Write the
shell as two small hand-rolled native projects (Swift + Kotlin) rather than adopting Capacitor
wholesale, unless a Capacitor scaffold measurably shortens phase 2 - the parts we actually need from
Capacitor are a WebView and a push plugin, and both are ~200 lines of platform code we will have to
touch anyway for the Notification Service Extension.

**Six reasons, in the order they matter.**

1. **The four things the person actually wants are all native-layer things, and none of them is a
   screen.** Push on any origin (G1), unreadable-by-vendor payloads (G2), background and offline
   behaviour (G3, G4), widgets and Live Activities (G7). Every one lives outside the WebView. The
   screens - Streams, Prepare, Devices, Reprints - are already written, already tested, and already
   have to keep working for browser users.
2. **It answers the standing objection instead of ignoring it.** The capabilities research rejected
   an app because it would be *"a second front end for the same API."* This shape is not a second
   front end. There is exactly one front end and it gains a host.
3. **WKWebView is exempt from the iOS local-network prompt** (TN3179). The LAN link - the one the
   user is on most of the time - just works, with no permission dialog and no explanation to write.
   Every other option puts native networking on the LAN and buys the prompt on first launch.
4. **New hub features ship without an app release.** This fork lands a feature most weeks. Reprints
   is designed but unbuilt; the archive's Stage 2 API is not final. Under B/C/D/E, every one of
   those becomes an app release, a store review (if published), and a version-skew matrix between
   app and hub. Under A it is a file copy into `resources/web/orca/`.
5. **The camera problem is already solved in the page** and is the single most annoying thing to
   re-solve natively - three transports, two of them printer-family-specific, one of them a raw
   H.264 relay written specifically to work around go2rtc misreading a mid-GOP start as HEVC.
6. **Effort.** §5 puts the MVP at 11-15 days. B or C is that plus a UI rewrite that never finishes,
   because the page keeps growing.

**What the native layer must contain from day one, so this is an app and not a wrapper** - this is
both the product argument and the App Store 4.2 argument:

- the pairing flow and the hub profile store (multiple hubs, two URLs each, health racing);
- the push registration, the decryption, the notification presentation, the actions;
- a **native** event history and printer summary that render with the PC off;
- the launch/route logic from a notification tap;
- widgets and Live Activities when they land.

**And the one thing to decide early:** whether the WebView is allowed to hold the token in a URL at
all. It must, because that is how the page authenticates - the shell loads
`https://<host>/r/<token>/`. So the shell must (a) pin the WebView to that origin and refuse
off-origin top-level navigations, (b) use a non-persistent or app-private data store so the `rt`
cookie does not outlive an unpair, and (c) never expose a JavaScript bridge that lets page script
read the device push key. The page is ours, but the discipline costs nothing and the hub's own
`X-Frame-Options: DENY` on `/r/<token>/` shows the same instinct.

---

## 4. The API

### 4.1 What already exists and suffices

Nothing in this list needs changing. All of it is already behind the constant-time token gate.

**Hub plane, `/r/<token>/`** (`RemoteHub.cpp:2570-2456`):

| Route | Use in the app |
|---|---|
| `GET /r/<token>/` | the page the WebView loads |
| `GET /r/<token>/state` | camera descriptors, `remote_login`; also the cheapest liveness probe for M2 |
| `GET /r/<token>/events?since=<id>` | the event feed - `{events:[...], last_id}`, ids assigned by the hub, oldest first. Feeds the native history (G4, S7) |
| `GET /r/<token>/api/instances`, `POST /r/<token>/api/instances/open` | instance list; drop a file into a fresh hidden slicer (C5's share extension is this route plus an `X-File-Name` header) |
| `GET/POST /r/<token>/i/<pid>/api/...` | the whole instance API through the allow-list: `/api/printers`, `/api/printers/{id}/control`, `/api/plates/...`, `/api/slice`, `/api/jobs/{id}`, `/api/presets*`, `/api/settings/process*`, `/api/snapmaker/*`. S1's notification actions use `/api/printers/{id}/control` |
| `GET /r/<token>/bambu?id=`, `GET /r/<token>/ff?id=` | Bambu MJPEG relay; FlashForge stream URL |
| `GET /stream.html?src=&mode=mse` + `GET /api/ws` | the go2rtc player and its tunnel, gated by the `rt` cookie or a loopback `lt` |
| `GET /r/<token>/push/key`, `POST`/`DELETE /r/<token>/push/subscription` | **reused verbatim for UnifiedPush on Android** (S4): a UnifiedPush endpoint is an RFC 8030 push resource taking an `aes128gcm` body with optional VAPID, which is exactly what this sender produces |
| `GET /r/<token>/manifest.webmanifest`, the icon routes, `GET /r/<token>/sw.js` | unused by the app, unchanged for browser users |

**Loopback admin plane, `/hub/*`** - not reachable by the app and must stay that way. The app never
speaks to `/hub/*`; the hub page does, with `X-Hub-Secret`.

**Instance plane** - `RemoteAccess.cpp`'s 33-entry manifest at `GET /api`, mirrored by
`instance_api_allowed` (`RemoteHub.cpp:2220-2278`) and diffed by `test_hardening.py`. Note that
`RemoteAccess`'s own only gate is `peer.is_loopback()` (`RemoteAccess.cpp:2171`); the token and the
allow-list are the LAN-facing protection, and the app changes nothing about that.

**Planned, not yet built** - the app is a consumer, not a driver, of these:
`GET /api/archive`, `/api/archive/{id}`, `/api/archive/{id}/thumbnail.png`,
`POST /api/archive/{id}/send|pin|delete` (archive design §7), and the hub-side read-only
`GET /r/<token>/archive` + `/archive/{id}/thumbnail.png` for when no slicer is open (§8 of that
design).

### 4.2 What must be added

Five things. Every one follows a shape the hub already has, which is the point.

#### N1. `GET /r/<token>/pair` - what the app is talking to

```json
{ "name": "ACE-PC", "version": "2.3.1", "hub": "ultra1/1",
  "urls": { "lan": "http://10.0.0.12:13640/r/<token>/",
            "remote": "https://ace-pc.tail1234.ts.net/r/<token>/" },
  "push": { "webpush": true, "apns": true, "fcm": false, "unified": true,
            "vapid": "<base64url 65-byte point>" },
  "features": ["events", "control", "send", "archive"] }
```

Main listener, token-gated like its neighbours. It exists so that the QR the hub page already draws
(`hub.html` + `qrcode.js`, loopback-only) stays a plain URL - scannable by any camera app, openable
in any browser - while the app, having scanned it, can learn both URLs and the hub's capabilities in
one call. `urls.remote` is present only when remote access is on. `push.*` tells the app which
transports to offer rather than making it guess.

`features` is how the app avoids a version-skew matrix: it renders the Reprints entry only when
`"archive"` is present. The hub already has a self-describing manifest habit
(`GET /r/<token>/api`, `version: 2`); this extends it.

#### N2. `POST` / `DELETE /r/<token>/push/device` - device token registration

Modelled on `/r/<token>/push/subscription` line for line: `Content-Type: application/json`, 16 KiB
cap, idempotent on the token, a hard cap on rows, and a `DELETE` that answers `{"ok":true}` whether
or not the row existed so it cannot be used as an oracle (`RemoteHub.cpp:2353-2368`).

```json
{ "platform": "apns",            // "apns" | "fcm"
  "env": "production",           // APNs only: "production" | "sandbox" - see risk R3
  "token": "<hex device token or FCM registration token>",
  "bundle": "dev.acerage.ultra1",
  "p256dh": "<base64url 65-byte point>",   // the app's own key pair, for M6
  "auth":   "<base64url 16 bytes>",
  "label": "Ace's iPhone", "app": "1.0.3", "os": "iOS 26.1" }
```

`p256dh`/`auth` are the same two fields a browser `PushSubscription` carries, and they mean the same
thing: the app generates a P-256 key pair and a 16-byte auth secret on first launch, keeps the
private half in the platform keychain/keystore, and registers the public half. The hub then encrypts
with the **existing** `WebPush::encrypt` path - RFC 8291 §3.4 key schedule, `aes128gcm`, one record,
delimiter `0x02`, fresh ephemeral key and 16-byte salt per message
(`WebPush.cpp:416-472`) - and puts the base64url of that blob in the APNs/FCM payload instead of the
cleartext. **That is the whole of M6 and M2's crypto: no new primitives, no new dependency.**

Storage: `<datadir>/hub/settings.json` gains an `apppush` key beside `notify` and `webpush`, with
the same row shape as a Web Push subscription (`id`, `added`, `last_sent`, `last_status`,
`last_error`, `failures`) so the pruning and dirty-flag machinery is shared.

#### N3. The sender: `src/slic3r/GUI/AppPush.{hpp,cpp}`

A third sink on the existing `accept_event` seam, beside `RemoteNotify::deliver` and
`WebPush::deliver` (`RemoteHub.cpp:1403-1458`). Same worker-thread-and-queue shape, same
`enabled`/`min_severity` filters, same retry rules.

- **APNs.** ES256 JWT with `alg`/`kid` header and `iss`/`iat` claims; reuse `WebPush.cpp`'s
  `der_to_raw` (`:479`), its base64url, and its per-origin JWT cache (`jwt_for`, `:547`) - Apple
  wants no more than one refresh per 20 minutes and no less than one per hour, and the existing
  cache already implements "reuse until close to expiry". POST to `/3/device/<token>` with
  `apns-topic: <bundle>`, `apns-push-type: alert`, `apns-priority: 10` for `error`/`warning`/
  `runout` and `5` otherwise, `apns-expiration` = now + TTL, and `apns-collapse-id` = the same
  `<printer id>:<kind>` string the Web Push `Topic` header already derives (`WebPush.cpp:577`), so
  a second *paused* for the same printer replaces the first instead of stacking. Payload:
  `{"aps":{"alert":{"title-loc-key":"e"},"mutable-content":1,"sound":"default",
  "thread-id":"<printer id>"},"e":"<base64url ciphertext>"}` - the visible strings are filled in by
  the Notification Service Extension after it decrypts `e`.
- **FCM v1.** Service-account JSON -> a signed JWT -> an OAuth2 access token (scope
  `https://www.googleapis.com/auth/firebase.messaging`) cached until expiry -> `POST
  https://fcm.googleapis.com/v1/projects/{projectId}/messages:send` with a **data-only** message
  carrying the same `e` field, plus `android.priority: "high"`, so the app's
  `FirebaseMessagingService` decrypts and posts the notification itself.
- **Pruning (M12).** APNs `410` with reason `Unregistered`, and `400 BadDeviceToken`, delete the
  row. FCM `UNREGISTERED` / `NOT_FOUND` delete the row. Transport failure, `429` and `5xx` retry
  three times at 1 s and 3 s, sliced so `stop()` is not blocked - the exact rules
  `WebPush.cpp:627-636` already uses.
- **The HTTP/2 problem.** APNs is HTTP/2 only. The fork's outbound client is the libcurl wrapper
  `slic3r/Utils/Http.hpp`. **Verify that the bundled libcurl reports `CURL_VERSION_HTTP2`** before
  committing to this path; if it does not, the options are rebuilding curl with nghttp2, or a small
  purpose-written HTTP/2 client for the one request shape APNs needs. This is risk **R2** and it
  gets a one-day spike at the front of phase 1.

#### N4. `/hub/apppush*` - the loopback control plane for the above

`GET /hub/apppush` (masked config plus the device list), `POST /hub/apppush` (key path, key id, team
id, bundle id, environment; FCM service-account path; `enabled`, `min_severity`),
`DELETE /hub/apppush?id=` (forget one device), `POST /hub/apppush/test` (one synchronous push to
every device, per-device results). All four behind the existing `/hub/*` gate: loopback peer,
loopback `Host`, no `Sec-Fetch-Site: cross-site`, and `X-Hub-Secret`. Masking follows
`RemoteNotify::masked_json` - `****` plus the last four - and a value posted back beginning `****`
means "keep the stored one" (`take_secret`, `RemoteNotify.cpp:106`), so a credential only ever
travels inward. The hub page gains a *Phones (app)* card next to *Phones (Web Push)*.

The `.p8` is **referenced by path**, not uploaded through the browser and not copied into the data
dir, so the person keeps it where they keep it and the hub only reads it.

#### N5. `GET /r/<token>/summary` - one call for a widget (S2)

```json
{ "time": 1757000000000,
  "printers": [ { "id": "sm:U1-0042", "name": "U1 left", "kind": "snapmaker",
                  "state": "printing", "percent": 63, "left_time_s": 2810,
                  "layer": 214, "total_layers": 340, "job": "bracket.gcode" } ],
  "events_last_id": 812 }
```

Served by the hub from what it can reach without a slicer - the LAN Snapmakers it can poll and the
last state each instance reported - so a widget refresh is one request and does not marshal onto any
slicer's GUI thread. `/api/printers` inside an instance calls `run_on_main` (`RemoteAccess.cpp:943`);
a widget waking every 15 minutes must not be able to stall the slicer's UI.

#### Optional, later

- `POST /r/<token>/push/device/activity` for **Live Activity** push tokens, which are **per
  activity**, arrive asynchronously after `Activity.request()`, and *"may change during the
  activity's lifetime"*
  ([Apple, Starting and updating Live Activities with ActivityKit push notifications](https://developer.apple.com/documentation/activitykit/starting-and-updating-live-activities-with-activitykit-push-notifications),
  read 2026-09-04). Requires `apns-push-type: liveactivity` and `apns-topic:
  <bundleID>.push-type.liveactivity`.
- `GET /r/<token>/events?since=&wait=<seconds>` - a bounded long-poll so a foregrounded app is
  sub-second without a 1-second timer. Purely an optimisation; the existing poll is correct.
- `GET /.well-known/apple-app-site-association` and `/.well-known/assetlinks.json` on the **remote
  origin only**, for C4.

### 4.3 Security rules

These are the existing rules, restated for the new surface, plus three that are new because a device
token is not a browser subscription.

1. **Nothing but the token leaves the LAN, and no cloud account exists.** The app's identity is the
   phone token plus a random device id it mints locally. There is no sign-up, no directory, no
   server the project runs. (This is exactly why §3.1's "a relay the project hosts" is not the
   recommendation - see R1.)
2. **Everything app-facing lives under `/r/<token>/`** and passes the same constant-time compare.
   Nothing new goes on a static path. `/hub/*` stays loopback-only and the app never touches it.
3. **Credentials travel inward only.** The `.p8` and the FCM service account are read from disk by
   the hub, masked in every `/hub/*` response, and appear in no response on the phone plane at all.
   The gate asserts the key bytes appear in no body on either listener (M11).
4. **The payload is encrypted end to end.** APNs and FCM see an origin, a size, a collapse id and a
   base64url blob. This is a strict improvement on the relays, where *"the server sees the whole
   body"*, and it preserves the property Web Push already gives. The collapse id is a truncated
   SHA-256 of `<printer id>|<kind>`, so even it leaks nothing readable.
5. **Device tokens are opaque and are treated as subscriptions.** Capped in number, pruned on
   terminal errors, never logged, masked in the UI.
6. **Errors are scrubbed.** `RemoteNotify.cpp:148`'s `scrub` already strips a destination's own
   secrets from libcurl's error text before it is stored or logged; the APNs/FCM sender uses it.
7. **Header and payload injection stays impossible.** Every string that came from a printer is
   already clipped and control-character-stripped by `accept_event`; the app sender adds no
   header built from event text.
8. **The token is not a capability the app may hand out.** No share sheet exports the profile, no
   deep link carries the token to another app, and the WebView is pinned to the paired origin.
9. **Unpairing is complete.** It deletes the device row on the hub (`DELETE
   /r/<token>/push/device`), the keychain key pair, the WebView data store and the cached history.
10. **Token rotation is a first-class state.** `POST /hub/newlink` invalidates the app's profile; the
    replaced-link 404 page (`RemoteHub.cpp:2461`) is the app's signal to prompt for a re-pair rather
    than to retry (M13).

---

## 5. MVP and phased plan

Effort is in ideal days for one person who knows this codebase, excluding hardware passes and
excluding the calendar time of Apple enrolment and review. It follows the convention of the other
specs in this directory.

### 5.1 The MVP, stated exactly

**Ultra1 v0.1 is an iOS and Android app, installed on the user's own devices, that:**

1. pairs by scanning the QR on the hub page, storing one profile with both URLs (M1);
2. picks the reachable URL on every launch and shows the page full-screen, with all four tabs
   working (M2, M8, M9);
3. registers an APNs (iOS) or FCM (Android) device token with the hub and re-registers on every cold
   launch (M3);
4. receives a notification within 30 seconds of any event the hub accepts, **with the app killed and
   the phone on cellular data with the PC unreachable** (M4, M5);
5. shows content that Apple and Google could not read, decrypted on the device (M6);
6. opens on the right printer when the notification is tapped (M7);
7. shows the event history and last known printer states with the PC off (M14);
8. keeps every credential on the PC and adds no cloud account (M10, M11).

**Not in the MVP:** notification actions, widgets, Live Activities, native camera playback,
UnifiedPush, multiple hubs, Reprints (which does not exist yet in the page either), and any store
submission.

**Distribution for the MVP is personal:** iOS via the user's own Apple Developer Program membership,
installed from Xcode or through TestFlight internal testing (no App Review); Android as a sideloaded
APK (no Play account, no $25, no 12-testers rule) that still receives FCM because the phone has Play
Services.

### 5.2 Phases

| Phase | What | Effort |
|---|---|---|
| **0. Decide and enrol** | Enrol in the Apple Developer Program; reserve a bundle id; create the APNs `.p8` (Key ID + Team ID); create a Firebase project and download the service-account JSON; pick a neutral product name (§6.7); **spike: does the bundled libcurl do HTTP/2?** (R2) | **1-2 d** + Apple enrolment latency |
| **1. The hub's push plane** | `AppPush.{hpp,cpp}`: APNs ES256 JWT (reusing `WebPush.cpp`'s DER->raw, base64url and JWT cache), HTTP/2 POST, header set, collapse ids; FCM v1 with the OAuth2 token exchange; the RFC 8291 encryption reused for the payload; `POST`/`DELETE /r/<token>/push/device`; `GET /r/<token>/pair`; `/hub/apppush*` and the hub-page card; the `apppush` settings key with masking; pruning and retries; the gate (`test_ultra1_push.py`, with `mock_apns.py` and `mock_fcm.py` verifying JWTs and decrypting payloads independently) | **5-7 d** |
| **2. The shell, both platforms** | Swift + Kotlin projects; pairing (QR scan, profile store, URL racing, health); keychain/keystore key pair; APNs/FCM registration and re-registration; the WebView host pinned to the origin with an app-private data store; the iOS **Notification Service Extension** and the Android `FirebaseMessagingService` that decrypt and present; notification tap routing; the offline banner | **5-7 d** |
| | **MVP complete: phases 0-2, ~11-16 days** | |
| **3. The native layer proper** | `GET /r/<token>/summary`; a native Devices summary and event history backed by a local store, correct with the PC off; background refresh (`BGAppRefreshTask` / `WorkManager`); notification actions *Pause* / *Stop* (S1) with the stop confirmation; permission-denied degradation (S6) | **4-5 d** |
| **4. Android without Google** | UnifiedPush as an alternative transport, reusing `/r/<token>/push/subscription` and the existing Web Push sender unchanged; a distributor picker; the gate proving the same bytes go out (S4) | **3-4 d** |
| **5. Multiple hubs, share extension** | Profile list, per-profile device registration, "which hub" on every notification (S3); the share extension into `POST /r/<token>/api/instances/open` (C5) | **3-4 d** |
| **6. Live Activities and widgets** | iOS Live Activity with per-activity push tokens and the extra registration route; a home-screen widget on both platforms; Android 16 `ProgressStyle` Live Update with `POST_PROMOTED_NOTIFICATIONS` | **5-7 d** |
| **7. Reprints** | Mostly free: the page's Reprints tab arrives in the WebView with a hub update. The native work is caching the list and thumbnails for offline (S8) and a *Reprint* action from a *finished* notification | **2 d** after archive Stage 2 |
| **8. Native camera** *(optional)* | WebRTC playback against go2rtc's `/api/webrtc` - needs `webrtc.listen` enabled, `/api/webrtc` added to `GO2RTC_PASSTHROUGH` (`RemoteHub.cpp:68`), and the `feat/webrtc-video` branch merged. Only worth doing if MSE-in-the-WebView proves bad in practice | **4-6 d** |
| **9. Store readiness** *(a separate decision, §6.7)* | Grow the native layer past guideline 4.2; a neutral name and icon; privacy manifest and nutrition labels; screenshots; App Review; Play closed testing with 12 testers for 14 days | **8-12 d** + review latency + permanent release burden |

**Totals.** MVP (0-2): **11-16 days**. Through phase 6 - the app the user actually described:
**~26-37 days**. Phase 9 roughly doubles the ongoing cost of the whole thing and should not be
started until the app has been in the user's hands for a season.

### 5.3 Ongoing costs

| Item | Cost | Source |
|---|---|---|
| Apple Developer Program | **$99/yr**, unavoidable for APNs | [developer.apple.com/programs/whats-included](https://developer.apple.com/programs/whats-included/), read 2026-09-04 |
| Google Play registration | **$25 one-time** - **only if publishing**; sideloading costs nothing | [support.google.com](https://support.google.com/googleplay/android-developer/answer/6112435), read 2026-09-04 |
| Firebase / FCM | A project is required. **No cost is verified at this volume** - do not quote a free-tier figure I have not confirmed | [firebase.google.com/docs/cloud-messaging/auth-server](https://firebase.google.com/docs/cloud-messaging/auth-server), read 2026-09-04 |
| APNs | No per-message cost is published | - |
| TestFlight rebuild cadence | A build is testable **up to 90 days**, so distributing that way means a rebuild each quarter | [Apple TestFlight overview](https://developer.apple.com/help/app-store-connect/test-a-beta-version/testflight-overview), read 2026-09-04 |
| CI / build service | **$0** if built locally on the user's Mac and PC. Expo EAS, if ever used: Free tier "15 Android and 15 iOS builds" (period UNVERIFIED), Starter **$19/mo**, Production **$199/mo** | [expo.dev/pricing](https://expo.dev/pricing), read 2026-09-04 |
| A relay service | **$0 - because the recommendation has none.** Only phase 9 (public distribution) would force one; see R1 |
| Maintenance | The real cost: two signing identities, two toolchains, an annual Xcode/SDK bump, and a native layer that must keep pace with the hub's event contract | - |

A Mac is required to build and sign the iOS app. That is a hardware prerequisite, not a fee.

---

## 6. Risks and open questions

### R1. The `.p8` in an open-source fork - the central question

**The problem.** APNs authenticates the *sender*, not the app. Whoever holds a team's `.p8` can push
to every app signed by that team. So a build of Snapmaker Orca Ultra distributed with a shared key
would hand every user the ability to push to every other user's phone. That is not acceptable, and
it means **the `.p8` can never be in the repository or in a release artifact.**

**Why the recommendation still works.** In the recommended shape the key is *the user's own*: they
enrol, they create the key, they sign their own build of Ultra1, and the key sits in their data dir
alongside the VAPID private key that is already there (`settings.json` -> `webpush.vapid.private`).
Nothing is shared, nothing is distributed, and the hub keeps its "no server we run" property intact.
This is a genuinely good fit for **one power user with a fork** and a genuinely bad fit for **a
public app**, and the plan should say so out loud rather than pretend otherwise.

**The open question for a public Ultra1** - and it needs a human decision, not an engineering one:

- **(a) A relay the project hosts.** hub -> a small HTTPS service -> APNs/FCM. It can be built so it
  learns nothing: the payload is already `aes128gcm` ciphertext keyed to the device, so the relay
  sees a device token, a size and a blob. But it is a service someone runs, pays for, keeps up, and
  is on the hook for when it is down - and it breaks the "no cloud" promise in spirit even if not in
  content.
- **(b) Publish the source and let each user build and sign.** Honest, free, and excludes everyone
  who does not own a Mac and $99.
- **(c) Do not publish; keep Ultra1 personal.** What §5 assumes.
- **(d) Stay on relays for the public build** (Pushover today) and offer the app only to people who
  bring their own key. A muddle, but a survivable one.

**Recommendation: (c) now, (a) if and when the fork has users who ask for it** - and design N3 as a
provider interface from day one so (a) is a new implementation of an existing seam, not a rewrite.

### R2. libcurl and HTTP/2

APNs speaks HTTP/2 only. The fork's outbound client is `slic3r/Utils/Http.hpp` over libcurl. If the
bundled build lacks nghttp2, every APNs request fails and phase 1 stalls. **Mitigation:** a one-day
spike in phase 0 checking `curl_version_info()->features & CURL_VERSION_HTTP2` in the actual build,
before any other phase-1 work. Fallbacks: rebuild curl with nghttp2 in `deps/`; or write a minimal
HTTP/2 client for the single request shape APNs needs (one stream, POST, fixed header set) on top of
the existing TLS. FCM v1 is plain HTTPS and is unaffected.

### R3. APNs environment mismatch

A token minted by a development build is not valid on the production APNs host and vice versa;
`BadDeviceToken` is the classic symptom and it looks like a code bug. **Mitigation:** the app reports
its environment in `POST /r/<token>/push/device` (`"env"`), the hub stores it per device and routes
to `api.sandbox.push.apple.com` or `api.push.apple.com` accordingly, and the gate covers both. The
hub page's *Test* button must show which host a device was reached on.

### R4. iOS local network privacy and ATS

The WebView is exempt from the local-network prompt (TN3179), but **native** calls to the LAN are
not - so the moment phase 3 adds `GET /r/<token>/summary` from Swift, the prompt appears.
Separately, since iOS 17 ATS no longer allows connections to IP addresses by default, and
`NSAllowsLocalNetworking` is not the fix - `NSExceptionDomains` naming the address or its CIDR is
([Apple](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking),
read 2026-09-04). **Mitigation:** ship `NSLocalNetworkUsageDescription` with an honest sentence; add
`NSExceptionDomains` entries for the RFC1918 ranges; and **prefer the tailnet URL for every native
request**, where it is ordinary HTTPS to a public name and neither mechanism applies. **Open
question:** exactly which `NSExceptionDomains` shape Apple accepts for a CIDR range in the current
SDK - this needs testing on a device, not reading.

### R5. Tailscale

Remote access depends on the user having the Tailscale app installed and connected; we cannot embed
it (`tsnet` is Go-only, no official mobile SDK) and we must not ship a competing VPN (one at a time
on both platforms). **Open questions:** whether MagicDNS names resolve for a third-party app on iOS
(**UNVERIFIED** - store the `100.x` address as well as the name and race both); and whether
Tailscale's VPN-on-demand behaviour can leave the tunnel down at the moment the app foregrounds,
which would make the remote URL fail a health check that would have succeeded a second later. The
app's URL race must therefore retry rather than latch.

### R6. Background execution and Live Activity duration

*"Typically, an app is in a suspended state when it's in the background"*, and none of the ten
`UIBackgroundModes` values covers a long-lived socket to a LAN device
([Apple, Configuring background execution modes](https://developer.apple.com/documentation/xcode/configuring-background-execution-modes),
read 2026-09-04). Background pushes are explicitly unreliable: *"the system doesn't guarantee their
delivery"* and *"don't try to send more than two or three per hour"*
([Apple, Pushing background updates to your app](https://developer.apple.com/documentation/usernotifications/pushing-background-updates-to-your-app),
read 2026-09-04). **This kills any design where the app watches the printer itself.** The hub
watches; the app is told. Our notifications are user-visible alerts, not background pushes, so the
throttle does not apply to them - but progress updates would hit it, which is why progress belongs
in a Live Activity or a widget, not in a stream of silent pushes.

Live Activities are capped: *"A Live Activity can be active for up to eight hours"*, remaining on
the Lock Screen *"for a maximum of 12 hours"*, with a 4 KB content limit
([Apple, Displaying live data with Live Activities](https://developer.apple.com/documentation/activitykit/displaying-live-data-with-live-activities),
read 2026-09-04). **Many prints are longer than eight hours**, so the Activity will end mid-print;
the design must end it gracefully and fall back to the widget rather than look broken. Android 16's
equivalent is `Notification.ProgressStyle` with `POST_PROMOTED_NOTIFICATIONS` and
`setRequestPromotedOngoing`; *"OEMs can enforce additional criteria"* and **no duration cap is
documented**
([Android, progress-centric notifications](https://developer.android.com/about/versions/16/features/progress-centric-notifications),
read 2026-09-04).

### R7. App Store rules, if we ever submit

- **4.2 Minimum Functionality**: *"Your app should include features, content, and UI that elevate it
  beyond a repackaged website. If your app is not particularly useful, unique, or 'app-like,' it
  doesn't belong on the App Store."* A pure WebView wrapper is refused. The native layer of §3.3 is
  the mitigation and must exist *before* submission, not after a rejection.
- **4.2.3(i)**: *"Your app should work on its own without requiring installation of another app to
  function."* This is aimed at other *apps*, not at hardware - but Ultra1 does require the Tailscale
  app for remote use, and it requires a PC running the hub. There is **no guideline that
  affirmatively blesses a hardware-companion app**; the nearest is **3.1.4 Hardware-Specific
  Content**, which permits unlocking features that are *"dependent upon specific hardware to
  function"* (Apple's own example is an astronomy app synced with a telescope). **Open question:**
  whether a reviewer with no printer and no hub can evaluate the app at all - a demo mode with
  canned data is probably a submission requirement, not a nicety.
- **4.1(c)** *"You cannot use another developer's icon, brand, or product name in your app's icon or
  name, without approval"* and **5.2.1** on trademarks. **The app cannot be called "Snapmaker"
  anything.** Pick the neutral name in phase 0 and use it everywhere from the first commit.
- All quotations from [App Store Review Guidelines](https://developer.apple.com/app-store/review/guidelines/),
  read 2026-09-04. The page carries **no "last updated" date**; the most recent revision I could
  date is 2026-06-08 ([Apple developer news](https://developer.apple.com/news/?id=a233fmpw)), which
  touched 4.5.3 to say Live Activities *"may not be used to spam, phish, or send unsolicited
  messages."*
- Google Play, for a new personal account: closed testing with *"a minimum of 12 testers who have
  been opted in continuously for at least 14 days"* before production, then a review that *"usually
  takes seven days or less"*
  ([Google](https://support.google.com/googleplay/android-developer/answer/14151465), read
  2026-09-04). Sideloading skips all of it.
- EU alternative distribution exists and, from 2026-10-01, broadens - but requires Developer Program
  membership and, under the **Core Technology Commission** that replaces the Core Technology Fee
  from the same date, **5%** on digital sales through those channels
  ([Apple, DMA and apps in the EU](https://developer.apple.com/support/dma-and-apps-in-the-eu/),
  page dated 2026-08-18, read 2026-09-04). Irrelevant to a free app with no digital sales, but worth
  knowing before anyone proposes it as a distribution route.

### R8. The second-front-end burden - the objection that was right

The capabilities research rejected an app partly because it is *"a permanent release burden for a
fork of a slicer."* That is still true, and §3.3 only shrinks it: even a shell has two signing
identities, two toolchains, an annual SDK bump, and a native layer coupled to the hub's event
contract. **Mitigations built into the design:** `features` in `GET /r/<token>/pair` so an old app
hides what a new hub does not have and a new app tolerates an old hub; the page - not the app -
owning every screen; and the native layer touching exactly four hub routes (`pair`, `state`,
`events`, `summary`) plus `push/device`, so the coupling surface is five endpoints rather than
thirty-three. **Open question the user must answer before phase 1:** is there an appetite to keep
this alive for years, or is the honest answer that Pushover plus the installed web app is enough?

### R9. Payload privacy is a regression unless M6 is built

Without the Notification Service Extension, an APNs alert's text is visible to Apple - which is
*worse* than the Web Push channel it replaces and no better than Pushover. M6 is therefore not a
nice-to-have; it is the thing that makes the app defensible. **Open question:** the extension must
run before every notification is shown, and if it crashes or times out iOS shows the fallback
payload - so decide now what that fallback says. Recommendation: `"Printer update"` and nothing
else.

### R10. Small things that will bite

- **Token rotation** silently orphans the app (M13); the replaced-link page is the signal.
- **Two hubs on one PC**, or a data dir copy, will produce duplicate device rows; key rows on
  `(platform, token)` and let re-registration replace.
- **The `rt` cookie** is what gates `/stream.html` and `/api/ws`; WebKit bug 272325 has session
  cookies in home-screen web apps resetting unpredictably. The page re-sets it from
  `location.pathname` on every load (`stream_center.html:314`), so this should be harmless in a
  WebView too - but watch for a first-frame flash of "camera relay is not running".
- **`/hub/push/debug` and `/api/debug/*`** are env-gated (`SNORCA_DEBUG_ROUTES=1`); the new
  `AppPush` debug route must be gated the same way and must 404 in a shipped hub.
- **UnifiedPush conformance** (S4): the spec is aligned but no page promises drop-in compatibility
  and distributor behaviour varies. The gate must prove it against a real distributor, not against
  the spec.

---

## 7. Sources

All URLs read **2026-09-04** unless a different date is given.

**Apple - push**
- [Establishing a token-based connection to APNs](https://developer.apple.com/documentation/usernotifications/establishing-a-token-based-connection-to-apns)
- [Sending notification requests to APNs](https://developer.apple.com/documentation/usernotifications/sending-notification-requests-to-apns)
- [Registering your app with APNs](https://developer.apple.com/documentation/usernotifications/registering-your-app-with-apns)
- [Pushing background updates to your app](https://developer.apple.com/documentation/usernotifications/pushing-background-updates-to-your-app)
- [Sending web push notifications in web apps and browsers](https://developer.apple.com/documentation/usernotifications/sending-web-push-notifications-in-web-apps-and-browsers)
- [Displaying live data with Live Activities](https://developer.apple.com/documentation/activitykit/displaying-live-data-with-live-activities)
- [Starting and updating Live Activities with ActivityKit push notifications](https://developer.apple.com/documentation/activitykit/starting-and-updating-live-activities-with-activitykit-push-notifications)

**Apple - platform, networking, distribution**
- [TN3179: Understanding local network privacy](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy) (revision 2026-02-17)
- [NSAllowsLocalNetworking](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking)
- [Developer Forums thread 66417](https://developer.apple.com/forums/thread/66417) (Apple DTS, June 2017)
- [Configuring background execution modes](https://developer.apple.com/documentation/xcode/configuring-background-execution-modes)
- [BGAppRefreshTask](https://developer.apple.com/documentation/backgroundtasks/bgapprefreshtask)
- [Apple Developer Program - What's included](https://developer.apple.com/programs/whats-included/) ($99/yr)
- [Apple Developer Enterprise Program](https://developer.apple.com/programs/enterprise/) ($299/yr)
- [Compare memberships](https://developer.apple.com/support/compare-memberships/) (Personal Team limits)
- [TestFlight overview](https://developer.apple.com/help/app-store-connect/test-a-beta-version/testflight-overview) and [Add internal testers](https://developer.apple.com/help/app-store-connect/test-a-beta-version/add-internal-testers)
- [App Store Review Guidelines](https://developer.apple.com/app-store/review/guidelines/) · [Developer news, 2026-06-08 revision](https://developer.apple.com/news/?id=a233fmpw)
- [DMA and apps in the EU](https://developer.apple.com/support/dma-and-apps-in-the-eu/) (page dated 2026-08-18)

**Google / Firebase / Android**
- [FCM: Authorize send requests (HTTP v1)](https://firebase.google.com/docs/cloud-messaging/auth-server)
- [FCM: Set up an Android client](https://firebase.google.com/docs/cloud-messaging/android/client)
- [FCM: APNs key upload for iOS](https://firebase.google.com/docs/cloud-messaging/ios/certs)
- [Firebase FAQ](https://firebase.google.com/support/faq) (legacy API deprecated 2023-06-20; migration window to 2024-06-21)
- [Play Console registration fee](https://support.google.com/googleplay/android-developer/answer/6112435) ($25 one-time)
- [Play closed-testing requirement](https://support.google.com/googleplay/android-developer/answer/14151465) (12 testers, 14 days)
- [Android 16 progress-centric notifications](https://developer.android.com/about/versions/16/features/progress-centric-notifications) and [Live updates](https://developer.android.com/develop/ui/views/notifications/live-update)

**WebKit / web push background**
- [Web Push for Web Apps on iOS and iPadOS](https://webkit.org/blog/13878/web-push-for-web-apps-on-ios-and-ipados/) (2023-02-16)
- [Meet Declarative Web Push](https://webkit.org/blog/16535/meet-declarative-web-push/) (2025-03-27)
- RFCs [8030](https://www.rfc-editor.org/rfc/rfc8030), [8188](https://www.rfc-editor.org/rfc/rfc8188), [8291](https://www.rfc-editor.org/rfc/rfc8291), [8292](https://www.rfc-editor.org/rfc/rfc8292)

**Cross-platform toolkits**
- [React Native versions](https://reactnative.dev/versions) (0.87 stable) · [rnfirebase messaging](https://rnfirebase.io/messaging/usage) · [Notifee](https://notifee.app/react-native/docs/overview) · [react-native-webrtc](https://github.com/react-native-webrtc/react-native-webrtc)
- [Flutter release notes](https://docs.flutter.dev/release/release-notes) (3.47.0; page updated 2026-08-12) · [firebase_messaging](https://pub.dev/packages/firebase_messaging) · [flutter_webrtc](https://pub.dev/packages/flutter_webrtc)
- [Kotlin Multiplatform supported platforms](https://kotlinlang.org/docs/multiplatform/supported-platforms.html)
- [Capacitor push notifications](https://capacitorjs.com/docs/apis/push-notifications) · [Capacitor config](https://capacitorjs.com/docs/config) · [Capacitor live reload](https://capacitorjs.com/docs/guides/live-reload)
- [Expo push FAQ](https://docs.expo.dev/push-notifications/faq/) · [Expo push setup](https://docs.expo.dev/push-notifications/push-notifications-setup/) · [Expo pricing](https://expo.dev/pricing)

**Other**
- [UnifiedPush Android spec (AND_3.1.0)](https://unifiedpush.org/developers/spec/android/) · [UnifiedPush developer intro](https://unifiedpush.org/developers/intro/) · [distributors](https://unifiedpush.org/users/distributors/)
- [ntfy: subscribe from your phone](https://docs.ntfy.sh/subscribe/phone/) (UnifiedPush distributor, Android only)
- [Tailscale: using other VPNs](https://tailscale.com/docs/reference/faq/other-vpns) · [Install on iOS](https://tailscale.com/kb/1020/install-ios) · [tsnet](https://tailscale.com/docs/features/tsnet) (last validated 2026-07-24) · [pkg.go.dev/tailscale.com/tsnet](https://pkg.go.dev/tailscale.com/tsnet)
- [go2rtc](https://github.com/AlexxIT/go2rtc) (v1.9.14, released 2026-01-19)

**In this repository**
- `docs/superpowers/specs/2026-09-02-remote-access-design.md` · `2026-09-02-hidden-service-mode.md` · `2026-09-03-phone-send-design.md` (with §3, Snapmaker over the LAN) · `2026-09-03-phone-print-control-design.md` · `2026-09-04-phone-events-design.md` · `2026-09-03-phone-mobile-capabilities-research.md` · `2026-09-04-gcode-archive-design.md` (on `docs/gcode-archive-design`)
- `src/slic3r/GUI/RemoteHub.cpp` · `RemoteAccess.cpp` · `RemoteNotify.cpp` · `WebPush.cpp` · `RemoteEvents.cpp` · `BambuCamRelay.cpp` · `resources/web/orca/stream_center.html` · `sw.js` · `hub.html` · `player.html`

### Explicitly unverified - do not plan against these

1. The FCM legacy-API **shutdown** date often quoted as 2024-07-22. The migration guide is a 404;
   the only Google page I could fetch says "decommissioning ... in June 2024".
2. That an Apple **Personal Team cannot use the Push Notifications capability**. Universally
   reported, stated by Apple nowhere I could find.
3. The App Store Review Guidelines' own "last updated" date (the page carries none).
4. A numeric execution budget for `BGAppRefreshTask`.
5. That Expo's free tier's "15 Android and 15 iOS builds" is per month.
6. That an unmodified RFC 8291/VAPID sender works against every **UnifiedPush** distributor.
7. That **MagicDNS** names resolve for arbitrary third-party apps on iOS.
8. Whether Live Activities specifically require a paid account (it follows from APNs, but Apple does
   not say it).
9. Any iOS 26-specific change to local network privacy.
10. That Apple explicitly denies native apps the use of Web Push endpoints (the negative is inferred
    from two Apple pages, not stated in one).

---

## Update (phase 1): the hub's push plane, as built

Branch `feat/app-push`, cut from `feat/ultra-preferences` at `48d24205c3`. This section records
what phase 1 actually shipped, where it differs from §4.2 above, and exactly what the app must
send. Where this section and §4.2 disagree, **this section is what the code does** - the
differences all come from [`2026-09-04-ultra1-phase0-spike.md`](2026-09-04-ultra1-phase0-spike.md),
which found them by compiling and running against the real dependencies.

Phases 2 and up - the app itself - are unchanged and unstarted.

### U1. The dependency change, and what an integrator must do

**The spike's finding stands: the bundled libcurl had no HTTP/2, and APNs speaks nothing else.**
The fix is `deps/NGHTTP2/NGHTTP2.cmake` (nghttp2 1.64.0, lib-only, static, pinned by hash),
`-DUSE_NGHTTP2:BOOL=ON` in `deps/CURL/CURL.cmake` with `-DNGHTTP2_STATICLIB` in the C flags, a
`cmake/modules/FindNGHTTP2.cmake` that knows about MSVC's `nghttp2_static` naming, and one
`target_link_libraries` line on the top-level `libcurl` interface target.

**On all three platforms, deliberately.** Scoping the flag to Windows would buy a smaller change
and an APNs feature that silently does not exist on a Mac or Linux hub. The cost is that the next
clean deps build on any platform inherits one more dependency.

**This does not take effect until the shared deps prefix is rebuilt**, which is a deliberate act
and not something a slicer build does on its own. Until it is, `Http::has_http2()` is false, the
hub page says *"This build of libcurl has no HTTP/2, so iOS push cannot work"*, `ApnsProvider`
reports itself unavailable with the same sentence, and **FCM works normally** - the v1 endpoint is
plain HTTPS/1.1 and needs none of this.

Two notes for whoever runs it. `nghttp2` 1.64 takes its library targets from `BUILD_STATIC_LIBS`,
not the `ENABLE_STATIC_LIB` of older releases; with both off it builds no library and then fails
on its own `nghttp2::nghttp2` alias. And on a *fresh* deps build directory `dep_CURL` pulls
`dep_OpenSSL` in with it, so a first run from scratch is much longer than the nghttp2 build alone
suggests; against the existing `deps/build` tree, where OpenSSL is already stamped, it is not.

### U2. Settings: the hub's `settings.json` gains `apppush`

```json
{ "apppush": {
    "enabled": true,
    "min_severity": "info",
    "apns": { "enabled": true,
              "bundle": "dev.acerage.ultra1",
              "key_id": "ABC123DEFG", "team_id": "DEF123GHIJ",
              "key_path": "C:/Users/ace/keys/AuthKey_ABC123DEFG.p8",
              "key_pem": "",
              "env": "production" },
    "fcm":  { "enabled": true,
              "project_id": "ultra1-1a2b3",
              "service_account_path": "C:/Users/ace/keys/ultra1-fcm.json",
              "service_account_json": "" },
    "devices": [
      { "id": "d_7f3a...", "platform": "apns", "env": "production",
        "token": "<opaque>", "bundle": "dev.acerage.ultra1",
        "p256dh": "<b64url 65 bytes>", "auth": "<b64url 16 bytes>",
        "label": "Ace's iPhone", "app": "1.0.3", "os": "iOS 26.1",
        "added": 1757000000, "last_sent": 0, "last_status": 200,
        "last_error": "", "last_host": "https://api.push.apple.com", "failures": 0 } ] } }
```

(Windows paths are stored with whatever separators they were given; the JSON above uses forward
slashes only to keep this document readable.)

- **Credentials are paths.** `key_path` and `service_account_path` are the source of truth; the
  file is read at `start()` and whenever the options change, and the parsed key is held in memory
  so a push at 03:00 does not fail because the file moved. Nothing is copied into the data dir.
- `key_pem` and `service_account_json` are the inline alternative for someone who would rather
  paste a key than keep a file. They are credentials in the fullest sense: stored only here, in no
  `/hub/*` response even masked, and never on the phone plane at all.
- **`env` is per device**, not per hub (risk **R3**). `apns.env` is only the default for a
  registration that does not say. `POST /hub/apppush/test`'s per-device result names the host it
  reached, so a `BadDeviceToken` from an environment mismatch is diagnosable from the hub page.
- Rows are keyed on **(platform, token)**. Cap: 16 devices, 16 KiB per registration body.
- Two debug-only keys, `apns.host_override` and `fcm.host_override` / `fcm.token_uri_override`,
  are read *and written* only when `SNORCA_DEBUG_ROUTES=1`. They exist so the gate can point the
  sender at its mocks; a shipped hub cannot be talked into sending a device's notifications
  anywhere but Apple and Google.

### U3. Routes

**Phone plane, `/r/<token>/`** - constant-time token gate, exactly like its neighbours:

| Route | What |
|---|---|
| `POST /r/<token>/push/device` | register; idempotent on `(platform, token)`; `Content-Type: application/json`; 16 KiB and 16 rows |
| `DELETE /r/<token>/push/device` | body `{platform, token}`; answers `{"ok":true}` either way, so it is not an oracle |

**Loopback admin plane, `/hub/*`** - loopback peer + loopback `Host` + no cross-site
`Sec-Fetch-Site` + `X-Hub-Secret`:

| Route | What |
|---|---|
| `GET /hub/apppush` | masked config, per-provider availability with a reason, device list with tokens masked |
| `DELETE /hub/apppush?id=` | forget one device |
| `POST /hub/apppush/options` | set everything; `****`-prefixed credential fields mean *keep the stored one* |
| `POST /hub/apppush/test` | one synchronous push per device, results naming provider, host and status |
| `POST /hub/apppush/debug` | `{"op":"collapse"｜"plaintext"｜"providers"}`; 404 without `SNORCA_DEBUG_ROUTES=1` |

**`GET /r/<token>/pair` (N1) is not built.** Nothing in phase 1 needs it and the app does not exist
yet; it belongs with phase 2, where the QR scan that consumes it lives. `AppPush::providers_json()`
already produces the `push` object it would carry.

### U4. What the app must send

```json
POST /r/<token>/push/device
Content-Type: application/json

{ "platform": "apns",
  "env": "production",
  "token": "<hex APNs device token, or the FCM registration token>",
  "bundle": "dev.acerage.ultra1",
  "p256dh": "<base64url, the 65-byte uncompressed P-256 point>",
  "auth":   "<base64url, 16 bytes>",
  "label": "Ace's iPhone", "app": "1.0.3", "os": "iOS 26.1" }
```

`platform` is `"apns"` or `"fcm"`; `env` is APNs-only and is `"production"` or `"sandbox"`.

- `p256dh` and `auth` are **the app's own**, generated on first launch, private half in the
  Keychain / Android Keystore. They mean exactly what a browser `PushSubscription`'s do, which is
  why the hub encrypts with the unchanged `WebPush::encrypt`.
- **Re-post this on every cold launch.** Push tokens rotate and no platform reliably says when.
  The row is replaced, not duplicated.
- A token containing a space, a control character, `/`, `?` or `#` is refused: it lands in a URL
  path on the APNs side.
- On iOS, the Keychain item **must be in a shared access group**: the Notification Service
  Extension is a separate process from the app, and this is the single most common way this design
  is got wrong.

**What arrives at the device.**

APNs - note the literal alert text, which corrects §4.2 N3's `title-loc-key`:

```json
{ "aps": { "alert": { "title": "Printer update", "body": "Tap to open" },
           "mutable-content": 1, "sound": "default", "thread-id": "<printer id>" },
  "v": 1, "e": "<base64url aes128gcm blob>" }
```

Headers: `authorization: bearer <ES256 JWT>`, `apns-topic: <bundle>`, `apns-push-type: alert`,
`apns-priority: 10` for warning and error and `5` otherwise, `apns-expiration: <now + ttl>`,
`apns-collapse-id: <24 base64url characters>`.

FCM - data-only, with **no `notification` block**, because a message carrying both goes to the
tray in the background and `onMessageReceived` is never called:

```json
{ "message": { "token": "<registration token>",
               "data": { "e": "<base64url blob>", "v": "1", "c": "<collapse id>" },
               "android": { "priority": "high", "ttl": "1800s", "collapse_key": "<collapse id>" } } }
```

The decrypted plaintext is the same JSON `WebPush.cpp` already sends to the service worker -
`title`, `body`, `kind`, `severity`, `printer`, `printer_id`, `tag`, `id`, `time` - so the iOS
extension, the Android service and the browser's service worker all read one format.

TTL is 1800 s for an alert and 300 s for `started` / `resumed`. The collapse id is a truncated
SHA-256 of `"<printer id>|<kind>"`, so a second *paused* for one printer replaces the first and
even the collapse id leaks nothing readable.

### U5. Pruning and retries

| Provider | Prune the row | Retry (3 tries, 1 s then 3 s, in 100 ms slices) | Re-mint and retry **once**, never prune |
|---|---|---|---|
| APNs | `410 Unregistered`; `400 BadDeviceToken` / `BadTopic` / `DeviceTokenNotForTopic` | `429`, `5xx`, transport error | `403 ExpiredProviderToken` |
| FCM | `404 UNREGISTERED` / `NOT_FOUND` | `429`, `5xx`, transport error | `401` |

### U6. Three corrections to §4.2, carried out

1. **Literal `alert` title and body**, not `title-loc-key` (U4). Apple requires alert information
   for the extension to run at all, and a key naming no `.strings` entry fails silently.
2. **No `exp` claim** in the APNs JWT. RFC 8292 VAPID has one; Apple does not use one.
3. **`Http` gained `http_version()` and `has_http2()`.** §4.2 assumed the wrapper could already
   express this; it could not, and neither could the libcurl underneath it.

### U7. The gate

`snorca_hubtest\test_app_push.py`, with `mock_apns.py` and `mock_fcm.py`. Eleven groups covering
the `/hub/*` gate on all five new routes, registration and its caps, masking and M11, the FCM
OAuth2 exchange and data-only message, the APNs header set and provider token, M6 (the cleartext
appears nowhere in the raw bytes of either conversation), every row of the U5 table, severity and
the off switch, the collapse id, the test route and a restart.

`mock_apns.py` is a **cleartext HTTP/2 (h2c)** server on loopback. That is only possible because
the sender is libcurl+nghttp2, which offers `CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE`: no TLS, no
certificate, nothing added to a trust store, and the gate runs anywhere. It needs Python's `h2`
package (pure Python, alongside the `cryptography` the existing Web Push gate already uses).

### U8. Still unverified after phase 1

Everything in the plan's own list, plus:

1. **A real APNs or FCM send has not happened.** Both need credentials only the user can create
   (§U9). Every assertion here is against a mock that implements the documented contract.
2. **The deps change has not been built on macOS or Linux.** The recipe is portable in principle;
   only Windows has run it. This is the residual risk the spike named.
3. Whether a **force-stopped** Android app receives FCM at all.

### U9. What the user must create before any of this reaches a phone

1. Enrol in the **Apple Developer Program** ($99/yr) - the free Personal Team's ability to use
   Push Notifications is unverified and should not be planned against.
2. Reserve a **bundle id** and note the 10-character **Team ID**.
3. Create an **APNs Auth Key**, download the `.p8` *once* (Apple will not offer it again), and
   note the 10-character **Key ID**.
4. Create a **Firebase project**, add an Android app, and download a **service-account JSON** with
   the `firebase.messaging` scope.
5. Put both files somewhere the hub can read and set `apns.key_path` / `fcm.service_account_path`,
   with `apns.bundle`, `apns.key_id`, `apns.team_id` and `fcm.project_id` beside them.
   **Neither file ever belongs in this repository or in a release artifact**: whoever holds a
   team's `.p8` can push to every app that team signed (risk **R1**).
