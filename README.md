# pnt-edge

[![ci](https://github.com/rreper/pnt-edge/actions/workflows/ci.yml/badge.svg)](https://github.com/rreper/pnt-edge/actions/workflows/ci.yml)

> **Under Construction.** This repository is an early-stage skeleton (roadmap Phase A.1). Interfaces,
> configuration formats and the `$PPNT` command set will change without notice; nothing here is fit for
> operational use yet. See `docs/ROADMAP.md` for what exists and what is planned.


An edge PNT appliance: the [Cobra C++ navigation filter](https://github.com/rreper/Cobra) (Pinson15 GNSS/INS
error-state EKF, pntOS plugin architecture) running as a service on a single-board computer, with

- a **tactical interface** (Ethernet 1): NMEA 0183 output over UDP and TCP (GGA, RMC, VTG, HDT, GST, ZDA, PASHR),
  ASPN-23 over LCM, and a `$PPNT` proprietary NMEA command set (docs/COMMAND_PROTOCOL.md);
- a **maintenance interface** (Ethernet 2): a browser UI with username/password authentication for health,
  configuration and software maintenance of the application and every connected device (Phase B).

Status: Phase A skeleton. The daemon runs the filter through its push API, takes measurements from an LCM log
or LCM UDP multicast, emits NMEA, and writes a JSON health snapshot every second. See docs/ROADMAP.md.

## Build

```bash
python3 -m venv .venv && .venv/bin/pip install meson ninja
.venv/bin/meson setup build          # fetches the Cobra port at the pinned release tag (subprojects/pntos-cpp.wrap)
.venv/bin/meson compile -C build
.venv/bin/meson test -C build
./build/pnt-edge configs/pnt-edge-replay.json --once --print-status   # 60 s replay, NMEA to 127.0.0.1:10110
```

`meson install` installs `pnt-edge`, `/etc/pnt-edge/pnt-edge.json` and the systemd unit.

## Configuration

`configs/pnt-edge.json` points at a Cobra filter config (`filter_config`, any file from the Cobra `configs/`
directory or your own), names the input (`lcm_log` or `lcm_udp`), the NMEA targets and the status file. Paths
are relative to the config file. The filter's own sensors, lever arms, presets and processors are described
in the Cobra config; `docs/GETTING_STARTED.md` of the Cobra repository explains that format.

## Layout

```
src/        nmea (sentences), inputs (LCM log, LCM UDP), outputs (UDP/TCP NMEA), status (health JSON), edge_config, main
configs/    appliance configs (field unit, bench replay)
packaging/  systemd unit
docs/       DESIGN.md, ROADMAP.md, COMMAND_PROTOCOL.md
tests/      GoogleTest (NMEA formatting, config, end-to-end replay)
```
