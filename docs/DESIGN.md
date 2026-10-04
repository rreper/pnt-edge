# pnt-edge design

## 1. What it is

A product application that runs the Cobra C++ navigation filter on an edge single-board computer with two
Ethernet interfaces: a tactical interface that streams NMEA 0183 (and ASPN-23 over LCM) and accepts `$PPNT`
commands, and a maintenance interface serving a browser UI with authentication for health, configuration and
software maintenance of the application and the connected devices.

The filter stays in its own repository (github.com/rreper/Cobra) and is consumed here as a versioned
dependency (`subprojects/pntos-cpp.wrap`, pinned to a release tag). Its accuracy is proven there (acceptance
against the Python reference, degraded-sensor matrix); this repository proves plumbing, health and operations.

## 2. Process architecture

One service, `pnt-edge` (C++20, systemd unit, runs as its own user):

```
sensors ──► inputs ──► cobra::Filter (push API) ──► solutions ──► NMEA (UDP/TCP) + ASPN/LCM (tactical eth0)
                │                 │                       │
                │                 └── registry ───────────┴──► status snapshot ──► HTTPS/REST/WS + UI (maintenance eth1)
                └── device health (rates, ages, parse errors)
```

- **Inputs** (`src/inputs.*`): one class per source, each ending in `Filter::push`. Phase A: LCM event log
  (replay, paced or looped) and LCM UDP multicast (Cobra's transport with a forwarding mediator). Phase A.2:
  serial GNSS (NMEA and u-blox UBX), serial / SPI IMU drivers. Every input keeps counters (per channel, decode
  failures, last time of validity) that feed the health model.
- **Filter**: `cobra::Filter` built from a Cobra JSON config. All sensor modelling (lever arms, IMU presets,
  processors, gating) lives in that file; pnt-edge never duplicates it.
- **Outputs** (`src/outputs.*`, `src/nmea.*`): NMEA 0183 sentences from each published solution, rate-limited,
  to UDP targets and TCP clients bound to the tactical interface; ASPN-23 over LCM through Cobra's UDP transport
  (Phase A.2). Time: solution time of validity plus a configured UTC offset; Phase A.2 derives the offset from
  the GNSS receiver's leap-second data.
- **Status** (`src/status.*`): a JSON snapshot every second (atomic file write) with input counters, solution
  count and age, last solution with sigmas, gating counters from the registry, TCP client count, and a one-word
  health verdict. The maintenance UI reads this model; the `$PPNT,STATUS` reply is a projection of it.
- **Commands**: `$PPNT` sentences on the tactical interface (docs/COMMAND_PROTOCOL.md).

## 3. Maintenance interface (Phase B)

An embedded HTTPS server in the daemon (civetweb or Boost.Beast; no second runtime on the board) serving a
static single-page UI and a REST + WebSocket API bound to eth1 only:

- Authentication: username / password with Argon2id hashes in `/var/lib/pnt-edge/users.json`, session
  cookies (HttpOnly, SameSite=Strict), CSRF token, login rate limiting, roles `viewer` and `maintainer`,
  forced password change on first login, no default credentials shipped.
- TLS: self-signed certificate generated on first boot; upload of an operator certificate.
- Health pages: overview (health verdict, position, sigmas, time), devices (per input: connected, rate, age,
  parse errors, serial settings), filter (alignment state, innovation and gating statistics, solution timeline),
  system (CPU, memory, temperature, disk, clock sync, service uptime), logs (ring buffer, download).
- Maintenance: edit and validate the Cobra config (the JSON files) and the appliance config, apply with
  restart, record raw data to disk with retention, download recordings, signed software update packages,
  reboot. Every change goes to an audit log with user and time.
- Network separation: the tactical services bind to eth0, the UI to eth1; an nftables rule set in the image
  enforces it.

## 4. Platform (Phase C)

Linux on the target SBC (Jetson Orin class assumed), systemd, read-only root with A/B updates (RAUC or Mender),
hardware watchdog, time from the GNSS receiver with a PTP or NTP server on the tactical network as an option.
Hardware-in-the-loop rig: Cobra's `lcm_log_player` feeds the tactical interface, `log_stats` and the
degraded-sensor matrix score the result.

## 5. Security notes

No inbound services on eth0 beyond NMEA/ASPN/command ports; commands that change configuration are logged;
the UI runs only over TLS; secrets never in the repository; the daemon drops privileges (systemd `User=`,
`ProtectSystem=strict`).

## 6. Open points

- Target SBC and OS image.
- Which serial GNSS and IMU models the first units carry (drivers in Phase A.2).
- Integrity monitoring (source exclusion, PACE state, protection level) is developed in a separate private
  repository as a Cobra orchestration plugin; pnt-edge will surface its state through NMEA fix quality, GST and
  `$PPNT,STATUS`.
