# Fleet Console Refactor Plan

Refactor `blinky-console` from a single-device WebSerial UI into a fleet management app. Host it from `blinky-server` for on-site installation use, keep it working from Firebase for remote/dev use, and add multiple connection types without duplicating transport logic.

## Status

**Phase 1 — Plumbing.** ✅ Complete.
**Phase 2 — Transport abstraction.** ✅ Complete.
**Phase 3 — Server-backed transport.** ✅ M8+M9 complete. M10 (URL management UI) pending.
**Phase 4 — Multi-device UI.** ⏳ Not started.
**Phase 5 — Fleet operations.** ⏳ Not started.
**Phase 6 — Web Bluetooth.** ⏳ Deferred.

See the per-milestone status column in the [Milestones](#milestones) tables below for commit hashes.

## Context

**Today.** `blinky-console` is a single-device PWA. All device I/O goes through `src/services/serial.ts` (WebSerial-only). Zero HTTP/WebSocket client. No routing, no device list, no multi-device state.

**Parallel reality.** `blinky-server` already has a full fleet API (~30 REST routes + `/ws/{device_id}` + `/ws/fleet`) with stable `hardware_sn` keying and multi-transport device management (USB serial + BLE). The console uses none of it.

**Deployment targets.**
- **Same-origin from `blinky-server`** — primary use case. Event installations on isolated networks (often no internet). Must work fully offline once loaded. Typically paired with a `blinky-host` Pi running the LemonCart wifi AP for device/laptop connectivity.
- **Firebase-hosted** — dev and general access. HTTPS, internet-available. Must be able to discover/connect to `blinky-server` instances and also talk directly to devices via WebSerial / Web Bluetooth when no server is present.

## Requirements

### R1 — Keep WebSerial as a fallback, not a replacement
The console must work against devices directly over WebSerial when no `blinky-server` is available, and equivalently through server-proxied WebSocket when one is.

### R2 — Multi-transport, no duplication
Supported transports: WebSerial, Web Bluetooth, server-proxied WebSocket. Protocol parsing (newline-delimited JSON, commands, streaming) must live in one place and be reused across transports. Transport-specific logic must not leak into shared parsers.

### R3 — Device-oriented model with SN-based dedup
Canonical device identity = firmware-reported hardware serial number (`sn`, FICR DEVICEID on nRF52840; matches `blinky-server`'s `hardware_sn`). A device reachable via multiple transports appears as **one** card with a transport selector; the user picks which transport is active. Switching transports reconnects without losing UI state.

**Pre-connect dedup limitation:** a device discovered pre-connection (e.g., a BLE advertisement with no SN yet) is an unresolved stub. Stubs merge into the canonical device when the first connection reveals the SN. Acceptable for v1.

### R4 — Multi-origin aware, no active discovery (yet)
The same build runs from any host. Server discovery is **deferred** — no mDNS, no registry. For v1: same-origin server (when the page is served from one) is auto-available, plus a user-managed list of additional `blinky-server` URLs persisted in `localStorage`.

### R5 — Offline-first when served from blinky-server
Zero runtime internet dependencies. All assets bundled, no CDN fonts/scripts, no analytics. Service worker precaches the full shell. Any internet-only features must degrade gracefully or hide when offline.

### R6 — Fleet management features
Device list view, per-device detail view (mirroring today's single-device UI), and fleet-level operations (generator/effect/settings apply-to-all, via existing `/api/fleet/*` endpoints).

## Architecture

Three-layer abstraction:

```
┌─────────────────────────────────────────────────────────┐
│  Source (device discovery)                              │
│    WebSerialSource    WebBluetoothSource                │
│    BlinkyServerSource(baseUrl)                          │
└────────────────┬────────────────────────────────────────┘
                 │ discovers device refs, provides Transports
                 ▼
┌─────────────────────────────────────────────────────────┐
│  DeviceProtocol (shared, transport-agnostic)            │
│    Newline-JSON framing, command/response, streaming    │
│    Typed events: deviceInfo, settings, audio, ...       │
└────────────────┬────────────────────────────────────────┘
                 │ uses any Transport
                 ▼
┌─────────────────────────────────────────────────────────┐
│  Transport (byte-level, swappable)                      │
│    WebSerialTransport                                   │
│    WebBluetoothTransport                                │
│    ServerWebSocketTransport (unwraps {type,device_id,   │
│      data} envelope for server /ws/{id} messages)       │
└─────────────────────────────────────────────────────────┘
```

**Device registry (UI-facing).** A singleton registry collects device refs from all active Sources, keys them by SN, and merges multi-transport devices into one `Device` entity with a list of available `Transport` options. The UI renders one card per canonical device.

**Selecting a transport.** Each device card shows available transports (e.g., "USB (this browser)", "BLE (this browser)", "via blinky-host-pi"). Selecting one instantiates `DeviceProtocol(transport)` for that device.

**Same-origin detection.** On boot the app checks `window.location.origin` for a `blinky-server`. If it responds, it's auto-added as a `BlinkyServerSource` with `baseUrl = window.location.origin`. Additional servers come from `localStorage`.

## Serving the console from blinky-server

FastAPI additions in `blinky-server`:

1. `StaticFiles(directory=STATIC_DIR, html=True)` mount at `/` — after all `app.include_router(...)` calls.
2. Catch-all `@app.get("/{full_path:path}")` returning `index.html` for SPA deep-link fallback. Must come after API routers.
3. `STATIC_DIR` configurable via env var; default = `blinky-server/web/`.

Vite additions in `blinky-console`:

1. `build.outDir = '../blinky-server/web'` — or continue building into `dist/` and copy on deploy (TBD during implementation).
2. Dev proxy in `vite.config.ts`:
   ```ts
   server: {
     port: 3000,
     proxy: {
       '/api': 'http://localhost:8420',
       '/ws':  { target: 'ws://localhost:8420', ws: true },
     },
   }
   ```
   Avoids build-and-reload during development.

**CORS.** Currently `allow_origins=["*"]` in `blinky-server/blinky_server/app.py`. Can tighten to `localhost:3000` (dev) once same-origin serving is in place, but leave permissive until Firebase-hosted clients are the only cross-origin callers and we've decided the mixed-content approach.

**Cache-Control.** FastAPI's `StaticFiles` doesn't set sensible defaults. `index.html` should be `no-store` / `must-revalidate`; hashed `/assets/*.{hash}.{js,css}` should be long-lived. Thin middleware or reverse proxy, TBD.

## Pre-refactor fixes — ✅ shipped in Phase 1

The four items listed here when the plan was written have all shipped (M1, M2 above). Kept as a reference for the reasoning behind them:

- **Add `sn` and optional `ble` to `DeviceInfoSchema`** at `blinky-console/src/schemas/device.ts`. Firmware already emits them (`blinky-things/inputs/SerialConsole.cpp:436`); the console was dropping them. Shipped in M1.
- **Note (still relevant):** `device.id` in the schema is the *device-type* (`hat_v1`, `tube_v2`, …), **not** a per-unit identifier. The dedup key is `sn`. M7's `DeviceRegistry` keys by `sn`.
- **Remove dead Google Fonts cache rules** at `blinky-console/vite.config.ts:76-104`. Shipped in M2.
- **Widen PWA precache glob** to include PNG assets. Shipped in M2.

## Milestones

PR-sized, roughly ordered. Each milestone should leave the app in a shippable state.

### Phase 1 — Plumbing (independent, low risk) — ✅ shipped

| # | Status | Milestone | Commit |
|---|--------|-----------|--------|
| M1 | ✅ | Add `sn` / `ble` fields to `DeviceInfoSchema` and surface through `useSerial` | `355860b` |
| M2 | ✅ | Remove dead font cache rules; widen PWA precache glob to include PNGs | `e23259d` |
| M3 | ✅ | Serve static files from `blinky-server` + SPA fallback catch-all + configurable `BLINKY_STATIC_DIR` env | `71beca5` |
| M4 | ✅ | Vite dev proxy for `/api` and `/ws`; build into `blinky-server/web/`; dev workflow documented | `dfd8155` |
| — | ✅ | Phase 1 review fixes (SPA reserved-prefix correctness, schema tolerance) | `547879c` |

### Phase 2 — Transport abstraction (the core internal refactor) — ✅ shipped

| # | Status | Milestone | Commit |
|---|--------|-----------|--------|
| M5 | ✅ | `Transport` interface + `WebSerialTransport` in `services/transport/`. `SerialService` delegates byte I/O to the transport. | `cf7a0ac` |
| M6 | ✅ | `DeviceProtocol` extracted into `services/protocol/` — JSON parsing, command/response, stream dispatch, high-level RPCs. `serial.ts` becomes a thin legacy facade. | `417b5da` |
| M7 | ✅ | `Source` interface, `Device` class, `DeviceRegistry` singleton, `WebSerialSource` in `services/sources/`. UI still single-device (registry not yet consumed). | `2782037` |
| — | ✅ | Phase 2 review fixes (lifecycle guards: setTransport connection check, disconnect-first on reconnect, dead-protocol replacement on upsert, last-non-empty displayName, synth-id counter) | `540c9fc` |
| — | ✅ | Zombie-state fix: WebSerialTransport auto-disconnects on any read-loop failure, not only `DEVICE_LOST` / `DISCONNECTED` | `783a6e5` |

Every Phase 2 milestone ships with no UI behavior change. 261 console tests + 115 server tests pass; the legacy `serialService` singleton continues to work unmodified.

### Phase 3 — Server-backed transport — ⏳ not started

| # | Status | Milestone | Touches |
|---|--------|-----------|---------|
| M8 | ✅ | `ServerWebSocketTransport` — wraps `/ws/{device_id}`, unwraps the `{type, device_id, data}` envelope. | `blinky-console/src/services/transport/` |
| M9 | ✅ | `BlinkyServerSource(baseUrl)` — lists via `GET /api/devices`, creates transports on demand. Auto-instantiated for same-origin when a server responds at `/api/devices`. | `blinky-console/src/services/sources/` |
| M10 | ⏳ | Server URL management UI (localStorage-backed list of additional servers, add/remove) | `blinky-console/src/components/Settings/` |

### Phase 4 — Multi-device UI — ⏳ not started

| # | Status | Milestone | Touches |
|---|--------|-----------|---------|
| M11 | ⏳ | Add routing (React Router). Move current single-device tabs to `/devices/:sn` route. No list view yet — opening the app auto-navigates to the only device, preserving today's UX when only one is available. | `blinky-console/src/App.tsx`, new `routes/` |
| M12 | ⏳ | `/devices` list view aggregating all Sources by SN. Device cards show transport selector (WebSerial / BLE / via server). Switching transport preserves route state. | `blinky-console/src/routes/DevicesList.tsx`, `src/components/DeviceCard/` |
| M13 | ⏳ | Real-time device list updates: either poll `GET /api/devices` from `BlinkyServerSource`, or add a `device_connected`/`device_disconnected` event stream to `blinky-server` and subscribe. Decision during implementation. | both repos, TBD |

### Phase 5 — Fleet operations — ⏳ not started

| # | Status | Milestone | Touches |
|---|--------|-----------|---------|
| M14 | ⏳ | Fleet-level command UI (apply generator/effect/settings to all or to selection) via existing `/api/fleet/*` endpoints | `blinky-console/src/routes/Fleet.tsx` |
| M15 | ⏳ | Flash/deploy UI gated behind an auth mechanism (TBD — see open questions). Hidden entirely when the current server doesn't accept the client's credentials. | `blinky-console/src/routes/Firmware.tsx`, `blinky-server` auth |

### Phase 6 — Web Bluetooth — ⏳ deferred

| # | Status | Milestone | Touches |
|---|--------|-----------|---------|
| M16 | ⏳ | `WebBluetoothTransport` + `WebBluetoothSource`. Requires coordination with firmware BLE NUS characteristic layout. | `blinky-console/src/services/transport/`, `src/services/sources/` |

## Out of scope (for this refactor)

- **Active server discovery** (mDNS, cloud registry) — R4 says manual URL list only for v1.
- **Firebase-to-LAN-server mixed content** — Firebase build will be functional for WebSerial / Web Bluetooth direct connections; cross-origin to HTTP blinky-server instances is deferred until we pick an approach (TLS on server / HTTP Firebase build / cloud relay / split-model).
- **Cross-source identity matching before first connection** — a BLE-advertised stub stays separate from a server-reported device until their SNs reconcile.
- **Replacing the PWA with something simpler** — even though the app isn't useful offline on Firebase, the PWA shell is needed for the same-origin-from-server offline case.
- **Visual redesign** — mechanical refactor; no new visual design work except what's necessary to expose new multi-device concepts.

## Open questions (decide during implementation)

1. **Auth for flash/deploy endpoints.** Today `/api/devices/{id}/flash` and `/api/fleet/*flash` require an API key. A browser can't safely hold an admin key. Options: session-based cookie auth on `blinky-server`, server-generated short-lived tokens, or hiding firmware UI entirely when the browser has no creds. Picks happen at M15.
2. **Real-time device-list updates.** Poll `/api/devices` every N seconds vs add a new WebSocket event stream. Simpler to poll; nicer UX with events. Picks happen at M13.
3. **Mixed-content approach** for Firebase-hosted clients talking to LAN `blinky-server`s. Deferred (see Out of scope).
4. **Firmware upgrade paths when SN isn't emitted.** Older firmware versions may not include `sn` in `json info`. Do we refuse to connect, fall back to a transport-local synthetic ID (e.g., USB serial number string from `navigator.serial.getInfo()`), or prompt the user to flash? Should be a TODO in M5 / M7.
5. **Fleet-server integration with Firebase build.** If Firebase build talks to a `blinky-server` over the internet (impossible today due to mixed content + LAN-only addressing), do we want a cloud-hosted `blinky-server` in the future? Informs but doesn't block this plan.

## Verified

Preflight checks completed 2026-04-19:

- Firmware emits `"sn"` in `json info` (`blinky-things/inputs/SerialConsole.cpp:436`, FICR DEVICEID on nRF52840). `"ble"` (MAC) also emitted on BLE-capable builds.
- `blinky-server`'s device model and wire protocol already align with the console's schemas (settings shape is identical; `DeviceResponse` is a superset of the console's `DeviceInfo`). Envelope wrapping on `/ws/{device_id}` is the only transformation `ServerWebSocketTransport` needs to handle.
- No CDN / runtime-internet dependencies in the current `blinky-console` build (all npm deps bundled, no external URLs in `src/`, fonts are system stack in `src/styles.css:39`, `index.html` references only local assets).
- `blinky-server` does not currently serve static files — `StaticFiles` mount is a clean addition.

## Phase 2 post-review fixes (April 2026)

- **Zombie state on `done: true`:** `startReading()` now calls `disconnect()` if the read loop exits cleanly with `isReading` still set (stream closed externally without error).
- **Schema validation failure:** `getDeviceInfo()` returns `null` instead of raw unvalidated data when Zod schema fails. Prevents type-unsafe access to missing fields.
- **No-SN path clarified:** `WebSerialSource.pickAndConnect()` comment documents that the synthesised-ID path is degraded-but-functional (device works, dedup doesn't).
- **Build targets documented:** `build` → `../blinky-server/web/` (local server), `build:firebase` → `dist/` (Firebase deploy). CI uses `build:firebase`.

### Known issues for Phase 3+

- `disconnect()` emits `disconnected` unconditionally (even if never connected). By-design per types.ts contract but can cause spurious UI transitions. Consumers must tolerate.
- `handleLine` has 6 near-identical try/catch dispatch blocks. A dispatch table helper would cut ~80 lines. Follow-up cleanup.
- `sendJsonResult` wrapper is unnecessary indirection — inline at 4 call sites.
- `validateCommand` silently mutates invalid commands instead of rejecting. Consider throwing `COMMAND_INVALID` on pattern failure.
- `getConnectionState()` collapses readable/writable into connected. No current consumer uses them independently. Deprecate or remove the method.

## Non-requirements

To prevent scope drift, this refactor explicitly does **not**:

- Change the wire protocol spoken by the firmware.
- Change the `blinky-server` REST or WebSocket API shape (may *add* a device-event stream at M13).
- Introduce a new state management library — keep hooks/context, add routing only.
- Block on firmware changes for the WebSerial/server paths; BLE work (M16) can wait for firmware readiness.
