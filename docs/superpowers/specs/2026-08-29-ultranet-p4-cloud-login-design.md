# Ultra Net P4 — Bambu Cloud Login + Device Discovery (Increment 1)

Date: 2026-08-29
Owner: Snapmaker Ultra fork session (in-plugin, C++). Distinct from the parallel Python-bridge prototype at C:\Dev\printer_bridge.
Status: approved (design), user waived spec-review gate; proceed to implementation after resolving the unknowns below.

## Goal
Log into a Bambu Lab account and auto-discover the user's cloud printers with live status, entirely inside the slicer's `ultranet` plugin (native "My Devices"). No cloud print/control/camera this increment. Region: US only.

## Non-goals (increment 1)
Cloud print, cloud control (pause/stop/light/speed), cloud camera, non-US regions, bind/unbind flows, automated silent token refresh (a re-login prompt is acceptable).

## Approach (A — reuse host WebView + clean-room cloud client)
1. `build_login_cmd()` → Bambu US web login URL. Host's existing `ZUserLogin` WebView shows Bambu's own page (2FA/captcha handled by Bambu), captures the token on redirect, and calls the plugin's `change_user(user_info)`.
2. `change_user`: parse + store token/uid/name → fire `on_user_login` → `connect_server()`.
3. `connect_server()`: cloud MQTT to `us.mqtt.bambulab.com:8883` (user = cloud username, pw = access token, **cert verify ON** — real CA). `is_server_connected` / `is_user_login` / `get_user_*` reflect state; `user_logout` clears + disconnects.
4. Discovery: `GET https://api.bambulab.com/v1/iot-service/api/user/printers` (Bearer token) → device list → host DeviceManager cloud list.
5. Status: per owned device, cloud MQTT subscribe `device/<sn>/report`, route reports to the host **cloud** message callback (`on_message`, not LAN `on_local_message`); prime with `pushall`. Unique `client_id` so Bambu Handy is not dropped.

## Components
- `src/ultranet/BambuCloud.hpp/.cpp` — cloud client: token/user state, HTTPS GET (over existing OpenSSL), owns cloud `BambuMqtt`.
- `BambuMqtt` — add a `verify`/`cert` mode flag (LAN = verify off self-signed; cloud = verify on real CA) and configurable broker host/user/clientid.
- `UltraNetAgent` — cloud state + wire the currently-stubbed cloud exports (build_login_cmd, change_user, connect_server, is_server_connected, is_user_login, user_logout, get_user_id/name/avatar, get_user_selected_machine, and whichever export the host calls to ingest the cloud device list).

## Testing
Unit: parse `/user/printers` JSON; report-routing (cloud vs LAN). End-to-end: in-app login → X1C (reconnected to cloud) appears in My Devices → live status streams. Low physical risk (no print/control).

## Unknowns to resolve before/while coding (spikes)
1. Exact login URL + `ZUserLogin` navigation/token-capture format (what `change_user` receives).
2. Cloud MQTT username format (email? `u_<uid>`?) + cert expectations.
3. Host cloud-device ingestion path — which export/callback populates DeviceManager's cloud "My Devices" (e.g., `on_message` user-bind list, `get_user_print_info`, `get_user_selected_machine`, `start_subscribe`).
4. Token refresh endpoint/flow (or confirm re-login-prompt is enough for v1).

## Clean-room note
Point the WebView at Bambu's real login page (interop, not redistribution). Only our own HTTPS GET + MQTT client code; no vendor binaries. Consult C:\Dev\BambuStudio (open host side) + the user's protocol docs as behavioral reference only.
