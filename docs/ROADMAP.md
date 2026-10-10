# pnt-edge roadmap

| Phase | Scope | Done when |
|---|---|---|
| A.1 (done) | Repository, daemon on the push API, LCM log and LCM UDP inputs, NMEA 0183 output (UDP/TCP), JSON status snapshot, systemd unit, CI | tests green; `pnt-edge configs/pnt-edge-replay.json --once` emits NMEA for the Cobra example log |
| A.2 | Serial GNSS (NMEA, UBX) and IMU drivers, ASPN-23 over LCM output, UTC offset from the receiver, recording to disk | a receiver and an IMU on serial ports drive the filter end to end on the bench |
| A.3 | `$PPNT` command parser and handlers (STATUS, VER, OUT, RESET, TIME), acknowledgements, audit log | a client can query and reconfigure output over the tactical port |
| B.1 | Embedded HTTPS + REST + WebSocket, authentication, roles, first-login flow | login works, status pages live |
| B.2 | Device, filter and system health pages; logs; config editor with validation; LEVERARM / HEADING commands | a maintainer changes a lever arm from the browser and the filter restarts with it |
| B.3 | Recording management, signed software updates, certificate upload, reboot | update applied from the browser |
| C | OS image for the SBC, A/B updates, watchdog, PTP/NTP, HIL rig and soak tests | 72 h soak on the bench with the log player |

Gates: unit tests and the replay smoke test in CI before every push; from A.2 on, a bench run with real serial
devices before each phase is closed.

## Phase B, delivered (2026-10-10): web surfaces

Tactical display and maintenance UI served by the daemon (cpp-httplib, plain ES modules, uPlot), login with
PBKDF2 users and sessions, bearer token for fleet readers, API v1 with SSE, control commands (filter reset,
application restart, NMEA settings, heading, lever arms, config save) with an audit log, in-memory solution
history and log through Cobra v0.2.6's log sink. `docs/WEB_UI.md`. Still open from the original Phase B: TLS
(a proxy for now), persistent history across restarts, the integrity timeline fed from the plugin's own events
rather than status polling.

