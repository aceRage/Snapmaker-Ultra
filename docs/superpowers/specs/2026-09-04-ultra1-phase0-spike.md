# Ultra1 phase 0: the push-plane technical spike (2026-09-04)

**What this is.** Phase 0 of [`2026-09-04-ultra1-app-plan.md`](2026-09-04-ultra1-app-plan.md) §5.2 - the
spike that decides whether the hub, *as it is built today*, can talk to APNs and FCM at all, plus the
concrete hub-side design for `AppPush`. It answers the five questions the plan left open, kills or
confirms risk **R2**, and revises the phase 1 estimate.

Branch `docs/ultra1-phase0-spike`, cut from `feat/ultra-preferences` at `ef81b7c194`. **Documentation
only - no hub code and no deps are changed on this branch.** Every probe was compiled and run against
the *real* built dependencies at `C:\Dev\SnapmakerOrca\deps\build\OrcaSlicer_dep\usr\local`; the probe
sources live in `C:\Users\acesa\AppData\Local\Temp\snorca_hubtest\` and are deliberately not committed.

Platform facts carry a URL and the date read. Facts established by running something carry the command
and its output. Anything that could not be established either way is marked **UNVERIFIED** and must not
be planned against.

---

## 0. The answer

> ### GO for FCM today. NO-GO for APNs today. GO for APNs after a ~2-hour deps change.

The blocker is exactly the one the plan predicted and nothing else. **The fork's bundled libcurl has no
HTTP/2**, because `deps/CURL/CURL.cmake` passes `-DUSE_NGHTTP2:BOOL=OFF`, and APNs speaks nothing but
HTTP/2. Everything else the push plane needs - ES256 over a `.p8`, RS256 for the FCM service account,
the RFC 8291 encryption, the retry/prune/mask machinery - **is already in the tree and was proved
working in this spike**.

| # | Question | Verdict | Evidence |
|---|---|---|---|
| 1 | Does the bundled libcurl do HTTP/2? | **No.** `HTTP2 no`, `nghttp2 version (NULL)`, and `CURLOPT_HTTP_VERSION` itself returns `CURLE_UNSUPPORTED_PROTOCOL` | §1 |
| 1b | Does APNs really require it? | **Yes**, and empirically so: APNs negotiates ALPN `h2` and offers *nothing* when only `http/1.1` is proposed | §1.2 |
| 1c | What does fixing it cost? | **~2 h of work, ~15 min of machine time.** One new dep (`nghttp2`, lib-only), one flag flip, one `target_link_libraries` line. No other dependency moves | §1.4 |
| 2 | Can the existing code sign the APNs JWT with the `.p8`? | **Yes**, with **one new line**: `PEM_read_bio_PrivateKey` instead of the raw-scalar loader. The rest of `WebPush.cpp`'s ES256 path is reused verbatim | §2 |
| 3 | Can the fork do the FCM OAuth2 flow? | **Yes, entirely with what is built today.** RS256 signing works; the FCM endpoint answers over **HTTP/1.1** | §3 |
| 4 | Can the hub reuse `aes128gcm` for the app payload? | **Yes**, unchanged - but the plan's proposed `aps` dictionary is **wrong** and would be rejected by the Notification Service Extension contract | §4 |
| 5 | Is the `AppPush` design sound? | Yes, with a provider seam, and with three corrections to §4.2 of the plan | §5 |

**Revised phase 1: 6-9 ideal days** (was 5-7), the increase being the deps change, the `Http` wrapper
change, and an h2 mock APNs server for the gate. **Revised phase 0: 1 day, of which this spike is
most.** §6 has the breakdown.

---

## 1. HTTP/2 (spike question 1)

### 1.1 The bundled libcurl does not have it - four independent proofs

**(a) The deps recipe says so.** `deps/CURL/CURL.cmake:23`:

```cmake
  -DUSE_NGHTTP2:BOOL=OFF
```

pinned to `curl-7_75_0` (`CURL.cmake:62-63`), with `-DCMAKE_USE_OPENSSL=ON` on every platform.

**(b) The generated config says so.** `deps/build/dep_CURL-prefix/src/dep_CURL-build/lib/curl_config.h:1009`:

```c
/* to enable NGHTTP2  */
/* #undef USE_NGHTTP2 */
```

**(c) The generated `curl-config` says so.** `deps/build/OrcaSlicer_dep/usr/local/bin/curl-config`, the
feature list it was configured to report:

```sh
    --feature|--features)
        for feature in SSL IPv6 libz AsynchDNS alt-svc NTLM HTTPS-proxy ""; do
```

No `HTTP2`. That list is not hand-written: curl's own `CMakeLists.txt:1384` builds it as
`_add_if("HTTP2" USE_NGHTTP2)`.

**(d) The built library says so, at runtime.** `probe_curl_http2.cpp` (compiled with the VS 2022
toolchain against `deps/.../lib/libcurl.lib`, the very archive the slicer links):

```
libcurl version : 7.75.0-DEV
version_num     : 0x074b00
ssl_version     : OpenSSL/1.1.1w
nghttp2 version : (NULL)
features bits   : 0x0120029d

feature flags:
  SSL          YES
  libz         YES
  IPv6         YES
  AsynchDNS    YES
  HTTP2        no
  HTTP3        no
  alt-svc      YES
  MultiSSL     no
  brotli       no
  HTTPS-proxy  YES
  NTLM         YES

protocols: file ftp ftps http https

setopt HTTP_VERSION_2_0   -> 1 (Unsupported protocol)
setopt HTTP_VERSION_2TLS  -> 1 (Unsupported protocol)
  (CURLE_UNSUPPORTED_PROTOCOL=1, CURLE_OK=0)

VERDICT HTTP/2: MISSING
```

The last two lines matter more than the feature bit: `curl_easy_setopt(CURLOPT_HTTP_VERSION,
CURL_HTTP_VERSION_2_0)` does not merely fall back to 1.1 here, it **refuses the option**. There is no
"try it and see" path.

Also confirmed: nothing else in the tree speaks HTTP/2. `grep -rn "winhttp\|WinHttp" src deps` finds
nothing, `grep -rn "nghttp2" deps` finds only a commented-out line in `deps-macos.cmake:85`, and
`grep -rln "HTTP_VERSION_2\|CURL_VERSION_HTTP2" src` finds nothing at all. And
`src/slic3r/Utils/Http.hpp` exposes no way to set the HTTP version even if curl could - see §1.5.

### 1.2 APNs requires HTTP/2 - documented and demonstrated

Apple, *Sending notification requests to APNs*
([developer.apple.com](https://developer.apple.com/documentation/usernotifications/sending-notification-requests-to-apns),
read 2026-09-04):

> Use HTTP/2 and TLS 1.2 or later to establish a connection between your provider server and APNs.

with `POST /3/device/<device_token>` to `api.push.apple.com` (production) or
`api.sandbox.push.apple.com` (development), port 443 or 2197.

Demonstrated rather than assumed, from this PC on 2026-09-04:

```
$ openssl s_client -connect api.sandbox.push.apple.com:443 -alpn h2,http/1.1 -servername api.sandbox.push.apple.com
ALPN protocol: h2

$ openssl s_client -connect api.sandbox.push.apple.com:443 -alpn http/1.1 -servername api.sandbox.push.apple.com
No ALPN negotiated

$ openssl s_client -connect api.push.apple.com:443 -alpn h2,http/1.1 -servername api.push.apple.com
ALPN protocol: h2

$ curl --http1.1 -X POST -H "apns-topic: example" -d '{"aps":{}}' https://api.sandbox.push.apple.com/3/device/deadbeef
curl: (1) Received HTTP/0.9 when not allowed
```

Offered both, APNs picks `h2`. Offered only `http/1.1`, it negotiates **nothing**. An HTTP/1.1 request
gets HTTP/2 frames back, which the client can only read as garbage. This is not a
"prefers HTTP/2" server; it is HTTP/2-only. (Those `openssl` and `curl` calls used the deps' own
`openssl.exe` and Windows' `curl.exe` respectively; no credential was sent and the device token is the
literal string `deadbeef`.)

Note in passing: Windows' own bundled `curl 8.21.0 (Schannel)` **also** has no HTTP/2
(`curl: option --http2: the installed libcurl version does not support this`), so "shell out to
system curl" is not a shortcut either.

### 1.3 FCM does *not* require it

```
$ curl --http1.1 -X POST -d '{}' https://fcm.googleapis.com/v1/projects/x/messages:send
http=1.1 code=401
```

The FCM v1 endpoint answers over HTTP/1.1 with a `401` (no credential presented), which is the correct
answer to that request. **FCM works with the fork's outbound stack exactly as it stands today.** That
is what makes this a partial GO rather than a stop.

### 1.4 The deps change, exactly

curl 7.75.0's `CMakeLists.txt:451-456` makes HTTP/2 synonymous with nghttp2 - there is no other
provider:

```cmake
option(USE_NGHTTP2 "Use Nghttp2 library" OFF)
if(USE_NGHTTP2)
  find_package(NGHTTP2 REQUIRED)
  include_directories(${NGHTTP2_INCLUDE_DIRS})
  list(APPEND CURL_LIBS ${NGHTTP2_LIBRARIES})
endif()
```

`lib/http2.c:47` additionally refuses anything older than nghttp2 1.12
(`#if (NGHTTP2_VERSION_NUM < 0x010c00) #error too old nghttp2 version, upgrade!`). ALPN, the other
prerequisite, is already satisfied: `lib/vtls/openssl.c:2177` enables it for OpenSSL >= 1.0.2 and the
deps ship **OpenSSL 1.1.1w (11 Sep 2023)**.

Four edits, in order:

1. **New `deps/NGHTTP2/NGHTTP2.cmake`**, in the shape of every other recipe:

   ```cmake
   Snapmaker_Orca_add_cmake_project(NGHTTP2
     URL       https://github.com/nghttp2/nghttp2/archive/refs/tags/v1.64.0.zip
     URL_HASH  SHA256=<pin it>
     CMAKE_ARGS
       -DENABLE_LIB_ONLY:BOOL=ON        # just libnghttp2; no apps, no C++, no deps
       -DENABLE_SHARED_LIB:BOOL=OFF
       -DENABLE_STATIC_LIB:BOOL=ON
       -DENABLE_DOC:BOOL=OFF
       -DBUILD_TESTING:BOOL=OFF
       -DCMAKE_POSITION_INDEPENDENT_CODE=ON
   )
   ```

   `ENABLE_LIB_ONLY` is the important one: nghttp2's *applications* are C++ (and, from v1.70,
   C++23), its library is plain C with no dependencies. Pin a version rather than tracking `master`;
   1.64.0 is a conservative choice well above curl's 1.12 floor. The current release is **v1.70.0
   (published 2026-07-29)**, which is also fine but drags a newer CMake floor in for no gain.

2. **`deps/CMakeLists.txt`**: `include(NGHTTP2/NGHTTP2.cmake)` immediately before the `CURL` block at
   line 321-324, and add `dep_NGHTTP2` to `dep_CURL`'s `DEPENDS`.

3. **`deps/CURL/CURL.cmake`**: `-DUSE_NGHTTP2:BOOL=OFF` -> `ON`, and **two things that will otherwise
   burn a day each**:
   - curl's bundled `CMake/FindNGHTTP2.cmake` is `find_library(NGHTTP2_LIBRARY NAMES nghttp2)` - it
     does not know about MSVC's static-lib naming, where nghttp2's static target lands as
     `nghttp2_static.lib`. Pass `-DNGHTTP2_LIBRARY=<prefix>/lib/nghttp2_static.lib` and
     `-DNGHTTP2_INCLUDE_DIR=<prefix>/include` explicitly rather than hoping.
   - `nghttp2.h` declares every symbol `__declspec(dllimport)` **unless `NGHTTP2_STATICLIB` is
     defined** ([nghttp2.h, read 2026-09-04](https://github.com/nghttp2/nghttp2/blob/master/lib/includes/nghttp2/nghttp2.h)),
     so curl must be compiled with `-DCMAKE_C_FLAGS=-DNGHTTP2_STATICLIB` or `http2.c` links against
     import stubs that do not exist.

4. **`CMakeLists.txt:572-573`**, the top-level curl interface target, gains the transitive library
   (a static libcurl carries no link interface of its own):

   ```cmake
   add_library(libcurl INTERFACE)
   target_link_libraries(libcurl INTERFACE CURL::libcurl)
   target_link_libraries(libcurl INTERFACE ${NGHTTP2_LIBRARY})   # new
   ```

**Cost.** From the existing stamp files, `dep_CURL` configure -> install took **1 min 42 s**
(`dep_CURL-configure` 13:57:04 -> `dep_CURL-install` 13:58:46, 2026-08-27). nghttp2 lib-only is a
smaller build than that. Add one slicer relink (3-8 min per the working brief). So: **~15 minutes of
machine time, and perhaps two hours of human time**, most of it spent on the two static-linking traps
above. Nothing else in the dependency graph moves - `dep_CURL` already depends only on ZLIB and
OpenSSL, and no other dep depends on curl.

**The blast radius, stated honestly.** `_curl_platform_flags` in `CURL.cmake` is shared by Windows,
macOS and Linux, so flipping the flag there commits all three to building nghttp2. The recipe is
portable and this is the right call (a hub on a Mac should be able to push too), but it means the next
person to run a clean deps build on any platform inherits a new dependency. The alternative -
`if (WIN32)`-scoping the flag - buys a smaller change and an APNs feature that silently does not exist
on macOS and Linux hubs. **Recommendation: enable it on all three**, and note it in the deps changelog.

### 1.5 One more small change the plan did not account for

`src/slic3r/Utils/Http.hpp` has `timeout_connect`, `timeout_max`, `header`, `set_post_body`,
`ssl_revoke_best_effort` and so on - but **nothing that sets `CURLOPT_HTTP_VERSION`**. Even with
nghttp2 present, libcurl's default for an HTTPS request in 7.75.0 is to try ALPN and take what it
gets, which happens to work for APNs; but a sender that *requires* h2 should say so and fail loudly
rather than silently, and the gate needs `CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE` (see §5.7). So phase 1
adds one method:

```cpp
// Ask for a specific HTTP version. APNs speaks HTTP/2 and nothing else, and the gate's mock
// speaks cleartext h2, so this is not a preference - it is a requirement the caller states.
Http& http_version(long curl_http_version);
```

plus, in `AppPush`, a startup check of `curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_HTTP2`
so a hub built without nghttp2 reports *"APNs needs an HTTP/2-capable build of libcurl"* on the hub
page instead of failing every push with a transport error.

### 1.6 The alternatives, evaluated

| Option | Verdict |
|---|---|
| **nghttp2 in `deps/`, `USE_NGHTTP2=ON`** (§1.4) | **Recommended.** ~2 h, ~15 min of build, one new dep, and it keeps every APNs request inside `Http.cpp`'s existing timeout/error/scrub plumbing |
| **WinHTTP, Windows-only** | Viable contingency, not the first choice - see below |
| **nghttp2's own API directly, bypassing curl** | Needs the same new dependency *and* a hand-written TLS/BIO/event loop on top. Strictly more work than option 1 |
| **A hand-written HTTP/2 client** | Rejected. "One stream, POST, fixed header set" understates it: HPACK alone is a 61-entry static table plus a Huffman code, and every byte of it is attacker-adjacent. 5-8 days and a permanent security liability, to avoid a 2-hour dependency change |
| **A relay the project hosts** | Out of scope. R1 recommendation (c) - the key is the user's own and no service is run |

**WinHTTP as a Windows-only fallback.** It genuinely supports HTTP/2:
`WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL` takes *"a DWORD bitmask of acceptable advanced HTTP versions"*
where `WINHTTP_PROTOCOL_FLAG_HTTP2 (0x1)` *"Enables HTTP/2 for the request"*, available from
**Windows 10 version 1607** and settable on both the session and the request handle
([Microsoft, WinHTTP Option flags](https://learn.microsoft.com/windows/win32/winhttp/option-flags),
read 2026-09-04). Crucially, *"The default is 0x0"* - HTTP/2 is opt-in for an ordinary desktop
process; the note that *"for app containers and system services since Windows 10, version 1709,
HTTP/2 is on by default"*
([About WinHTTP](https://learn.microsoft.com/windows/win32/winhttp/about-winhttp), read 2026-09-04)
does not cover the hub, which is neither. `WINHTTP_OPTION_HTTP_PROTOCOL_USED` can be queried on the
request handle afterwards, so a gate can assert HTTP/2 was actually negotiated.

Its constraints, which are why it is second:

- **Windows-only.** macOS and Linux hubs would still need option 1, so choosing WinHTTP means writing
  the sender twice or shipping APNs on one platform.
- **A different TLS stack.** WinHTTP is Schannel and the Windows trust store; the rest of the fork's
  outbound traffic is OpenSSL through `Http.cpp`. Two trust configurations to reason about.
- **None of `Http.cpp`'s plumbing.** Timeouts, the error text that `scrub()` cleans, the proxy
  handling, the size limits - all re-implemented, ~300 lines of `HINTERNET` code.
- **No cleartext HTTP/2.** WinHTTP negotiates h2 over TLS via ALPN only. libcurl+nghttp2 additionally
  offers `CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE`, which lets the gate's mock APNs be a plain **h2c**
  server on loopback - no certificate, no trust-store surgery, no `--insecure`-equivalent in shipped
  code. This is a real and underrated advantage: it is the difference between a gate that runs
  anywhere and a gate that needs a locally-trusted certificate installed on the machine.
- Documented caution: WinHTTP *"is not reentrant except during asynchronous completion callback"*
  (same page). Harmless here - `AppPush` is one worker thread, like `RemoteNotify` - but it forecloses
  ever calling it from two threads.

**Contingency rule:** if the deps change misbehaves on macOS or Linux, ship option 1 on the platforms
where it works and keep WinHTTP in the drawer; do not start with WinHTTP.

---

## 2. APNs token authentication (spike question 2)

### 2.1 The facts, from Apple

All from *Establishing a token-based connection to APNs*
([developer.apple.com](https://developer.apple.com/documentation/usernotifications/establishing-a-token-based-connection-to-apns),
read 2026-09-04):

| Element | Apple's words |
|---|---|
| `alg` | *"APNs supports only the ES256 algorithm, so set the value of this key to `ES256`."* |
| `kid` | *"The 10-character Key ID you obtained from your developer account"* |
| `iss` | *"The issuer key, the value for which is the 10-character Team ID you use for developing your company's apps."* |
| `iat` | *"The 'issued at' time... the number of seconds since Epoch, in UTC. The value must be no more than one hour from the current time."* |
| Expiry behaviour | *"If the value in the `iat` field is more than one hour old, APNs rejects any notifications containing the token, returning an `ExpiredProviderToken` (403) error."* |
| Refresh cadence | *"Refresh your token no more than once every 20 minutes and no less than once every 60 minutes... APNs report an error if you use a new token more than once every 20 minutes on the same connection."* |
| Header | `authorization = bearer <jwt>` |
| The key | *"An authentication token signing key, specified as a text file (with a `.p8` file extension)."* |
| Key limits | Team-scoped: *"Each environment can have a maximum of two keys."* Topic-specific: *"a maximum of 200 keys for Sandbox and 200 for Production, with up to 400 topics per topic-based key."* |

**There is no `exp` claim** - unlike RFC 8292 VAPID, which has one. The lifetime is implied by `iat`.
This is a real difference from `WebPush::vapid_jwt` and a trivially easy thing to get wrong by copying.

Request side, from *Sending notification requests to APNs* (same read date):

| Header | Rule |
|---|---|
| `:method` / `:path` | `POST` `/3/device/<device_token>`, hex device token |
| `authorization` | required for token auth, `bearer <provider_token>` |
| `apns-topic` | **required** - the bundle ID (with a suffix for some push types) |
| `apns-push-type` | required on watchOS 6+, recommended everywhere; *"Must accurately reflect notification payload contents"* |
| `apns-id` | optional UUID; APNs mints one and returns it if omitted |
| `apns-expiration` | UNIX epoch seconds. `0` = *"deliver once only, don't store"*; nonzero = *"store up to 30 days"* |
| `apns-priority` | `10` = immediately (**the default when omitted**), `5` = power-considerate, `1` = do not wake the device |
| `apns-collapse-id` | *"Max 64 bytes"*; same value merges notifications |
| Payload cap | **4 KB (4096 bytes)** standard, 5 KB VoIP; *"JSON payload must not be compressed"* |

Our encrypted event is ~250 base64url characters, so the cap is a non-issue - as the plan said.

Hosts: `api.push.apple.com` (production) / `api.sandbox.push.apple.com` (development), port 443 or
2197 (*"Port 2197 can be used as an alternative to allow APNs traffic through your firewall while
blocking other HTTPS traffic"*).

### 2.2 Can `WebPush.cpp`'s ES256 code sign it? Yes - after one line

The `.p8` Apple hands out is an **unencrypted PKCS#8 PEM** (`-----BEGIN PRIVATE KEY-----`) holding a
`prime256v1` key. `WebPush.cpp`'s `ec_from_private` (`:303`) takes a **raw 32-byte scalar** - the form
the VAPID key is stored in - and would reject a `.p8` outright. That is the entire delta.

`probe_apns_jwt.cpp` proves it: generate a stand-in `.p8` with the deps' own `openssl.exe`
(`ecparam -name prime256v1 -genkey | pkcs8 -topk8 -nocrypt`), parse it with `PEM_read_bio_PrivateKey`,
then sign an APNs-shaped JWT using **`WebPush.cpp`'s `der_to_raw` copied verbatim** and the same
`EVP_DigestSign` sequence as `sign_es256` (`:493`):

```
--- head of the stand-in .p8 ---
-----BEGIN PRIVATE KEY-----
OpenSSL: OpenSSL 1.1.1w  11 Sep 2023
key type id: 408 (EVP_PKEY_EC=408)
is EC      : YES
curve      : prime256v1 (nid 415)
is P-256   : YES (same curve as the VAPID key)
sig (raw)  : 64 bytes (JOSE wants 64)
jwt_len    : 184
VERDICT: .p8 parsed and APNs JWT signed with WebPush.cpp's own ES256 path
```

And verified **independently**, by `verify_apns_jwt.py` using Python's `cryptography` 46.0.3 (a
different implementation entirely, in the shape `mock_push.py` already uses for Web Push):

```
PASS  header alg is ES256  (ES256)
PASS  header has kid  (ABC123DEFG)
PASS  header has no other keys  (['alg', 'kid'])
PASS  claim iss is the team id  (DEF123GHIJ)
PASS  claim iat is an int  (1788568749)
PASS  no 'exp' claim (Apple does not use one)  (['iat', 'iss'])
PASS  signature is 64 raw bytes (JOSE, not DER)  (64)
PASS  base64url has no padding  (Sosg)
PASS  key is P-256  (secp256r1)
PASS  ES256 signature verifies  (ok)

10/10 checks passed
```

**So: the curve is the same, the signing path is the same, `der_to_raw` is the same, the base64url is
the same. The only new code is the loader.** Concretely, one function beside `ec_from_private`:

```cpp
// Apple's .p8 is an unencrypted PKCS#8 PEM, not the raw scalar the VAPID key is stored as, so it
// needs OpenSSL's PEM reader rather than ec_from_private(). Everything after this is shared.
static bool ec_from_p8_pem(const std::string& pem, EVP_PKEY** out, std::string& err);
```

Two notes for phase 1:

- **Store the PEM text, not just the path.** The plan (§4.2 N4) has the `.p8` *"referenced by path,
  not uploaded"*, which is the right default - but read it once at `start()` and hold the parsed
  `EVP_PKEY`, so a push at 03:00 does not fail because the file moved. Re-read on a config change.
  Keep the path as the source of truth and treat the in-memory key as a cache.
- **The JWT cache is nearly right already.** `jwt_for` (`:547`) reuses a token until it is within
  `JWT_REFRESH` of expiring, which is exactly Apple's "no more than once per 20 minutes, no less than
  once per hour". Key the APNs cache on `(host, kid)` rather than on the origin, and refresh at 45
  minutes: comfortably inside the hour, comfortably outside the 20.

---

## 3. FCM HTTP v1 (spike question 3)

### 3.1 The flow

Two requests, the first cached:

1. **Mint an assertion and exchange it for an access token.** JWT header `{"alg":"RS256","typ":"JWT"}`;
   claims `iss` (*"The email address of the service account"*), `scope`, `aud`, `exp`, `iat`; *"The
   maximum token lifetime is 1 hour after the issued time"*; `aud` for a token request is
   `https://oauth2.googleapis.com/token`. `POST` that endpoint as
   `application/x-www-form-urlencoded` with
   `grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=<jwt>`; the response carries
   `access_token`, `scope`, `token_type` and `expires_in`
   ([Google, Using OAuth 2.0 for server-to-server applications](https://developers.google.com/identity/protocols/oauth2/service-account),
   read 2026-09-04).
2. **Send.** `POST https://fcm.googleapis.com/v1/projects/{projectId}/messages:send` with
   `Authorization: Bearer <access_token>`; scope
   `https://www.googleapis.com/auth/firebase.messaging`
   ([Firebase, Authorize send requests](https://firebase.google.com/docs/cloud-messaging/auth-server),
   read 2026-09-04). The private key comes from the service-account JSON's `private_key` field, which
   is an unencrypted PKCS#8 PEM RSA key.

### 3.2 The message shape for our encrypted blob

```json
{ "message": {
    "token": "<FCM registration token>",
    "data":  { "e": "<base64url aes128gcm blob>", "v": "1" },
    "android": { "priority": "high", "ttl": "1800s" }
} }
```

**Data-only, deliberately.** *"Notification messages"* are *"handled by the FCM SDK automatically"* and
would put our (nonexistent) cleartext in the tray; *"data messages"* are *"handled by the client app"*
with *"only your user-defined custom key-value pairs"*
([Firebase, Message types](https://firebase.google.com/docs/cloud-messaging/customize-messages/set-message-type),
read 2026-09-04). Only the data form lets `FirebaseMessagingService.onMessageReceived` decrypt before
anything is displayed, which is M6. Note there must be **no** `notification` block at all: a message
with both is delivered to the tray in the background and `onMessageReceived` is not called.

Constraints, all confirmed:

- **4096 bytes** maximum payload for both message types
  ([Message types](https://firebase.google.com/docs/cloud-messaging/customize-messages/set-message-type),
  read 2026-09-04). Our data map is ~300 bytes.
- **TTL** 0 to 2,419,200 seconds (28 days), default four weeks
  ([Setting message lifespan](https://firebase.google.com/docs/cloud-messaging/customize-messages/setting-message-lifespan),
  read 2026-09-04). Use 1800 s for alerts and 300 s for `started`: a print-finished notice delivered
  two days later is noise, not news.
- **Priority.** *"FCM attempts to deliver high priority messages immediately, allowing FCM to wake a
  sleeping device"*, whereas normal priority *"delivery may be delayed... until the device exits
  doze"* ([Android message priority](https://firebase.google.com/docs/cloud-messaging/android-message-priority),
  read 2026-09-04). Use `high` for `error`/`warning`/`runout`/`paused`/`finished`, `normal` for
  `started` and progress - mirroring `WebPush.cpp`'s `Urgency` rule at `:608`.
- **A caveat worth writing down:** the same page notes messages *"may be deprioritized if FCM detects
  patterns of non-user-visible notifications."* Our design always posts a visible notification from
  `onMessageReceived`, so we are on the right side of that - but a future "silent progress update"
  feature would not be, and would degrade the alerts that matter. Do not add one.
- `onMessageReceived` *"has a short execution window"*; decryption is microseconds, so this is fine,
  but the Android side must not fetch a thumbnail there
  ([Receive messages in an Android app](https://firebase.google.com/docs/cloud-messaging/android/receive),
  read 2026-09-04).

### 3.3 Can the fork do this today? Yes, entirely

- **HTTP.** Plain HTTPS/1.1, demonstrated in §1.3. `Http::post` handles it unchanged.
- **RS256.** `probe_fcm_rs256.cpp`, compiled against the deps' OpenSSL and run:

  ```
  key type id: 6 (EVP_PKEY_RSA=6)  bits=2048
  sig bytes  : 256
  VERDICT: FCM service-account RS256 assertion signed with the fork's OpenSSL
  ```

  Same `PEM_read_bio_PrivateKey` + `EVP_DigestSignInit(EVP_sha256())` as the APNs path, and *simpler*:
  RS256 signatures are already raw, so there is no `der_to_raw` step. `deps/build/.../curl_config.h:501`
  confirms `HAVE_OPENSSL_RSA_H`.
- **JSON.** `nlohmann::json` is already the hub's parser, so reading the service-account JSON's
  `client_email`, `private_key`, `token_uri` and `project_id` is a few lines.

**So FCM needs no dependency change of any kind.** If the deps change in §1.4 stalls, Android push can
ship on its own while iOS waits - a genuinely useful decoupling that the phase plan should exploit.

---

## 4. Encrypted payloads to the app (spike question 4)

### 4.1 The plan's idea holds - the crypto is reused unchanged

`WebPush::encrypt(p256dh_b64u, auth_b64u, plaintext, "", "", body, err)` is already exported in
`WebPush.hpp:96` and takes exactly the two fields a browser `PushSubscription` carries. The app
generating a P-256 key pair and a 16-byte auth secret on first launch, keeping the private half in the
Keychain / Android Keystore and registering the public half, makes it **byte-for-byte the same call**
the Web Push sender makes today: RFC 8291 §3.4 key schedule, `aes128gcm`, one record, `0x02`
delimiter, fresh ephemeral key and 16-byte salt per message (`WebPush.cpp:416-472`).

Both app platforms can hold the other half: a P-256 key in the iOS Keychain via `SecKeyCreateRandomKey`
with `kSecAttrKeyTypeECSECPrimeRandom`, and in the Android Keystore via `KeyPairGenerator` with
`KeyProperties.KEY_ALGORITHM_EC` + `secp256r1`. Both platforms have HKDF and AES-GCM in their standard
crypto libraries (CryptoKit / `javax.crypto`). No third-party crypto dependency on either side, which
matters for M10.

### 4.2 The envelope

The RFC 8188 body `WebPush::encrypt` produces is binary (`salt|rs|idlen|keyid|ciphertext|tag`, ~110
bytes overhead + plaintext). APNs and FCM payloads are JSON, so it is base64url'd - the same encoding
the rest of the hub already uses. ~200 bytes of plaintext becomes ~415 base64url characters. Well
inside 4096 on both.

**APNs:**

```json
{ "aps": { "alert": { "title": "Printer update", "body": "Tap to open" },
           "mutable-content": 1,
           "sound": "default",
           "thread-id": "<printer id>" },
  "v": 1,
  "e": "<base64url aes128gcm blob>" }
```

**This corrects the plan.** §4.2 N3 proposes `"alert":{"title-loc-key":"e"}`. Apple's contract for the
service extension is explicit:

> The payload must include the `mutable-content` key with a value of `1`.
>
> The payload must include an `alert` dictionary with title, subtitle, or body information.

([Apple, `UNNotificationServiceExtension`](https://developer.apple.com/documentation/usernotifications/modifying-content-in-newly-delivered-notifications),
read 2026-09-04). A `title-loc-key` naming a key that does not exist in any `.strings` file is not
title information, and the failure mode is silent. Use **literal placeholder text** instead - which
also settles R9's open question about the fallback: the extension has *"only about 30 seconds"*, and
*"if you fail to call the completion handler... the system displays the original contents of the
notification."* So the cleartext the user sees when the extension crashes is precisely the placeholder
above. `"Printer update" / "Tap to open"` leaks nothing and reads as intentional.

**FCM:** as §3.2 - the same `e` and `v` fields in `data`, no `notification` block.

### 4.3 The iOS flow

1. APNs delivers the payload. Because `mutable-content` is `1`, iOS launches the app's
   **Notification Service Extension** before displaying anything.
2. `didReceive(_:withContentHandler:)` reads `request.content.userInfo["e"]`, base64url-decodes it,
   loads the device's P-256 private key and auth secret from the **shared** Keychain access group
   (the extension is a separate process from the app - this is the single most common way this
   design is got wrong), runs the RFC 8291 decryption, and rewrites `bestAttemptContent.title`,
   `.body`, `.threadIdentifier` and `.userInfo` from the decrypted JSON.
3. It calls the completion handler. On failure it calls it with the unmodified content, which shows
   the placeholder.
4. `serviceExtensionTimeWillExpire()` returns whatever it has. Decryption is sub-millisecond; the
   30-second budget is irrelevant unless someone adds a network call, which nothing should.

### 4.4 The Android flow

1. FCM delivers a data-only, `priority: high` message; `FirebaseMessagingService.onMessageReceived`
   runs whether the app is foreground or background.
2. It decrypts `data["e"]` with the Keystore key and posts the notification itself via
   `NotificationManager`, using `<printer id>:<kind>` as the notification tag so a second *paused*
   replaces the first - the same coalescing rule `WebPush.cpp:577`'s `topic_for` implements.
3. It must post something visible every time (§3.2's deprioritisation note) and must not do work in
   the callback beyond decrypting and posting.

**Known limit, and it belongs in M4's wording:** a **force-stopped** Android app (Settings ->
Force stop, or some OEM battery managers) receives no FCM at all until it is next launched. "App
killed" in M4 must mean "swiped out of recents", which is the normal case and does work. Google does
not state the force-stop behaviour on the pages read here, so treat the precise boundary as
**UNVERIFIED** and make it a hardware-checklist line rather than a gate assertion.

---

## 5. The `AppPush` module design (spike question 5)

`src/slic3r/GUI/AppPush.{hpp,cpp}`, a third sink on the `accept_event` seam beside
`RemoteNotify::deliver` and `WebPush::deliver` (`RemoteHub.cpp:1403-1458`), with the same
worker-thread-and-queue shape.

### 5.1 The header

Deliberately the same surface as `WebPush.hpp:34-88`, so `RemoteHub.cpp` treats all three sinks alike:

```cpp
namespace Slic3r { namespace GUI { namespace AppPush {

void start(const nlohmann::json& saved);
void stop();
nlohmann::json settings_json();          // for settings.json - secrets in the clear, on disk only
nlohmann::json masked_json();            // for /hub/* - secrets masked
std::pair<int,std::string> register_device(const std::string& body);   // POST /r/<token>/push/device
std::pair<int,std::string> forget_device(const std::string& body);     // DELETE, same route
std::pair<int,std::string> remove(const std::string& id);              // DELETE /hub/apppush?id=
std::pair<int,std::string> set_options(const std::string& body);       // POST /hub/apppush
std::pair<int,std::string> test();                                     // POST /hub/apppush/test
void deliver(const nlohmann::json& event);
bool has_devices();
bool consume_dirty();
std::pair<int,std::string> debug_op(const std::string& body);          // SNORCA_DEBUG_ROUTES=1 only

}}}
```

### 5.2 Settings

`<datadir>/hub/settings.json` gains `apppush` beside `notify` and `webpush`:

```json
{ "apppush": {
    "enabled": true,
    "min_severity": "info",
    "apns": { "key_path": "C:\\Users\\ace\\keys\\AuthKey_ABC123DEFG.p8",
              "key_id": "ABC123DEFG", "team_id": "DEF123GHIJ",
              "bundle": "dev.example.ultra1", "enabled": true },
    "fcm":  { "service_account_path": "C:\\Users\\ace\\keys\\ultra1-fcm.json",
              "project_id": "ultra1-1a2b3", "enabled": true },
    "devices": [
      { "id": "d_7f3a…", "platform": "apns", "env": "production",
        "token": "<opaque>", "p256dh": "<b64url 65 bytes>", "auth": "<b64url 16 bytes>",
        "label": "Ace's iPhone", "app": "1.0.3", "os": "iOS 26.1",
        "added": 1757000000, "last_sent": 0, "last_status": 200, "last_error": "", "failures": 0 }
    ] } }
```

Notes that matter:

- **Credentials are paths, not contents** (the plan's rule, kept). `project_id` is cached from the
  service-account JSON so the send URL can be built without re-reading it, but is re-read on change.
- **Rows are keyed on `(platform, token)`** so re-registration replaces rather than duplicates - the
  plan's R10 requirement. A hard cap (16 devices, matching the Web Push subscription cap's spirit) and
  a 16 KiB body cap, both as `push/subscription` already does (`RemoteHub.cpp:2344-2372`).
- **`env` is per device** (R3), and `test()`'s per-device result names the host it reached, so a
  `BadDeviceToken` is diagnosable from the hub page rather than from a debugger.

### 5.3 The provider interface - the seam R1(a) needs

R1 recommends *"(c) now, (a) if and when"* and asks that the sender be *"a provider interface from day
one so (a) is a new implementation of an existing seam, not a rewrite."* That is one small abstract
class:

```cpp
struct PushRequest {                     // everything a provider needs, nothing it does not
    std::string device_token, env, bundle;
    std::string ciphertext_b64u;         // the aes128gcm blob, already encrypted to the device
    std::string collapse_id;             // truncated SHA-256 of "<printer id>|<kind>"
    std::string thread_id;               // the printer id, for grouping on the lock screen
    int         priority { 10 };         // APNs 10/5; FCM maps to "high"/"normal"
    int         ttl_seconds { 1800 };
};
struct PushResult { bool ok; int status; bool gone; std::string error; };

// A destination for already-encrypted notifications. Direct APNs and direct FCM are two
// implementations; a hosted relay would be a third, and would see exactly what these see:
// an opaque token, a size, a collapse id and a blob.
struct Provider {
    virtual ~Provider() = default;
    virtual const char* name() const = 0;
    virtual bool        available(std::string& why) const = 0;   // creds present, HTTP/2 present
    virtual PushResult  send(const PushRequest&) = 0;
};
```

`ApnsProvider`, `FcmProvider`, and later `RelayProvider`. `available()` is where the
`CURL_VERSION_HTTP2` check from §1.5 lives, so the hub page can say *"APNs is unavailable: this build
of libcurl has no HTTP/2"* rather than failing silently.

### 5.4 Routes

**Phone plane, `/r/<token>/` - token-gated, exactly like its neighbours:**

- `POST /r/<token>/push/device` - body as the plan's N2 (`platform`, `env`, `token`, `bundle`,
  `p256dh`, `auth`, `label`, `app`, `os`). Idempotent on `(platform, token)`. 16 KiB cap, row cap,
  `Content-Type: application/json` required.
- `DELETE /r/<token>/push/device` - answers `{"ok":true}` whether or not the row existed, so it is not
  an oracle (`RemoteHub.cpp:2353-2368`'s rule).
- `GET /r/<token>/pair` - the plan's N1, whose `push` object is now answerable for real:
  `{"webpush":true,"apns":<ApnsProvider::available()>,"fcm":<FcmProvider::available()>,"unified":true}`.

**Loopback admin plane, `/hub/*` - loopback peer + loopback `Host` + no cross-site `Sec-Fetch-Site`
+ `X-Hub-Secret`:**

- `GET /hub/apppush` - `masked_json()`: config with secrets masked, plus the device list with tokens
  masked.
- `POST /hub/apppush` - set options. Credential fields go through `take_secret`
  (`RemoteNotify.cpp:106`): a value beginning `****` means *keep the stored one*, so a credential only
  ever travels inward.
- `DELETE /hub/apppush?id=` - forget one device.
- `POST /hub/apppush/test` - one synchronous push to every device, per-device results naming provider,
  host and status.
- `GET /hub/apppush/debug` - env-gated on `SNORCA_DEBUG_ROUTES=1`, 404 otherwise (plan R10).

### 5.5 Masking

`mask()` at `RemoteNotify.cpp:96` - `"****"` plus the last four characters, `"****"` alone for anything
shorter. Applied to: the APNs key id and team id (last 4 visible - they are not secret but they
identify the account), the `.p8` path and service-account path (basename only), and every device token.
The `.p8` **contents** and the service-account `private_key` are never in any JSON at all, masked or
otherwise. `scrub()` (`RemoteNotify.cpp:148`) strips the device token, the bundle id and both key paths
out of libcurl's error text before it is stored in `last_error` or logged - libcurl's message carries
the URL, and the URL contains the device token.

### 5.6 Pruning and retries

Reuse `WebPush.cpp:627-636`'s rules with the provider-specific terminal set:

| Provider | Delete the row | Retry (3 tries, 1 s then 3 s, slept in 100 ms slices so `stop()` is not blocked) | Keep, do not retry |
|---|---|---|---|
| APNs | `410` reason `Unregistered`; `400` reason `BadDeviceToken`; `400 BadTopic` | `429`, `5xx`, transport error (`status == 0`) | `403 ExpiredProviderToken` -> mint a fresh JWT and retry **once**, then keep |
| FCM | `404 UNREGISTERED`, `404 NOT_FOUND` | `429`, `5xx`, transport error | `401` -> refresh the access token and retry **once**, then keep |

The `403 ExpiredProviderToken` and `401` rows are new next to the Web Push rules and are the two
credential-expiry cases that must never prune a live device. Everything else is the existing
`is_gone` / `worth_retrying` shape.

### 5.7 The gate: `test_ultra1_push.py`, `mock_apns.py`, `mock_fcm.py`

Following `test_phone_webpush.py` + `mock_push.py`: an isolated data dir, no printer, and an
**independent** Python implementation to compare the hub's bytes against.

- **`mock_apns.py`** - a loopback **h2c** (cleartext HTTP/2) server using Python's `h2` library, so no
  certificate is needed. This is why §1.6 prefers libcurl: `AppPush` points at it with
  `CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE` under a debug-only host override. It asserts, per request:
  `:path` is `/3/device/<hex>`; `apns-topic` equals the configured bundle; `apns-push-type` is
  `alert`; `apns-priority` is `10` or `5`; `apns-expiration` is in the future; `apns-collapse-id` is
  <= 64 bytes; and it **verifies the ES256 JWT** against the test `.p8`'s public key, checking
  `alg=ES256`, the `kid`, `iss` = the team id, `iat` within the hour, and that a second push inside
  20 minutes reuses the same token. It can be told to answer `410 Unregistered`, `429`, `503` and
  `403 ExpiredProviderToken` to drive §5.6.
- **`mock_fcm.py`** - plain HTTP/1.1. Serves the OAuth2 token endpoint (verifying the RS256 assertion
  against the test service account's public key, and that `aud`, `scope` and `exp <= iat + 3600` are
  right) and `messages:send` (asserting data-only, no `notification` block, `android.priority`,
  `android.ttl`, and the 4096-byte cap). Answers `UNREGISTERED` and `401` on demand.
- **The crypto assertion (M6, and the point of the whole thing).** Both mocks decrypt the `e` field
  with an independently-implemented RFC 8291 decryptor - `mock_push.py` already has one - and assert
  the plaintext round-trips. Then, separately, **the cleartext title, body, printer name and event
  code must appear nowhere in the raw HTTP request bytes**, headers included.
- **M11.** Sweep every response body on both listeners for the `.p8`'s base64 lines and the service
  account's `private_key`, and assert a masked value posted back to `POST /hub/apppush` leaves the
  stored one intact.
- **M12.** Drive every row of the §5.6 table and assert the device count after each.
- **M15.** `test_hardening.py` already diffs the instance-API manifest against `instance_api_allowed`;
  no new instance route is added here, so it should keep passing untouched - assert that it does.

### 5.8 Three corrections to the plan's §4.2

1. **The `aps` dictionary** must carry literal `alert` title/body text, not `title-loc-key` (§4.2
   above).
2. **No `exp` claim** in the APNs JWT (§2.1). Copying `vapid_jwt` wholesale would add one.
3. **`Http` needs `http_version()`** and `AppPush` needs an HTTP/2 availability check (§1.5). The plan
   assumed `Http` could already express this.

---

## 6. Revised phase 1 estimate

| Work | Days | Change from the plan |
|---|---|---|
| Deps: nghttp2 recipe, `USE_NGHTTP2=ON`, static-link traps, link line, verify with the probe | **0.5** | **new** - the plan folded this into a phase-0 spike and assumed no fix |
| `Http::http_version()` + the `CURL_VERSION_HTTP2` availability check | **0.25** | **new** |
| `AppPush` skeleton: settings, masking, worker thread, queue, provider seam | 1.0 | as planned |
| `ApnsProvider`: `.p8` loader, JWT cache keyed on `(host, kid)`, headers, collapse ids, env routing | 1.0 | slightly cheaper than planned - the ES256 half is proven and reused verbatim |
| `FcmProvider`: service-account JSON, RS256 assertion, token exchange with caching, data-only send | 1.0 | as planned; **and it has no dependency on the deps change** |
| Routes: `push/device` (both verbs), `pair`, the four `/hub/apppush*`, the hub-page card | 1.25 | as planned |
| Pruning, retries, the two credential-expiry cases, `scrub` wiring | 0.5 | as planned |
| Gate: `mock_apns.py` (h2c + JWT verification), `mock_fcm.py` (OAuth2 + send), `test_ultra1_push.py` | **1.5** | **up from ~1.0** - an HTTP/2 mock is more work than an HTTP/1.1 one |
| **Total** | **7.0 typical, 6-9 range** | was **5-7** |

**MVP (phases 0-2) becomes 12-18 days**, was 11-16. Phase 0 itself is now ~1 day (this spike plus the
Apple enrolment paperwork, which is latency rather than effort).

**A sequencing recommendation the spike makes possible.** FCM needs nothing from the deps change.
Build `FcmProvider` and the whole route/settings/gate skeleton first, land it, and do the nghttp2 work
and `ApnsProvider` as a second commit. That way the deps change - the only part with real
cross-platform risk - is isolated, revertible, and does not block Android push if it goes badly.

---

## 7. Open items

**Decisions for the user, before phase 1:**

1. **Enable nghttp2 on all three platforms, or Windows only?** §1.4 recommends all three; it is the
   larger change and the honest one.
2. **Which nghttp2 version to pin.** §1.4 suggests 1.64.0 over the current 1.70.0 for a lower CMake
   floor. Either satisfies curl's 1.12 minimum.
3. R8 still stands unanswered: *is there an appetite to keep an app alive for years?* This spike
   removes the technical doubt, not that one.

**Still unverified, and must not be planned against:**

1. **Whether a force-stopped Android app receives FCM.** Google does not state it on the pages read
   here. Hardware checklist, not a gate assertion (§4.4).
2. **Whether an Apple Personal Team can use the Push Notifications capability** - the plan's item 2,
   unchanged. Plan on the paid program.
3. **The exact `NSExceptionDomains` shape for a CIDR range** (plan R4) - device testing, not reading.
4. **MagicDNS resolution for third-party apps on iOS** (plan R5).
5. **Whether nghttp2 1.64/1.70 builds clean under the fork's macOS and Linux deps scripts.** The
   recipe is portable in principle; nobody has run it. This is the main residual risk in §1.4's
   two-hour estimate and the reason for the sequencing recommendation in §6.
6. Everything else in the plan's own unverified list is untouched by this spike.

**Also true and worth remembering:** APNs is HTTP/2-only *today*. Nothing about that is likely to
change, but the check in §1.5 means a build without HTTP/2 degrades to an honest message on the hub
page rather than a mystery.

---

## 8. Sources

Read **2026-09-04** unless stated otherwise.

**Apple**
- [Sending notification requests to APNs](https://developer.apple.com/documentation/usernotifications/sending-notification-requests-to-apns) - HTTP/2 requirement, hosts, headers, 4 KB
- [Establishing a token-based connection to APNs](https://developer.apple.com/documentation/usernotifications/establishing-a-token-based-connection-to-apns) - ES256, `kid`/`iss`/`iat`, refresh cadence, key limits
- [UNNotificationServiceExtension / Modifying content in newly delivered notifications](https://developer.apple.com/documentation/usernotifications/modifying-content-in-newly-delivered-notifications) - `mutable-content`, the `alert` requirement, the 30-second budget, the failure behaviour

**Google / Firebase**
- [Authorize send requests](https://firebase.google.com/docs/cloud-messaging/auth-server) - the v1 endpoint and the `firebase.messaging` scope
- [Using OAuth 2.0 for server-to-server applications](https://developers.google.com/identity/protocols/oauth2/service-account) - RS256 assertion, claims, `https://oauth2.googleapis.com/token`, `urn:ietf:params:oauth:grant-type:jwt-bearer`, one-hour maximum
- [Message types](https://firebase.google.com/docs/cloud-messaging/customize-messages/set-message-type) - notification vs data, 4096 bytes
- [Setting message lifespan](https://firebase.google.com/docs/cloud-messaging/customize-messages/setting-message-lifespan) - TTL 0-2,419,200 s
- [Android message priority](https://firebase.google.com/docs/cloud-messaging/android-message-priority) - high vs normal, Doze, the deprioritisation warning
- [Receive messages in an Android app](https://firebase.google.com/docs/cloud-messaging/android/receive) - `onMessageReceived` and its short execution window
- `firebase.google.com/docs/cloud-messaging/migrate-v1` still returns **HTTP 404** (re-checked 2026-09-04), so the plan's unverified item 1 stays unverified

**Microsoft**
- [WinHTTP Option flags](https://learn.microsoft.com/windows/win32/winhttp/option-flags) - `WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL`, `WINHTTP_PROTOCOL_FLAG_HTTP2 (0x1)`, Windows 10 1607, *"The default is 0x0"*
- [About WinHTTP](https://learn.microsoft.com/windows/win32/winhttp/about-winhttp) - HTTP/2 on by default for app containers and system services since 1709; the reentrancy caution

**curl / nghttp2**
- [CURLOPT_HTTP_VERSION](https://curl.se/libcurl/c/CURLOPT_HTTP_VERSION.html)
- [nghttp2.h, `NGHTTP2_EXTERN` / `NGHTTP2_STATICLIB`](https://github.com/nghttp2/nghttp2/blob/master/lib/includes/nghttp2/nghttp2.h)
- nghttp2 latest release **v1.70.0**, published 2026-07-29 (GitHub releases API)

**In this repository (at `ef81b7c194`)**
- `deps/CURL/CURL.cmake` · `deps/CMakeLists.txt:321-324` · `CMakeLists.txt:559-576` ·
  `src/slic3r/CMakeLists.txt:876-883`
- `src/slic3r/GUI/WebPush.cpp` (`:303` `ec_from_private`, `:416-472` `encrypt`, `:479` `der_to_raw`,
  `:493` `sign_es256`, `:547` `jwt_for`, `:577` `topic_for`, `:601-621` `push_once`,
  `:627-636` pruning) · `WebPush.hpp:96`
- `src/slic3r/GUI/RemoteNotify.cpp` (`:96` `mask`, `:106` `take_secret`, `:148` `scrub`)
- `src/slic3r/Utils/Http.hpp` · `src/slic3r/GUI/RemoteHub.cpp:1403-1458`, `:2344-2372`

**Built artifacts probed (not in the repository)**
- `deps/build/OrcaSlicer_dep/usr/local/lib/libcurl.lib`, `include/curl/curlver.h`, `bin/curl-config`
- `deps/build/dep_CURL-prefix/src/dep_CURL-build/lib/curl_config.h:1009`
- `deps/build/dep_CURL-prefix/src/dep_CURL/CMakeLists.txt:451-456`, `:1384`; `lib/http2.c:47`;
  `lib/vtls/openssl.c:2177`; `CMake/FindNGHTTP2.cmake`
- OpenSSL 1.1.1w (11 Sep 2023), `include/openssl/opensslv.h`

**Probe sources** (in `C:\Users\acesa\AppData\Local\Temp\snorca_hubtest\`, deliberately uncommitted):
`probe_curl_http2.cpp` + `probe_curl.bat`, `probe_apns_jwt.cpp` + `probe_apns_jwt.bat`,
`verify_apns_jwt.py`, `probe_fcm_rs256.cpp` + `probe_fcm.bat`.
