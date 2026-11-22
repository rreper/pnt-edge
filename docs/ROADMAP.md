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

## Cobra v0.2.17 (2026-11-16)

`subprojects/pntos-cpp.wrap` names Cobra v0.2.17 (v0.2.8 before), and `meson.build` asks for at least that
version, so that a checkout still holding the old subproject stops with one clear message. Nothing changes by the
move: all that Cobra added since v0.2.8 is off unless the filter configuration names it. The 60 s replay gives the
same 350 NMEA sentences byte for byte, and the build with the integrity plugin the same 200; 28 tests pass in both.

What is there to use, once a filter configuration for the bench asks for it: handling of lost, late and sampled IMU
data (v0.2.9, the items of the inertial review), measurements applied in time order and body velocity with
calibration states (v0.2.10), alignment in motion, so that the heading no longer comes from the configuration
(v0.2.11), zero-velocity and zero-rate updates at a standstill (v0.2.12, v0.2.15), an IMU replay across the
initialization (v0.2.13), and the receiver's time offsets (v0.2.16, v0.2.17). Cobra's
`configs/pos_ins_ground_vehicle.json` is the example that puts them together for a road vehicle; its channel names
and time biases are those of the example recording.

## A filter configuration for real devices (2026-11-22)

Every configuration here named Cobra's `pos_ins.json`, which carries the heading, the IMU mounting, the antenna
lever arm and the time biases of Cobra's example recording, and re-stamps every IMU sample 10 ms after the last.
On the recording that is right; on a vehicle with a VN-100 and a u-blox on serial ports none of it is, and
`configs/pnt-edge-serial.json` set the VN-100 to 200 Hz on top of it.

`configs/filters/road_vehicle.json` is Cobra's `pos_ins_ground_vehicle.json` (v0.2.17) for real devices: alignment
in motion, zero-velocity and zero-rate updates at a standstill that the receiver's velocity confirms, the
constraints of a road vehicle, the receiver's velocity as a measurement with its age as a filter state, the
live-IMU handling of v0.2.9 in place of the `TimeAdjusterConfig`; mounting identity, lever arms zero, time biases
zero. `configs/pnt-edge-serial.json` names it, sends the receiver's velocity to the filter (`velocity_channel`)
and sets the VN-100 to 100 Hz; `meson install` puts the file into `/etc/pnt-edge/filters`. The replay and demo
configurations keep `pos_ins.json`, which is the filter that fits the recording they play.

The full recording (43 minutes) replayed through the daemon, its GGA and HDT sentences against the recording's
reference solution:

| filter configuration | first position | horizontal RMS (worst) | heading RMS (worst) |
|---|---|---|---|
| Cobra `pos_ins.json` (the heading from the file) | 10 s | 1.54 m (2.8) | 0.83° (3.4) |
| Cobra `pos_ins_ground_vehicle.json` | 137 s | 1.54 m (2.8) | 0.65° (2.6) |
| `road_vehicle.json` with the recording's mounting, lever arm and 150 ms on the fixes filled in | 137 s | 1.51 m (3.4) | 0.65° (2.5) |
| `road_vehicle.json` with mounting and lever arm only | 137 s | 1.87 m (4.0) | 0.64° (2.4) |
| `road_vehicle.json` as it is installed, nothing filled in | 137 s | 2.05 m (4.2) | 1.16° (3.0) |

- The car of the recording stands for its first 125 s. A filter that finds its heading while driving has nothing
  to say until then; the status reports `aligning`. Whether the daemon should pass the receiver's own fix on in
  that time is open.
- The filter finds the receiver's velocity 0.30 s older than its time stamps in each of the three runs.
- The 0.36 m between the third and the fourth row are the 150 ms of the fixes, measured against a reference
  solution whose time base is not the IMU's (Cobra `docs/DESIGN.md` §7.29). It says nothing about devices stamped
  by the drivers here, where zero is the figure to start from.

`pnt-edge-serial-sim` now sends the receiver's velocity of the log in each NAV-PVT (`--velocity-channel`). It
sent zero before, which a filter that listens to the velocity takes for a vehicle that never moves.

Test: `EdgeConfig.TheRoadVehicleFilterLoadsAndCarriesNothingOfTheExampleRecording`; 29 tests.

**The serial path with this filter**, without hardware: `pnt-edge-serial-sim` plays the first ten minutes of the
recording in real time as a VN-100 and a u-blox, the daemon reads them through its drivers with host time stamps
(`road_vehicle.json` with the recording's mounting and lever arm). Aligned 138 s into the recording, no error and
no warning in the daemon's log, the receiver's velocity found 0.31 s old, 1.52 m horizontal RMS (3.0 m at worst)
and 0.96° of heading (2.1°) over the 460 s that follow.

**Integrity status.** The status file already held every key the integrity plugin writes (43 with cobra-integrity
v0.2: the vertical protection level, why a source is excluded, which have gone silent, and the rest), and the
maintenance page's Integrity tab lists them all. The two outputs an operator reads did not: `$PPNT,INTEG` has four
more fields behind its six, the vertical protection level and alert limit, the cause of each exclusion and the
silent sources (`docs/COMMAND_PROTOCOL.md`; a reader that counts the fields has to be told), and the tactical page
shows the same three things. Tests `Integrity.TheSentenceCarriesTheVerticalLevelAndWhyASourceIsOut` and
`TheStatusIsReadFromThePluginsKeys`; 31 tests in the plain build and in the one with the plugin.

**The integrity plugin on top of this filter**, with `"orchestration": "protected"` and the integrity keys of
cobra-integrity's `pos_ins_protected1.json`, over the full recording. Two things had to be put right first.

- cobra-integrity did not hand the measurement models the sensor errors the inertial is applying, which Cobra
  added in v0.2.12: 4926 velocity updates and 624 zero-rate updates were refused, each with an ERROR, while a
  solution still came (1.53 m, 0.75° of heading). Fixed there on 22 November 2026 (its `docs/PROGRESS.md`); this
  repository needs a cobra-integrity from that day on.
- The status loop read the registry groups `integrity/status` and `fusion/gating` from the main thread while the
  filter was writing them: one `Batch "integrity/status" already live` in the 2445 epochs of the replay at 25
  times real time, and the daemon crashed on its way out. The solution callback, which runs in the thread that
  feeds the filter, now keeps both and the status loop takes its copy. No unit test reaches that loop; the check
  is the replay, twice: no ERROR line, exit code 0, the status file with its integrity and gating keys.

With both: 1.51 m and 0.65° of heading, which is the run without the plugin; PRIMARY throughout; HPL 6.1 m and
VPL 8.7 m in the median (7.9 m and 10.6 m while the velocity updates were refused).

Not done: a protected variant of `road_vehicle.json` among the configurations; a run with real devices; the
receiver's fix passed on while the filter is still aligning.
