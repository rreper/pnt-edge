# Web surfaces

pnt-edge serves two HTTP surfaces from the daemon itself (cpp-httplib, no interpreter, no build step, works
offline). Both are bound to configured interfaces (`web` in the config) and share one JSON API.

| surface | default | login | what |
|---|---|---|---|
| tactical display | `:8080` on the tactical network | none | large readouts (position, heading, speed, fix quality, PACE, HPL, excluded sources), the track, and a **Reset filter** button |
| maintenance UI | `:8081` on the maintenance network | user + password (plain HTTP) or a bearer token (read-only) | overview, inputs, solution with plots, integrity timeline, outputs, log, config; control |

```json
"web": {
  "tactical":    { "bind": "192.168.10.1", "port": 8080 },
  "maintenance": { "bind": "192.168.20.1", "port": 8081 },
  "users_file":  "/etc/pnt-edge/users.json",
  "api_token":   "",
  "audit_file":  "/var/log/pnt-edge/audit.log",
  "history_seconds": 7200,
  "log_lines": 2000
}
```

A surface with `"port": 0` (or `false`) is not served. The pages live in `www/` (installed to
`<datadir>/pnt-edge/www`; `web.www` overrides; a build-tree binary finds the source tree's `www/` by itself).

## Users and sessions

```
pnt-edge /etc/pnt-edge/pnt-edge.json --set-password admin      # prompts, or reads PNT_EDGE_PASSWORD
```

Passwords are stored as PBKDF2-HMAC-SHA256 (100 000 iterations, per-user salt) in `users_file` (mode 0600).
A login sets an `HttpOnly` session cookie valid for 12 hours of inactivity; failed logins are delayed and
logged. The maintenance surface is plain HTTP by design (a dedicated maintenance network); put a TLS proxy in
front of it if that changes.

## Fleet access

A central server reads the same API on the maintenance surface with `Authorization: Bearer <api_token>`
(`web.api_token`). Token access is read-only: control routes answer 403.

## API v1

All responses are JSON. Read routes (both surfaces; the maintenance surface requires a session or token):

| route | content |
|---|---|
| `GET /api/v1/status` | the status snapshot (what `status.file` holds: health, uptime, inputs, gating, integrity, last solution) |
| `GET /api/v1/solution` | latest solution: lat/lon/alt, local NED, speed, heading, sigmas, HPL, PACE, integrity object |
| `GET /api/v1/history?seconds=600&points=1200` | arrays of the solution history (t, n, e, d, speed, heading_deg, sigma_h, sigma_d, hpl, pace) and the local origin |
| `GET /api/v1/events?n=200` | timeline: HEALTH, PACE, EXCLUDE, READMIT, RESET, CONTROL entries with wall time |
| `GET /api/v1/log?n=300&level=INFO` | the recent log (Cobra's and pnt-edge's lines, through the log sink) |
| `GET /api/v1/config` | maintenance only: `{edge, filter}` — both active configs |
| `GET /api/v1/version`, `GET /api/v1/whoami` | version; who the caller is and whether read-only |
| `GET /api/v1/stream` | Server-Sent Events: `update` with `{status, solution}` on every change (≤ 5 Hz), heartbeat every 5 s |

Control routes (POST with a JSON body; queued to the main loop; every call is written to the audit log and the
event timeline with the caller and address):

| route | body | surface | effect |
|---|---|---|---|
| `POST /api/v1/control/reset` | `{"what": "filter"}` | tactical, maintenance | tears the filter, input and NMEA output down and re-aligns from scratch |
| `POST /api/v1/control/reset` | `{"what": "app"}` | maintenance | exits with code 3; the systemd unit (`Restart=always`) starts it again |
| `POST /api/v1/control/nmea` | `{"rate_hz", "sentences", "udp_targets"}` (any subset) | maintenance | applied immediately (the runtime is rebuilt), saved to the config file |
| `POST /api/v1/control/heading` | `{"deg", "sigma_deg"}` | maintenance | written to the filter config's manual-heading alignment; effective on the next filter reset |
| `POST /api/v1/control/leverarm` | `{"label", "x", "y", "z"}` | maintenance | written to the filter config's processor of that label; effective on the next filter reset |
| `PUT /api/v1/config` | the edge config object | maintenance | validated, saved; NMEA changes applied, the rest on restart |

Errors: 400 (bad JSON), 401 (no session/token), 403 (not permitted on this surface or read-only token).

## What the maintenance pages show

Overview (health, position, integrity, recent events) · Inputs (counts, rates per channel, decode failures,
gating counters per processor) · Solution (track in local NED with the 1-sigma ellipse and the HPL circle,
horizontal sigma and HPL over time, speed and heading over time, selectable window) · Integrity (the whole
`integrity/status` group, the exclusion/readmission/PACE timeline) · Outputs (NMEA counters and targets; change
rate, sentences and targets; set the initial heading and lever arms; reset filter / restart application) · Log
(level filter) · Config (edit and save the edge config; the filter config read-only).

Everything is plain ES modules plus two vendored MIT libraries: uPlot 1.6.31 (time series) and cpp-httplib
0.18.3 (server). No Node, no bundler, no framework.

## Development

```
./build/pnt-edge configs/pnt-edge-replay.json          # tactical on :8080, maintenance on :8081
PNT_EDGE_PASSWORD=... ./build/pnt-edge configs/pnt-edge-replay.json --set-password admin
```
