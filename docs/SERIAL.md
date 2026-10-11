# Serial devices (Phase A.2)

`"input": {"type": "serial"}` reads one or more devices on serial ports (or any character device, including the
pseudo-terminals of the simulator) and pushes their measurements into the filter. Sources: `src/serial/`.

## Drivers

| driver | device | frames understood | message pushed |
|---|---|---|---|
| `vn100` | VectorNav VN-100 (also VN-200/300 output groups) | binary output (sync `0xFA`, groups 1–6, CRC16): TimeStartup, TimeGps, DeltaThetaVel, Imu, Accel + AngularRate; ASCII `$VNDTV`, `$VNIMU`, `$VNYMR`, `$VNERR` | `MeasurementImu`, INTEGRATED (delta velocity m/s, delta angle rad) when the device sends DeltaThetaVel, else SAMPLED |
| `ubx` | u-blox (ZED-F9, NEO-M9 …) | UBX NAV-PVT (position, accuracy, velocity, UTC), NAV-TIMELS (leap seconds) | `MeasurementPosition` geodetic, sigma from hAcc / vAcc; optional `MeasurementVelocity` NED |
| `nmea` | any NMEA 0183 receiver | GGA / GNS (position), GST (sigmas), RMC (date, speed, course), ZDA (date) | `MeasurementPosition` geodetic, sigma from GST, else HDOP × UERE; optional velocity from RMC |

The filter config decides what is done with the messages (the IMU preset, lever arms, processors): the device's
`channel` must match the channel the filter config names (`/sensor/vn-100/imu`, `/sensor/ublox-ZED-F9T/position`
in Cobra's `pos_ins.json`). A velocity channel needs a velocity processor in that config; leave it empty otherwise.

## Device entry

```json
{"name": "imu", "driver": "vn100", "port": "/dev/ttyUSB0", "baud": 921600, "channel": "/sensor/vn-100/imu",
 "time_source": "host", "latency_ms": 0.5, "init": ["VNWRG,06,0", "VNWRG,75,1,4,01,0801"]}
```

| key | meaning |
|---|---|
| `port`, `baud` | the device file and rate (any rate: termios2 `BOTHER`); a pty ignores the rate |
| `channel`, `velocity_channel` | ASPN channels pushed (velocity only when set) |
| `time_source` | `host` (default): the device's own clock is mapped onto the host clock with the transport latency removed (below); `device`: the receiver's UTC is used as is (only when the host clock is disciplined to it, e.g. chrony from this receiver) |
| `latency_ms` | fixed latency subtracted from every stamp (USB-serial adapters: 0.5–2 ms) |
| `init` | sent once when the port opens: `vn100` ASCII register writes (checksum computed, `*XX` may be omitted); `ubx` raw hex frames `"B5 62 …"`; `nmea` plain lines |
| `configure`, `rate_hz`, `ubx_port` | `ubx` only: on open send CFG-VALSET (RAM layer) enabling UBX and disabling NMEA on `ubx_port` (`uart1` / `uart2` / `usb`, default by the device path), NAV-PVT and NAV-TIMELS every epoch, measurement rate `rate_hz` |
| `uere_m`, `sigma_floor_m` | `nmea`: sigma = HDOP × UERE without GST (RTK fixed 0.02 m, float 0.5 m, DGPS 1 m); all GNSS: horizontal sigma floor, vertical 1.5× |

Input-level keys: `reorder_ms` (default 50: every message is held this long so the devices' messages reach the
filter in time order, since a late position would otherwise be applied at the filter's current time) and
`record_dir` (raw bytes of every device appended to `<dir>/<name>_<UTC time>.raw` for replay through the parsers).

### VN-100 register writes used in `configs/pnt-edge-serial.json`

- `VNWRG,06,0` — stop the ASCII async output.
- `VNWRG,75,1,4,01,0801` — binary output 1 on serial port 1, rate divisor 4 (800 Hz / 4 = 200 Hz), group 1 fields
  `0x0801` = TimeStartup + DeltaThetaVel. Add `0x0200` for the raw Imu field, `0x0002` for TimeGps on a VN-200/300.
  Use port 2 (`VNWRG,75,2,…`) when the device is wired through its second UART. `VNWRG,05,921600` changes the baud
  rate (then re-open at the new rate); `VNWNV` saves to non-volatile memory.

The VN-100 reports delta angles in degrees and delta velocity in m/s; the driver converts to radians. Both are in
the sensor body frame; the mounting rotation is the filter config's `ImuRotatorConfig`, as for the recorded data.

### u-blox

`configure: true` is enough for a ZED-F9 on USB or UART1. The generated frames are logged at INFO on open. The
receiver's UTC (NAV-PVT with `validDate` + `validTime`) feeds the time mapping, the leap-second count from
NAV-TIMELS appears in the status. Fix quality for GGA follows NAV-PVT: RTK fixed 4, float 5, DGPS 2, dead reckoning
6, no fix → nothing is pushed.

## Timing

Everything the filter sees must be on one timeline. By default that is the host's `CLOCK_REALTIME`:

- A device clock (VN-100 TimeStartup, GNSS UTC or time of week) is mapped with `offset = host − device`, tracked
  by its running minimum (the samples with the least transport latency) and followed with a bounded slew
  (fast during the first 10 s, then 1 ms/s), so stamps never run backwards. A jump of more than 1 s restarts the
  mapping (device power cycle, week rollover). A device without any clock is stamped at arrival.
- The GNSS driver also measures `host − UTC`. With `nmea.utc_from_receiver` (default true) the NMEA output uses it
  to print true UTC even when the host clock is off (`clock.utc_correction_s` in the status).
- `time_source: "device"` for the GNSS uses the receiver's UTC directly; use it only when the host clock is
  synchronised to the receiver, otherwise the IMU (host-stamped) and the GNSS would sit on different timelines.

Message times reach the filter in order through the reorder buffer (`reorder_ms`).

## Health

The status (`input.devices[]`, maintenance UI → Inputs → Serial devices) shows per device: `state` (`opening`,
`open`, `streaming`, `no data` after 5 s of silence, `lost` when the port vanished, `closed` with the open error),
byte / frame / error counters, the frame rate, the age of the last frame and the clock offset. An unplugged device
is reopened every second; a device that reconnects resumes without a restart. The `$VNERR` code of a rejected
VN-100 command appears in `detail`.

## Without hardware: the simulator

`pnt-edge-serial-sim` plays an LCM log as two devices on pseudo-terminals:

```
./build/pnt-edge-serial-sim subprojects/pntos-cpp/testdata/example_60s.log --loop \
    --imu-link /tmp/pnt-edge/imu --gnss-link /tmp/pnt-edge/gnss
./build/pnt-edge configs/pnt-edge-serial-sim.json
```

Options: `--imu binary|ascii`, `--gnss ubx|nmea`, `--speed` (0 = as fast as possible), `--utc-offset` (seconds added
to the log's times), `--imu-channel` / `--gnss-channel` (which log channels to play). The VN-100 frames carry
TimeStartup and TimeGps, the UBX frames a full UTC date, so both `host` and `device` time sources can be tried. The
symlinks make the config independent of the pty numbers.

The test suite drives the same path (`tests/test_serial.cpp`): parsers and encoders, the clock mapping, the serial
input end to end over ptys with the 60 s log at 20× (every frame pushed, the filter aligns and publishes).

## Bench checklist for a new installation

1. `ls -l /dev/serial/by-id/` and use those stable names in the config instead of `ttyUSB0`.
2. Start pnt-edge, open the maintenance UI → Inputs: both devices `streaming`, IMU rate ≈ 200 Hz, GNSS at the
   configured rate, errors 0, clock offsets steady.
3. Run `tools/imu_allan.py` of the Cobra repository on a 30-minute static recording (`record_dir`, then replay
   through the sim or the LCM path) and the private tuning procedure to derive the IMU preset for this unit.
4. Set the lever arms and the mounting rotation in the filter config (maintenance UI → Config, or `$PPNT,LEVERARM`).

Not in this phase: ASPN-23 over LCM output, SPI IMUs, serial-port NMEA output. See docs/ROADMAP.md.
