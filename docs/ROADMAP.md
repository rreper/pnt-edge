# pnt-edge roadmap

| Phase | Scope | Done when |
|---|---|---|
| A.1 (done) | Repository, daemon on the push API, LCM log and LCM UDP inputs, NMEA 0183 output (UDP/TCP), JSON status snapshot, systemd unit, CI | tests green; `pnt-edge configs/pnt-edge-replay.json --once` emits NMEA for the Cobra example log |
| A.2 (drivers done) | Serial GNSS (NMEA, UBX) and IMU drivers, UTC offset from the receiver, recording to disk — delivered 2026-10-11; ASPN-23 over LCM output still open | a receiver and an IMU on serial ports drive the filter end to end on the bench (simulator passes; hardware bench run pending) |
| A.3 | `$PPNT` command parser and handlers (STATUS, VER, OUT, RESET, TIME), acknowledgements, audit log | a client can query and reconfigure output over the tactical port |
| B.1 | Embedded HTTPS + REST + WebSocket, authentication, roles, first-login flow | login works, status pages live |
| B.2 | Device, filter and system health pages; logs; config editor with validation; LEVERARM / HEADING commands | a maintainer changes a lever arm from the browser and the filter restarts with it |
| B.3 (update done) | Recording management, signed software updates, certificate upload, reboot; software update from the browser delivered 2026-10-12 (checksum + execution check, backup, in-place restart, rollback; signing open) | update applied from the browser |
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

## Phase A.2, drivers delivered (2026-10-11): serial devices

VN-100 (binary output and ASCII), u-blox UBX (NAV-PVT, NAV-TIMELS, CFG-VALSET on open) and generic NMEA drivers as
a `serial` input with one thread per device, host-clock time mapping with latency removal, a reorder buffer, UTC from
the receiver for the NMEA output, per-device health in the status and the maintenance UI, raw recording,
reconnection on unplug. `pnt-edge-serial-sim` plays an LCM log as the two devices on pseudo-terminals, which the
test suite and `configs/pnt-edge-serial-sim.json` use. `docs/SERIAL.md`. Open: the bench run with real devices
(VN-100 register writes and the u-blox port keys are per the vendor protocols but not yet exercised on hardware),
ASPN-23 over LCM output, serial NMEA output. Soak (2026-10-12, 8 h + continuing on the simulator): NMEA pacing froze on a
backwards UTC step (fixed), Cobra retained every solution behind the callback (fixed in v0.2.8), a host clock step after
suspend/resume left the filter in error for good (the daemon now restarts it, event `CLOCK_STEP`). Option kept open: run the
filter on `CLOCK_MONOTONIC` and apply the UTC offset only at the outputs, which would make host clock steps invisible.

## Phase B.3, software update delivered (2026-10-12)

Software tab on the maintenance UI: upload the release package with its `.sha256`, staged and checked (tarball,
contents, checksum, the packaged binary runs here), apply into the install prefix (bin, lib, share; never etc) with a
backup and a start check of the installed executable, in-place re-exec of the application (works without systemd),
roll back. Verified against the published v0.1.1 package on an installed tree. `docs/WEB_UI.md`. Releases are now
relocatable (`$ORIGIN/../lib` rpath) and the version is 0.2.0. Open in B.3: package signatures, recording
management, certificate upload, reboot.

## Phase A.3, delivered (2026-10-11): `$PPNT` commands

Parser with checksum and length checks, STATUS / VER / TIME replies from the hub, OUT / LEVERARM / HEADING /
RESET queued through the same command layer as the web control routes, acknowledgement of every sentence,
commands read on the NMEA TCP connections and on a UDP command port, audit log shared with the web surfaces.
Cobra v0.2.7 so that `VER` reports the real library version.

