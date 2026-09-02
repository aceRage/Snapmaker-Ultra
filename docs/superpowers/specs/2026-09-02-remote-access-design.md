# Remote access for the phone app - design and rollout

Date: 2026-09-02 · Branch: feat/ultra-preferences · Status: draft for review. Four read-only research passes (transport options with 2026 pricing and limits; hub-side login and 2FA; threat model and relay fallback; a verification sub-pass on DDNS, certificates, CGNAT, UPnP, WebRTC, TURN, relays and comparable projects), consolidated here with a reconciled recommendation. Every external fact in Parts A-D carries its source URL and check date.

Part D adds four facts that sharpen the recommendation: iPhone Safari has no `MediaSource` (only `ManagedMediaSource`, which the bundled go2rtc player already handles, so WebRTC is the safer video path on iOS anyway); iOS suspends WebRTC and WebSockets when Safari is backgrounded or the screen locks, so the page must re-signal on `visibilitychange`; Let's Encrypt IP-address certificates need HTTP-01 or TLS-ALPN-01 (inbound 80/443), so they do not rescue a CGNAT'd host; and a project-run relay cannot legally share one wildcard certificate key the way plex.direct historically did (Let's Encrypt SA v1.8 §3.4), so any relay would follow Nabu Casa's SNI-passthrough + per-device DNS-01 shape.

## 1. Summary and recommendation

**Goal.** Reach the hub (cameras, slicer instances, uploads, slicing, previews) from a phone outside the home LAN, with basic two-factor authentication, without asking the user to run or pay for hosting where a good free path exists; a low-friction paid option is acceptable if the free ones are worse.

**Recommendation, in order:**

1. **Fix two live bugs first, before any remote work** (they are exploitable today on the LAN-only build, from any web page the PC's browser visits): the hub configures go2rtc with `api.origin: "*"` on a fixed loopback port while go2rtc trusts loopback peers without authentication, which is a command-execution class (CVE-2024-29192 shape); and the hub's own `/hub/*` control routes accept cross-site POSTs because their only guard is "peer is loopback". Fixes: go2rtc `local_auth` with random credentials, `allow_paths` limited to the streaming routes, no fixed port; a per-process CSRF header on `/hub/*` and a **separate loopback-only admin listener** on an ephemeral port that no tunnel can front. Detail: Part C findings F1/F2, Part B "network policy".
2. **Hub-side login + TOTP (Part B, phases A-B).** Owner password (scrypt, PBKDF2 fallback; OpenSSL 1.1.1w is already linked), TOTP enrolled from the hub page by QR with 8 backup codes and replay protection, a 60-day trusted-device session cookie (HttpOnly, SameSite=Lax, Secure whenever HTTPS), rate limiting and lockout with an Unlock button on the PC, CSRF on every non-GET, security headers, the LAN QR link kept as a one-shot bootstrap that mints a session (so the bearer token never lives on in a URL). Roughly a week of work; it is transport-independent and is the "basic 2FA" asked for.
3. **Primary remote transport: Tailscale with `tailscale serve`** (Part A). Free Personal plan (6 users, unlimited devices, since 2026-04-08), WireGuard end-to-end so nobody in between sees plaintext, CGNAT-proof, a real auto-renewing certificate on a stable hostname, and it injects `Tailscale-User-Login` identity headers (stripping client copies) so the transport's own identity provider and MFA can be accepted as a full session on that path. Cost: the user installs Tailscale on the PC and on the phone (about ten one-time steps). The hub must bind loopback-only in that mode and drive the user's own Tailscale install (no embeddable client exists for C++ on Windows).
4. **Move camera video to WebRTC regardless of transport** (Part A §5). The bundled go2rtc already contains it; we switched it off. Video then goes peer-to-peer (public STUN, optional cheap TURN) instead of through a metered or ToS-constrained tunnel, and it also removes the risk of stalls when Tailscale falls back to its DERP relays. One config line, one passthrough entry, one `mode=webrtc` switch in the page, plus a low-bitrate / snapshot-only remote profile.
5. **No-app path (a plain link that works in any browser), chosen by experiment:** zrok (free 5 GB/day, stable HTTPS name, built-in OAuth, Apache-2.0 agent) or Tailscale Funnel (no second vendor, but beta, an undisclosed bandwidth cap, and an open WebSocket-over-HTTP/2 bug). **Lowest-friction paid option: Pinggy Pro at 3 USD/month** - persistent subdomain, unlimited bandwidth, and no binary to bundle because it is driven by the `ssh.exe` Windows already ships - conditional on verifying WebSocket support.
6. **Not recommended:** Cloudflare Tunnel (its video policy explicitly covers Tunnel public hostnames since 2026-08-25, a 100 MB body cap breaks 2 GiB uploads, 7 unautomatable onboarding steps, and Cloudflare sees plaintext), ngrok (about 71 minutes of video per month on free; its terms require written consent for our distribution model), TryCloudflare (testing only, often blocked), plain port-forwarding as a default (fails behind CGNAT for a large share of users; keep it as a power-user Phase 4), and a project-run relay (6-12 weeks plus permanent operations, and a lower security ceiling than Tailscale unless the end-to-end crypto is done very well - Part C).

**Answer to the user's constraints.** Free and no self-hosting: Tailscale Serve + WebRTC video, with the hub's own password+TOTP as the second line (or the tailnet identity alone if the owner prefers). Least friction even if paid: Pinggy Pro (3 USD/month) with the hub's password+TOTP doing all authentication. "Family": one shared owner password with a named, individually revocable trusted-device session per phone (Part B), or per-person Tailscale identities on the Serve path.

## 2. Experiments to run before committing (each under an hour)

1. Does go2rtc's MSE WebSocket (`/api/ws`) survive `tailscale serve` from a real iPhone/Android browser? Open issue tailscale/tailscale#20882 says WebSocket upgrades fail when the browser negotiates HTTP/2. If it reproduces: raw TCP passthrough with the hub terminating TLS, or WebRTC video (then only control traffic uses the WebSocket). **Gates the primary recommendation.**
2. Does zrok's public frontend pass WebSocket upgrades? (Undocumented.) Decides the free no-app path.
3. Does Pinggy carry WebSockets, and do its terms allow being driven from a bundled app? Decides the paid no-app path.
4. How often does a phone on cellular fall back to DERP on the Tailscale path (`tailscale ping` / `status`)? If often, WebRTC video becomes urgent rather than valuable.
5. Measure Tailscale Funnel's real sustained bandwidth on a 1080p stream (undisclosed cap).
6. Verify empirically that an HttpOnly 60-day cookie survives 8+ days on iOS Safari (ITP exemption for HTTP-set cookies is documented but old).

## 3. Reconciliation across the three passes

- The **admin listener split** appears in all three (Part A Phase 0 step 1, Part B "network policy", Part C B2). It is the single most important change; without it any local tunnel exposes the hub's control plane to the internet, and Tailscale direct connections are additionally blocked today because `is_private_v4` omits 100.64.0.0/10.
- **Identity on the Tailscale Serve path**: Part A proposes trusting `Tailscale-User-Login` (with an allow-list of logins) and retiring the token there; Part B designs hub-side login for every transport. Resolution: hub-side login is always available; on the Serve path a header-authenticated allow-listed login may be accepted as a `full` session (owner's choice in the hub page), because the transport already enforced an IdP with MFA. Funnel and every other transport supply no identity, so they always require the hub's own login + TOTP.
- **TLS**: never terminate TLS in the hub for the first phases; let Serve / zrok / Pinggy provide it and take the scheme from a configured, trusted `X-Forwarded-Proto`. Hub-side TLS (Part B phase E) only for the power-user direct path.
- **Video**: Part A's WebRTC move is what makes Part C's free-tier arithmetic survivable and what removes Part A's DERP-stall risk; it is transport-independent and should follow directly after Phase 0.
- **Uploads**: 2 GiB `MAX_UPLOAD` versus tunnel body caps (Cloudflare 100 MB, others unstated): keep the LAN limit, cap remote uploads (Part B suggests 512 MiB), and add chunked/resumable upload later.
- **Crypto**: everything in Part B is available from the already-linked OpenSSL 1.1.1w (HMAC/SHA/PBKDF2/scrypt/RAND_bytes/CRYPTO_memcmp) plus a 40-line base32; passkeys (WebAuthn) need a stable HTTPS hostname and a hand-rolled CBOR/COSE/ES256 verifier - optional, and only after the hostname decision.

## 4. Phased rollout (cross-referenced)

| Phase | Content | Source sections | Effort |
|---|---|---|---|
| 0a (now) | go2rtc `local_auth` + `allow_paths` + non-fixed port; `/hub/*` CSRF header; drop `token` from `/hub/info` | C F1, F2 | 1 day |
| 0b | Separate loopback admin listener; `Host` validation; bounded header read + timeouts + connection cap; CSPRNG token + constant-time compare; upload quota/GC/collision-proof names; instance spawn cap; explicit proxy allow-list instead of the `/api` catch-all | C B2-B4, B7-B9; A §7 | 2-3 days |
| A | Hub login: scrypt password, sessions + trusted-device cookie, `/login` `/logout`, middleware, routes to `/h/...`, go2rtc gate on the session, LAN QR bootstrap, security headers, remote upload cap | B phase A | 4-5 days |
| B | TOTP enrolment + backup codes + rate limiting + lockout + Unlock now | B phase B | 2 days |
| 1 | Tailscale Serve: detect and drive the user's install, loopback-only bind in remote mode, identity headers with an allow-list, `hub.json` / QR carry the `https://<machine>.<tailnet>.ts.net/` URL, 100.64/10 + IPv6 acceptance for direct connections | A Phase 1 | 2-3 days |
| 2 | WebRTC video: go2rtc `webrtc.listen`, `/api/webrtc` passthrough, `mode=webrtc` with MSE fallback, low-bitrate remote profile, snapshot-only mode, byte counter | A Phase 2, §7h | 2 days |
| 3 | No-app path by experiment: zrok or Funnel (free) and Pinggy (paid) as spawned children with the same loopback-proxy shape as Phase 1 | A Phase 3 | 2 days after the experiments |
| D | Device list + revoke + sign-out-everywhere, auth log + viewer, tray/balloon on lockout and new device | B phase D | 2 days |
| C (opt.) | Passkeys once one stable HTTPS hostname is settled | B phase C | 4-6 days |
| 4 (opt.) | Direct path: DDNS + Let's Encrypt (DNS-01 or the new 6-day IP certificates) + hub-terminated TLS + UPnP | A §4, B phase E | 3-5 days |
| - | Before any remote print-start route ships: the explicit route allow-list (0b) must exist | C recommendation 7 | - |

## 5. Decisions taken in this document

- Keep the LAN path (`http://<lan-ip>:13640/`) working unchanged as the default; remote is opt-in.
- Prefer inheriting identity from a transport that provides it (Tailscale) over building more of our own; but ship the hub's own password + TOTP anyway, because every no-app path needs it.
- Hidden-mode instances (the parallel plan) and remote access compose: a phone-initiated open still spawns a hidden instance; nothing here changes that.
- Test files, batch files and projects for these experiments live at `C:\Users\acesa\AppData\Local\Temp\snorca_hubtest\` (MAX_PATH).

---

# Part A - Transport options (research pass)

## Remote access - transport options

Reaching the Snapmaker-Ultra hub from a phone **outside** the home LAN. Windows-first,
single owner (possibly family), iOS Safari / Android Chrome, no vendor cloud we operate.
Repo was read only; nothing below has been applied. Branch `feat/ultra-preferences`.
Project licence is **AGPL-3.0** (`LICENSE.txt:1`), which matters for what we may bundle.

### Current state

Confirmed by reading the sources, with line numbers.

**One listener, plain HTTP, LAN only.**
`HUB_PORT = 13640` (`src/slic3r/GUI/RemoteHub.cpp:66`). `HubServer::bind(bool lan)`
(`RemoteHub.cpp:728-758`) opens a `tcp::v4()` acceptor and binds
`address_v4::any()` when phone access is on, `loopback()` otherwise -
so **IPv4 only, and 0.0.0.0:13640 while "phone access" is on**. `accept_loop`
(`RemoteHub.cpp:762-771`) is a blocking accept that detaches **one std::thread per
connection**. There is no TLS anywhere: `grep ssl::|SSL_CTX|asio::ssl` over
`RemoteHub.cpp` and `RemoteAccess.cpp` returns nothing.

**The peer gate is a private-IPv4 test, not authentication.**
`HubServer::serve` (`RemoteHub.cpp:1179-1186`) drops any connection whose peer is not
`is_private_v4`. That helper (`RemoteHub.cpp:119-124`) accepts `10/8`, `127/8`,
`172.16/12`, `192.168/16`, `169.254/16`, and returns false for every IPv6 address -
so today an IPv6 client cannot reach the hub at all.

**Routes.**
- `GO2RTC_PASSTHROUGH` = `/stream.html`, `/video-stream.js`, `/video-rtc.js`, `/api/ws`
  (`RemoteHub.cpp:68`), served at the **root** and gated only by the `rt` cookie equalling
  the token (`RemoteHub.cpp:1200-1207`). They exist at the root because go2rtc's player
  builds its WebSocket URL from `location.origin`.
- `/hub/*` - loopback peers only (`RemoteHub.cpp:1230-1234`).
- `/r/<token>/...` - the phone surface (`RemoteHub.cpp:1235-1254`); `/r/<token>/` serves
  `resources/web/orca/stream_center.html` and sets `Set-Cookie: rt=<token>; Path=/;
  SameSite=Lax` (`RemoteHub.cpp:1223-1225`).
- Per-instance API is proxied at `/r/<token>/i/<pid>/api/...` (`RemoteHub.cpp:1133-1134`).
  Each slicer instance's own listener binds **loopback on an ephemeral port**
  (`RemoteAccess.cpp:148`) and rejects non-loopback peers (`RemoteAccess.cpp:1466`), so
  the hub is the only way in. That part is already correct.

**The token.** `random_token()` (`RemoteHub.cpp:185-195`) is 14 characters from a
32-symbol alphabet = **70 bits**, but drawn from `std::mt19937_64` seeded with two
`std::random_device` calls - a Mersenne Twister, not a CSPRNG. `valid_token`
(`RemoteHub.cpp:180-183`) accepts 10-32 lowercase alphanumerics. There is **no rate
limiting and no lockout** on wrong tokens.

**WebSocket and proxying.** `tunnel()` (`RemoteHub.cpp:540-552`) connects to
`127.0.0.1:<port>`, replays the rewritten head and splices both directions with a thread
each; `force_close()` (`RemoteHub.cpp:506-522`) deliberately leaves the head untouched
when it sees an `Upgrade:` header, so **WebSocket upgrades already pass through
unmodified**. Every non-tunnelled response is `HTTP/1.1 ... Connection: close`
(`RemoteHub.cpp:490-500`) - **there is no keep-alive**, so each of the page's polls
(3 s / 5 s / 10 s / 15 s intervals, 1 s while slicing -
`stream_center.html:1110,1140,1141,1170,1781,2035`) costs a fresh TCP connection.

**Uploads.** `MAX_UPLOAD = 2 GiB` (`RemoteHub.cpp:70`), `Content-Length` is **required**
and chunked bodies are not handled (`RemoteHub.cpp:599-600`); API bodies are capped at
`MAX_API_BODY = 64 KiB` (`RemoteHub.cpp:69,1161`).

**No `Host` and no `Origin` handling at all.** The `Request` struct
(`RemoteHub.cpp:442-446`) keeps method/target/path/query/head/pending/cookies/file_name
and nothing else; grepping for `Host`/`Origin` in `RemoteHub.cpp` finds only unrelated
hits. So there is no virtual-host check, no WebSocket `Origin` check, and no
`X-Forwarded-For`/`Forwarded` support.

**The page.** `stream_center.html:214-217` derives remote mode from
`/^(\/r\/[a-z0-9]+)\/(?:index\.html)?$/` on `location.pathname` and prefixes every call
with it (`hubApi`, `instBase` at `:1019-1020`); line 220 re-sets the `rt` cookie.
Camera cells use a **root-relative** `'/stream.html?src=...&mode=mse'` iframe
(`stream_center.html:485`) in remote mode vs `http://127.0.0.1:<go2rtcPort>/...` locally
(`:400`). Only `localStorage` is used - no API on the page needs a secure context today
(`stream_center.html`, grep for `navigator.`/`crypto.`/`serviceWorker` finds none).

**The QR page.** `hub.html:41,47-66` renders `info.url` to a canvas client-side with
`qrcode.js`, so retargeting it at a new URL is a one-line change. `info.url` is built as
`http://<lan-ip>:<port>/r/<token>/` in three places
(`RemoteHub.cpp:706-708`, `982-983`, `1509`).

**Bundled go2rtc is more capable than we use.** `resources/tools/go2rtc/go2rtc.exe` is
**v1.9.14, MIT, bundled unmodified** (`LICENSE-NOTE.txt`), 19.7 MB. The hub writes a
config that explicitly disables everything except the API:
`webrtc:\n  listen: ""` and `rtsp`/`srtp` likewise (`RemoteHub.cpp:793-799`). But the
binary contains `/api/webrtc`, `ice_servers`, `candidates`, `RTCPeerConnection` **and
`ManagedMediaSource`** (verified by grepping the exe). So (a) iPhone Safari MSE already
works through the bundled player, and (b) go2rtc's own WebRTC path is one config line and
one passthrough entry away.

#### Two things that must be fixed before *any* proxy is put in front of the hub

1. **`/hub/*` is authenticated by nothing but `peer.is_loopback()`**
   (`RemoteHub.cpp:1230-1234`), and `handle_hub` (`RemoteHub.cpp:1049-1099`) exposes
   `GET /hub/info` - which returns the **token** in clear (`info_json`,
   `RemoteHub.cpp:694-709`) - plus `POST /hub/new` (spawn a slicer),
   `POST /hub/phone?on=&token=` (toggle access, **set the token**), `POST /hub/quit`,
   `POST /hub/instances/<pid>/window|quit`, and a 4 MB `POST /hub/state`.
   Every tunnel agent (cloudflared, `tailscale serve`, ngrok, nginx) connects to
   `127.0.0.1:13640`, i.e. **from loopback**, so the moment one is put in front, all of
   the above becomes reachable from the internet with **no token required**. This is a
   hard blocker, independent of which transport is chosen.
2. `is_private_v4` accepts `127/8`, and the `/r/<token>/` guard is
   `(!phone && !peer.is_loopback())` (`RemoteHub.cpp:1238`), so a loopback-fronted proxy
   reaches the phone surface even when phone access is switched **off**.

### Options

Bandwidth yardstick used throughout: **2 Mbps H.264 = 0.9 GB/hour**; 4 Mbps = 1.8 GB/hour.
A phone left watching a camera for an evening is ~3 GB. Every "free tier" below lives or
dies on that number.

#### 1. Tailscale

Tailscale is **two different transports wearing one brand**, and the distinction decides
everything. Docs moved from `tailscale.com/kb/NNNN/...` to `tailscale.com/docs/...` during
2026; both resolve. Current client is **v1.102.2 (2026-08-04)**.

**Free "Personal" plan** (https://tailscale.com/pricing, checked 2026-09-02): **"$0 / Free
forever"**, **up to 6 users**, **unlimited user devices** ("A user device is simply
anything that is not tagged as a resource! User devices are free and unlimited"), 50 tagged
resources, 3 ACL groups, MagicDNS, ACLs, **user approval and device approval both
included**, SSO with any IdP, basic device posture, and **Funnel**. Standard is
**$8/user/month**. This is the result of **Pricing v4 on 2026-04-08**, which retired
Personal Plus and moved Personal from **3 to 6 users** - the free tier got *better*
(https://tailscale.com/blog/pricing-v4, checked 2026-09-02). Secondary sources still
quoting "3 users / 100 devices" are stale.

**One caveat that matters to us as a vendor, not to our users:** the pricing FAQ says
Personal *"is only suitable for non-commercial use of Tailscale"*, and tailnets created
from public-domain email (Gmail, Apple, personal GitHub) are auto-enrolled in Personal
while custom-domain signups are treated as business use. Our end users on Gmail are fine;
**we could not run our own tailnet on Personal for a commercial deployment.**

**Identity is free and better than anything we would build.** Native IdPs are **Apple,
Google, GitHub, Microsoft (MSA/Office365/AD/Entra), Okta, OneLogin**, plus arbitrary custom
OIDC, and *"Tailscale supports the use of passkey authentication"*. Explicitly: *"Tailscale
does not handle authentication itself. Instead, you can enable MFA features in your single
sign-on identity provider"* (https://tailscale.com/kb/1013/sso-providers, checked
2026-09-02). Gotcha to warn users about: *"you cannot migrate your tailnet from/to GitHub
or Apple as an identity provider."*

##### 1a. `tailscale serve` - app on the phone. This is the good one.

`tailscale serve --https=443 http://127.0.0.1:13640` reverse-proxies the hub over HTTPS on
the node's MagicDNS name, with an automatically provisioned and **auto-renewing** TLS
certificate. Traffic rides the tailnet (WireGuard, direct peer-to-peer where NAT allows).
**No bandwidth limit is documented for Serve.**

The feature that makes this decisively better than every other option in this document:
**Serve injects authenticated identity headers** - `Tailscale-User-Login`,
`Tailscale-User-Name`, `Tailscale-User-Profile-Pic` - and **strips any client-supplied
copies to prevent spoofing** (https://tailscale.com/kb/1312/serve, checked 2026-09-02).
That means **we can delete our token scheme entirely on this path** and trust a header. The
docs' own security note is a direct instruction to us: *"it's best practice to only have
the service listen on localhost. Otherwise, any user that can call your service directly…
could trivially provide their own values for these HTTP headers."* So in Serve mode the hub
must bind **loopback only**, not `0.0.0.0`.

##### 1b. `tailscale funnel` - no app on the phone, and three reasons not to rely on it

Funnel exposes the service to the public internet with no Tailscale account needed by the
visitor, and is *"available for all plans"* including free
(https://tailscale.com/kb/1223/funnel, checked 2026-09-02). But:

1. **It is still beta** - no GA announcement appears anywhere in the changelog through
   2026-08-26, and it had a **connection-failure regression fixed only in v1.102.2
   (2026-08-04)**.
2. **"Traffic sent over a Funnel is subject to non-configurable bandwidth limits."** The
   number is published nowhere - not in the docs, not the changelog, and not in the
   open-source client (the limit lives in Tailscale's closed-source relay fleet). A
   Tailscale employee's public gloss: "there is a bandwidth limit, it's a funnel, not a
   hose" (https://news.ycombinator.com/item?id=35375794, checked 2026-09-02).
3. **Three open WebSocket bugs, one of which is exactly our path.**
   github.com/tailscale/tailscale **#20882** (opened 2026-08-15): *"WebSocket handshakes
   fail when the client negotiates HTTP/2 (RFC 8441 extended CONNECT not proxied)… The
   backend receives a plain GET and replies 426 Upgrade Required."* HTTP/2 over ALPN **is
   the browser default**, so this hits go2rtc's `/api/ws` head-on; it works when forced to
   HTTP/1.1 or via `--tcp` raw passthrough. Also **#18827** (WebSocket connections drop
   every 10-40 s with code 1001) and **#18651** (query parameters stripped from WebSocket
   upgrades - which would break any token-in-query scheme). All three are user reports, not
   Tailscale-confirmed, but they are independent and recent (all checked 2026-09-02).

Other Funnel constraints: **only ports 443, 8443 and 10000**; public DNS propagation *"can
take up to 10 minutes"*; the same port cannot be Serve and Funnel simultaneously; and
**Funnel supplies no identity headers** - *"Funnel traffic, which is publicly available,
does not include identity headers"* - so on this path our own authentication carries the
entire burden. The Acceptable Use Policy (last updated 2025-06-30) says nothing about
video, streaming or bandwidth; the only applicable clause forbids *"creating an undue
burden on the Tailscale Solution"*, with full discretion to suspend.

**Privacy is the best of any hosted option here.** Funnel is SNI passthrough: *"Funnel
relay servers do not decrypt the traffic between public devices and your device. This
ensures that Tailscale cannot access or read any content."* TLS terminates on **our**
Windows box, which holds the Let's Encrypt private key. Compare Cloudflare, ngrok, zrok,
Pinggy and Microsoft, all of which terminate TLS on their own infrastructure and see
plaintext. (Caveat: Tailscale controls the DNS records, so this is an operational rather
than cryptographic guarantee.)

##### Certificates

Enable MagicDNS, then the tailnet-wide HTTPS toggle - acknowledging that **machine names
are published to the public Certificate Transparency ledger**; the docs warn *"Do not
enable the HTTPS feature if any of your machine names contain sensitive information."*
`tailscale cert <machine>.<tailnet>.ts.net` then issues a real Let's Encrypt certificate
via DNS-01 (or **TLS-ALPN-01 since v1.102.1, 2026-08-03**, when Funnel is live on :443).
Key details: the **ACME account key is per-device, not per-tailnet**, and *"Tailscale never
sees them"*; a node can only get a certificate for **its own** name; certificates issued by
`tailscale cert` are 90 days and *"you are responsible for renewing"*, but certificates
managed internally by tailscaled - i.e. via `serve`/`funnel` - **auto-renew**, including
proactively on idle servers. Let's Encrypt rate limits can leave you *"waiting 34 hours"*
(https://tailscale.com/kb/1153/enabling-https, checked 2026-09-02). **Use `serve`, do not
call `tailscale cert` and terminate TLS ourselves.**

##### CGNAT, DERP, and why video may still stall

Tailscale is immune to CGNAT for reachability. But the fallback path matters. Connection
type is deterministic (https://tailscale.com/docs/reference/device-connectivity, checked
2026-09-02): easy-NAT to easy-NAT is direct; **easy-to-hard and hard-to-hard are relayed**,
as is anything with UDP blocked. **A phone on mobile-carrier CGNAT is frequently a hard
NAT**, so DERP fallback is a realistic steady state for exactly our scenario, not an edge
case.

DERP is end-to-end encrypted - *"it's impossible for a DERP server to decrypt your
traffic"* - and unmetered (no data-transfer line item exists on any plan). But it is
**rate-limited and TCP-based**. Tailscale's own words
(https://tailscale.com/blog/nat-traversal-improvements-pt3-looking-ahead, 2025-10-24,
checked 2026-09-02): *"Being TCP-based, it incurs extra latency… If a connection has packet
loss, TCP will introduce retransmission delays… Additionally, DERP servers enforce rate
limits and fair usage policies that can throttle throughput."* The rate-limit mechanism is
visible in the open-source `derp/derpserver` (a per-client token bucket,
`PerClientRateLimitBytesPerSec`, commented *"Rate limiting is experimental and subject to
change"*) but the production values are undisclosed. The docs' own example shows **50-282 ms
RTT over DERP vs ~35 ms direct**. TCP-over-TCP head-of-line blocking is poor for continuous
H.264. **Direct connection: video is fine. DERP fallback: expect stalls.** The often-quoted
"90% direct" figure comes from a **2020** blog post and is an author's estimate, not a
measurement. Mitigations: enable UPnP/NAT-PMP or open UDP **41641**; Tailscale's own answer
is **Peer Relays** (now GA and available on Personal), but that needs a third always-on
well-connected device, which a two-endpoint home setup does not have.

##### Embedding in C++: there is no viable path on Windows today

This is the finding that most constrains implementation.

- **`tsnet` is Go-only** (https://pkg.go.dev/tailscale.com/tsnet, checked 2026-09-02).
- **`libtailscale` (the C API) does not support Windows.** It is alive and BSD-3-Clause
  (last push 2026-08-31, 335 stars), and gives `tailscale.h` with
  `tailscale_new/start/up/listen/dial`. But the implementation uses POSIX primitives -
  `syscall.Socketpair(AF_LOCAL)`, `SCM_RIGHTS` fd passing, `golang.org/x/sys/unix` - the
  header documents a connection as *"a pipe(2)"*, the Makefile targets only local/darwin/
  iOS, CI runs **ubuntu-latest only**, and **PR #25 "Fix/windows port finishing" has been
  open and unmerged since 2024-01-22**.
- **`tailscale-rs`** was announced **2026-08-31** with C, Python and Elixir bindings and
  Windows CI targets - but it is *"experimental (pre-alpha)… more direction than production
  code"*, and its README lists as not-yet-implemented: **direct connections (NAT traversal,
  STUN, Disco)** - so all traffic goes through DERP - and as unsupported: **Funnel, Serve,
  HTTPS certificates, MagicDNS**. Unusable for us today.
- **We cannot redistribute the official Windows client.** The Windows GUI is closed source
  (https://tailscale.com/opensource) and the ToS (last updated 2026-08-25) §2.1 grants a
  *"non-transferable"* licence while §2.3 forbids *"copy, reproduce, create derivative
  works from… sell, resell, rent or lease"*. We could build the BSD-3 `tailscaled.exe` and
  `tailscale.exe` from source without the GUI, or - far more sensibly - **ask the user to
  install Tailscale themselves and simply drive it.**

**So the design is: detect and drive the user's own Tailscale.** Silent install is possible
if we want to offer it (`msiexec /i tailscale-setup-<ver>-amd64.msi /quiet`, with
`TS_NOLAUNCH`, `TS_UNATTENDEDMODE=always`, `TS_ALLOWINCOMINGCONNECTIONS`), and
`tailscale up --auth-key=… --hostname=… --unattended` scripts the login (note *"Flags are
not persisted between runs"*). Detection and identity come from `tailscale status --json`
(`Self.DNSName`, `Self.TailscaleIPs`), `tailscale whois <ip>`, or `tailscale whoami` (added
v1.102.1). For a no-subprocess path there is the **LocalAPI over the Windows named pipe**
`\\.\pipe\ProtectedPrefix\Administrators\Tailscale\tailscaled`, speaking plain HTTP/1.1 at
`/localapi/v0/{status,whois,whoami,cert-domains}` with **`Host: local-tailscaled.sock`** and
**no `Referer` or `Origin` header** (otherwise 403), connecting at **SecurityIdentification**
impersonation level; the pipe ACL grants Built-in Users, so a non-elevated process can
connect, but it must run as the same Windows user that owns the Tailscale session.

**Important consequence for our peer check:** with `serve` or `funnel`, tailscaled proxies
to `http://127.0.0.1:13640`, so **the hub always sees `127.0.0.1` and never a `100.x`
address**. CGNAT-range detection (`100.64.0.0/10`, and IPv6 `fd7a:115c:a1e0::/48`) is only
useful in the *direct-connect* model where the phone dials `100.x.y.z:13640` itself. And
because Serve arrives from loopback, the `/hub/*` exposure described above is a **hard
prerequisite**, not a nicety.

##### User setup, counted

*Serve path - 10 steps (6 PC, 4 phone):* install Tailscale on the PC; log in with an
existing Google/Microsoft/GitHub/Apple account; enable unattended mode so it survives
logout; turn on our "Remote access" toggle (the hub runs `tailscale serve`); wait for the
first certificate; read the URL. Then on the phone: install Tailscale, sign in with the
same account, approve the VPN profile prompt, scan our QR code.
*Funnel path - 10 steps, but only 1 on the phone:* the same PC steps plus enabling HTTPS
and adding the `funnel` node attribute (the `tailscale funnel` CLI does both via an
interactive consent page), then up to 10 minutes for DNS. The phone just opens the URL in
any browser - **and so can anyone else who learns it.**

##### Risk

Funnel is beta with an undisclosed, unilaterally changeable bandwidth cap and open
WebSocket bugs. The free tier is generous and moving in the right direction (v4 raised it),
and Tailscale is well funded - a **$160M Series C led by Accel**
(https://tailscale.com/blog/series-c, checked 2026-09-02; the widely reported ~$1.45B
valuation and the Border0 acquisition are third-party reporting I could not verify against
a first-party source). But "free forever" is marketing, not contract: ToS §1.14 reserves
the right to *"modify our Plans and any applicable service fees at any time."*

#### 2. Cloudflare Tunnel (cloudflared) + Cloudflare Access

**Ruled out for video as of August 2026.** Cloudflare's video-delivery policy page gained a
section headed "Cloudflare Tunnel" (page last updated **2026-08-25**), which says public
hostname routes on Free/Pro/Business "traffic is subject to the terms described on this
page", and that the restriction "does not apply to private network routes"
(https://developers.cloudflare.com/fundamentals/reference/policies-compliances/delivering-videos-with-cloudflare/,
checked 2026-09-02). The governing wording moved out of the old ToS §2.8 into the
Service-Specific Terms (last updated **2026-06-02**,
https://www.cloudflare.com/service-specific-terms-application-services/, checked
2026-09-02): you must buy Stream/Images/Developer Platform "in order to serve video and
other large files via the CDN", and Cloudflare "reserves the right to disable or limit your
access". Enforcement is discretionary and per-customer, with an email notice - there is no
numeric threshold to engineer against. The sanctioned carve-out (private network routes)
requires the WARP / Cloudflare One client on the phone, which defeats the point.

Everything else about it is good, which makes the above the more frustrating:

- **Free tier is generous**: "50 user limit", "$0 forever", ZTNA/Access, Gateway, WARP and
  the Tunnel connector all ticked; no uptime SLA; 24 h log retention; $7/user/month above
  50 (https://www.cloudflare.com/plans/zero-trust-services/, checked 2026-09-02).
- **Auth is the best in this survey**: one-time PIN by email with no IdP at all (PIN
  single-use, expires 10 minutes,
  https://developers.cloudflare.com/cloudflare-one/identity/one-time-pin/, checked
  2026-09-02); social/corporate IdPs; service tokens via `CF-Access-Client-Id`/`-Secret`
  headers with a "Service Auth" policy; per-path scoping, though **"Port numbers are not
  supported in Access application paths"**
  (https://developers.cloudflare.com/cloudflare-one/access-controls/policies/app-paths/,
  checked 2026-09-02). New Zero Trust orgs now default to Cloudflare's own IdP rather than
  OTP (changelog 2026-05-19).
- **WebSockets work**: "WebSockets are supported on all Cloudflare plans"
  (https://developers.cloudflare.com/network/websockets/) and "Cloudflare Tunnel has full
  support for Websockets" (Tunnels FAQ), both checked 2026-09-02. The `CF_Authorization`
  cookie rides a browser upgrade handshake because a WS upgrade is an HTTP request and
  browsers attach same-origin cookies - **but no Cloudflare page states this explicitly**;
  it is sound-but-undocumented. Browser JS cannot set headers on `new WebSocket()`, so a
  browser client must use the cookie path. The often-quoted **100 s WebSocket idle timeout
  is not in current Cloudflare docs** (verified against the rendered page and the raw
  `.mdx`); the docs say only that idle connections are closed and that Enterprise can
  request a custom value. Design a ping/pong heartbeat regardless.
- **Hard 100 MB request-body cap on Free and Pro** (200 MB Business, 500+ Enterprise)
  (https://developers.cloudflare.com/cache/concepts/default-cache-behavior/, checked
  2026-09-02). Our `MAX_UPLOAD` is **2 GiB** (`RemoteHub.cpp:70`), and the documented
  escape hatch - a grey-clouded DNS-only record - is structurally unavailable, because a
  tunnel hostname is a CNAME to `<UUID>.cfargotunnel.com` which only resolves proxied. So
  uploads would have to be chunked client-side.

**Domain ownership is mandatory** for a named tunnel with a public hostname: prerequisites
are "An active domain on Cloudflare" and a full or partial (CNAME) setup
(https://developers.cloudflare.com/cloudflare-one/access-controls/applications/http-apps/self-hosted-public-app/,
checked 2026-09-02). Cloudflare Registrar sells "at cost… no markup"
(https://developers.cloudflare.com/registrar/faq/, checked 2026-09-02); Verisign's
wholesale .com fee rises from $10.26 to $10.97 on **2026-11-01**
(https://domainnamewire.com/2026/04/23/breaking-verisign-raising-wholesale-com-prices/,
checked 2026-09-02), so ~$10.4/yr now, ~$11.2/yr after. A free DDNS subdomain cannot be
used - the zone must be in the user's own account.

**Setup cost: ~14-18 steps, of which 7 cannot be automated** (create account, buy domain,
add zone, delegate nameservers, wait for zone activation - unbounded, enable Zero Trust
with a team name, provide payment details). Our app could only automate tunnel creation
onward, and only with a Cloudflare API token the user has already minted.

**Bundling is legally fine**: cloudflared is **Apache-2.0**
(https://raw.githubusercontent.com/cloudflare/cloudflared/master/LICENSE, checked
2026-09-02), latest **2026.8.3** released 2026-08-31. But `cloudflared-windows-amd64.exe`
is **54,841,128 bytes (~52 MiB)** (GitHub releases API, checked 2026-09-02) on top of our
existing 19.7 MB go2rtc, there is **no auto-update on Windows** ("You will need to perform
manual updates"), support is limited to versions within one year, and **2026.8.0/8.1
introduced origin path-normalisation regressions fixed in 2026.8.2** - pin >= 2026.8.2.
Running it as a child process needs no admin; only service install does. A
*remotely-managed* tunnel runs from a token alone (`--token`, or `TUNNEL_TOKEN`), avoiding
the browser OAuth of `cloudflared tunnel login`. Note the Tunnel API deprecations
**effective 2026-10-05** (CIDR route endpoints retired for `route_id`; `connections` array
removed).

**Privacy: Cloudflare sees plaintext.** "Cloudflare must decrypt traffic in order to cache
and filter malicious traffic" (https://developers.cloudflare.com/ssl/troubleshooting/faq/,
checked 2026-09-02). That is a disclosure item for a product streaming the inside of
someone's house. It is also structural, not incidental - Access evaluates its JWT at the
edge on every request.

**Latency**: every byte hairpins phone -> PoP -> PoP holding the cloudflared connection ->
PC and back, even when the phone is in the same room. Cloudflare publishes no Tunnel
latency benchmark; third-party figures of ~15-45 ms near a PoP are unverified.

**TryCloudflare quick tunnels** (`cloudflared tunnel --url`) - 2 steps, no account, no
domain, no cost, random `*.trycloudflare.com` hostname. Disqualified as a product
transport by the docs themselves: "Quick Tunnels are intended for testing and development
only", "We don't guarantee any SLA or uptime", a **hard 200 in-flight request cap**
returning HTTP 429, **no Server-Sent Events**, and it refuses to run if a `config.yaml`
exists in `.cloudflared`
(https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/do-more-with-tunnels/trycloudflare/,
checked 2026-09-02). The hostname changes on every restart (universally reported, not
stated in that page). **Access cannot be applied** - Access requires the hostname to be in
a zone you own, and `trycloudflare.com` is Cloudflare's - so the URL is world-reachable by
anyone who learns it, protected only by our 70-bit token. Separately,
`*.trycloudflare.com` is heavily abused for malware/phishing delivery and some security
vendors and DNS filters block the domain outright
(https://cofense.com/blog/how-cloudflare-services-are-abused-for-credential-theft-and-malware-distribution,
checked 2026-09-02), so some users' phones simply would not resolve it.

#### 3. ngrok and the other public-URL tunnels

**ngrok - disqualified twice over.**
Free tier (https://ngrok.com/docs/pricing-limits/free-plan-limits/, checked 2026-09-02):
**1 GB data transfer/month**, **20,000 HTTP requests/month**, 3 online endpoints, 1 user,
**1 randomly-named static dev domain** (`*.ngrok-free.dev`, permanently assigned -
https://ngrok.com/blog/free-static-domains-ngrok-users, checked 2026-09-02), 5 Traffic
Policy rules per policy, 3 OAuth/OIDC monthly active users, and **TLS endpoints "Not
available"**. Endpoints have **no session timeout** - the widely repeated "2-hour cap" is
third-party folklore contradicted by the official page. 1 GB/month is **~71 minutes of
2 Mbps video per month**, and 20k requests is ~28 hours of a page that polls every 5 s.
Pay-as-you-go overage is $0.10/GB + $0.02/endpoint-hour, i.e. **~$0.09 per hour of
streaming** on top of $20/month. Plans are Free / Hobbyist $10 / Pay-as-you-go $20 /
Enterprise (https://ngrok.com/pricing, checked 2026-09-02).
The **interstitial still exists on free** in 2026 - "To deter phishing attacks, ngrok shows
an interstitial page in front of all HTML browser traffic on the free tier"
(https://ngrok.com/docs/errors/err_ngrok_6024/, checked 2026-09-02); bypasses are an
`ngrok-skip-browser-warning` header or a custom User-Agent, **neither of which a browser
can apply to the initial document navigation**. WebSockets are supported with no config
(https://ngrok.com/docs/universal-gateway/http/, checked 2026-09-02). Agent SDKs exist for
Go, Rust, Python, JS and Java - **no C or C++ binding**.
The decisive problem is the **ToS**: distributing the agent to third parties is permitted
only where "you, not your customer, maintain an account with ngrok", and distributing to
"customers who maintain their own accounts with ngrok" requires "ngrok's **prior written
consent** (which it may grant or withhold in its discretion)" (https://ngrok.com/tos,
checked 2026-09-02). Shipping ngrok inside the slicer for users with their own free
accounts is exactly the clause needing consent. Also, ngrok terminates TLS and sees
plaintext on Free, since agent-terminated TLS endpoints are a paid feature.

**zrok (NetFoundry / OpenZiti) - the strongest free tier found.**
FREE plan: **$0/month, 5 daily GB, 25 environments, 50 share backends (ephemeral or
reserved), 50 private access frontends, "No Interstitials with Verified Credit Card"**
(https://zrok.io/pricing/, checked 2026-09-02). **5 GB/day is ~5.5 hours/day at 2 Mbps** -
about 150x ngrok's monthly allowance. Reserved shares give a **stable public HTTPS URL**
`https://<uniquename>.share.zrok.io` (4-32 lowercase alphanumerics, globally unique -
https://docs.zrok.io/docs/concepts/sharing-reserved/, checked 2026-09-02). **Auth is built
in**: `--basic-auth`, plus OAuth with Google or GitHub via `--oauth-provider` /
`--oauth-email-domains` (https://blog.openziti.io/the-zrok-oauth-public-frontend, checked
2026-09-02). Agent is **Apache-2.0** (https://github.com/openziti/zrok, checked
2026-09-02), so bundleable; SDKs are Go/Python/Node - **no C/C++**, so we would spawn the
binary. Self-hosting the frontend is supported with no built-in limits. Immune to CGNAT.
Caveats: the `share.zrok.io` frontend **terminates TLS**, so NetFoundry sees plaintext;
removing the anti-phishing interstitial requires the user to **register a credit card**
(at $0, but still friction); and **WebSocket passthrough through the public frontend is
not documented anywhere** - it is an HTTP reverse proxy so it almost certainly works, but
**this must be tested before committing**, because our whole video path is a WebSocket.

**Pinggy - the cheapest credible paid option.** Free is unusable (60-minute tunnel timeout,
random subdomain that changes on reconnect). **Pro is $3.00/month with "unlimited
bandwidth"**, persistent/custom subdomains, and built-in HTTP basic auth, bearer/key auth
and IP allowlisting; HTTPS with automatic Let's Encrypt certs (https://pinggy.io/ and
https://pinggy.io/compare/pinggy-vs-ngrok/, checked 2026-09-02). The elegant part for us:
**there is no agent binary to bundle** - Pinggy is driven by a plain
`ssh -p 443 -R0:localhost:13640 ...`, and Windows 10/11 ship the OpenSSH client in-box, so
the hub just spawns `ssh.exe`. Unverified: WebSocket support (only third-party claims), the
annual price, the free concurrent-tunnel limit, and Pinggy's ToS on bundling into a
third-party app. Pinggy terminates TLS and sees plaintext.

**Microsoft Dev Tunnels - the surprise entry.** Free with a Microsoft, Entra ID or GitHub
account, no Azure subscription; **persistent URLs** like
`https://<tunnelid>-13640.devtunnels.ms`; and uniquely in this survey **WebSocket support
is explicitly documented** ("Tunnel ports using the HTTP(S)/WS(S) protocols can be
accessed directly via the provided web-forwarding url"). Access is gated on the user's
Microsoft/Entra/GitHub identity, or `--allow-anonymous`, or bearer tokens via
`X-Tunnel-Authorization`. The anti-phishing interstitial is skipped for non-GET, for
requests whose `Accept` lacks `text/html`, or with an `X-Tunnel-Skip-AntiPhishing-Page`
header. But it is **public preview with no SLA and "not for production workloads"**, and
Microsoft terminates TLS at its ingress and rewrites headers, so it sees plaintext
(https://learn.microsoft.com/en-us/azure/developer/dev-tunnels/overview and
.../security, checked 2026-09-02). **Bandwidth quotas are not published** and whether the
`devtunnel` CLI may be redistributed inside a third-party app is **unverified**.

**Ruled out quickly.**
*localtunnel*: the public `localtunnel.me` server now makes the **visitor type the tunnel
host's public IPv4 address as a password** (github.com/localtunnel/localtunnel issues
##598, #671, #654, checked 2026-09-02) - unusable from a phone on cellular, and broken by
any WAN IP change; last code push 2025-08-29.
*bore*: **raw TCP only, no TLS** - `https://bore.pub/` refuses connections on 443 (checked
2026-09-02). You get `bore.pub:<random-port>`, which can never carry a browser-trusted
certificate. MIT, self-hostable.
*frp*: Apache-2.0, excellent (HTTP/HTTPS proxies carry WebSockets, token and OIDC auth,
per-proxy bandwidth limits) but there is **no hosted service** - it needs a VPS with a
public IP plus a domain.
*Pangolin*: has a **hosted free "Basic" tier** ($0, no card, 5 users, 5 sites, 15 public
resources) with the richest built-in auth of any option (SSO/OIDC, RBAC, PIN codes,
passcodes, email OTP, geoblocking, allow-lists) (https://pangolin.net/pricing and
https://github.com/fosrl/pangolin, checked 2026-09-02). Two blockers: **bandwidth limits
are not stated anywhere on the pricing page** (a third-party blog claims 25 GB/month -
unverified), "provided domains" are a Team-and-above feature so Basic likely still needs
your own domain, and the Newt client is **AGPLv3 + a commercial licence** whose free grant
is limited to personal/hobbyist use and businesses under $100K revenue.
*serveo* (outages reported on its own Google Group in Aug 2026), *pgrok* (repo archived),
*telebit* (could not confirm it still exists), *holesail* (P2P - needs the app on the
phone too).

**Mesh VPNs that are not Tailscale.** All three fail the same two ways: they need an app on
the phone, and they give no browser-trusted HTTPS origin, so the page stays a non-secure
context.
*ZeroTier*: free Personal is **10 devices, 1 network** (https://www.zerotier.com/pricing/,
checked 2026-09-02) - the "25 nodes" figure still circulating is stale. Essential is
$18/month. L2 overlay, no DNS name, no certificate. MPL-2.0 core plus a source-available
`nonfree/` directory.
*NetBird*: free is **5 users / 100 machines** (https://netbird.io/pricing, checked
2026-09-02); Team is EUR6/user/month. It has a **Reverse Proxy with automatic Let's Encrypt
TLS and SSO/password/PIN auth** and a `netbird expose` CLI - but it is "currently in beta
and only available for self-hosted deployments. Cloud support is coming soon"
(https://docs.netbird.io/manage/reverse-proxy, checked 2026-09-02). **Worth re-checking in
six months**; if that lands on the cloud free tier it becomes a serious contender.
BSD-3-Clause except `management/`, `signal/`, `relay/` which are AGPLv3.
*Twingate*: Starter is free for **5 users, 1 admin, 5 devices/user, 10 remote networks, 50
resources**; Home is $15/month (https://www.twingate.com/pricing, checked 2026-09-02).
Proprietary client - cannot be bundled - and no clientless/browser access option.


#### 4. Port forwarding + dynamic DNS + Let's Encrypt, hub terminates TLS

The only option with **no third party in the data path at all**, and the only one whose
bandwidth is limited solely by the user's own uplink. It fails for a large fraction of
users for reasons outside our control.

**Certificates got easier in 2026.** Let's Encrypt made **IP address certificates and
6-day certificates generally available on 2026-01-15**; both have a **160-hour (just over
six days)** lifetime, both IPv4 and IPv6 are supported, and the client must "select the
'shortlived' certificate profile"
(https://letsencrypt.org/2026/01/15/6day-and-ip-general-availability, checked 2026-09-02).
The stated reason is directly relevant: "IP addresses are more transient than domain names,
so validating more frequently is important." Separately, ordinary certificate lifetimes are
dropping from 90 to 45 days (https://letsencrypt.org/2025/12/02/from-90-to-45, checked
2026-09-02). Two consequences: a box with a **static public IP can now get a publicly
trusted certificate with no domain at all**, and **renewal automation is mandatory** - a
manual step every six days is not a product.

What this does *not* solve: CA/Browser Forum Baseline Requirements forbid publicly trusted
certificates for reserved IP ranges, so **no CA will certify `192.168.x.x`**. (That is my
own reading of the BRs rather than a page I fetched today - verify before relying on it.)
The LAN path therefore stays HTTP, or needs a private CA.

**Free DNS with DNS-01.** DuckDNS and deSEC are both free and both supported by `acme.sh`;
**deSEC is the better choice** because DuckDNS has had wildcard-certificate problems with
`acme.sh` (checked 2026-09-02). deSEC is a nonprofit with a proper REST API for TXT
records. One caveat I could not settle today and which matters: Let's Encrypt rate limits
are per *registered domain*, so whether every `*.duckdns.org` user shares one bucket
depends on `duckdns.org` being on the **Public Suffix List** - historically free-DNS
providers have hit exactly this wall (Let's Encrypt community thread "Exempt FreeDNS
domains from the rate limit", checked 2026-09-02). **Verify PSL status before shipping.**

**ACME in C++ is the weak link.** There is no mature C or C++ ACME client to link against;
practical options are shelling out to `win-acme` or `lego`, or implementing ACME over the
libcurl + OpenSSL we already have (a few hundred lines: JWS with ES256, order, DNS-01
challenge, CSR, download). With six-day certificates this code has to be genuinely
reliable.

**CGNAT is the killer.** Port forwarding needs a routable public IPv4, and in 2026 that is
not the default: **mobile networks are effectively 100% CGNAT**, fixed wireless and
satellite (Starlink, T-Mobile Home Internet) use it by default, and cable/DSL providers
increasingly deploy it residentially (checked 2026-09-02). IPv6 helps but does not rescue
it: global adoption is around **50-60%**, **mobile networks 95%+**, major ISPs 70%+, and
**dual-stack is the norm rather than IPv6-only** (checked 2026-09-02). The good case -
phone on cellular (IPv6-capable) reaching a home box with a routable IPv6 prefix - often
works, but it depends on the home ISP delegating a prefix *and* the router not firewalling
inbound v6, and it silently fails for anyone whose home line is v4-only behind CGNAT. Our
hub is **IPv4-only today** (`RemoteHub.cpp:728-758` opens `tcp::v4()`; `is_private_v4`
rejects every v6 address), so IPv6 support is a prerequisite for even attempting this.

**UPnP/NAT-PMP** could open the port automatically; `miniupnpc` and `libnatpmp` are the
standard C libraries. I could not re-verify their 2026 maintenance status or current
router-default statistics - the web search budget was exhausted - so **treat that as
unverified**. What is certain is that UPnP is disabled by default on a meaningful share of
routers for security reasons, and it cannot help at all behind CGNAT.

**Authentication would be entirely ours**: login, session cookies, rate limiting, 2FA. See
section 7 - TOTP is cheap, WebAuthn is not.

**Verdict:** the right *fallback* for users who have a real public IP and are comfortable
with their router, and a good power-user escape hatch. Not a default, because a large
fraction of users cannot use it and the failure mode is invisible to them.

*Related sub-option:* a **private CA** installed on the phone would give the LAN path HTTPS
with no third party. On iOS this is a deliberate two-step - install the profile, then
separately enable it under Settings > General > About > **Certificate Trust Settings**,
because a manually installed certificate "isn't automatically trusted for SSL"
(https://support.apple.com/en-us/102390, checked 2026-09-02); only MDM/Apple Configurator
deployment auto-trusts. Too much friction for a consumer feature, and it fixes only the
certificate, not reachability.

#### 5. WebRTC from the phone browser

**The cheapest thing we could ship, because most of it is already in the box.** The bundled
go2rtc v1.9.14 already contains `/api/webrtc`, `ice_servers`, `candidates` and
`RTCPeerConnection` (verified by grepping `go2rtc.exe`), and go2rtc "automatically detects
your external address with a STUN server" and accepts host candidates via a `candidates:`
list including the `stun:8555` form (https://go2rtc.org/internal/webrtc/, checked
2026-09-02). We disable all of it: `webrtc:\n  listen: ""` (`RemoteHub.cpp:798`), and both
call sites force `mode=mse` (`stream_center.html:400,485`). **Enabling WebRTC for the
camera path is roughly one config line, one entry in `GO2RTC_PASSTHROUGH`
(`RemoteHub.cpp:68`) for `/api/webrtc`, and a `mode=webrtc` switch in the page.**

What that buys: direct peer-to-peer video from the PC to the phone browser with NAT
traversal, **no app on the phone** (iOS Safari and Android Chrome both do WebRTC natively),
no bandwidth meter, and no third party in the media path when hole punching succeeds. It
also sidesteps every "no video on our free tier" term in this document, because the video
never enters the tunnel.

What it does not buy: **it only moves the video.** The page, the JSON API, uploads and the
G-code preview are plain HTTP and still need a way in. And WebRTC in a browser requires a
**secure context**, so the page must already be on HTTPS - WebRTC is an *optimisation on
top of* a transport, not a replacement for one.

**Signalling** must be reachable by both ends. If the page is already served through a
tunnel, signalling rides the same tunnel and costs nothing extra - the neat version of this
design. A standalone signalling service is also cheap: **Cloudflare Workers + Durable
Objects have a free tier of 100,000 requests/day and 313,000 GB-s/day**, with WebSocket
Hibernation so idle connections accrue no duration charge, and "There is no charge for
outgoing WebSocket messages"
(https://developers.cloudflare.com/durable-objects/platform/pricing/, checked 2026-09-02).

**TURN is where it costs money.** When hole punching fails the media must be relayed. Free
options exist: Open Relay (metered.ca) gives **20 GB/month free with a verified card, 0.5
GB/month without** (https://www.metered.ca/tools/openrelay/, checked 2026-09-02), and
**Cloudflare Realtime TURN is $0.05 per real-time GB with a 1,000 GB free tier**
(https://developers.cloudflare.com/realtime/turn/, checked 2026-09-02) - at 0.9 GB/hour
that is roughly 1,100 hours of relayed video free, then about $0.045/hour. Genuinely
affordable. I could **not** verify a current figure for the fraction of connections needing
TURN; the commonly cited 8-20% range is unsourced here.

**In C++**, `libdatachannel` is the right library if we ever need our own data channels
rather than go2rtc's media: C/C++ with C bindings, CMake, Windows supported, **OpenSSL
backend** (matching our existing deps), media transport optional at compile time, and
**MPL-2.0 since version 0.18** (https://github.com/paullouisageneau/libdatachannel, checked
2026-09-02) - compatible with our AGPL-3.0. Dependencies: usrsctp, plog, libjuice/libnice,
libsrtp, nlohmann-json (we already vendor the last).

**Verdict: not a transport, but the best *video* answer, and worth adopting regardless of
which transport wins** - it is what makes every metered or ToS-constrained free tier
survivable.

#### 6. A relay we or the user hosts, and the vendor-cloud model

**Free hosting has largely evaporated.** **Fly.io discontinued its free tier on
2024-10-07** and has none in 2026; the cheapest always-on machine is roughly $2-5/month
(checked 2026-09-02). **Oracle Cloud Always Free** is the outlier - 4 ARM Ampere OCPUs,
24 GB RAM, **up to 10 TB/month outbound** - but instances are subject to **idle
reclamation**: "less than 10 percent CPU utilization and less than 10 percent network
utilization over a 7-day period" may be stopped
(https://docs.oracle.com/en-us/iaas/Content/FreeTier/freetier_topic-Always_Free_Resources.htm,
checked 2026-09-02). A relay idle most of the week is exactly the profile that gets
reclaimed, and ARM capacity has been notoriously scarce. **Cloudflare Workers + Durable
Objects** (100k requests/day free, WebSocket Hibernation, no charge for outgoing WebSocket
messages) is the one credible free platform for a *signalling* or *control* relay - but it
is shaped and priced for messages, not for relaying megabits of video. A paid VPS is honest
and cheap: **Hetzner CX22 at about EUR4.35/month with 20 TB of traffic** (checked
2026-09-02).

**End-to-end encryption through a dumb relay** is what would make a project-run relay
defensible: the relay forwards opaque bytes and holds no keys, as Tailscale's DERP does
("it's impossible for a DERP server to decrypt your traffic"). Achievable - but note the
sequencing problem: if the relay cannot terminate TLS, the phone browser still needs a
certificate valid for the relay's hostname, and the only ways to get one are to give the
node a name under a domain *we* control (the plex.direct pattern below) or to do TLS-SNI
passthrough with the node holding its own certificate.

**Who operates it is the real question, not what it costs.** Any relay we run turns an
offline desktop application into a service with uptime expectations, abuse handling, a
privacy policy, and a bill that scales with users. Our licence compounds it: the project is
**AGPL-3.0** (`LICENSE.txt:1`), so a network-facing relay we operate carries source
provision obligations - fine, but it should be planned rather than discovered.

**The vendor-cloud reference points are instructive.** *OctoEverywhere* does exactly this
for 3D printing, and its **free tier includes unlimited remote access and full-frame-rate,
full-resolution webcam streaming for up to 3 printers**, with paid plans from **$3.99/month**
(https://octoeverywhere.com/supporter, checked 2026-09-02). *Home Assistant's* Nabu Casa
Remote UI is roughly **$6.50-8/month** depending on region and billing period (checked
2026-09-02). Both are companies whose business *is* the relay. That is the honest
comparison: a vendor cloud is the best user experience by a distance, and it is a business,
not a feature. For an open-source fork it is the wrong commitment - and it would make us
the party able to see every user's camera, which is precisely what a self-hosted slicer's
users are avoiding.

*The Plex pattern deserves a mention* as the elegant answer to "HTTPS on a home box". Plex
partnered with **Let's Encrypt** and uses "DNS magic" so a server is reachable over HTTPS
"regardless of how many addresses a server might be accessible through"
(https://support.plex.tv/articles/206225077-how-to-use-secure-server-connections/, checked
2026-09-02). The widely documented mechanism is a hostname encoding the server's IP as a
label under `*.plex.direct`, resolved by Plex's own DNS, with a per-server wildcard
certificate delivered over the authenticated plex.tv API - **the details beyond "Let's
Encrypt" and "DNS magic" are not stated on that support page and I could not verify them
today**. Legally it is clean because Plex owns the domain and issues to itself. Replicating
it needs a domain, a DNS server and an issuing service that we operate - a vendor cloud
with extra steps, and it still does not solve reachability behind CGNAT.

#### 7. What the hub must gain regardless of transport

None of this depends on which option wins; all of it is a prerequisite for any of them.

**a. Split the loopback trust from the proxy trust.** Today `/hub/*` is gated only by
`peer.is_loopback()` (`RemoteHub.cpp:1230-1234`) and hands out the token via
`GET /hub/info` (`RemoteHub.cpp:694-709`). Any tunnel agent connects from 127.0.0.1, so it
would inherit full hub control. The fix is a **separate loopback control listener on its
own ephemeral port** (the pattern `RemoteAccess.cpp:148` already uses), with `/hub/*`
removed from :13640 entirely; the port goes in `hub.json`, which is already how instances
find the hub. Failing that, at minimum require a second, separate control secret on
`/hub/*` and drop `token` from `/hub/info`.

**b. Decide what "remote" means separately from "LAN".** `is_private_v4`
(`RemoteHub.cpp:119-124`) is the only admission control and it is a topology test, not
authentication. Introduce three explicit modes - `off`, `lan`, `remote` - rather than
overloading `m_phone`. In `remote` mode the private-IPv4 gate must be replaced by "accept
from the proxy's source address only", and the `(!phone && !peer.is_loopback())` shortcut
(`RemoteHub.cpp:1238`) must go.

**c. Real credentials.** A 70-bit path token in a URL is acceptable on a LAN and not on the
internet: it appears in browser history, in `Referer` on any outbound link, and in any
proxy's access log, and there is **no rate limiting or lockout** on guessing. Minimum:
generate it from a CSPRNG (`BCryptGenRandom` on Windows) instead of `std::mt19937_64`
(`RemoteHub.cpp:185-195`); move the secret out of the path into an `HttpOnly; Secure;
SameSite=Lax` cookie set once by a `POST` login; add exponential backoff plus a lockout on
failures. TOTP is cheap to add on top (HMAC-SHA1 via the OpenSSL already linked, ~40 lines).
**WebAuthn/passkeys are not cheap**: there is no mature C++ relying-party library, so it
means hand-rolling CBOR + COSE + P-256 verification. Prefer to inherit identity from the
transport (Tailscale's IdP, Cloudflare Access) over building it.

**d. HTTPS is worth more than it looks.** No API on the page needs a secure context today
(only `localStorage`), but WebAuthn is restricted to secure contexts and **`localhost`/
`127.0.0.1` is the only HTTP exemption - a private LAN IP is not**
(https://www.w3.org/TR/webauthn-3/ secure-context requirement; w3c/webauthn issue #1204,
checked 2026-09-02). The same rule blocks service workers, so **the page cannot be a real
installable PWA over `http://192.168.x.x`**. Whatever transport wins, HTTPS unlocks
passkeys, PWA install, WebCrypto and the removal of the browser's "Not secure" chrome.

**e. Proxy-awareness, carefully.** Add `Host`, `Origin`, `X-Forwarded-Proto` and
`X-Forwarded-For` to the `Request` struct (`RemoteHub.cpp:442-446`), but **trust them only
when the peer is the configured proxy address**, never unconditionally - otherwise any LAN
client can forge them. Two concrete needs: (i) an `Origin` check on the `/api/ws` upgrade,
which today has none, and (ii) knowing the external scheme so `Secure` cookies and any
absolute URL are correct.

**f. Keep-alive.** Every response is `Connection: close` (`RemoteHub.cpp:490-500`) and the
page polls at 3/5/10/15 s, 1 s while slicing
(`stream_center.html:1110,1140,1141,1170,1781,2035`). On a LAN that is free; across a
tunnel it is a TCP (and often TLS) handshake per poll, which costs mobile battery and adds
a round trip to every UI update. Implement HTTP/1.1 keep-alive, and consider collapsing the
polls into one `/state` long-poll or a second WebSocket.

**g. Thread-per-connection will not hold.** `accept_loop` detaches a `std::thread` per
connection (`RemoteHub.cpp:762-771`) and `tunnel()` spawns another two per WebSocket
(`RemoteHub.cpp:540-552`). That is fine for a handful of LAN clients and a poor fit for a
public endpoint, both for footprint and because it is trivially DoS-able by opening
connections. Move to an Asio async model, or at least cap concurrent connections.

**h. Bandwidth shaping for video.** Since every candidate transport is either metered,
rate-limited or ToS-constrained on video, the hub should be able to hand the phone a
*cheaper* stream when remote: lower resolution/bitrate/framerate profile, an explicit
"video off / snapshots only" mode (a periodic JPEG via go2rtc's `/api/frame`, which the
bundled binary has), and a byte counter so the user can see consumption. This is the single
highest-leverage feature for making any free tier viable.

**i. Keep the LAN path exactly as it is.** `http://<lan-ip>:13640/r/<token>/` should keep
working unchanged and stay the default; remote is an additional, opt-in surface. The page
already branches on `location.pathname` (`stream_center.html:214-217`) and the QR code is
drawn from `info.url` client-side (`hub.html:47-66`), so both can carry a second URL with
little disturbance.

**j. Uploads.** `MAX_UPLOAD` is 2 GiB and `Content-Length` is mandatory
(`RemoteHub.cpp:70,599-600`). Any proxy that re-chunks a request body will break the
upload path, and Cloudflare would cap it at 100 MB. Accept `Transfer-Encoding: chunked`,
and add resumable/chunked upload so a large model does not restart from zero on a dropped
mobile connection.

### Comparison matrix

"Steps" counts discrete end-user actions from nothing to a working phone link, including
account creation and app installs. All facts sourced in the sections above; all checked
2026-09-02.

| | Steps | Cost | CGNAT | HTTPS / secure context | Auth from transport | WebSocket | Video verdict | Phone app? | Who sees plaintext | Vendor risk |
|---|---|---|---|---|---|---|---|---|---|---|
| **Tailscale Serve** | **10** (4 on phone) | **$0** (6 users, unlimited devices) | Immune | **Real LE cert, auto-renewing** | **Identity headers, IdP + MFA, passkeys** | Untested here; 3 open bugs are against Serve/Funnel | **Best** - no documented limit, direct WireGuard; **stalls on DERP fallback** | **Yes** | **Nobody** (E2E; TLS ends on our PC) | Low-med: free tier improved in 2026 |
| **Tailscale Funnel** | 10 (1 on phone) | $0 | Immune | Real LE cert | **None** (no identity headers) | **#20882: fails under browser HTTP/2** | Risky - undisclosed non-configurable cap | **No** | Nobody (SNI passthrough) | **High**: beta, undisclosed cap |
| **zrok** | ~6 | $0 (5 GB/**day**) | Immune | Real cert on `*.share.zrok.io` | basic-auth + Google/GitHub OAuth | **Unverified - must test** | **Good**: ~5.5 h/day at 2 Mbps | No | NetFoundry frontend | Medium |
| **Cloudflare Tunnel + Access** | **14-18** (7 unautomatable) | ~$10-11/yr domain | Immune | Real cert | **Best**: OTP email, IdPs, service tokens | Yes, documented | **Prohibited** on Free/Pro/Business since 2026-08-25 | No | **Cloudflare** | Medium; policy already adverse |
| **TryCloudflare** | **2** | $0 | Immune | Real cert | **None** | Undocumented; no SSE | No - 200 in-flight cap; URL rotates each restart | No | Cloudflare | **Very high**: "testing only", no SLA, often blocked |
| **Pinggy Pro** | ~4 | **$3/mo** | Immune | Real LE cert | basic auth, bearer, IP allowlist | Unverified | **Good**: "unlimited bandwidth" | No | Pinggy | Medium |
| **MS Dev Tunnels** | ~4 | $0 | Immune | Real cert | MS/Entra/GitHub identity, bearer tokens | **Yes, explicitly documented** | Unknown - no published quota | No | Microsoft | High: public preview, "not for production" |
| **ngrok** | ~5 | $0 → $20/mo | Immune | Real cert | OAuth/OIDC/basic/JWT | Yes | **~71 min/month** on free | No | ngrok | **Blocked**: ToS needs written consent to ship |
| **Port forward + DDNS + LE** | ~8 | $0 | **Fails behind CGNAT** | Real cert (incl. **IP certs, GA 2026-01-15**) | **None - all ours** | Ours already works | **Best possible** - own uplink, no meter | No | **Nobody** | **None** (no vendor) |
| **WebRTC (video only)** | 0 extra | $0, TURN ~$0.045/h | Traverses; TURN fallback | Needs an HTTPS page already | None | N/A | **Excellent, and already in our go2rtc** | No | Nobody (P2P) | Low |
| **Own/vendor relay** | ~2 | EUR4.35/mo VPS, or we operate it | Immune | Ours | Ours | Ours | Good | No | Us, unless E2E | We become the vendor |

### Recommendation and phased rollout

**Best free, no self-hosting: Tailscale with `tailscale serve`, phone app installed.**
It is the only option that is simultaneously free, unmetered, CGNAT-proof, gives a real
auto-renewing certificate, keeps Tailscale itself out of the plaintext, and - uniquely -
**hands us authenticated identity in a header so we can stop relying on a URL token
altogether**. The price is four one-time steps on each phone. For a single owner and family
that is a fair trade; for a casual "show my friend the print" link it is not, which is what
the second path is for.

**Best lowest-friction even if paid: Pinggy Pro at $3/month** - persistent subdomain,
unlimited bandwidth, built-in basic auth, and *no binary to bundle at all* because it is
driven by `ssh.exe`, which Windows already ships. **Conditional on verifying WebSocket
support**, which no official page states. If that test fails, the paid fallback is
Tailscale Standard at $8/user/month, which buys nothing extra technically but removes the
non-commercial-use question.

**Do not build on Cloudflare or ngrok.** Cloudflare closed the ambiguity on 2026-08-25 by
naming Cloudflare Tunnel explicitly in its video-delivery restriction, and enforcement is
discretionary suspension with no numeric threshold; the 100 MB body cap also breaks our
2 GiB uploads. ngrok's free tier is ~71 minutes of video per month, and its ToS requires
**written consent** for the exact distribution model we would need.

**Adopt WebRTC for video regardless of which transport wins.** It is the cheapest
high-value change in this document - the bundled go2rtc v1.9.14 already contains
`/api/webrtc`, `ice_servers`, `candidates` and `RTCPeerConnection`; we simply switched it
off. Moving video off the tunnel makes every metered or ToS-constrained free tier viable
and removes the DERP-stall risk on the Tailscale path.

#### Phase 0 - security prerequisites (must ship before any transport)

Nothing below is optional, because putting *any* reverse proxy in front of the hub today
exposes `/hub/*` to the internet with no authentication.

1. Move `/hub/*` onto a **separate loopback listener on an ephemeral port**, following
   `RemoteAccess.cpp:148`; publish that port in `hub.json`. Remove `/hub/*` from :13640
   entirely (`RemoteHub.cpp:1230-1234`). Drop `token` from `/hub/info`
   (`RemoteHub.cpp:694-709`).
2. Replace `std::mt19937_64` in `random_token()` (`RemoteHub.cpp:185-195`) with
   `BCryptGenRandom`.
3. Add failure rate limiting and lockout on the `/r/<token>` prefix check
   (`RemoteHub.cpp:1235-1241`).
4. Add `Host`, `Origin`, `X-Forwarded-Proto` and `X-Forwarded-For` to `Request`
   (`RemoteHub.cpp:442-446`); **trust them only from the configured proxy peer**. Enforce
   an `Origin` check on the `/api/ws` upgrade (`RemoteHub.cpp:1200-1207`).
5. Replace the two-state `m_phone` with three modes - `off` / `lan` / `remote` - so
   "remote" can bind loopback-only while "lan" keeps binding `0.0.0.0`.

#### Phase 1 - Tailscale Serve

**C++ (`RemoteHub.cpp` / `RemoteHub.hpp`):**
- Detect Tailscale: run `tailscale status --json` (or the LocalAPI named pipe
  `\\.\pipe\ProtectedPrefix\Administrators\Tailscale\tailscaled`, `Host:
  local-tailscaled.sock`, no `Referer`/`Origin`). Read `Self.DNSName` and
  `Self.TailscaleIPs`.
- On enabling remote mode, spawn `tailscale serve --bg --https=443
  http://127.0.0.1:13640`; on disable, `tailscale serve --https=443 off`. Reuse the
  existing job-object child management that already supervises go2rtc
  (`RemoteHub.cpp:800-816`).
- **Bind loopback only in remote mode** - mandatory, because otherwise any LAN client can
  forge the `Tailscale-User-*` headers.
- Trust `Tailscale-User-Login` as the authenticated principal when the peer is loopback and
  the mode is `remote`; keep the `/r/<token>/` path for LAN. Add an allow-list of accepted
  logins so a shared tailnet does not mean shared printer access.
- Extend `Info` (`RemoteHub.hpp:29-41`) with `remote_kind`, `remote_url` and
  `tailscale_state`; extend `info_json`/`url()` (`RemoteHub.cpp:694-709`) to emit the
  `https://<machine>.<tailnet>.ts.net/r/<token>/` form.
- Add `is_tailscale_v4` for `100.64.0.0/10` and IPv6 `fd7a:115c:a1e0::/48` alongside
  `is_private_v4` (`RemoteHub.cpp:119-124`) for the direct-connect case; while there, give
  the acceptor a v6 listener (`RemoteHub.cpp:728-758` is `tcp::v4()`-only).

**HTML:**
- `hub.html` (`:47-66`) needs only a second QR/URL row - it already draws whatever
  `info.url` contains.
- `stream_center.html` needs almost nothing: `REMOTE` is derived from `location.pathname`
  (`:214-217`), so an HTTPS origin works unchanged, and the root-relative
  `/stream.html?...` iframe (`:485`) keeps working because Serve proxies the whole origin.
  Add a "signed in as <login>" affordance and a remote/LAN indicator.

#### Phase 2 - WebRTC video

- In the go2rtc config (`RemoteHub.cpp:793-799`) set `webrtc: listen: ":8555"` with
  `ice_servers` (a public STUN, plus an optional TURN the user can paste) and
  `candidates: [stun:8555]`.
- Add `/api/webrtc` to `GO2RTC_PASSTHROUGH` (`RemoteHub.cpp:68`).
- In `stream_center.html:485` switch remote cells to `mode=webrtc` with MSE fallback (the
  bundled player already handles both, and already has `ManagedMediaSource` for iPhone).
- Add the bandwidth controls from section 7h: a low-bitrate remote profile, a
  snapshot-only mode using go2rtc's `/api/frame`, and a byte counter.

#### Phase 3 - a no-app path, chosen by measurement

Run the experiments below, then ship whichever of **zrok** (stable
`https://<name>.share.zrok.io`, 5 GB/day, built-in OAuth, Apache-2.0 agent we may bundle)
or **Tailscale Funnel** (no second vendor, no second account) actually passes. From the
hub's point of view both are the same shape as Phase 1 - an HTTPS reverse proxy arriving
from loopback - so Phase 0 and Phase 1 already do the work. The only new C++ is spawning a
different child process and a different URL in `info_json`.

#### Phase 4 - power-user direct path (optional)

Own domain or DDNS, TLS terminated in the hub with Boost.Asio + OpenSSL, ACME via libcurl
or a bundled `win-acme`, optional UPnP. Ship only if users ask; it serves the minority with
a real public IP, and it is the only path with no third party at all.

### Open questions / things to verify by experiment

Ordered by how much they would change the plan.

1. **Does go2rtc's `/api/ws` MSE stream survive `tailscale serve` from a real phone
   browser?** Issue #20882 says WebSocket upgrades fail when the client negotiates HTTP/2,
   which browsers do by default. If it reproduces, the fallbacks are
   `--tls-terminated-tcp` / `--tcp` raw passthrough (with us terminating TLS), or moving
   video to WebRTC (Phase 2) so the WebSocket carries only control traffic. **Test this
   first - it gates the primary recommendation.**
2. **What is Tailscale Funnel's actual bandwidth limit?** Undisclosed by design. Measure
   sustained throughput of a 1080p stream over Funnel for an hour and watch for throttling.
3. **Does zrok's public frontend proxy WebSocket upgrades?** Undocumented. A ten-minute
   test decides whether zrok is the no-app answer.
4. **Does Pinggy carry WebSockets**, and do its terms permit us to drive it from a bundled
   app? Both unverified.
5. **How often does the Tailscale path fall back to DERP for a phone on cellular?** Run
   `tailscale ping` and `tailscale status` from a phone on several carriers; if DERP is
   common, Phase 2 becomes urgent rather than merely valuable.
6. **Is `duckdns.org` on the Public Suffix List?** Determines whether every DuckDNS user
   shares one Let's Encrypt rate-limit bucket. Affects Phase 4 only.
7. **Are `miniupnpc` / `libnatpmp` still maintained in 2026, and what share of routers have
   UPnP enabled?** I could not verify either - the web-search budget was exhausted.
   Affects Phase 4 only.
8. **What fraction of WebRTC connections need TURN** for this traffic shape? The commonly
   cited 8-20% is unsourced here, and it sets the TURN bill.
9. **Would Tailscale grant redistribution rights** for a bundled client? Their ToS says no
   by default; only their partnerships team can answer. Not needed if we ask users to
   install it themselves, which is the plan.
10. **Confirm the CA/Browser Forum prohibition on certificates for reserved IP ranges** -
    I asserted it from memory rather than a fetched source. Affects Phase 4 only.

#### Not verified in this pass

The plex.direct hostname/IP-encoding mechanism beyond "Let's Encrypt" and "DNS magic";
current router-UPnP-default statistics; `miniupnpc`/`libnatpmp` maintenance status;
Microsoft Dev Tunnels bandwidth quotas and redistribution terms; Pangolin Cloud Basic's
bandwidth cap; and the exact Twingate Teams/Business prices. The web-search budget was
consumed partway through; every remaining fact above carries its source and check date.

---

# Part B - Hub login and 2FA design (research pass)

## Remote access - hub login and 2FA design

Branch `feat/ultra-preferences`, repo `C:\Dev\SnapmakerOrca`. Read-only pass; nothing was modified.
Scope: hub-side authentication that works identically across every transport the separate
transport pass will pick (Tailscale, Cloudflare Tunnel, ngrok, port-forward+DDNS+TLS, relay).

---

### Findings

#### 1. The token scheme today

**Generation.** `random_token()` (`src/slic3r/GUI/RemoteHub.cpp:185-194`) draws 14 chars from a
32-char confusable-free alphabet using `std::mt19937_64` seeded with
`((uint64_t) rd() << 32) ^ rd()`. That is **at most 64 bits of entropy** (the seed), not the 70
bits the 14 characters suggest, and Mersenne Twister is not a CSPRNG - 14 outputs (70 bits) exceed
the state size, so the generator is recoverable from the token. `RAND_bytes` is never called
anywhere in `src/` (confirmed repo-wide).

**Validation.** `valid_token()` (`RemoteHub.cpp:180-183`) accepts any lowercase alnum string of
length 10-32 - a *wider* alphabet than the generator uses, so `--hub-token` /
`/hub/phone?token=` can install a weak token (e.g. `aaaaaaaaaa`). No entropy floor.

**Storage.**
- `<datadir>/hub/hub.json` - written by `write_hub_json()` (`RemoteHub.cpp:713-726`), token at
  line 721, alongside pid/port/phone/go2rtc_port/version. Plaintext, no ACL.
- App config keys `stream_phone_access` / `stream_phone_token`, written from the Stream tab
  (`src/slic3r/GUI/StreamPanel.cpp:82-88`, read at `:70`) and read at startup by
  `GUI_App::start_remote_access()` (`src/slic3r/GUI/GUI_App.cpp:3974-3976`).
- `SNORCA_PHONE_ACCESS=<token>` env var overrides both (`GUI_App.cpp:3971-3973`).
- CLI `--hub-token` (`RemoteHub.cpp:1604`; option defined `src/libslic3r/PrintConfig.cpp:9245-9259`).
  A token passed on argv is visible in the process table.

**Rotation.** `HubServer::set_phone()` (`RemoteHub.cpp:992-1008`): a new token is minted only on
the **off -> on** edge and only when the caller does not supply a remembered one
(`:1001`). There is no expiry, no rotate-now control, and no revocation of a leaked link short of
toggling phone access off and on.

**Transmission - every read site.**
| Where | What |
|---|---|
| `RemoteHub.cpp:708` | `info_json()` returns `url = http://<ip>:<port>/r/<token>/` **and** `token` as a bare field |
| `RemoteHub.cpp:721` | persisted to `hub.json` |
| `RemoteHub.cpp:983` | `snapshot().url`, used by the tray |
| `RemoteHub.cpp:1203` | go2rtc passthrough gate: `cookie_value(r.cookies, "rt") != token` |
| `RemoteHub.cpp:1215-1221` | path prefix gate `"/r/" + token` |
| `RemoteHub.cpp:1224-1226` | `Set-Cookie: rt=<token>; Path=/; SameSite=Lax` on the page |
| `RemoteHub.cpp:1267,1270` | loaded from `hub.json` at start, else fresh |
| `RemoteHub.cpp:1509,1517` | client-side `Info::url()` / `Info::json()` |
| `stream_center.html:214-217` | `REMOTE` = `/r/<token>/` parsed from `location.pathname` |
| `stream_center.html:219-223` | page re-sets `document.cookie = 'rt=' + token` |
| `stream_center.html:301,490,1019-1023,1089` | every phone API call prefixes `REMOTE` |
| `hub.html:63-67` | QR canvas + copyable URL, drawn by the bundled `qrcode.js` (`hub.html:41`) |
| `stream_center.html:551-565` | the same QR in the PC Stream tab's phone modal |

So the token is simultaneously a URL path segment, a cookie value, a QR payload, a JSON field on
a loopback API, and a line in two on-disk config files. **It is a bearer credential in a URL**,
which means it leaks through browser history, `Referer` on any cross-origin subresource, screen
sharing, and - once a tunnel is involved - the tunnel provider's request logs.

**The private-IPv4 gate.** `is_private_v4()` (`RemoteHub.cpp:119-124`) accepts `10/8`, `127/8`,
`172.16/12`, `192.168/16`, `169.254/16` and **only IPv4** (`if (!a.is_v4()) return false`). Enforced
first thing in `serve()` (`RemoteHub.cpp:1185`). The acceptor is opened `tcp::v4()` only
(`RemoteHub.cpp:732,739`).

**What a remote transport breaks - concretely.**

1. **Tailscale direct is rejected today.** Tailnet addresses are `100.64.0.0/10` (CGNAT), which
   `is_private_v4()` does not match. A phone on the tailnet hitting `http://100.x.y.z:13640`
   gets its connection dropped at `RemoteHub.cpp:1185`. Only `tailscale serve` (which proxies
   from the local machine) works unmodified.
2. **Port-forward + DDNS is rejected today** - public source addresses fail the same gate.
3. **`/hub/*` becomes remotely reachable through any local tunnel.** This is the most serious
   finding. The admin routes are gated by `peer.is_loopback()` (`RemoteHub.cpp:1211`), and
   `cloudflared` / `ngrok` / `tailscale serve` all connect from `127.0.0.1`. Once one of them
   fronts port 13640, an unauthenticated remote request to `/hub/info` returns the token in
   cleartext (`RemoteHub.cpp:699`), and `/hub/new`, `/hub/phone`, `/hub/quit`,
   `/hub/instances/<pid>/quit` all become remotely callable (`RemoteHub.cpp:1049-1099`).
   **Loopback is not an authorisation boundary once a tunnel exists.**
4. **IPv6 loopback breaks tunnels silently.** `localhost` resolves to `::1` first on Windows; the
   hub binds v4 only, so a tunnel configured with `http://localhost:13640` gets connection
   refused. It must be `http://127.0.0.1:13640`.
5. **Proxy headers are not parsed at all.** `read_request()` (`RemoteHub.cpp:448-482`) keeps only
   `Cookie`, `Content-Length` and `X-File-Name` (`:472-474`). There is no `Host`, `Origin`,
   `Referer`, `User-Agent`, `X-Forwarded-For` or `X-Forwarded-Proto`. So behind a tunnel every
   client collapses to one indistinguishable peer - rate limiting, lockout and the device list
   all lose their key unless forwarded headers are read.
6. **Cookies and origin.** `Set-Cookie: rt=...` has no `Secure`, no `HttpOnly` (deliberately -
   the page writes it itself at `stream_center.html:220`), no `Max-Age`. Moving from
   `http://192.168.1.50:13640` to `https://pc.tailnet.ts.net` is a different origin: the cookie
   does not travel, and Chrome's schemeful-same-site makes `http://h` and `https://h` cross-site
   for SameSite purposes even on one hostname. Cookies are also **not port-isolated**
   (RFC 6265 §8.5) - anything else listening on the PC's LAN IP can read `rt`.
7. **WebSocket/origin coupling.** `GO2RTC_PASSTHROUGH` (`RemoteHub.cpp:68`) mounts
   `/stream.html`, `/video-stream.js`, `/video-rtc.js`, `/api/ws` at the **root**, because
   go2rtc's player builds its WS URL from `location.origin`. Any transport that mounts the hub
   at a sub-path (e.g. `tailscale serve --set-path /orca`) breaks the player.

**No abuse resistance exists today.** No rate limiting, no lockout, no request timeouts, no
header-size limit (`asio::streambuf req;` at `RemoteHub.cpp:450` is unbounded - a slow header
stream is an unbounded allocation), one detached `std::thread` per connection
(`RemoteHub.cpp:770`), no security headers anywhere in the tree (grep for
`X-Frame-Options|Content-Security-Policy|X-Content-Type-Options` in `src/` and `resources/web/`
returns nothing), and all comparisons are `std::string::operator!=`.

**The upload -> spawn path.** `spool_upload()` (`RemoteHub.cpp:583-620`) already does the right
basics: strips path separators, drops control chars and `<>:"|?*`, truncates to 100 chars,
requires `Content-Length`, and enforces an extension allow-list of
`.3mf .stl .obj .step .stp` (`:594-598`). Cap is `MAX_UPLOAD = 2 GiB` (`RemoteHub.cpp:70`).
Reachable at `/r/<token>/api/instances/open` (`RemoteHub.cpp:1122-1131`) which then calls
`spawn_slicer()` (`RemoteHub.cpp:1010-1020`) - a phone request starts a process. Two residual
gaps: the cleaned name may still begin with `-`, and it is passed as bare `argv[1]`
(`RemoteHub.cpp:1013`), so it can be read as a CLI option; and the per-upload folder is
`timestamp_compact()` at 1-second resolution (`RemoteHub.cpp:603`), so two uploads in the same
second collide.

#### 2. Crypto available in-tree, no new dependencies

**OpenSSL 1.1.1w**, built from source, static.
`deps/OpenSSL/OpenSSL.cmake:43` pins `OpenSSL_1_1_1w`;
`deps/build/OrcaSlicer_dep/usr/local/include/openssl/opensslv.h:43` confirms
`"OpenSSL 1.1.1w  11 Sep 2023"`. Configured `no-shared no-asm no-ssl3-method no-dynamic-engine`
(`OpenSSL.cmake:48-55`). Top-level `find_package(OpenSSL REQUIRED)` at `CMakeLists.txt:559` with
no version constraint.

`RemoteHub.cpp` is in `SLIC3R_GUI_SOURCES` (`src/slic3r/CMakeLists.txt:403`), and
`libslic3r_gui` links `OpenSSL::SSL OpenSSL::Crypto` (`src/slic3r/CMakeLists.txt:852`). Nothing
new to link.

| Primitive | Availability | In-tree precedent |
|---|---|---|
| `HMAC(EVP_sha1/sha256, ...)` | yes | `GUI_App.cpp:5236` (HMAC-SHA256) |
| `SHA256()` / `SHA256_Init/Update/Final` | yes | `SimplyPrint.cpp:57`, `SSWCP.cpp:404-414` |
| `PKCS5_PBKDF2_HMAC` | yes | `src/slic3r/Utils/FileDecrypt.cpp:31-39` |
| `EVP_PBE_scrypt` | yes (OpenSSL >= 1.1.0) | **none** - no callers to copy |
| Argon2id | **no** - needs OpenSSL >= 3.2; no libargon2/libsodium in `deps/` either | - |
| `RAND_bytes` | yes | **none** - unused repo-wide |
| `CRYPTO_memcmp` | yes (`<openssl/crypto.h>`) | **none** |
| ECDSA P-256 verify (`EVP_DigestVerify*`) | yes | - |
| base64 encode+decode | yes, header-only | `boost::beast::detail::base64`, used at `HttpServer.cpp:8` (decode `:783-798`), `GUI_App.cpp:51`, `SimplyPrint.cpp:60-61` |
| base64url encode | yes | `GUI_App.cpp:5134-5144`; the decoder is inlined twice, never factored out |
| hex | yes | `boost::algorithm::hex_lower`, `AppConfig.cpp:31` |
| **base32** | **absent repo-wide** - must write (~40 lines) | - |
| CSPRNG via Boost | yes - `boost::uuids::random_generator` resolves to `BCryptGenRandom` here (`BOOST_USE_WINAPI_VERSION=0x602` at `CMakeLists.txt:447`) | `Plater.cpp:18350`, `MultiComMgr.cpp:43` |

Note: every `bcrypt` hit in the tree is Windows CNG (`bcrypt.lib`), **not** the bcrypt password KDF.

**Boost 1.84.0** (`deps/Boost/Boost.cmake:14`). **`asio::ssl` already compiles and links in this
exact target**: `src/slic3r/GUI/BambuCamRelay.cpp:86-92` builds an
`asio::ssl::context(tls_client)` and an `asio::ssl::stream<tcp::socket>` - and `RemoteHub.cpp:6`
includes `BambuCamRelay.hpp`. So the hub terminating HTTPS itself is a code change, not a
dependency change. Server side needs `ssl::context::tls_server` +
`use_certificate_chain_file` / `use_private_key_file`.

**libcurl 7.75.0 with the OpenSSL backend on Windows** - `deps/CURL/CURL.cmake:29` sets
`-DCMAKE_USE_OPENSSL=ON`; the Schannel line at `:28` is commented out. Relevant only if an ACME
client is ever added.

Also present but not proposed for reuse: a full Boost.Beast HTTP server
(`src/slic3r/GUI/HttpServer.cpp`) and a Beast WebSocket server
(`src/slic3r/GUI/WebSocketDebugServer.cpp`). The hub's hand-written layer stays; it is small and
the auth work does not justify a rewrite.

#### 3. Standards facts that constrain the design

All URLs accessed 2026-09-02.

- **TOTP** - RFC 6238 §4.1-4.2 (https://www.rfc-editor.org/rfc/rfc6238.txt): `T = floor((now - T0)/X)`,
  defaults `T0 = 0`, `X = 30`, HMAC-SHA-1, 8-byte counter, must be 64-bit past 2038. Note RFC 6238's
  own test vectors are **8-digit**; the 6-digit default comes from RFC 4226 §5.3
  (https://www.rfc-editor.org/rfc/rfc4226.txt) and the otpauth URI spec. RFC 4226 §4 R6: secret
  **MUST be >= 128 bits, RECOMMENDED 160 bits**. Dynamic truncation (RFC 4226 §5.3): offset =
  low 4 bits of byte 19; take 4 bytes at that offset; mask the top byte with `0x7f`; `mod 10^d`.
- **otpauth URI** (https://github.com/google/google-authenticator/wiki/Key-Uri-Format): base32
  secret, "padding ... is not required and should be omitted". `algorithm`, `digits`, `period` are
  **ignored by several major apps** (Authy both platforms, Google Authenticator on Android,
  Microsoft Authenticator, Duo - per https://labanskoller.se/blog/2023/03/16/mobile-authenticator-apps-algorithm-support-review-2023-edition/),
  which is harmless only if we emit the defaults. Set both the label prefix and `issuer`.
  *Unverified:* no authoritative source on which parameters iOS Passwords honours.
- **Base32** - RFC 4648 §6 (https://datatracker.ietf.org/doc/html/rfc4648): `A-Z2-7`, pad `=`.
  Emit unpadded in the URI; accept padded/unpadded and case-insensitive on input.
- **Secure contexts** - W3C Secure Contexts §3.1 (https://w3c.github.io/webappsec-secure-contexts/):
  `https`/`wss`, `127.0.0.0/8`, `::1`, and `localhost`/`*.localhost` are potentially trustworthy.
  **`http://192.168.x.x` is not.**
- **WebAuthn** - W3C Rec (https://www.w3.org/TR/webauthn-3/): RP ID is a *valid domain string* and
  must be a registrable domain suffix of, or equal to, the caller's effective domain.
  `create()` throws `SecurityError` if the effective domain is not a valid domain - and per the
  URL Standard (https://url.spec.whatwg.org/) an IP address is not a domain, so **an RP ID can
  never be an IP address**. Credentials are scoped to the RP ID and do not transfer between
  hostnames. MDN: `PublicKeyCredential` is secure-context only and **does not work inside an
  iframe** (https://developer.mozilla.org/en-US/docs/Web/API/PublicKeyCredential).
- **No C or C++ WebAuthn relying-party library exists.** The canonical list
  (https://github.com/yackermann/awesome-webauthn) has JS/Python/Java/Go/Rust/... and nothing for
  C/C++. `libfido2` (BSD-2, https://github.com/Yubico/libfido2) exposes `fido_cred_verify` /
  `fido_assert_verify` but is a client library and **requires OpenSSL >= 3.0**, which this tree
  does not have. `mod_authnz_fido2` self-describes as unfinished. The viable path is
  **libcbor (MIT, https://github.com/PJK/libcbor)** or a hand-written CBOR subset, plus OpenSSL.
  ES256 signatures in WebAuthn are DER `ECDSA-Sig-Value`, so `EVP_DigestVerify*` takes them
  directly.
- **Cookies** - MDN Set-Cookie
  (https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Set-Cookie): "Insecure
  sites (`http:`) cannot set cookies with the `Secure` attribute", except on localhost.
  RFC 6265bis §5.7 step 19: `SameSite=None` is ignored without `Secure`. §4.1.3.2: `__Host-`
  implies Secure + `Path=/` + no `Domain`, so it is unusable over plain HTTP.
  §5.5 recommends a **400-day** lifetime cap (Chrome enforces it) - 90 days is fine.
  RFC 6265 §4.1.2.2: `Max-Age` beats `Expires`. §8.5: cookies are **not isolated by port or
  scheme**.
- **Safari ITP does not cap our trusted-device cookie.** WebKit ITP 2.1
  (https://webkit.org/blog/8613/intelligent-tracking-prevention-2-1/): the 7-day cap applies to
  "persistent cookies created through `document.cookie`" and explicitly "Only cookies created
  through document.cookie are affected"; the March 2020 post extended it to script-writable
  *storage*, not to HTTP-set cookies. The one `Set-Cookie` exception is CNAME-cloaked
  third-party subresources (https://webkit.org/blog/11338/cname-cloaking-and-bounce-tracking-defense/),
  which does not apply to a first-party navigation. **A 30-90 day `HttpOnly` `Set-Cookie` survives
  on iOS Safari.** It would *not* if the page set it via `document.cookie` the way `rt` does today
  (`stream_center.html:220`).
- **WebSocket handshakes carry cookies** - WHATWG WebSockets
  (https://websockets.spec.whatwg.org/) sets the request's credentials mode to `include`;
  RFC 6455 §4.1 requires browsers to send `Origin`. Empirically this already works in this exact
  deployment: the `rt` cookie is what gates `/api/ws` today (`RemoteHub.cpp:1203`) and the phone
  player works on iOS Safari and Android Chrome.
- **CSRF** - OWASP cheat sheet
  (https://cheatsheetseries.owasp.org/cheatsheets/Cross-Site_Request_Forgery_Prevention_Cheat_Sheet.html):
  SameSite is "defense-in-depth ... does not replace a proper CSRF defense"; Origin/Referer are
  absent on ~1-2% of legitimate traffic; Fetch Metadata may be relied on for modern browsers
  *with a fallback*. MDN Origin
  (https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Origin): user agents add
  `Origin` to same-origin `POST`/`PUT`/`PATCH`/`DELETE`. `Sec-Fetch-Site` is Baseline since
  March 2023 - Chrome 76, Firefox 90, **Safari/iOS 16.4** (https://caniuse.com/mdn-http_headers_sec-fetch-site),
  and being a `Sec-` forbidden header name it cannot be forged by page JS.
  *Flagged as not independently verified:* current Safari's same-origin-POST `Origin` behaviour.
- **Password hashing** - OWASP Password Storage cheat sheet
  (https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html, source last
  edited 2026-06-24): Argon2id preferred (19 MiB / t=2 / p=1 minimum); **scrypt** equivalents
  `N=2^17,r=8,p=1` or `N=2^16,r=8,p=2`; **PBKDF2-HMAC-SHA256 = 600,000 iterations**.
- **Let's Encrypt** (https://letsencrypt.org/docs/challenge-types/): DNS-01 is the only challenge
  that works without inbound 80/443 and the only one that issues wildcards. 90-day certs; a
  6-day "shortlived" profile exists (https://letsencrypt.org/2025/02/20/first-short-lived-cert-issued/)
  but its wildcard/DNS-01 support is *unverified*.
- **Tunnel TLS** - Tailscale issues real Let's Encrypt certs for
  `<machine>.<tailnet>.ts.net` with MagicDNS on (https://tailscale.com/kb/1153/enabling-https,
  https://tailscale.com/kb/1312/serve); Cloudflare Tunnel serves your own domain under Universal
  SSL (https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/) -
  *note Universal SSL covers apex + one subdomain level only, verify before choosing a deep name*;
  ngrok auto-provisions ACME certs and free accounts now get a static `*.ngrok-free.dev` domain
  (https://ngrok.com/docs/gateway/domains/tls-certificates). **All three are genuine secure
  contexts, so WebAuthn works over them.**

---

### Design

#### Trust model

One **owner**. "Family" is handled by sharing the owner password and giving each phone its own
**trusted-device session** that can be named and revoked independently - which is what a household
actually needs. Real multi-user (per-person passwords, per-person TOTP) is a schema extension:
`auth.json` keeps a `users` array so adding a second entry is not a format break. Nothing in the
routes assumes a single user.

#### Network policy - replaces `is_private_v4` as the gate

Three axes, evaluated per connection in `serve()`:

```
peer_class  := loopback | private_v4 | tailscale(100.64/10) | other
via_proxy   := peer is loopback AND cfg.proxy.enabled AND the configured header is present
client_ip   := via_proxy ? first hop from cfg.proxy.header : peer
scheme      := hub_tls ? https : (via_proxy ? X-Forwarded-Proto : http)
```

Policy table (all defaults, all settable on the hub page):

| Situation | Allowed | Auth required |
|---|---|---|
| loopback, admin listener | yes | none (it is the PC) |
| private_v4 or tailscale, `scheme=http` | yes | full session, **or** the trusted-network QR bootstrap |
| any peer, `scheme=https` | yes | full session |
| `other` peer, `scheme=http` | **refused, 403 with an explanation page** | - |
| via_proxy, `X-Forwarded-Proto != https` | **refused** | - |

`is_private_v4()` gains `100.64.0.0/10` so Tailscale direct works, and the whole gate becomes
policy rather than a hard-coded `return`.

**Forwarded headers are only trusted when the peer is loopback and the owner explicitly enabled
"behind a local tunnel/reverse proxy" and named the header** (`X-Forwarded-For`,
`CF-Connecting-IP`, `X-Real-IP`). Otherwise they are ignored entirely. Unconditional trust would
let anyone forge `client_ip` and defeat rate limiting.

**The `/hub/*` hole is closed by a second socket, not by a peer check.** When phone access is on,
the hub binds two acceptors: the existing public one on `0.0.0.0:13640`, and a new loopback-only
admin acceptor on an ephemeral port recorded as `admin_port` in `hub.json`. `/hub/*` is served
**only** on the admin acceptor and 404s on the public one regardless of peer address. Client-side
`hub_port_from_file()` (`RemoteHub.cpp:1541-1549`) reads `admin_port` with a fallback to `port`
for a hub started by an older build. This is the single most important change in the plan: it is
what makes fronting the hub with `cloudflared`/`ngrok`/`tailscale serve` safe at all.

#### Credentials

**(a) Owner password.** Set on the PC only, at `/hub/` (loopback admin listener). Never in a QR,
never in `hub.json`, never on the command line. Stored in `<datadir>/hub/auth.json` as a
PHC-style string:

```
$scrypt$ln=16,r=8,p=2$<b64 salt, 16 bytes>$<b64 hash, 32 bytes>
```

`EVP_PBE_scrypt(pw, len, salt, 16, 1<<16, 8, 2, 192u<<20 /*maxmem*/, out, 32)`. OWASP's
`N=2^16,r=8,p=2` (64 MiB) - a desktop can afford it and logins are rare. Guard with
`#ifdef OPENSSL_NO_SCRYPT` and fall back to `$pbkdf2-sha256$i=600000$...` via
`PKCS5_PBKDF2_HMAC` (the call shape already exists at `FileDecrypt.cpp:31-39`). The algorithm and
params live in the string, so an Argon2id upgrade after an OpenSSL 3.2 bump is a verifier change
only. Minimum length 10 chars, no composition rules; reject the 100 most common passwords from a
tiny embedded list.

**(b) TOTP.** RFC 6238, `T0=0`, `X=30`, HMAC-SHA-1, 6 digits, 20-byte (160-bit) secret from
`RAND_bytes`. Acceptance window `±1` step (90 s total). Enrolled from `/hub/` by scanning

```
otpauth://totp/Snapmaker-Ultra%20Hub:<pc-name>?secret=<BASE32,unpadded>&issuer=Snapmaker-Ultra%20Hub&algorithm=SHA1&digits=6&period=30
```

drawn with the already-bundled `qrcode.js` (`hub.html:41` loads it; `hub.html:47-56` has a ready
`drawQr`). Emitting the defaults explicitly means the apps that ignore `algorithm`/`digits`/
`period` still compute the right code. **Two-step enrolment**: `/hub/auth/totp/begin` returns a
*staged* secret; nothing is committed until `/hub/auth/totp/confirm` verifies a live code. This
prevents locking yourself out with a mis-scanned QR.

**Replay protection**: `totp_last_step` in `auth.json`. A code is accepted only if its step is
strictly greater than `totp_last_step`; on success the field is set to that step. This kills the
±1 window replay and the "same code twice within 30 s" case.

**Backup codes**: 8 codes, `XXXXX-XXXXX` from the 32-char confusable-free alphabet already in
`random_token()` (~50 bits each), from `RAND_bytes`. Shown once at enrolment with a
copy/print affordance. Stored as `$pbkdf2-sha256$i=100000$<salt>$<hash>` per code with a
`used_at` timestamp. Verification walks the unused codes with early exit; worst case 8 x ~25 ms.
Single use. "Regenerate codes" invalidates all eight.

**(c) Passkeys / WebAuthn** - Phase C, HTTPS only, and worth being blunt about the constraints:

- `navigator.credentials` is secure-context only, so **it cannot be offered on
  `http://192.168.1.50:13640`** at all. It works on `http://localhost` (the PC's own hub page),
  which is useless for a phone.
- The RP ID must be a valid domain and **cannot be an IP address**; `create()` throws
  `SecurityError` on an IP origin before the secure-context check even matters.
- Credentials are bound to one RP ID. Reaching the hub as `192.168.1.50` at home and
  `pc.tailnet.ts.net` away means **two different origins and a passkey that only works on one**.
  Making passkeys genuinely usable therefore requires committing to **one stable hostname used
  from everywhere**, e.g. always `pc.tailnet.ts.net` (RP ID = that full name), or your own
  `printer.example.com` behind Cloudflare Tunnel with split-horizon DNS so the LAN resolves the
  same name. Transports that satisfy this: Tailscale (with MagicDNS + `tailscale serve`/`cert`),
  Cloudflare Tunnel on your own domain, port-forward+DDNS with a real Let's Encrypt cert.
  Transports that do not: bare-IP LAN, bare-IP port-forward, and ngrok if the domain ever rotates.
- `PublicKeyCredential` also does not work inside an iframe, which is fine here (the login page is
  top-level) but rules out any future embedding.
- **Server-side verification**: there is no C/C++ relying-party library. `libfido2` is a client
  library and needs OpenSSL >= 3.0 (we have 1.1.1w). So it is hand-rolled. It is *reasonable* if
  and only if we request `attestation: "none"`: registration then needs only enough CBOR to pull
  `authData` out of the `attestationObject` and decode the COSE_Key map
  (`kty=2, alg=-7, crv=1, x=-2, y=-3`) into an uncompressed point; assertion needs
  `SHA-256(clientDataJSON)`, `EVP_DigestVerify` of the DER signature over `authData || hash`, plus
  checks on `type`, single-use challenge, exact `origin`, `rpIdHash == SHA-256(RP ID)`, the UP/UV
  flag bits, and a `signCount` that is advisory only (passkey providers commonly send 0 - do not
  hard-fail). Budget ~500-700 lines including a minimal CBOR reader and tests, or vendor
  libcbor (MIT, no runtime deps). **Verdict: hand-roll the restricted subset, do not attempt
  attestation verification.** Passkeys are an *addition* to password+TOTP, never a replacement,
  so a lost phone never locks the owner out.

**(d) Trusted-device session.** One cookie, one server-side record.

- Cookie `sid` = 32 bytes from `RAND_bytes`, base64url, 43 chars.
- Attributes: `HttpOnly; Path=/; SameSite=Lax; Max-Age=<ttl>` always, plus `Secure` whenever
  `scheme == https`. `SameSite=Lax` (not Strict) so that following a bookmark or a link to the
  hub does not land on a login screen; every state-changing route is additionally CSRF-checked,
  which is where Strict's value would have come from.
- Server-side record, keyed by `SHA-256(sid)` (not the raw value - an `auth.json` leak must not
  hand over live sessions). A random 256-bit token needs no slow KDF.
- Fields: `h` (sha256 hex), `stage` (`pending_2fa` | `full`), `created`, `expires`, `last_seen`,
  `remember`, `name`, `ip_first`, `ip_last`, `ua`, `csrf` (32 bytes b64url).
- TTL: 12 h by default; **60 days** when "Remember this phone" is ticked (settable 30-90).
  Sliding: `expires` is pushed out on use, but only rewritten to disk when it has moved by more
  than 24 h, so a polling phone does not rewrite `auth.json` every 15 s
  (`stream_center.html:2035` polls `/state` on a 15 s timer).
- **The long session *is* the trusted device.** One mechanism, one revocation surface, and
  "log out" genuinely means "this phone must do password+TOTP again". The alternative - a separate
  long-lived `dev` cookie that survives logout and lets you skip only the second factor - is
  noted and rejected: it doubles the credential surface for a household that logs out roughly
  never. With a 60-day session the friction is already near zero.
- **Session id rotation on privilege change**: a new `sid` is minted when `pending_2fa` becomes
  `full`, defeating session fixation.
- `__Host-sid` is the stricter name (it forces `Secure`, `Path=/`, no `Domain`, which matters
  because cookies are not port-isolated on a shared LAN IP). It cannot be used over plain HTTP,
  so: emit `__Host-sid` when `scheme == https`, `sid` otherwise, and accept either on read.

#### Flows

**First-run (PC).** `/hub/` shows a "Security" card. Until a password is set, phone access can
still be turned on but only in trusted-network mode, and the card says so in one line.

**Login (phone).**
1. Any protected path without a `full` session ->
   `302 /login?next=<path>` for navigations, `401 {"error":"login required","login":"/login"}` for
   XHR (so `jsonOf` at `stream_center.html:1018` can redirect).
2. `GET /login` -> `login.html`.
3. `POST /login` `{password, remember}` -> scrypt verify -> mint `sid`, `stage = totp_enrolled ?
   pending_2fa : full` -> `{"next": "/login/totp"}` or `{"next": "/"}`.
4. `GET /login/totp` (needs `pending_2fa`) -> code entry, `inputmode="numeric"`,
   `autocomplete="one-time-code"` so iOS/Android offer the code from the authenticator.
5. `POST /login/totp` `{code}` or `{backup}` -> verify, check `step > totp_last_step`, rotate
   `sid`, set `stage=full` and the real TTL, name the device from the User-Agent, log the event.
6. `POST /logout` -> delete the record, `Set-Cookie: sid=; Max-Age=0`, 302 `/login`.

**Trusted-network QR bootstrap (LAN).** Keep it - do not replace it, and do not leave it as-is.

`GET /r/<token>/` from a `private_v4`/`tailscale` peer while `trusted_network` is on mints a
**full session** named "LAN QR" and `302`s to `/`. The token becomes a **one-shot bootstrap
credential**, not a standing one: after that first request the phone holds a proper `HttpOnly`
session cookie and the URL in its history is inert to anyone off the LAN. On any other peer class,
or with `trusted_network` off, `/r/<token>/*` returns 404 exactly like an unknown path.

Why keep it rather than replace it: it is the entire reason the current phone experience is
zero-friction, it costs ~15 lines, and scoping it to the LAN removes the property that actually
worries us (a bearer secret in a URL that reaches the internet). Defaults: `trusted_network = on`
for an existing install (behaviour is unchanged for today's users); turning on any remote
transport prompts "Set a password" and offers to switch it off. Also: `random_token()` moves to
`RAND_bytes` and `valid_token()` gains an entropy floor, because even a LAN-only bearer token
should not come out of a 64-bit-seeded Mersenne Twister.

**Migration of the path token and the `rt` cookie.** The session cookie replaces both.

- Phone routes move to the root, with the hub's own API under a distinct prefix so it cannot
  collide with go2rtc's `/api/ws`:

  | Old | New |
  |---|---|
  | `/r/<t>/` | `/` |
  | `/r/<t>/state` | `/h/state` |
  | `/r/<t>/api/...` | `/h/api/...` |
  | `/r/<t>/i/<pid>/...` | `/h/i/<pid>/...` |
  | `/r/<t>/bambu`, `/ff` | `/h/bambu`, `/h/ff` |
  | `/stream.html`, `/video-*.js`, `/api/ws` | unchanged (root, go2rtc) |

- `stream_center.html`: `REMOTE` becomes `'/h/'` when `location.pathname` is not under `/hub/`;
  the `document.cookie = 'rt=...'` line (`:220`) is **deleted** - which is also what makes the
  session cookie exempt from Safari's 7-day script-cookie cap.
- The go2rtc gate at `RemoteHub.cpp:1203` becomes `require_session(r).stage == full`. Cookies are
  attached to the `/api/ws` handshake in this deployment today, and that is exactly what the `rt`
  check has been relying on - so this is a like-for-like swap, not a new assumption.

#### `<datadir>/hub/auth.json`

Written atomically through the existing `write_file()` (`RemoteHub.cpp:98-115`). Created 0600 on
POSIX; on Windows rely on the profile ACL of `%APPDATA%` and note it in the file header comment.

```jsonc
{
  "version": 1,
  "trusted_network": true,
  "remember_days": 60,
  "session_hours": 12,
  "proxy": { "enabled": false, "header": "X-Forwarded-For", "require_https": true },
  "users": [{
    "name": "owner",
    "password": "$scrypt$ln=16,r=8,p=2$<b64salt>$<b64hash>",
    "password_set": 1772668800,
    "totp": { "secret_b32": "…", "digits": 6, "period": 30, "algo": "SHA1", "last_step": 58947321 },
    "backup": [{ "hash": "$pbkdf2-sha256$i=100000$<b64salt>$<b64hash>", "used_at": 0 }],
    "webauthn": [{ "id_b64u": "…", "pubkey_cose_b64": "…", "sign_count": 0,
                   "rp_id": "pc.tailnet.ts.net", "name": "Pixel 9", "created": 0 }]
  }],
  "sessions": [{
    "h": "<sha256 hex of sid>", "user": "owner", "stage": "full",
    "created": 0, "expires": 0, "last_seen": 0, "remember": true,
    "name": "iPhone (Safari)", "ip_first": "192.168.1.44", "ip_last": "100.83.1.9",
    "ua": "Mozilla/5.0 (iPhone…)", "csrf": "<b64url>"
  }],
  "lockout": { "global_fails": 0, "global_until": 0 }
}
```

Sessions are held in memory and flushed on create / promote / revoke / expiry-sweep, plus a lazy
flush from the existing 2-second `loop()` tick (`RemoteHub.cpp:1281-1301`).

---

### Abuse resistance

**Rate limiting and lockout.** In-memory `std::map<std::string, Bucket>` keyed by `client_ip`
(post-proxy-resolution), swept on the `loop()` tick.

| Consecutive failures | Delay before the next attempt is accepted |
|---|---|
| 1-2 | 0 |
| 3-4 | 5 s |
| 5-7 | 30 s |
| 8-11 | 5 min |
| >= 12 | 30 min (capped, does not escalate further) |

Reset on success. Applies to `POST /login`, `POST /login/totp` and the WebAuthn assertion route.
Because a tunnel collapses every client to one address, a **global** bucket runs alongside:
20 failures in 15 minutes locks all logins for 15 minutes. Both are visible on the hub page with
a countdown and an **Unlock now** button on the loopback admin listener, so the owner can never
be locked out of their own PC. Every rejected attempt still costs the attacker a full scrypt
evaluation only *after* the delay check - do the cheap check first so lockout is not itself a CPU
amplifier.

**Constant-time comparison.** `CRYPTO_memcmp` (`<openssl/crypto.h>`, present in 1.1.1w, unused
today) for the session-hash lookup, the TOTP digits, backup codes, the CSRF token, and the
bootstrap path token. Wrap it in one `ct_eq(a, b)` helper that also compares lengths without
early exit.

**TOTP replay.** `totp_last_step` monotonic counter, described above. Also reject codes that are
not exactly 6 ASCII digits before doing any HMAC.

**Backup code hashing.** PBKDF2-SHA256, 100k iterations, per-code salt, single use with a
`used_at` stamp. Never logged, never echoed.

**CSRF - three overlapping checks on every non-GET.**
1. `SameSite=Lax` on `sid` (blocks cross-site POST cookies).
2. `Sec-Fetch-Site` must be absent or in `{same-origin, same-site, none}`; if it says
   `cross-site`, reject. Baseline since March 2023 (Safari 16.4+), so a fallback is still needed.
3. `Origin` (or `Referer`) must, when present, match `scheme://host` exactly - full-host
   comparison, never a suffix match. When both `Origin` and `Sec-Fetch-Site` are absent, require
   the custom header `X-Hub-CSRF: <session.csrf>`, which a cross-origin form cannot set and which
   forces a CORS preflight the hub answers with nothing. The page sends this header on every
   `fetch`/XHR anyway, so this is the primary defence and 1-3 are depth. This also covers
   `POST /login` itself, which has no session yet - there, Origin/Sec-Fetch-Site are the only
   checks available, which is why they are mandatory rather than best-effort on that route.

**Clickjacking and response hygiene.** `respond()` (`RemoteHub.cpp:490-501`) gains a fixed header
block on every hub-generated response:

```
X-Frame-Options: SAMEORIGIN
X-Content-Type-Options: nosniff
Referrer-Policy: no-referrer
Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline';
  style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self';
  frame-ancestors 'self'; base-uri 'none'; form-action 'self'
```

`SAMEORIGIN`/`frame-ancestors 'self'` still permits the same-origin `/stream.html` iframe
(`stream_center.html:485`). `'unsafe-inline'` is unavoidable while the pages carry inline
`<script>`/`<style>`; a nonce is a later cleanup. **`frame-src` is deliberately omitted** - the
`rkind === 'url'` cell (`stream_center.html:492-497`) frames a printer's own web page on another
origin, and a `frame-src 'self'` would break it. Responses produced by `tunnel()`
(`RemoteHub.cpp:542-553`) pass through unchanged and are not affected.

**Transport DoS hardening** (currently absent, and the reason the hub cannot be exposed as-is):
- Cap the request head at 16 KiB - `asio::streambuf req(16384)` at `RemoteHub.cpp:450` instead of
  the unbounded default, plus a check that `\r\n\r\n` was actually found.
- Per-socket read/write timeouts (~20 s idle) - today a slowloris holds a thread forever.
- Cap concurrent connections (e.g. 64) in `accept_loop()` (`RemoteHub.cpp:762-772`); beyond that,
  respond 503 and close. One detached thread per connection is fine at that scale.
- Reject requests with `Transfer-Encoding` present (the parser only understands `Content-Length`).

**Upload and spawn.**
- `POST /h/api/instances/open` and `POST /h/i/<pid>/open` require `stage == full` **and** the CSRF
  header. The XHR already sends a custom header (`X-File-Name`, `stream_center.html:1100`), so
  adding `X-Hub-CSRF` is one line.
- Keep the existing extension allow-list (`RemoteHub.cpp:594-598`) - it is already correct.
- Make `MAX_UPLOAD` a policy value: keep 2 GiB for `private_v4`/loopback, default 512 MiB when
  the client is remote. Reject before reading the body (the check at `RemoteHub.cpp:600` already
  runs on `Content-Length`).
- One in-flight upload per session; a second gets 409.
- **Argument-injection fix**: reject a cleaned name that begins with `-`, and pass the absolute
  path to `spawn_slicer()` (`RemoteHub.cpp:1013`) already prefixed so it can never be parsed as an
  option. `CreateProcessW` quoting (`RemoteHub.cpp:348-361`) already rules out shell injection.
- Add a 4-hex-char random suffix to the upload folder name (`RemoteHub.cpp:603`) to kill
  same-second collisions.
- Strip `Cookie` and `Authorization` from the head replayed by `tunnel()`
  (`RemoteHub.cpp:1164-1167`) so the session cookie never reaches the instance API or go2rtc.

**Auth event log.** `<datadir>/hub/auth.log`, JSON-lines, separate from `hub.log`, capped at 5000
lines with rotation:
`{"ts":…, "event":"login_bad_password", "ip":"…", "ua":"…", "device":"…", "detail":"…"}`.
Events: `login_ok`, `login_bad_password`, `totp_ok`, `totp_bad`, `totp_replay`, `backup_used`,
`backup_exhausted`, `lockout_start`, `lockout_end`, `session_new`, `session_revoked`, `logout`,
`password_changed`, `totp_enrolled`, `totp_removed`, `qr_bootstrap`, `policy_changed`. Never log
secrets, codes, or cookie values.

**What the PC shows.**
- `/hub/` gains a **Security** card: password state, TOTP state, remaining backup codes, the
  trusted-device list with last-seen/IP and a **Revoke** button, **Sign out everywhere**, the
  current lockout state with a countdown + **Unlock now**, and the last 20 auth events.
- Tray: the tooltip built in `HubTaskBarIcon::refresh()` (`RemoteHub.cpp:1356-1364`) gains a line
  ("Sign-in locked - 12 failed attempts"), and a balloon fires once per lockout and once when a
  new device is trusted. The 5-second timer at `RemoteHub.cpp:1439-1440` already polls
  `snapshot()`, so `Snapshot` just needs `locked_until` and `new_device` fields.

---

### HTTPS and transport interplay

**What degrades over plain HTTP** - and it is not a long list, but two entries are decisive:

| Feature | plain HTTP on a LAN IP | `https://` (any source) |
|---|---|---|
| Password sent in the clear | yes | no |
| `Secure` cookie flag | **rejected by the browser** | yes |
| `__Host-` cookie prefix | **unusable** (implies Secure) | yes |
| `SameSite=None` | unusable (requires Secure) | n/a, we use Lax |
| WebAuthn / passkeys | **unavailable** (not a secure context) | yes, if the RP ID is a domain |
| TOTP | works | works |
| 30-90 day HttpOnly session cookie | works (Safari's 7-day cap is `document.cookie` only) | works |

**The rule the hub enforces**: *plain HTTP outside the LAN is refused.* Concretely, a request is
served only if `scheme == https`, **or** the peer class is `loopback`/`private_v4`/`tailscale`.
Anything else gets a 403 page explaining that the hub needs a TLS-terminating transport. Logins
are never accepted, and `Secure` cookies never issued, over plain HTTP from a non-private peer.

**How the phone can trust a cert when the hub terminates TLS itself** (the port-forward path):

1. **Self-signed, tap-through.** Browser interstitial every time the exception is cleared; iOS
   Safari makes it deliberately unpleasant. **Fingerprint pinning from the QR is impossible** - a
   browser gives page JS no access to the peer certificate, and there is no web API to pin one.
   Verdict: acceptable as "at least it is encrypted and `Secure` cookies work", bad as a daily
   experience.
2. **Self-signed + a per-device trusted CA.** The hub generates a small CA once, issues itself a
   cert for a fixed hostname, and exposes the CA cert for download from `/hub/`. iOS: install the
   profile, then Settings > General > About > Certificate Trust Settings > enable full trust.
   Android: install as a user CA (Chrome accepts it for browsing). Five minutes per device, once.
   **This is the only genuinely no-hosted-service HTTPS path**, and it also unlocks WebAuthn
   because the origin becomes a real trusted `https://<hostname>`.
3. **Let's Encrypt via DNS-01 + DDNS.** A real publicly trusted cert with no inbound port 80/443
   requirement - DNS-01 is the only challenge type that manages that, and the only one that does
   wildcards. Costs: a domain, a DNS provider with an API, an ACME client, and 90-day renewals.
   Best long-term answer if a domain is already owned.
4. **Let the transport terminate TLS** (recommended default). Tailscale, Cloudflare Tunnel and
   ngrok all present real certs on stable hostnames; the hub then speaks plain HTTP to the local
   agent on `127.0.0.1` and takes `scheme` from a **configured, trusted** `X-Forwarded-Proto`.
   No cert handling in the hub at all.

**Hub-side TLS is cheap to add when wanted**: `asio::ssl` already compiles in this target
(`BambuCamRelay.cpp:86-92`). The awkward part is that `write_all`, `respond`, `read_request`,
`read_small_body`, `spool_upload`, `pump`, `tunnel`, `serve`, `handle_hub` and `handle_phone` all
take `tcp::socket&`; they need templating on a `Stream&` so the client side can be an
`ssl::stream<tcp::socket>` while the upstream splice stays a plain socket. Mechanical, ~11
signatures, no logic change.

**Ranking for this project**: Tailscale + `tailscale serve` is the best fit - one stable hostname
(`<machine>.<tailnet>.ts.net`), a real cert, no inbound ports, a secure context so passkeys become
possible later, and the hub sees a loopback peer so only the proxy-header configuration is needed.
Cloudflare Tunnel on an owned domain is the equivalent if a permanent public name is wanted.
Port-forward+DDNS is the only one that needs option 2 or 3 above.

---

### Implementation plan

#### New files

| File | Contents |
|---|---|
| `src/slic3r/GUI/HubAuth.hpp/.cpp` | everything below: `auth.json` load/save, scrypt/PBKDF2 hash+verify, `RAND_bytes` wrapper, base32, TOTP, backup codes, session store, rate limiter, CSRF, the auth log. wx-free, like `RemoteHub.cpp`. |
| `resources/web/orca/login.html` | password screen, TOTP screen, backup-code screen; self-contained, one file, same visual language as `hub.html` |
| `scripts/hub_auth_smoke.py` | curl-level tests (below), in the style of `scripts/orca_cli_smoke.py` |

Add `GUI/HubAuth.cpp` / `.hpp` to `SLIC3R_GUI_SOURCES` near `src/slic3r/CMakeLists.txt:403`.
No CMake link changes - OpenSSL is already there (`src/slic3r/CMakeLists.txt:852`).

#### Routes

**Public listener** (`0.0.0.0:13640`):

| Method | Path | Auth |
|---|---|---|
| GET | `/login` | none (rate-limited) |
| POST | `/login` | none; Origin/Sec-Fetch-Site mandatory |
| GET/POST | `/login/totp` | `pending_2fa` session |
| POST | `/logout` | any session |
| GET | `/` `/index.html` | `full` |
| GET | `/h/state` | `full` |
| GET/POST | `/h/api/...`, `/h/i/<pid>/...` | `full` + CSRF on non-GET |
| GET | `/h/bambu`, `/h/ff` | `full` |
| * | `/stream.html`, `/video-*.js`, `/api/ws` | `full` |
| GET | `/r/<token>/…` | trusted-network bootstrap, LAN peers only; else 404 |
| (Phase C) | `/login/webauthn/{begin,finish}`, `/h/passkeys/{begin,finish}` | as noted |

**Admin listener** (`127.0.0.1:<admin_port>`), all existing `/hub/*` plus:

`GET /hub/auth` · `POST /hub/auth/password` · `POST /hub/auth/totp/begin` ·
`POST /hub/auth/totp/confirm` · `POST /hub/auth/totp/remove` ·
`POST /hub/auth/backup/regenerate` · `GET /hub/auth/devices` ·
`POST /hub/auth/devices/<h>/revoke` · `POST /hub/auth/devices/revoke_all` ·
`POST /hub/auth/unlock` · `POST /hub/auth/policy` · `GET /hub/auth/log`

#### Session middleware in `serve()`

The whole change is a block inserted after `read_request()` (`RemoteHub.cpp:1188`) and before the
go2rtc passthrough loop (`RemoteHub.cpp:1201`), plus the peer gate at `:1185` becoming a policy
call:

```cpp
// after read_request(client, r)
const Peer p = classify_peer(peer, r, m_auth.policy());   // replaces is_private_v4 at :1185
if (!p.allowed) { respond(client, 403, "text/html; charset=utf-8", refused_page(p)); return; }

if (is_admin_listener) { if (r.path.compare(0, 5, "/hub/") == 0) { handle_hub(client, r); return; } }
else if (r.path.compare(0, 5, "/hub/") == 0) { respond(client, 404, "text/plain", "not found"); return; }

const Session* s = m_auth.session_for(cookie_value(r.cookies, "sid"),
                                      cookie_value(r.cookies, "__Host-sid"));  // ct_eq inside
if (r.method != "GET" && r.method != "HEAD" && !m_auth.csrf_ok(r, s)) {
    respond_json(client, 403, json_error("cross-site request refused")); return;
}
if (!s || s->stage != Session::full) {
    if (handle_login_routes(client, r, p, s)) return;          // /login, /login/totp, /logout
    if (bootstrap_from_token(client, r, p)) return;            // LAN QR -> mint session, 302 /
    if (wants_json(r)) respond_json(client, 401, "{\"error\":\"login required\",\"login\":\"/login\"}");
    else respond(client, 302, "text/plain", "", "Location: /login?next=" + percent_encode(r.path) + "\r\n");
    return;
}
m_auth.touch(s, p.client_ip);   // last_seen, lazy expiry extension
```

Everything downstream (`GO2RTC_PASSTHROUGH`, `handle_phone`) then simply drops its token checks:
`RemoteHub.cpp:1203` and `:1215-1226` disappear.

`read_request()` (`RemoteHub.cpp:466-475`) needs five more header cases - `host`, `origin`,
`referer`, `user-agent`, and the configured forwarded header (plus `x-forwarded-proto`,
`sec-fetch-site`, `x-hub-csrf`) - and `Request` (`RemoteHub.cpp:442-446`) the matching fields.

#### Representative code

```cpp
// --- base32 (RFC 4648 §6), unpadded, for the otpauth secret -------------------
static std::string base32_encode(const unsigned char* p, size_t n)
{
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string out; uint32_t buf = 0; int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        buf = (buf << 8) | p[i]; bits += 8;
        while (bits >= 5) { out += A[(buf >> (bits - 5)) & 31]; bits -= 5; }
    }
    if (bits) out += A[(buf << (5 - bits)) & 31];
    return out;                                    // padding omitted per the Key-Uri spec
}

// --- TOTP: RFC 6238 §4.2 + RFC 4226 §5.3 -------------------------------------
static uint32_t totp_at(const std::vector<unsigned char>& key, uint64_t step)
{
    unsigned char msg[8];
    for (int i = 7; i >= 0; --i) { msg[i] = (unsigned char)(step & 0xff); step >>= 8; }
    unsigned char mac[EVP_MAX_MD_SIZE]; unsigned int len = 0;
    HMAC(EVP_sha1(), key.data(), (int) key.size(), msg, sizeof(msg), mac, &len);
    const int off = mac[len - 1] & 0x0f;                        // low 4 bits of the last byte
    const uint32_t bin = ((uint32_t)(mac[off] & 0x7f) << 24) |  // "last 31 bits"
                         ((uint32_t) mac[off + 1] << 16) |
                         ((uint32_t) mac[off + 2] <<  8) |
                          (uint32_t) mac[off + 3];
    return bin % 1000000u;                                      // 6 digits
}

// Accept t-1, t, t+1; reject anything at or below the last accepted step (replay).
bool HubAuth::totp_verify(User& u, const std::string& code, long long now)
{
    if (code.size() != 6 || code.find_first_not_of("0123456789") != std::string::npos) return false;
    const uint64_t t = (uint64_t)(now / 30);
    for (int d = -1; d <= 1; ++d) {
        const uint64_t step = t + d;
        if (step <= u.totp_last_step) continue;                 // replay / window reuse
        char want[7]; std::snprintf(want, sizeof(want), "%06u", totp_at(u.totp_key, step));
        if (ct_eq(want, code)) { u.totp_last_step = step; return true; }
    }
    return false;
}

// --- password: scrypt with a PBKDF2 fallback, params carried in the string ----
std::string HubAuth::hash_password(const std::string& pw)
{
    unsigned char salt[16], out[32];
    if (RAND_bytes(salt, sizeof(salt)) != 1) throw std::runtime_error("no entropy");
##ifndef OPENSSL_NO_SCRYPT
    if (EVP_PBE_scrypt(pw.data(), pw.size(), salt, sizeof(salt),
                       1u << 16, 8, 2, 192ull << 20, out, sizeof(out)) == 1)
        return "$scrypt$ln=16,r=8,p=2$" + b64(salt, 16) + "$" + b64(out, 32);
##endif
    PKCS5_PBKDF2_HMAC(pw.data(), (int) pw.size(), salt, sizeof(salt),
                      600000, EVP_sha256(), sizeof(out), out);
    return "$pbkdf2-sha256$i=600000$" + b64(salt, 16) + "$" + b64(out, 32);
}

static bool ct_eq(const std::string& a, const std::string& b)
{   // length is not secret here, but never early-exit on content
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}
```

Cookie emission:

```cpp
std::string set_session_cookie(const std::string& sid, bool https, long long max_age)
{
    return std::string("Set-Cookie: ") + (https ? "__Host-sid=" : "sid=") + sid +
           "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" + std::to_string(max_age) +
           (https ? "; Secure" : "") + "\r\n";
}
```

#### Page changes

**`resources/web/orca/stream_center.html`** (small, five spots):
- `:214-217` - `REMOTE` becomes `'/h/'` (keep parsing `/r/<t>/` only to detect a legacy URL and
  redirect once).
- `:219-223` - **delete** the `document.cookie = 'rt=…'` line; keep the two `display:none` lines.
- `:1018` - `jsonOf` gains: on `401` with a `login` field, `location.href = j.login + '?next=' +
  encodeURIComponent(location.pathname)`.
- `:1019-1023, 1089` - path prefixes follow the table above.
- `:1095-1125` - `uploadFile` adds `xhr.setRequestHeader('X-Hub-CSRF', CSRF)`; a small
  `fetch` wrapper adds the same header to every non-GET. `CSRF` is delivered in the page as a
  `<meta name="hub-csrf">` the hub substitutes when serving `/`, or fetched once from `/h/state`.
- A footer "Signed in as owner · Log out" that POSTs `/logout`.

**`resources/web/orca/hub.html`** - a new **Security** card between the two existing cards
(`:30-33`), built with the same `el()`/`post()` helpers (`:45-46`) and the existing `drawQr`
(`:47-56`) for the otpauth QR:
- Set / change password (two fields, strength hint).
- Enrol TOTP: `begin` -> QR + the base32 secret in text for manual entry -> a 6-digit confirm
  field -> `confirm`. Remove TOTP (requires the current password).
- Backup codes: show once after generation, `Copy` and `Print`, plus "N of 8 remaining" and
  Regenerate.
- Trusted devices: name, first/last seen, last IP, Revoke; Sign out everywhere.
- **Trusted network** toggle, with one line of plain-English consequence.
- Behind-a-tunnel toggle + header picker + "require HTTPS" checkbox.
- Lockout banner with countdown and Unlock now; last 20 auth events.
- The existing phone card's copy changes: the QR is described as a LAN shortcut, and when a
  remote transport is configured it shows the hostname to use instead.

#### Phasing and effort

Estimates are for one developer already familiar with this code, including tests and manual
device checks on one iPhone and one Android.

| Phase | Content | Effort |
|---|---|---|
| **A** | Admin/public listener split (closes the `/hub/` hole); policy-based peer gate incl. `100.64/10`; forwarded-header handling; `HubAuth` skeleton + `auth.json`; `RAND_bytes`/`ct_eq`; scrypt password; sessions + trusted-device cookie; `/login`, `/logout`; middleware in `serve()`; route move to `/h/…`; go2rtc gate swap; LAN QR bootstrap; security headers; transport DoS caps (head size, timeouts, connection cap); upload/spawn gating + argv fix. Works on LAN and through every tunnel. | **4-5 days** |
| **B** | base32; TOTP verify + `last_step`; two-step enrolment with the otpauth QR on `hub.html`; 8 backup codes; `/login/totp`; rate limiter + lockout + Unlock now. | **2 days** |
| **C** | Passkeys. Minimal CBOR reader, COSE_Key -> `EVP_PKEY`, ES256 assertion verification, challenge store, registration with `attestation:"none"`, RP-ID configuration and a clear warning that changing the hostname orphans credentials. **Only worth starting once one stable HTTPS hostname is settled.** | **4-6 days** |
| **D** | Device list UI + revoke + sign-out-everywhere; `auth.log` + rotation + the hub-page viewer; tray tooltip/balloon on lockout and new device; policy editor UI. | **2 days** |
| **E** (optional) | Hub-terminated TLS: template the stream type across ~11 signatures, `ssl::context(tls_server)`, cert/key loading, self-signed + local-CA generation and a CA download from `/hub/`. | **2-3 days** |

Phase A alone makes remote exposure defensible. B is what the user asked for as "basic 2FA". C is
optional and gated on transport choice. D is quality-of-life plus the forensics you want the first
time a login fails at 3 a.m.

---

### Test plan

`scripts/hub_auth_smoke.py` (pattern: `scripts/orca_cli_smoke.py`, `check(cond, msg, failures)`),
driving a hub started with a throwaway `--datadir`. Every case is curl-level so it also documents
the wire format.

**Session and cookie mechanics**
1. `GET /` with no cookie -> `302` to `/login?next=%2F`.
2. `GET /h/state` with `Accept: application/json` and no cookie -> `401`, body has `login`.
3. `POST /login` wrong password -> `401`, no `Set-Cookie`.
4. `POST /login` correct, TOTP enrolled -> `200`, `Set-Cookie` present, `next` = `/login/totp`,
   and `GET /` still `302`s (a `pending_2fa` session must not open anything).
5. `POST /login/totp` with a code from an independent Python TOTP implementation -> `200`, and the
   `sid` value **differs** from step 4 (rotation).
6. Cookie flag assertions: over HTTP, `sid=…; Path=/; HttpOnly; SameSite=Lax; Max-Age=…` and
   **no** `Secure`, name is `sid`. With `X-Forwarded-Proto: https` from a trusted-proxy config,
   name is `__Host-sid` and `Secure` is present.
7. `remember=1` -> `Max-Age` within 1 % of `remember_days`; `remember=0` -> `session_hours`.
8. `POST /logout` -> `Max-Age=0`, and the old cookie is dead on the next request.

**TOTP**
9. Replay: the same code twice -> second attempt `401` with `totp_replay` in `auth.log`.
10. Window: `t-1` and `t+1` accepted, `t-2` and `t+2` rejected.
11. Non-numeric / 5-digit / 7-digit input rejected without an HMAC being computed.
12. Backup code: one accepted, the *same* code rejected on reuse, remaining count decrements.
13. Enrolment is not committed unless `confirm` succeeds - kill the hub between `begin` and
    `confirm`, restart, and `auth.json` has no `totp` block.

**Lockout**
14. 12 bad passwords from one IP -> the 13th returns `429` with `Retry-After`; a *correct*
    password during the window still returns `429`.
15. `POST /hub/auth/unlock` on the admin port clears it immediately.
16. Global: 20 failures spread over 20 forged `X-Forwarded-For` values with the proxy config on ->
    global lock engages (this is the tunnel case).

**Boundaries**
17. `GET /hub/info` on the **public** port -> `404` (this is the regression test for the tunnel
    hole; it must fail loudly if the listener split is ever reverted).
18. `GET /hub/info` on the admin port -> `200`, and `token` is absent from the payload.
19. `GET /r/<token>/` from `127.0.0.1` with `trusted_network=1` -> `302 /` + a session cookie;
    with `trusted_network=0` -> `404`; with a forged public `X-Forwarded-For` -> `404`.
20. `POST /h/api/instances/open` with a valid session but no `X-Hub-CSRF` -> `403`;
    with `Origin: https://evil.example` -> `403`; with `Sec-Fetch-Site: cross-site` -> `403`.
21. Upload of a `.exe`, of a name starting with `-`, and of a body larger than the remote cap ->
    `400`/`413`, and no process is spawned (assert the instance count is unchanged).
22. A 32 KiB request head -> connection closed, hub still responsive.
23. 100 simultaneous idle connections -> hub still answers on the 101st or returns 503; no
    unbounded memory growth.
24. Security headers present on `/login`, `/`, and every JSON response.

**Unit tests** (Catch2, `tests/slic3rutils/`): RFC 6238 Appendix B vectors adapted to 6 digits,
RFC 4648 §10 base32 vectors, scrypt round-trip and the PBKDF2 fallback, `ct_eq`, and the
`auth.json` parser against a truncated/corrupt file (it must fail closed - refuse all logins and
say so on the hub page, never fail open).

**Manual**: iOS Safari and Android Chrome - scan the otpauth QR into Google Authenticator and iOS
Passwords; confirm the 60-day cookie survives a browser restart, a device reboot, and 8+ days
(the Safari ITP question); confirm the go2rtc player still plays after the `rt` cookie is removed;
confirm `autocomplete="one-time-code"` surfaces the code.

---

### Risks / open questions

1. **OpenSSL 1.1.1w is end-of-life** (final release 11 Sep 2023, `opensslv.h:43`). Everything in
   this design works on it, but it means no Argon2id and no security updates. A 3.x bump is a
   separate, larger piece of work; the PHC-style hash strings are chosen so that adding
   `$argon2id$…` later is a verifier addition, not a migration.
2. **`EVP_PBE_scrypt` has no in-tree precedent.** Verify at build time that the deps build did not
   set `OPENSSL_NO_SCRYPT` (the configure line at `deps/OpenSSL/OpenSSL.cmake:48-55` does not
   disable it, but it has never been exercised). The PBKDF2 fallback exists precisely for this.
3. **64 MiB of scrypt on every login attempt is a DoS lever.** The rate-limit check must run
   *before* the KDF, and the global lockout must be reachable, or an attacker can pin a core with
   ~20 requests. This is called out in the abuse section but is the easiest thing to get wrong.
4. **iOS Safari's 7-day cap on the trusted-device cookie**: the WebKit posts are explicit that
   HttpOnly `Set-Cookie` is exempt, but that is 2019/2020 documentation and Safari has changed a
   lot since. Verify empirically over 8+ days before promising 60 days in the UI; if it turns out
   capped, degrade gracefully by re-issuing the cookie on every visit (sliding) rather than
   claiming a fixed lifetime.
5. **Safari's `Origin` on same-origin POST is spec-mandated but was not independently verified**
   for a current Safari build. The design does not depend on it (the `X-Hub-CSRF` header is the
   primary check and `Sec-Fetch-Site` covers Safari 16.4+), but the fallback ordering should be
   confirmed on a real device before Phase A ships.
6. **Cookies are not port-isolated** (RFC 6265 §8.5). On a shared LAN IP, any other listener on
   the PC can read the session cookie. `__Host-` does not help over plain HTTP. This is inherent
   to the LAN-over-HTTP mode and is an argument for making an HTTPS transport the default
   recommendation rather than a nicety.
7. **Mixing `http://192.168.x.x` and `https://name` for the same hub gives two independent
   sessions** (different origins, and Chrome's schemeful-same-site treats them as cross-site).
   The UI should present one recommended address rather than implying they are interchangeable.
8. **`/hub/*` currently leaks the token to anything that reaches it from loopback**
   (`RemoteHub.cpp:699,1211`). Until the listener split lands, **the hub must not be placed behind
   a tunnel at all.** Worth stating in the commit message and in any interim README.
9. **`hub.json` and `auth.json` are plaintext on disk.** Anyone with the user's profile can read
   the password *hash*, the TOTP secret, and the session hashes. The TOTP secret is the real
   exposure - it is a symmetric secret and cannot be hashed. Windows DPAPI (`CryptProtectData`)
   could wrap it; `crypt32` is already linked for `src/ultranet` but not for `libslic3r_gui`.
   Deferred, flagged.
10. **Passkeys need a hostname decision that this document cannot make.** Phase C should not start
    until the transport pass picks one name that will be used from inside and outside the LAN.
11. **Family beyond one shared password** would want per-person users and per-person TOTP. The
    `users` array supports it; the UI does not. Decide before Phase D whether the device list is
    labelled per person.
12. **`spawn_slicer` from an authenticated phone request remains the highest-value target** in the
    whole surface - it starts a process with an attacker-supplied file. The extension allow-list
    and the argv fix reduce it, but the residual risk is the slicer's own 3MF/STEP parsers. Worth
    a separate look at whether the spawned instance can be sandboxed (job object with restricted
    limits already exists for go2rtc at `RemoteHub.cpp:801-809`).
13. **Not investigated**: whether `wxTaskBarIcon::ShowBalloon` is available in the vendored
    wxWidgets build on Windows. If not, the lockout notification falls back to the tooltip text,
    which `HubTaskBarIcon::refresh()` (`RemoteHub.cpp:1356-1364`) already rebuilds every 5 s.

---

# Part C - Threat model, hardening and relay fallback (research pass)

## Remote access — threat model, hardening and relay fallback

Target: `snapmaker-orca --hub` (`src/slic3r/GUI/RemoteHub.cpp`) on branch `feat/ultra-preferences`,
exposed beyond the LAN. Read-only review at `C:\Dev\SnapmakerOrca`; external facts fetched
2026-09-02 and cited inline.

> **Line numbers are against the working tree as of 2026-09-02 15:39 local**, not HEAD. Both files
> were edited *during* this review (`RemoteHub.cpp` 1631→1642, `RemoteAccess.cpp` 1521→1807 lines);
> the diff is the new "needs attention" plumbing plus `api_attention_clear` and `api_debug`, and it
> does not change any finding below. Re-verify offsets with `grep -n` before acting.

**Headline.** Two findings are *live today on the LAN-only build* and need no internet exposure:
any web page the user visits while the hub runs can (a) reach `POST http://127.0.0.1:1364x/hub/*`,
because the only gate is "peer is loopback" (`RemoteHub.cpp:1220-1221`), and (b) reach go2rtc's full
admin API on the fixed port `127.0.0.1:21984`, which the hub configures with `origin: "*"`
(`RemoteHub.cpp:799`) and which skips authentication for loopback peers by design. (b) is a
browser-to-RCE chain. Fix both before thinking about the internet.

**Second headline.** Every proposed transport except one terminates at 127.0.0.1 on the PC, so it
inherits `peer.is_loopback() == true` and hands the internet the `/hub/*` control plane. And the one
exception — Tailscale — **does not work at all today**, because `is_private_v4`
(`RemoteHub.cpp:119-124`) omits the 100.64.0.0/10 CGNAT range that Tailscale assigns.

---

### Threat model

Assets, in rough order of what an attacker wants:

| Asset | Where it lives | Reachable via |
|---|---|---|
| Arbitrary code execution on the PC | — | go2rtc `/api/config` + `/api/restart`; unbounded process spawning |
| The user's files and open project | `load_project`, `save_project`, preset writes | `/i/<pid>/api/project/open`, `/api/settings/process/save`, `/api/quit` |
| Camera credentials (`rtsp://user:pass@…`) | `m_state` in RAM, `<datadir>/hub/streams.json` **in plaintext** (`RemoteHub.cpp:1082`), go2rtc's stream table | `GET /api/streams` on :21984; `GET /hub/info`→token (`:702`) |
| Printer control / Bambu access codes | `lookup_host` (`RemoteHub.cpp:881-898`), `DeviceManager` | `/r/<t>/bambu`, `/r/<t>/api/printers`; **remote print-start is a future route and will be the crown jewel** |
| Cloud logins the slicer holds | `GUI_App` / connectivity plugin — not exposed by these routes *today* | future `/i/<pid>/api/*` growth (the proxy is a catch-all) |
| The LAN behind the PC | go2rtc source URLs; `Http::post("http://" + ip …)` (`RemoteHub.cpp:1254`) | SSRF primitives below |
| Camera video | go2rtc tunnel | `/api/ws`, `/stream.html` |

Attackers: **IS** internet scanner · **CS** credential stuffing / token guessing · **LAN** malicious
device on the same L2 · **VEN** compromised or curious tunnel vendor · **PH** lost/stolen phone ·
**LEAK** shared-link leak (Referer, screenshot, history, vendor logs) · **WEB** any website the user
visits in a browser on the PC.

Likelihood/impact are for the post-exposure state, except rows marked **(today)**, which are
exploitable on the current LAN-only build.

| # | STRIDE | Entry point | Attacker | Lik. | Imp. | Existing mitigation (file:line) | Gap |
|---|---|---|---|---|---|---|---|
| T1 | **E**oP → RCE **(today)** | `POST http://127.0.0.1:21984/api/config` then `/api/restart` | WEB | High | Critical | none — `api.origin: "*"` is set explicitly at `RemoteHub.cpp:799`; go2rtc skips auth for loopback peers | Fixed port; no auth from loopback; `origin:"*"` emits `Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS` so a cross-origin preflight succeeds. Config-file streams bypass the `exec:` validation ⇒ write-config-then-restart = arbitrary command execution. Same class as CVE-2024-29192 |
| T2 | **T**ampering / **E**oP **(today)** | `POST /hub/state`, `/hub/new`, `/hub/phone`, `/hub/quit` | WEB (CSRF), VEN, IS after tunnelling | High | High | loopback-only check `RemoteHub.cpp:1221` | No Origin/Host/CSRF check. A `text/plain` body is a CORS-simple request ⇒ no preflight ⇒ the side effect fires. Port 13640 with a 20-port fallback scan (`:66`, `:740-744`) is trivially enumerated. `/hub/phone?on=1&token=<attacker>` (`:1085`) pins a known token **and** binds 0.0.0.0 |
| T3 | **E**oP | any `/hub/*` route once a tunnel or relay fronts the hub | IS, VEN | High | High | *the loopback check is the mitigation, and the tunnel defeats it* | cloudflared / ngrok / a relay / socat / SSH all connect from 127.0.0.1, so `peer.is_loopback()` is true and the entire control plane is on the internet. **Single most important blocker** |
| T4 | **S**poofing | every route | IS; WEB via DNS rebinding | Med | High | peer-address gates only (`RemoteHub.cpp:1195`, `:1221`; `RemoteAccess.cpp:1752`) | No `Host:` validation anywhere (`RemoteHub.cpp:463-477` parses only cookie / content-length / x-file-name). A domain rebinding to 127.0.0.1 makes the attacker's JS same-origin: it reads `GET /hub/info`, which returns the token in the clear (`:702`), sets `document.cookie`, and drives everything |
| T5 | **I**nfo disclosure **(today)** | `GET http://127.0.0.1:21984/api/streams` | WEB | High | High | none (`origin:"*"`) | go2rtc stream objects carry their producer URLs — i.e. the `rtsp://user:pass@…` sources `register_streams` PUTs in (`RemoteHub.cpp:840`). Directly breaks the design's central promise that "camera passwords never leave the PC" |
| T6 | **S**poofing / **E**oP | `/r/<token>/…` | CS, LEAK, PH | Med | High | 14 chars from a 32-symbol alphabet (`RemoteHub.cpp:185-194`); prefix check `:1226-1228` | A bearer credential **in the URL path**: leaks via `Referer`, history, QR screenshots, vendor access logs and anyone who saw the phone. No expiry, no per-device revocation. No rate limit, no lockout, no failed-attempt log. Generated from MT19937 with a 64-bit seed (F9) |
| T7 | **I**nfo disclosure | `rt` cookie | LAN, VEN | Med | Med | `Set-Cookie: rt=…; Path=/; SameSite=Lax` (`RemoteHub.cpp:1236`) | No `Secure`, no `HttpOnly`, no `Max-Age`. **Cookies ignore port**, so `rt` is sent to *every* HTTP service on the same host — Moonraker, OctoPrint, the app's own 13650 server — and any of them can overwrite it. On a shared vendor domain (`*.ngrok-free.app`, `*.trycloudflare.com`) cookie scope must be re-checked against the Public Suffix List |
| T8 | **I**nfo/**T** | `/r/<t>/…` in cleartext | IS, LAN, VEN, any hop | High | High | none — the hub has no TLS | Token, uploads, previews, camera video and printer state are plaintext unless the transport encrypts. With Cloudflare Tunnel / ngrok / Funnel the **vendor** holds the TLS session and sees all of it |
| T9 | **D**oS | any connection | IS | High | High | none | `asio::streambuf req;` is unbounded (`RemoteHub.cpp:450`), no read/write deadlines, one detached OS thread per connection (`:773`) plus a second per tunnel (`:550`). Slow-loris ⇒ memory and thread exhaustion. Same shape at `RemoteAccess.cpp:1757`, `:1739` |
| T10 | **D**oS / resource | `POST /r/<t>/api/instances/open` | CS+token, LEAK | Med | High | 2 GiB `Content-Length` cap (`RemoteHub.cpp:70`, `:600`); extension allow-list (`:594-598`) | No disk quota, no aggregate cap, no concurrency cap, and **nothing anywhere deletes `<datadir>/hub/uploads`** (grep for `uploads_dir` returns only `ensure_dirs`, `spool_upload` and the read-side guard). Each call also spawns a full slicer + OpenGL process (`:1135` → `:1020-1030`), unthrottled |
| T11 | **E**oP → memory safety | uploaded `.3mf` / `.stl` / `.obj` / `.step` | CS+token, PH, LEAK | Med | Critical | extension allow-list only | Once the token is known, the internet feeds attacker-chosen bytes into libslic3r's 3MF (zip + XML), STL, OBJ and OCCT STEP loaders — large C++ parsers written for trusted local files. **The most likely path to memory-corruption RCE** |
| T12 | **S**SRF / **I** | `POST /hub/state` → `register_streams` | WEB (CSRF), VEN, IS post-tunnel | Med | Med | `src` is percent-encoded (`RemoteHub.cpp:840`) | `src` is fully attacker-controlled: go2rtc will fetch arbitrary `rtsp://` / `http://` URLs from the PC ⇒ LAN scanning and blind exfil. `name` is **not** encoded, so `&` in a name injects extra query parameters into the PUT. `exec:` is refused by go2rtc ≥1.9.12's insecure-source validation — one upstream check, not a design guarantee |
| T13 | **S**SRF | `GET /r/<t>/ff?id=` | needs a poisoned `m_state` | Low | Low | `ip` rejected if it contains `"'\<>` (`RemoteHub.cpp:1249`) | The filter still allows `/ @ ? #`, so `ip = "evil.com/x?"` retargets `Http::post("http://" + ip + ":8898/detail")` (`:1254`). Reachable only after T2/T12 poison `m_state` |
| T14 | **T**ampering / **D**oS | `/r/<t>/i/<pid>/api/*` proxy | any token holder | Med | High | `MAX_API_BODY` 64 KiB (`RemoteHub.cpp:1171`); instance is loopback-bound (`RemoteAccess.cpp:375`, `:1752`) | The proxy is `sub.compare(0, 4, "/api") == 0` (`RemoteHub.cpp:1170`) — a **catch-all**, so the whole instance API is exposed and every future route is auto-exposed. Today that includes `/api/quit`, `/api/settings/process/save` (writes preset files), `/api/objects/transform`, `/api/slice`, `/api/project/open`, and the brand-new `/api/debug/{sleep,modal,file}` (`RemoteAccess.cpp:1450-1475`, env-gated off by default, but it blocks the GUI thread and opens native dialogs). Prompts auto-answer Yes (`RemoteAccess.cpp:113`, `:336`), so nothing on the PC asks the human |
| T15 | **I**nfo disclosure | error and success bodies | any token holder | High | Low | none | `j["file"] = path` returns the absolute Windows path incl. the username (`RemoteHub.cpp:1138`); `"cannot write to " + uploads_dir()` (`:608`); `"slicer unreachable: " + e.what()` (`:1179`); project paths at `RemoteAccess.cpp:1425`, `:507`, `:1588` |
| T16 | **S**poofing | `/api/ws` WebSocket | WEB after T4 | Med | Med | `rt` cookie check (`RemoteHub.cpp:1213`); `SameSite=Lax` blocks the cross-site subresource handshake | The hub performs **no `Origin` check**, and `api.origin: "*"` disables the one go2rtc has (`internal/api/ws/ws.go` forces `CheckOrigin` to always-true when origin is `*`). After DNS rebinding (T4) the attacker's own JS sets the cookie |
| T17 | **R**epudiation | everything | all | High | Med | `BOOST_LOG_TRIVIAL` for lifecycle events only | No access log, no failed-auth log, no source IP recorded. After an incident there is nothing to look at |
| T18 | **I**nfo/**T** via vendor | Cloudflare Tunnel / ngrok / Funnel | VEN | Low | High | none | The vendor terminates TLS and sees plaintext: token, model files, camera video, printer state. Plus an availability dependency and an account-takeover path |
| T19 | **S**poofing | lost phone | PH | Med | High | none — the phone keeps the URL and the cookie indefinitely | No device binding, no session expiry, no selective revoke. The only revocation is toggling phone access off, which rotates the token for *every* device (`RemoteHub.cpp:1009-1011`) |
| T20 | **T**ampering | `--hub-token` / `SNORCA_PHONE_ACCESS` | local malware | Low | Med | `valid_token` 10–32 chars (`RemoteHub.cpp:180-183`) | Anyone who can set an environment variable or spawn `--hub --hub-token aaaaaaaaaa` fixes the token to a known value |

**Confirmed loopback-only, no bypass found.** `/hub/*` is gated at `RemoteHub.cpp:1220-1221`; the
check precedes `handle_hub` and every route is an exact string match, so `//hub/`, `/HUB/` and
`/r/<t>/../hub/info` all fall through to a 404. The instance API binds `address_v4::loopback()`
(`RemoteAccess.cpp:375`) and re-checks the peer (`:1752`). The app's other servers (13619/13650,
which used to serve `/localfile/<any path>`) are bound to 127.0.0.1 at `HttpServer.cpp:230-241`.
**All of that is undone the moment a tunnel or relay makes the internet look like 127.0.0.1** — T3.

---

### Code review findings (prioritised)

#### F1 — CRITICAL, exploitable today: `api.origin: "*"` on a fixed loopback port

`RemoteHub.cpp:796-803` writes go2rtc's config; line 799 is
`cfg << "api:\n  listen: \"127.0.0.1:" << GO2RTC_API_PORT << "\"\n  origin: \"*\"\n"`, with
`GO2RTC_API_PORT = 21984` at `:67`.

go2rtc "passes requests from localhost and Unix sockets without HTTP authorization, even if you have
it configured" (https://go2rtc.org/internal/api/, 2026-09-02) — `local_auth` defaults to false.
`origin: "*"` installs a CORS middleware emitting `Access-Control-Allow-Origin: *`,
`Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS` and
`Access-Control-Allow-Headers: Authorization, Content-Type` (`internal/api/api.go`), and sets the
WebSocket `CheckOrigin` to always-true (`internal/api/ws/ws.go`).

The registered endpoints include `GET/POST/PATCH /api/config`, `POST /api/restart`, `POST /api/exit`
and `PUT/PATCH/DELETE /api/streams`. `POST /api/config` writes attacker YAML to disk with no scheme
validation; `POST /api/restart` re-execs the binary; and config-file streams are loaded through
`NewStream` **without** the `Validate()` insecure-source check that guards the API path. go2rtc's own
README warns: *"If an attacker gains access to the API, you are in danger… an attacker can use
insecure sources such as echo and exec. And get full access to your server."*
(https://github.com/AlexxIT/go2rtc, 2026-09-02). This is the class of
**CVE-2024-29192 / GHSA-qgj8-g9q4-7f2p** (CSRF on `/api/config` → arbitrary command execution,
CVSS v4 8.7, fixed in 1.9.0) — `origin: "*"` opts back out of the fix.

Bundled version is **v1.9.14** (`resources/tools/go2rtc/LICENSE-NOTE.txt`; released 2026-01-19, with
no release since — the project is effectively single-maintainer, ~88% of commits from one author).
`internal/exec` and `exec/exec.go` are present in the shipped binary (verified via `strings` on
`resources/tools/go2rtc/go2rtc.exe`).

**Fix (`RemoteHub.cpp:796-803`)**: drop `origin: "*"`; set `local_auth: true` with a random per-run
`username`/`password`; add `allow_paths: [/api/ws, /api/webrtc, /api/frame.jpeg]` so `/api/config`,
`/api/restart`, `/api/exit` and `/api/streams` are never registered; replace the fixed 21984 with a
`unix_listen` socket (POSIX) or a random high port recorded only in `hub.json` (`:67`, `:724`). The
hub then injects credentials when proxying `/api/ws` (`:1211-1218`) and in `register_streams`
(`:840`). Note `allow_paths` filters by *path*, not method, so it cannot make `/api/streams`
read-only — leaving the route unregistered and keeping stream registration on a channel the browser
cannot reach is the only safe configuration.

#### F2 — CRITICAL: `/hub/*` is CSRF-able today and internet-exposed behind any tunnel

`RemoteHub.cpp:1220-1224` is the entire authorisation story for the control plane:

```cpp
if (r.path.compare(0, 5, "/hub/") == 0) {
    if (!peer.is_loopback()) { respond(client, 404, "text/plain", "not found"); return; }
    handle_hub(client, r);
```

No `Origin` check, no CSRF token, no `Content-Type` requirement. A cross-origin `POST` with
`Content-Type: text/plain` is a CORS-simple request: the browser sends it, the side effect happens,
and only the *response* is withheld from the attacker's script. The port is fixed at 13640 with a
20-port fallback scan (`:66`, `:740-744`), so a page just tries all twenty.

From a plain web page, while the hub runs:
* `POST /hub/state` (`:1075-1084`) — 4 MiB of attacker JSON becomes `m_state`, is written to
  `<datadir>/hub/streams.json` (`:1082`), and every `{rname, rsrc}` pair is PUT into go2rtc
  (`:823-851`) ⇒ T12.
* `POST /hub/phone?on=1&token=<attacker>` (`:1085-1087`) — binds 0.0.0.0 with a token the attacker
  chose. Combined with a LAN foothold, the phone surface is theirs permanently.
* `POST /hub/new` (`:1069-1074`) — spawns slicer processes, unbounded.
* `POST /hub/quit` (`:1088-1090`) — kills the hub.

Once *any* tunnel fronts the hub, `peer.is_loopback()` is true for internet traffic and all of the
above becomes remote-unauthenticated.

**Fix**: (a) require a per-process CSRF secret on every `/hub/*` request — the PC page already
fetches `/hub/info` first, so hand it the secret there and require it as a *custom header*, which
forces a preflight that a cross-origin page cannot satisfy; (b) reject any request whose `Host:` is
not `127.0.0.1:<port>` or `localhost:<port>` (also kills T4); (c) require
`Content-Type: application/json` on `/hub/state`; (d) stop relying on the loopback check once a
tunnel exists — bind `/hub/*` to a **separate acceptor on a random loopback port** that the tunnel
never fronts (`:731-763`), and keep 13640 for `/r/<token>/` only.

#### F3 — CRITICAL: unbounded header read, no timeouts, thread-per-connection

`RemoteHub.cpp:448-452`:

```cpp
asio::streambuf req;                                  // default max_size = SIZE_MAX
asio::read_until(client, req, "\r\n\r\n", ec);        // no deadline
```

A client that dribbles bytes without ever sending `\r\n\r\n` grows the streambuf without limit and
holds a detached OS thread (`:765-775`, detach at `:773`) plus, for tunnels, a second (`:550`).
Windows commits ~1 MiB of stack per thread by default, so a few thousand connections suffice.
`spool_upload` (`:612-617`) and `read_small_body` (`:561-566`) likewise `read_some` with no deadline.
The instance API repeats the pattern at `RemoteAccess.cpp:1757` and `:1739`, and is reachable through
the `/i/<pid>/api` splice.

**Fix**: `asio::streambuf req(16 * 1024)` (`RemoteHub.cpp:450`, `RemoteAccess.cpp:1757`); a
`steady_timer` deadline per phase — ~10 s for headers, ~30 s idle for body and tunnel; a global
atomic connection counter with a hard cap (e.g. 64) that 503s beyond it (`RemoteHub.cpp:765-775`);
and an Asio thread pool instead of thread-per-connection.

#### F4 — HIGH: no `Host:` validation ⇒ DNS rebinding defeats every gate

Neither server parses `Host` (`RemoteHub.cpp:463-477` handles only `cookie`, `content-length`,
`x-file-name`; `RemoteAccess.cpp:1766-1780` only `content-length`). The peer-address checks at
`RemoteHub.cpp:1195`, `:1221` and `RemoteAccess.cpp:1752` are therefore bypassable by a browser: an
attacker domain rebinding to 127.0.0.1 gives their JS a same-origin channel, and `GET /hub/info`
returns `token` in the clear (`:702`).

**Fix**: add a `host` case to the header loop and reject anything outside an allow-list
(`127.0.0.1:<port>`, `localhost:<port>`, the addresses from `lan_ips()`, and — once configured — the
tunnel hostname).

#### F5 — HIGH: uploads unbounded in aggregate and never deleted

`spool_upload` (`RemoteHub.cpp:583-620`). What is **right**: the basename is taken after
`percent_decode` and path-separator stripping (`:585-587`); control characters and `<>:"|?*` are
removed (`:589-590`); trailing space/dot stripped (`:591`); truncation keeps the **tail** so the
extension survives (`:592`); the extension allow-list is applied (`:594-598`); each upload gets its
own directory (`:603`). There is **no path traversal**, and the reader side re-checks containment
with `weakly_canonical` plus a parent-of-parent equality test (`RemoteAccess.cpp:1552-1555`). Good.

What is **wrong**:
* The per-file cap is 2 GiB (`:70`, `:600`) but there is **no cap on the number of files** and
  **no deletion anywhere in the tree** — grep for `uploads_dir` returns only `ensure_dirs`,
  `spool_upload` and the read-side guard. The disk fills.
* The folder name is `timestamp_compact()` at **one-second resolution** (`:603`): two uploads in the
  same second share a folder and the second overwrites the first — a correctness bug and a small
  TOCTOU window against `api_project_open`.
* Windows device names pass the filter: `NUL.stl` has extension `.stl`, so the `ofstream` opens the
  NUL device and the upload silently vanishes (the later `is_regular_file` check turns it into 404).

**Fix**: append `random_token()` to the folder name (`:603`); reject
`^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)` case-insensitively (`:593`); enforce a total quota over
`uploads_dir()` before writing (`:601`) and a per-token concurrent-upload limit; add a GC on hub
start and hourly that removes upload folders older than N hours and not referenced by a live
instance.

#### F6 — HIGH: no rate limit on process spawning

`POST /r/<t>/api/instances/open` (`RemoteHub.cpp:1132-1142`) calls `spawn_slicer` (`:1020-1030`) for
every upload — a full wxWidgets + OpenGL process each. `POST /hub/new` (`:1069-1074`) does the same
with no upload at all. Nothing counts them.

**Fix**: cap concurrent instances using `instances(false).size()` (already available at `:928`) at a
configurable maximum (default 4) and return 429 beyond it, at both `:1070` and `:1135`.

#### F7 — MEDIUM: `spawn_process` argv handling is safe, but only by construction

The brief asked specifically about a file name starting with `-`. It is **not** exploitable:
`spawn_slicer` (`:1022-1023`) passes the *absolute* path produced by `spool_upload` (`:606`), which
always begins with a drive letter, so no argument can be read as a switch. The Windows path builds a
command line via `quote_arg` (`:348-361`), which implements the MSVCRT backslash/quote rules
correctly; the POSIX path uses `execv` with a `char*[]` (`:420-424`) and never a shell. No injection.

The residual risk is that this safety is *incidental* — it depends on `uploads_dir()` staying
absolute. **Fix**: assert `fs::path(file).is_absolute()` in `spawn_slicer` (`:1022`) so a future
refactor cannot quietly break it.

#### F8 — MEDIUM: the tunnel rewrite is safe by accident; `force_close` misses `Transfer-Encoding`

`RemoteHub.cpp:1174-1175` builds a new request line from client-controlled tokens:

```cpp
const std::string line = r.method + " " + sub + (r.query.empty() ? "" : "?" + r.query) + " HTTP/1.1\r\n";
const std::string head = line + r.head.substr(r.head.find("\r\n") + 2);
```

CRLF injection is impossible **only because** `first >> r.method >> r.target` (`:461`) uses stream
extraction and `\r`/`\n` are whitespace for `std::isspace`, so neither token can contain them. That
is an undocumented dependency on iostream semantics guarding a request-smuggling primitive.

`force_close` (`:507-523`) strips `Connection:` and appends `Connection: close`, but leaves
`Transfer-Encoding` alone. The instance API ignores `Transfer-Encoding` and reads only
`Content-Length` (`RemoteAccess.cpp:1766-1780`), so `Content-Length: 0` + `Transfer-Encoding:
chunked` slips past the 64 KiB `MAX_API_BODY` check at `RemoteHub.cpp:1171` and the hub pumps
unlimited bytes downstream. **Actual smuggling is not achievable** — the hub handles exactly one
request per socket, forces `Connection: close`, and tears both sockets down in `pump` (`:535-537`) —
but the body cap is bypassable and the design is one refactor from being wrong.

**Fix**: validate `r.method` against a fixed set and reject any target byte outside `0x21-0x7E` in
`read_request` (`:459-461`); strip `Transfer-Encoding`, `X-Forwarded-*` and `Expect` (and `Upgrade`
outside the go2rtc passthrough) in `force_close` (`:507-523`); percent-encode `name` as well as `src`
in `register_streams` (`:840`).

#### F9 — MEDIUM: token from MT19937 with 64 bits of seed entropy

`random_token` (`RemoteHub.cpp:185-194`) seeds `std::mt19937_64` with two 32-bit `std::random_device`
draws and emits 14 symbols from a 32-symbol alphabet. The token *space* is 32^14 = 2^70, but the
*seed* space is 2^64, and Mersenne Twister is not a CSPRNG. Comparison is also non-constant-time
(`:1213` `!=` on `std::string`; `:1226` `compare`), though remote timing attacks across the internet
against a 14-byte compare are not practical.

**Fix**: draw the 14 symbols directly from `std::random_device` (or `BCryptGenRandom` / `getrandom`)
with rejection sampling (`:185-194`); add a `constant_time_equals` helper and use it at `:1213` and
`:1226`.

#### F10 — MEDIUM: cookie and response hygiene

`RemoteHub.cpp:1236` sets `rt=<token>; Path=/; SameSite=Lax` with no `Secure`, no `HttpOnly`, no
`Max-Age`; `stream_center.html:220` also sets it from JS, so `HttpOnly` alone is insufficient — the
page must stop needing it. Cookies ignore port, so `rt` is broadcast to every other HTTP service on
the host. `respond` (`:490-501`) emits no `Referrer-Policy`, `X-Content-Type-Options` or CSP —
`Referrer-Policy` matters directly here because the token is in the path and the page loads
third-party `rurl` iframes (`stream_center.html:496`).

**Fix**: once TLS exists, use a `__Host-rt` cookie (`Secure`, `Path=/`, no `Domain`) and remove the
JS write at `stream_center.html:220`; add `Referrer-Policy: no-referrer`,
`X-Content-Type-Options: nosniff` and a CSP in `respond` (`RemoteHub.cpp:490-501`).

#### F11 — LOW: information leakage in responses

`RemoteHub.cpp:1138` (`j["file"] = path`, absolute path incl. the Windows username); `:608`
(`"cannot write to " + uploads_dir()`); `:1179` (`e.what()` from the tunnel);
`RemoteAccess.cpp:1425`, `:507`, `:1588` (project paths). Also `read_file` returns `""` for a missing
resource, so a broken install serves `200 OK` with an empty body (`RemoteHub.cpp:1064`, `:1235`).

**Fix**: return an opaque upload id instead of the path at `:1138`; map exceptions to fixed strings
at `:1179`; 500 on an empty resource read.

#### F12 — LOW: `is_private_v4` semantics

`RemoteHub.cpp:119-124` accepts 10/8, 127/8, 172.16/12, 192.168/16 and 169.254/16. It does **not**
accept 100.64.0.0/10, which is what Tailscale assigns — see the Recommendation. It also accepts
169.254/16 link-local, which is not a meaningful trust boundary, and it `return`s with no response at
`:1195` for a rejected peer, which is fine against scanners but opaque to debug.

#### F13 — LOW: `BambuCamRelay` hardening

`BambuCamRelay.cpp:87` sets `verify_none` — unavoidable for the printer's self-signed certificate,
but a LAN attacker who can ARP-spoof the printer harvests the access code, which the relay sends in
an 80-byte auth packet (`:96-102`). `:109` emits `Access-Control-Allow-Origin: *` on the MJPEG
response; the relay is loopback-bound on a random port (`:173`), so the risk is low, but the header
is unnecessary. `parse_query` (`:20-53`) calls `std::stoi` on a percent escape without a `try`, but
`serve_client`'s catch (`:145`) turns a malformed `%zz` into a clean 502, not a crash.

---

### Hardening backlog

#### Blockers — do not expose to the internet until all of these are done

| | Fix | Where |
|---|---|---|
| **B1** | Remove `origin: "*"`; add `local_auth: true` + random credentials; `allow_paths` down to the streaming routes; move off the fixed port 21984 | `RemoteHub.cpp:796-803`, `:67`; inject credentials at `:840` and `:1211-1218` |
| **B2** | Move `/hub/*` to a separate loopback acceptor on a random port that no tunnel fronts; require a per-process CSRF secret as a custom header; require `Content-Type: application/json` on `/hub/state` | `RemoteHub.cpp:731-763`, `:1059-1109`, `:1220-1224` |
| **B3** | Validate `Host:` against an allow-list on both servers | `RemoteHub.cpp:463-477`; `RemoteAccess.cpp:1766-1780` |
| **B4** | Bound the header streambuf; per-phase deadlines; cap concurrent connections; thread pool instead of thread-per-connection | `RemoteHub.cpp:450`, `:556-568`, `:583-620`, `:765-775`; `RemoteAccess.cpp:1739`, `:1757` |
| **B5** | Real authentication in front of `/r/<token>/`: password + TOTP (the parallel design), a session cookie replacing the URL token, per-session expiry and revoke, failed-attempt throttling with exponential backoff and lockout, and an access log with source IP | new middleware in `HubServer::serve`, `RemoteHub.cpp:1189-1269` |
| **B6** | TLS on the hub listener, **or** a documented hard requirement that the transport is end-to-end encrypted to the device (Tailscale / WireGuard). Set `Secure` + `__Host-` on the cookie once TLS exists | `RemoteHub.cpp:731-763`, `:1236` |
| **B7** | Upload quota + GC + collision-proof folder names + Windows device-name rejection; cap concurrent slicer instances | `RemoteHub.cpp:583-620`, `:1070`, `:1135` |
| **B8** | CSPRNG for the token; constant-time comparison | `RemoteHub.cpp:185-194`, `:1213`, `:1226` |
| **B9** | Replace the `/i/<pid>/api` catch-all (`RemoteHub.cpp:1170`) with an explicit method+path allow-list, so new instance routes are opt-in rather than auto-exposed | `RemoteHub.cpp:1170-1182` |
| **B10** | Decide explicitly about hostile model files (T11). Minimum: run spawned instances at a lowered integrity level with job-object limits, and treat a parser crash as expected rather than surprising | `RemoteHub.cpp:366-438`, `:1020-1030` |

#### Nice-to-have

* Strip `Transfer-Encoding` / `X-Forwarded-*` / `Expect` in `force_close` (`RemoteHub.cpp:507-523`);
  validate the method and target charset (`:459-461`).
* Percent-encode `name` in `register_streams` (`RemoteHub.cpp:840`).
* Opaque upload ids and fixed error strings (`:1138`, `:1179`, `:608`).
* `Referrer-Policy: no-referrer`, `nosniff`, CSP in `respond` (`:490-501`).
* Assert the spawn path is absolute (`:1022`).
* Tighten the `/ff` host filter to a hostname/IP charset (`:1249`).
* Drop the unnecessary `Access-Control-Allow-Origin: *` in `BambuCamRelay.cpp:109`.
* Compile `/api/debug/*` (`RemoteAccess.cpp:1450-1475`, `:1662`) out of release builds rather than
  gating it on an environment variable — it blocks the GUI thread and opens native modal dialogs, and
  it reaches the phone through the catch-all proxy.

---

### Relay fallback design

Goal: no port forwarding, no vendor account, no vendor able to read the traffic.

```
PC ── outbound WSS ──▶ relay ◀── WSS ── phone browser
      (persistent)      pairs by id     (page loaded from the relay)
```

The PC dials out, so no inbound firewall rule and no public IP. The relay keeps
`map[pairId]*hostConn`, accepts one `/host?id=` and N `/join?id=`, and copies frames between them.

#### Why "just terminate HTTPS at the relay" is the wrong answer

TLS is end-to-end between the browser and *whoever holds the private key for the certificate the
browser validates*. Three options:

1. **Relay terminates TLS** with its own cert for `*.relay.example`. Simplest, and exactly what
   Cloudflare Tunnel, ngrok and Tailscale Funnel do — and it means the relay sees every byte: the
   session cookie, the model files, the camera video. Fails the stated goal.
2. **The hub holds a browser-trusted cert for its relay hostname**, relay does raw TCP SNI
   passthrough. Now the relay genuinely cannot read the stream. Cost: the relay operator must run a
   DNS zone and delegate `_acme-challenge.<id>.relay.example` per device; the hub must ship an ACME
   client and renew every 60 days; the relay must be an L4 SNI router, not an HTTP server —
   **which rules out Cloudflare Workers/Durable Objects entirely**, since a Worker sits *above* TLS
   termination and never sees the handshake. This is a real service, not a weekend project.
3. **Relay stays a dumb byte pipe; encrypt inside it.** Noise_NK over the WebSocket: the hub holds a
   static X25519 key whose public half is in the QR/pairing code, the phone is anonymous, and the
   hub-side password + TOTP authenticates the human afterwards. The only option that keeps the relay
   free, dumb and untrusted.

**The honest caveat for option 3**, which must appear in any user-facing claim: the page that
performs the encryption is served *by the relay, over the relay's TLS*. A malicious relay operator
can serve backdoored JavaScript and read everything. Option 3 therefore defends against a **passive**
relay — logs, subpoenas, a compromised host, a curious co-tenant, traffic retention — but **not**
against an actively malicious operator, unless the page is delivered out of band (an installed PWA
with a pinned service worker, an app-store build, or the page served by the hub itself under option
2). State this plainly rather than marketing it as "end-to-end encrypted".

#### Crypto, concretely

* **Hub side**: OpenSSL is already linked (`BambuCamRelay.cpp:4` pulls in `boost/asio/ssl.hpp`).
  `EVP_PKEY_X25519` + HKDF-SHA256 (`EVP_KDF`) + AES-256-GCM or ChaCha20-Poly1305 — no new dependency.
* **Phone side**: WebCrypto has X25519 via `crypto.subtle.deriveBits({name:'X25519'})`, plus HKDF and
  AES-GCM. Verify X25519 support on the actual target phones before committing; the fallback is a
  small audited library (noble-curves), which reintroduces the supply-chain question.
* Handshake: **Noise_NK** (`-> e, es` / `<- e, ee`) then a rekeying transport with a 64-bit nonce
  counter per direction, ~150 lines each side. Do not invent a bespoke handshake.

#### Multiplexing HTTP + video over one connection

The page needs many concurrent things: JSON API calls, PNG preview polls, a long-lived MJPEG `<img>`
from `BambuCamRelay` (`stream_center.html:490`), and the go2rtc MSE WebSocket
(`stream_center.html:485`). Two designs:

* **(a) One relay connection per browser connection.** Trivial relay, but the browser opens 6+ per
  origin, each needs a pairing round-trip, and it multiplies the relay's connection accounting.
* **(b) One relay connection with a yamux-style mux inside the Noise session.** Frame =
  `[u32 stream_id][u8 type][u24 len][payload]`, types `OPEN/DATA/CLOSE/WINDOW`; ~150 lines each side.
  The relay never sees stream boundaries — it forwards ciphertext. **Credit-based per-stream windows
  are mandatory**: without them a stalled video stream head-of-line-blocks every API call on the one
  TCP connection. This is where the "200 lines" estimate stops being true.

**The hard part is the browser side, not the relay.** For `<img src>` and `<iframe src>` to keep
working, a **Service Worker** must intercept `fetch` for the app's paths and pipe it through the
muxed encrypted socket, returning streaming `ReadableStream` responses — genuinely significant
engineering (and it needs a secure context, which the relay's TLS provides). The cheap alternative —
rewriting the page to call a JS shim instead of `fetch` — is far simpler but breaks exactly the two
things that matter most: the go2rtc `<iframe src="/stream.html">` and the MJPEG `<img src>`. Choose
consciously.

#### What a minimal relay actually is

* **Go, ~250–350 lines**: `net/http` + `gorilla/websocket`, two handlers, a mutex-guarded
  `map[string]*host`, `io.Copy` both ways, ping/pong every 30 s (ngrok idles out at **5 minutes**,
  most load balancers at 60–100 s), a token the host proves on connect, and byte accounting.
  Deploy on **Oracle Cloud Always Free** — 2× `VM.Standard.E2.1.Micro` (1/8 OCPU, 1 GB, one public
  IPv4, 50 Mbps) or `VM.Standard.A1.Flex` (up to 2 OCPU / 12 GB from 1,500 OCPU-hours + 9,000
  GB-hours per month), with **10 TB/month egress**
  (https://docs.oracle.com/en-us/iaas/Content/FreeTier/freetier_topic-Always_Free_Resources.htm,
  2026-09-02). Caveats from the same page: idle instances "may be reclaimed" when the 7-day 95th
  percentile CPU **and** network utilisation are both under 20% — a near-idle relay is at real risk —
  and "out of host capacity" errors are common.
  It is the only genuinely $0 option that can carry video: GCP's free `e2-micro` allows just
  **1 GB/month egress** (https://docs.cloud.google.com/free/docs/free-cloud-features, 2026-09-02);
  Fly.io stopped offering free plans to new customers on **2024-10-07**
  (https://fly.io/docs/about/discontinued-plans/) and now costs ~$2–4/mo; and AWS replaced the
  12-month free tier with **$200 of credits expiring after six months** for accounts created from
  mid-July 2025
  (https://aws.amazon.com/about-aws/whats-new/2025/07/aws-free-tier-credits-month-free-plan/).

* **Cloudflare Worker + Durable Object**: viable and free-tier-eligible, with a sharp ceiling.
  Durable Objects have been on the Workers Free plan since **2025-04-07**, SQLite-backed only
  (https://developers.cloudflare.com/changelog/post/2025-04-07-durable-objects-free-tier/;
  https://developers.cloudflare.com/durable-objects/platform/pricing/, 2026-09-02). One DO per
  pairing id via `idFromName(pairId)`; use the **WebSocket Hibernation API**
  (`state.acceptWebSocket`) so the idle PC connection accrues no duration charges
  (https://developers.cloudflare.com/durable-objects/best-practices/websockets/) — with plain
  `accept()`, "duration charges for the entire time the WebSocket is connected" apply. Ping/pong
  keepalives are handled by the runtime and do not interrupt hibernation.
  Free limits: **100,000 requests/day** (Workers and DO), 10 ms CPU per request, 128 MB memory,
  32 MiB max WebSocket message, and **6 simultaneous outbound connections per invocation**
  (https://developers.cloudflare.com/workers/platform/limits/). Inbound WebSocket messages bill at
  **20:1** against DO requests ("100 WebSocket incoming messages would be charged as 5 requests").
  **The arithmetic that decides it**: 100,000 DO requests/day × 20 ≈ **2M inbound messages/day**;
  a single continuously-watched 15 fps MJPEG stream is ~1.3M messages/day — **one camera, watched all
  day, roughly exhausts the free tier.** So: comfortable for the control API, marginal-to-unusable
  for video. There is also no documented maximum inbound WebSocket duration and no documented max
  connections per DO ("thousands of clients per instance"), so plan for reconnects.
  On terms: the old "non-HTML content / tunnelling" clause is **gone** — the current
  Service-Specific Terms (last updated 2026-06-02,
  https://www.cloudflare.com/service-specific-terms-application-services/) contain no section 2.8 and
  the words "tunnel"/"proxy" do not appear; the surviving large-file restriction is scoped to the
  **CDN** on Free/Pro/Business, and the Developer Platform terms (same date) impose no proxy/relay
  restriction. A control relay is not prohibited; sustained video through the edge is still the thing
  that attracts attention.

#### Who hosts it

**The user, not the project.** A project-run relay makes the project a bandwidth cost centre, an
abuse-report destination (it carries video from strangers' homes), a single point of failure, and a
target: even with option-3 encryption, the relay learns pairing ids, IP addresses, timing and volume.
Ship the relay as a self-hostable single Go binary plus a `docker run` one-liner and a Terraform
snippet for Oracle Always Free. If a hosted instance is ever offered, treat it as best-effort, with a
published abuse policy, hard per-id byte quotas, and no logging beyond counters.

#### Abuse controls

Pairing code shown as a QR on the PC: 8 characters, single-use, 60-second TTL, exchanged for a
long-lived per-device credential. The id on the wire is a random 128-bit value, never the
human-readable code. One host per id, N phones capped. Per-IP connect rate limit. Per-id daily byte
quota. No enumeration endpoint, and `/join` for an id with no live host returns the same response as
a wrong id, so scanning yields nothing.

#### Honest effort estimate

| Work | Estimate |
|---|---|
| Noise_NK: hub (OpenSSL) + page (WebCrypto), incl. tests | 2–3 weeks |
| Mux with credit-based flow control, both sides | 1–2 weeks |
| Service Worker fetch interception with streaming responses (the risky part) | 2–4 weeks |
| Relay server, deployment, abuse controls, ops runbook | 1 week |
| **Total** | **6–12 weeks for one developer, plus permanent operational ownership** |
| **Tailscale, for comparison** | **~1 hour** — one clause in `is_private_v4` (`RemoteHub.cpp:123`) plus documentation |

And the relay's security ceiling is *lower* than Tailscale's unless the crypto is done very well:
Tailscale is WireGuard — audited, device-authenticated, with mature key rotation and ACLs. Build the
relay only if "no third-party account" is a hard product requirement, not because it looks cheap.

---

### Recommendation

#### Transport verdict

| Transport | Verdict | Why |
|---|---|---|
| **Tailscale (tailnet, not Funnel)** | **Recommended** — but broken today | WireGuard end-to-end to the device, device-authenticated, nothing public, no vendor in the plaintext path. Free Personal plan: up to 6 users, unlimited devices, MagicDNS included (https://tailscale.com/pricing, 2026-09-02). **Blocker: `is_private_v4` (`RemoteHub.cpp:119-124`) rejects 100.64.0.0/10, exactly what Tailscale assigns, so every connection is silently dropped at `:1195`.** One-clause fix at `:123`: add `\|\| (v >> 22) == 0x191`. Because the peer is then 100.x and *not* loopback, `/hub/*` correctly stays closed (`:1221`) — **the only transport with that property** |
| **WireGuard / self-hosted VPN** | **Acceptable** | Same properties without the vendor. 10.x already passes `is_private_v4` |
| **Cloudflare Tunnel** | Acceptable **only with B1–B10 done** | Outbound-only, no port forwarding, TLS to Cloudflare; put **Cloudflare Access** in front so the hub is never anonymously reachable. Two costs: Cloudflare terminates TLS and sees plaintext (T18), and cloudflared connects from **127.0.0.1**, so `/hub/*` is internet-reachable unless B2 lands. Publishing an application requires a domain on Cloudflare; TryCloudflare quick tunnels need none but are "intended for testing and development only", cap at **200 in-flight requests** (429 beyond), have no SLA, and do not support Server-Sent Events (https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/do-more-with-tunnels/trycloudflare/, 2026-09-02) |
| **ngrok** | Testing only | Same loopback problem. Free tier: **1 GB/month**, 20,000 HTTP requests/month, 3 endpoints, **no static domain**, an interstitial on HTML traffic, and a **5-minute idle timeout** (https://ngrok.com/docs/pricing-limits/free-plan-limits and https://ngrok.com/docs/universal-gateway/http/, 2026-09-02). 1 GB/month is roughly two hours of one camera. WebSockets work out of the box |
| **Tailscale Funnel** | **Not recommended** | Makes the hub *public* — the thing to avoid. TLS-only, ports 443/8443/10000, "non-configurable bandwidth limits", still beta (https://tailscale.com/kb/1223/funnel, 2026-09-02) |
| **Port-forward + TLS** | **Not recommended** | Directly exposes the hand-written HTTP stack (F3, F8) to internet background noise, requires certificate management on the PC, and provides no defence in depth |
| **Custom relay** | Acceptable, expensive | See above. Its one genuine advantage over Cloudflare Tunnel is that a correctly built option-3 relay cannot read the traffic |
| **WebRTC data channel** | Not recommended here | Still needs a signalling server (i.e. a relay) *and* TURN for symmetric-NAT cases — the same hosting problem with more moving parts, and no advantage over the WebSocket relay for this workload |

#### Ordered rollout

1. **Now, independent of remote access — these are live bugs.** B1 (go2rtc `origin: "*"` →
   `local_auth` + `allow_paths` + non-fixed port, `RemoteHub.cpp:796-803`) and B2's CSRF secret
   (`:1220-1224`). Ship both in the next release regardless of what happens with remote access.
2. **Hardening pass.** B3 (Host validation), B4 (limits and timeouts), B7 (upload quota, GC, spawn
   cap), B8 (CSPRNG + constant-time compare), B9 (proxy allow-list), then the nice-to-have list. All
   small, mechanical and testable.
3. **Authentication.** B5 — password + TOTP from the parallel design, a session cookie replacing the
   URL-path token, throttling, lockout, access log. Nothing goes to the internet before this exists.
4. **Tailscale.** Fix `is_private_v4` (`:123`), document the setup, ship it as *the* supported remote
   path. One hour of work for the best security outcome available, and the only transport that does
   not put the internet on the loopback interface.
5. **Cloudflare Tunnel + Access**, documented as the "I can't install Tailscale on this phone"
   fallback, with B2's separate `/hub/*` acceptor as a hard prerequisite and the
   vendor-sees-plaintext trade-off stated in the docs.
6. **Only then**, and only if "no third-party account" proves to be a real user requirement, build
   the relay — starting with the control API over the mux and leaving video on a separate connection,
   so the free-tier arithmetic above stays survivable.
7. **Before remote print-start ships** (the future route named in the brief): B9 must already be in
   place. An explicit route allow-list has to exist *before* a route that moves a hot nozzle is
   reachable from a phone on the internet.

---

# Part D - Verification sub-pass: DDNS, Let's Encrypt, CGNAT, UPnP, WebRTC, TURN, relays, reference projects (research pass)

## Remote-access transport options for a Windows C++ hub (port 13640) - verification sub-pass

Everything below was checked **2026-09-02**; that date applies to every claim. Items verified first-hand (raw file downloads, live DNS queries, GitHub/CT APIs) are marked **[verified directly]**.

### Executive orientation

Three findings reframe the question before the transport choice matters:

1. **The video requirement is constrained by the phone, not the transport.** iPhone Safari still does not expose `MediaSource` in 2026 - MDN's compat data says verbatim "Exposed in Mobile Safari on iPad but not on iPhone" ([BCD `api/MediaSource.json`](https://raw.githubusercontent.com/mdn/browser-compat-data/main/api/MediaSource.json)) **[verified directly]**. The working path on iPhone is `ManagedMediaSource` (iOS 17.1+), which Chrome and Firefox do **not** implement ([caniuse](https://caniuse.com/mdn-api_managedmediasource)) **[verified directly]**. Feature-detect both, and on Safari set `video.disableRemotePlayback = true` or `sourceopen` never fires ([WebKit 17.1](https://webkit.org/blog/14735/webkit-features-in-safari-17-1/), [MDN](https://developer.mozilla.org/en-US/docs/Web/API/ManagedMediaSource)). (The bundled go2rtc player already handles `ManagedMediaSource`.)
2. **Every comparable project warns against port forwarding**, and every hosted free relay throttles video below a 1-4 Mbps target.
3. **1-4 Mbps sustained is above what any free relay tier will carry.** Plex caps its own relay at 2 Mbps.

### A. Port forwarding + DDNS + Let's Encrypt

#### A1. Free DDNS providers with DNS-01 support

All five provider sites returned HTTP 200 **[verified directly]**.

| Provider | Cost | Update API | DNS-01 | acme.sh / lego | TTL | Free limit |
|---|---|---|---|---|---|---|
| **DuckDNS** | Free, donation-funded | `GET /update?domains=&token=` | Yes, `&txt=` | yes / yes | zone min 600 s | count UNVERIFIED |
| **deSEC** | Free, nonprofit | `update.dedyn.io`, HTTP Basic | Full REST API | yes / yes | zone min 3600 s | **1 domain** by default |
| **afraid.org** | Free / $5 mo | `sync.afraid.org/u/[token]/` | **Screen-scraping only** | partial / no | UNVERIFIED | UNVERIFIED |
| **Dynu** | Free, never expires | REST + OAuth2 | Yes, TXT via API | yes / yes | claims 30 s | UNVERIFIED |
| **No-IP** | Free / $2.99 mo | `dynupdate.no-ip.com/nic/update` | **None** | no / no | zone min 1800 s | **1 hostname** |
| **Cloudflare** | Free (own domain) | REST, Bearer token | Best-in-class | yes / yes | 60 s min, `Auto`=300 s | 1200 req/5 min |

Disqualifiers: **No-IP requires manual monthly confirmation** ("Free hostnames require confirmation every 30 days", [No-IP KB](https://www.noip.com/support/knowledgebase/confirm-my-hostname-free-account-support-question-day)) and has no acme.sh/lego plugin. **afraid.org cannot do automated DNS-01 on a free shared hostname** (acme.sh wiki: "you must own the top level domain"). **DuckDNS has exactly one TXT slot per domain** ([spec.jsp](https://www.duckdns.org/spec.jsp)) **[verified directly]**, so a SAN cert for `x.duckdns.org` + `*.x.duckdns.org` needs lego's `DUCKDNS_SEQUENCE_INTERVAL` serialisation. deSEC publishes precise limits: dynDNS 2 per 2 min per domain; RRset changes 2/s, 15/min, 100/h, 300/day ([docs](https://desec.readthedocs.io/en/latest/rate-limits.html)) **[verified directly]**.

#### A2. Let's Encrypt in 2026

**PSL question answered.** LE uses the Public Suffix List to identify registered domains ([rate limits](https://letsencrypt.org/docs/rate-limits/), updated 2026-08-05) **[verified directly]**. PSL `VERSION: 2026-09-02_06-03-53_UTC` exact-line matches **[verified directly]**: `duckdns.org` (line 13040), `dedyn.io` (12927), No-IP zones (85 entries), Dynu zones (28 entries) and `ui.nabu.casa` are all present -> each hostname gets its own 50-certs/week bucket. **afraid.org's `mooo.com`, `chickenkiller.com`, `crabdance.com`, `strangled.net` are absent** -> all users worldwide share one bucket; likely saturated for popular ones.

Current limits: 50 new certs per registered domain / 7 days; 5 duplicate certs per identifier set / 7 days; 5 authorization failures per identifier per account per hour; 300 new orders per account / 3 hours. **ARI-coordinated renewals are exempt from all rate limits.**

Profiles ([docs/profiles](https://letsencrypt.org/docs/profiles/), 2026-07-14) **[verified directly]**: `classic` 90 d, `tlsserver` 45 d, `shortlived` 160 h, `tlsclient` removed 2026-07-08. OCSP responders shut off 2025-08-06. `classic` -> 64 d on 2027-02-10, -> 45 d on 2028-02-16.

**IP address certificates GA 2026-01-15** ([announcement](https://letsencrypt.org/2026/01/15/6day-and-ip-general-availability)), `shortlived` profile only, and challenges are **HTTP-01 and TLS-ALPN-01 only** ([community thread](https://community.letsencrypt.org/t/getting-ready-to-issue-ip-address-certificates/238777)) **[verified directly]** - so an IP cert still needs inbound 80/443 and does not rescue a CGNAT'd host.

**C++ ACME**: `acme-lw` (MIT, C++ static library, last commit 2026-08-29, Windows CI green) **[verified directly via GitHub API]** - HTTP and DNS challenges, wildcards via DNS-01, in-process, deps cmake + OpenSSL + libcurl (already present). Caveat: no ACME profile selection and no ARI. Alternatives: `uacme` (C, GPL-3.0), `lego` (Go, MIT, v5.4.1 2026-08-31, native DuckDNS/deSEC/Dynu/Cloudflare) as a shell-out, `win-acme` (Apache-2.0, slower cadence, no DuckDNS/deSEC plugin). DNS-01 is the right choice for a NAT'd box (no inbound port, wildcard-capable). Watch `dns-persist-01` ([announced 2026-02-18](https://letsencrypt.org/2026/02/18/dns-persist-01/)) - absent from the challenge-types doc today, treat as not GA.

#### A3. CGNAT in 2026

No current subscriber-level measurement exists; the best data is AS-level from 2016: Richter et al. (IMC 2016) 13.3% of non-cellular ASes use CGN, 17-18% of eyeball ASes, cellular over 92%; Livadariu et al. found three-quarters of CGN ASes have mixed per-subscriber configurations. Always-CGNAT classes: cellular/hotspots; **Starlink Residential/Roam** (100.64/10, no inbound; public IPv4 only on Priority plans - search-surfaced excerpts, starlink.com returned 403); T-Mobile Home Internet and Verizon 5G Home reported as CGNAT but UNVERIFIED officially.

IPv6 adoption (Google data file) **[verified directly]**: global 46.15% on 2026-08-31; France 86.2, Germany 76.6, India 71.9, US 55.8, Japan 55.2, UK 53.8, Canada 43.6, Australia 37.3, China 10.6. An IPv6-only home server is reachable from an iPhone on cellular (464XLAT gives the phone a routable IPv6; no translation when the destination has AAAA), but two gates remain: the home ISP must delegate a prefix, and the home router's inbound IPv6 firewall must be opened (RFC 7084 / RFC 6092 take no position; consumer CPE ships default-deny; no widely-deployed automatic pinhole protocol).

#### A4. UPnP IGD / NAT-PMP / PCP

Fraction of routers with UPnP enabled in 2026: UNVERIFIED (circulating "~92%" figures are vendor marketing). Dated points: Akamai 2018 UPnProxy (277k of 3.5M UPnP devices vulnerable), Richter 2016 (UPnP returned the CPE address in 40% of Netalyzr sessions). Libraries **[verified directly]**: miniupnpc (BSD-3, repo pushed 2026-08-29, last client tag 2.3.3 of 2025-05-26; two Windows fixes only on master: socket timeout handling and a poll()/FD_SET crash fix, 2025-11-29; IGD-only), libnatpmp (BSD-3, frozen since 2013), libpcpnatpmp (BSD-2, the only maintained PCP library, pushed 2026-05-20). **Silent failure mode**: under CGNAT, UPnP succeeds on the inner router and returns a 100.64/10 or RFC1918 "external" address - the app must validate that the returned address is globally routable. CallStranger (CVE-2020-12695) targets eventing, not IGD port mapping.

### B. WebRTC data channels

#### B5. libdatachannel

MPL-2.0, v0.24.5 (2026-06-12), pushed 2026-08-07, 2,725 stars **[verified directly]**. CMake + MSVC first-class; vcpkg port at 0.24.5 (libjuice, nlohmann-json, openssl, plog, usrsctp); OpenSSL is the default backend; `NO_MEDIA=1` trims the footprint. DataChannels, WebSocket client and server, TURN client in-stack (libjuice, RFC 5766/8656); media is RTP-level only. It answers browser offers automatically; interop tested against Firefox/Chromium/Safari. Throughput 208 Mbit/s at 12-20 ms RTT, 17.6 Mbit/s at 200 ms RTT; back-pressure via `rtcGetBufferedAmount`; 16 KiB is the safe cross-browser chunk; TURN throughput can fall to 100s of KB/s with 25% loss off-LAN.

#### B6. Signalling - free tiers

Minimum job: relay SDP offer/answer and ICE candidates, rendezvous, presence/wake-up, authentication. The offer/answer carries the DTLS fingerprint - an attacker who can inject into signalling can MITM the DataChannel.

| Host | Long-lived idle WS? | Key numbers |
|---|---|---|
| **Cloudflare Workers + Durable Object** | **Yes - best fit** | DO on the free plan since 2025-04-07 (SQLite-backed): 100k req/day, 13,000 GB-s/day, 5 GB storage; hibernation: no GB-s during hibernation, automatic pong |
| Workers without DO | No | stateless isolates cannot relay between two clients |
| Deno Deploy | Marginal | 1M req/mo, 20 GiB egress; isolate idle timeout 5 s-10 min |
| Val Town | No | 1 minute wall clock per run |
| Firebase RTDB (Spark) | Yes | 100 simultaneous connections, 1 GB stored, 10 GB/mo download |
| Supabase Realtime | Yes, but | 200 concurrent, 2M msg/mo, project paused after 1 week inactivity |

#### B7. STUN / TURN

Google's `stun.l.google.com:19302` has no policy, ToS, SLA or support document - development-grade. **Cloudflare's `stun.cloudflare.com` is "free and unlimited"** ([TURN FAQ](https://developers.cloudflare.com/realtime/turn/faq/)).

| TURN provider | Free tier | Beyond |
|---|---|---|
| **Cloudflare Realtime** | 1,000 GB free | $0.05/GB, egress only |
| Open Relay (Metered) | 20 GB/mo | - |
| Metered commercial | 500 MB trial | $99/mo = 150 GB; bills ingress+egress |
| Twilio | STUN free | $0.40/GB US/EU, up to $0.80/GB |
| ExpressTURN | 1,000 GB/mo claimed | $9/mo = 5,000 GB |
| coturn | BSD-3, v4.17.2 (2026-08-08) | needs a public IP |

Fraction needing TURN: the most-cited hard figure is 22% (callstats.io 2015-16 via webrtcHacks); the "8-20%" is conventional wisdom without a primary source; for residential-NAT-to-mobile-CGNAT assume TURN is sometimes mandatory. Economics: 4 Mbps = 1.80 GB/h; Cloudflare $0.09/h after ~555 free hours; Twilio $0.72/h; Metered ~$1.44/h; Open Relay's 20 GB/mo = ~5.6 h/month.

#### B8. Browser support

RTCDataChannel: iOS Safari since 11 (current through 26.6), Chrome for Android 151, 97.05% global **[verified directly via caniuse]**. **iOS constraint**: WebRTC and WebSockets are suspended when Safari backgrounds or the screen locks; background timers throttle to about once a minute; no Apple policy document. Assume the connection dies on tab-switch or lock and re-signal on `visibilitychange` / `pageshow`.

### C. Outbound relay on a free tier

#### C9. Cloudflare Workers + Durable Objects

Free plan: 100k req/day, 10 ms CPU per invocation, 128 MB, 50 subrequests, 3 MB script, **max request body 100 MB on Free and Pro**. Wall-clock is not CPU; HTTP requests have no duration limit while the client stays connected **[verified directly]**. **WebSocket messages routed through a Worker do not count as requests** (only the Upgrade), and DOs bill incoming WS messages at 20:1. Frame size decides: 4 Mbps with 64 KB frames = ~34.6k billed req/day (fits free); with 8 KB frames = ~276k (exceeds). **No egress charges** on Workers. ToS: the old §2.8 moved to the Application Services Service-Specific Terms (2026-06-02) under a CDN heading; the Developer Platform terms carry no video/bandwidth prohibition, but whether Workers **Free** is covered by the carve-out is UNVERIFIED; the $5/mo Paid plan removes the ambiguity. Cloudflare Tunnel traffic on a proxied hostname passes through the CDN, so the video clause and 100 MB cap apply there.

#### C10. Fly.io

No free allowance since 2024-10-07 ([discontinued plans](https://fly.io/docs/about/discontinued-plans/)); cheapest always-on machine ~$2.02/month; dedicated IPv4 $2/mo; egress $0.02/GB NA+EU, $0.04 APAC/Oceania/SA, $0.12 Africa/India.

#### C11. Oracle Cloud Always Free - worse than commonly believed

Oracle halved the Ampere allowance to **2 OCPU / 12 GB effective 2026-06-15** with no announcement ([InfoQ 2026-07-03](https://www.infoq.com/news/2026/07/oracle-cloud-free-tier-limits/)); 200 GB block storage; 10 TB/month outbound ([Oracle docs](https://docs.oracle.com/en-us/iaas/Content/FreeTier/freetier_topic-Always_Free_Resources.htm)). Idle reclamation (7-day 95th percentile CPU, network and memory all under 20%) targets exactly a mostly-idle relay; plus "Out of host capacity" difficulty and account-termination reports.

#### C12. Cheapest VPS

| Provider | Price | Included egress | Overage |
|---|---|---|---|
| Hetzner CX23 (2 vCPU/4 GB) | EUR 5.49/mo (raised 2026-06-15; CX22 discontinued) | 20 TB (EU) | ~EUR 1/TB (UNVERIFIED) |
| Hetzner CPX11 US | - | 1 TB only | - |
| OVHcloud VPS-1 (2 vCore/4 GB) | $4.54/mo | unlimited @ 500 Mbps | $0 |
| Contabo VPS 4 | EUR 5.50/mo (24 mo) | "unlimited", 200 Mbit/s | fair-use throttle |
| DigitalOcean | $4.00/mo | 500 GiB pooled | $10.24/TiB |
| Vultr | ~$2.50/mo | 2 TB pooled | $10/TB |
| Fly.io | $2.02/mo | none | $20/TB (NA/EU) |
| ngrok PAYG | $20/mo | 5 GB | $100/TB |

2 Mbps 24/7 = ~648 GB/month; 4 Mbps = ~1.30 TB/month.

#### C13. Relay design references

**Nabu Casa Remote UI - SNI passthrough confirmed officially**: "These servers operate at the TCP level, forwarding all encrypted data"; routing by SNI during the TLS handshake; the local instance decrypts; "This private key is only stored on your Home Assistant system." [SniTun](https://github.com/NabuCasa/snitun) (GPL-3.0): Fernet-token auth, AES challenge-response, 32-byte encrypted multiplexer headers, message types New/Data/Close/Ping/Pause/Resume. Price $6.50/month or $65/year. Tailscale free Personal: up to 6 users, unlimited devices (no published DERP bandwidth limit); TryCloudflare 200 in-flight requests, testing only; ngrok free 1 GB/month (~1.1 hours of 2 Mbps video). Vendor 3D-printer patterns: Bambu MQTT over TLS 8883 (community reverse-engineering, [OpenBambuAPI](https://github.com/Doridian/OpenBambuAPI)); **Prusa Connect uses outbound HTTPS polling, not WebSocket** ([SDK](https://github.com/prusa3d/Prusa-Connect-SDK-Printer)), telemetry at least once per second.

### D. Reference points

#### D14a. OctoPrint ecosystem

**OctoEverywhere**: plugin AGPL-3.0; the relay/backend is not published. Free: 3 printers, AI failure detection, **20-second webcam streams**, limited transfer; Standard $4.99/mo (5 printers, 2-min streams, 500 MB); Elite $9.99/mo (10 printers, unlimited streams, 5 GB). **Obico**: free 10 AI hours/month, 1 printer, 5 FPS throttled, tunnel capped at 300 MB/month; Pro $6.99-8.99/mo; **server AGPL-3.0 and self-hostable** ([obico-server](https://github.com/TheSpaghettiDetective/obico-server), pushed 2026-09-02) **[verified directly]** - outbound client WebSocket to `/ws/dev/` with a bearer token; Obico notes tunnelled data "has to be replicated 4 times". OctoPrint's position: "Putting OctoPrint onto the public internet is a terrible idea" ([blog](https://octoprint.org/blog/2018/09/03/safe-remote-access/)); order: relay plugins -> VPN -> authenticated reverse proxy.

#### D14b. Home Assistant

The ACME client runs on the user's box, generates the key locally, DNS-01 only; Nabu Casa runs the DNS zone and publishes the TXT record. CT logs: 5,000 `ui.nabu.casa` certs, 100% Let's Encrypt, zero wildcards, format `<32-char-label>.ui.nabu.casa`. HA ranks Cloud -> VPN (Tailscale/ZeroTier) -> reverse proxy -> port forwarding. The DuckDNS add-on is first-party and maintained (v2.0.0, integrated Let's Encrypt via DNS-01) **[verified directly]**; Tailscale and Cloudflared add-ons are community.

#### D14c. Klipper stack

Moonraker/Mainsail/Fluidd have no built-in remote access; Mainsail: "Please do not open ports of Mainsail/Moonraker in your router to the rest of the world" **[verified directly]**, endorsing OctoEverywhere and Obico or VPN + DDNS; Moonraker recommends a reverse proxy.

#### D14d. plex.direct - mechanism

Probed Plex's authoritative nameservers directly **[verified directly]**: hostname `<ip-with-dashes>.<32-char-hash>.plex.direct:32400`; DNS decodes the dashed IP into the A record including private and loopback space (`192-168-1-1.`, `10-0-0-5.`, `127-0-0-1.` all resolve); the hash is not validated at the DNS layer (only the 32-char label length is) - it is a TLS-layer scoping device; the pattern is DNS-rebinding-shaped, so hardened resolvers need `rebind-domain-ok=/plex.direct/`; the private key is delivered to the user's server ("only your Plex Media Server has the certificate"); originally DigiCert via a Plex-operated intermediate that expired 2025-01-14, now "Let's Encrypt" per Plex's article - the 2026 shape is UNVERIFIED.

CA/B BR v2.2.9 (2026-08-06) **[verified directly]**: §1.6.1 key compromise = disclosure to an unauthorized person; §4.9.1.1(3) revoke within 24 hours; §6.1.2 revoke if a key was communicated to "an organization not affiliated with the Subscriber"; §6.1.1.3 the CA shall not generate the subscriber's key pair. Whether Plex's users count as affiliated is unresolved; precedent: Trustico/DigiCert 2018, 23,000 certs revoked in 24 hours. **Let's Encrypt forecloses the key-sharing half** (SA v1.8, 2026-07-06 **[verified directly]**: §3.4 keys generated by you on your systems; §3.1 keep private keys secret and confidential; §3.7 immediate revocation on compromise). **The reusable half is the DNS trick**: dashed-IP wildcard DNS on a project-owned domain (same hostname on LAN and WAN) plus per-device ACME with locally generated keys and DNS-01 answered by the project - Nabu Casa's shipped design. The 50-certs/week cap throttles new onboarding only (~2,600 devices/year per domain); for one home user it never binds.

**Plex Relay**: 2 Mbps cap for all subscribers; since 2025-04-29 remote playback requires a subscription (Plex Pass $6.99/mo, $69.99/yr, $749.99 lifetime; Remote Watch Pass $2.99/mo). **Jellyfin**: no hosted relay; "you must manually expose it to the internet" - port forwarding "not recommended", reverse proxy, VPN, or VPS reverse proxy.

### Consolidated UNVERIFIED list

DuckDNS per-account domain limit, rate limits and user-record TTL; afraid.org free subdomain count/TTL/rate limits; Dynu free hostname count; No-IP update rate limits; deSEC production default `minimum_ttl`; `dns-persist-01` production availability; residential port-80 blocking prevalence; T-Mobile Home Internet and Verizon 5G Home CGNAT policy from official pages (403); Starlink Priority pricing; any 2024-2026 subscriber-level CGNAT measurement; fraction of routers with UPnP enabled in 2026; current CISA/NCSC UPnP guidance; whether Workers Free qualifies for the CDN video carve-out; Hetzner per-TB overage; Oracle egress above 10 TB; Cloudflare Zero Trust free seat count; RackNerd/Vultr specs; Xirsys pricing; Cloudflare's WebSocket idle-timeout value; the 2026 plex.direct certificate shape; any CA/B or Mozilla ruling on the plex.direct pattern. The sub-pass's web-search budget was fully consumed.
