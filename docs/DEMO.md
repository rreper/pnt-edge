# Running pnt-edge by hand and demoing the web interface

Everything below runs from the repository root on the build machine; the same commands work on the appliance
with the installed paths (`/usr/bin/pnt-edge`, `/etc/pnt-edge/pnt-edge.json`, `/usr/local/share/pnt-edge/www`).

## 1. Build

Build exactly as in the README, with the meson installed in the repository's venv (the system meson may be too
old; a build directory configured by one meson version is not accepted by an older one):

```
python3 -m venv .venv && .venv/bin/pip install meson ninja     # once
.venv/bin/meson setup build && .venv/bin/meson compile -C build && .venv/bin/meson test -C build
```

If you already built from the README, `build/pnt-edge` exists and this step is done. For the integrity variant,
with the private plugin symlinked at `subprojects/cobra-integrity`:

```
.venv/bin/meson setup build-integ -Dintegrity=enabled && .venv/bin/meson compile -C build-integ
```

## 2. Set the maintenance password (once)

```
PNT_EDGE_PASSWORD='choose a long one' ./build/pnt-edge configs/pnt-edge-demo.json --set-password admin
```

It writes `/tmp/pnt-edge/users.json` for the demo config (`/etc/pnt-edge/users.json` for the installed one).

## 3. Run

```
./build/pnt-edge configs/pnt-edge-demo.json
```

replays the 60 s example log at real time and loops it, serving on every interface:

| what | where |
|---|---|
| tactical display | http://\<host\>:8080/ (no login) |
| maintenance UI | http://\<host\>:8081/ (login) |
| NMEA stream + `$PPNT` commands | TCP \<host\>:10110 |
| `$PPNT` commands by datagram | UDP \<host\>:10111 |
| status snapshot | /tmp/pnt-edge/status.json, audit log /tmp/pnt-edge/audit.log |

For a long demo with driving dynamics use the full 43-minute recording instead of the 60 s loop:

```
./build/pnt-edge configs/pnt-edge-demo.json --input-log <path>/cobra_gps_ins_example_data.log
```

The recording (474 MB) ships in a Python package on PyPI, so no git access is needed to get it:

```
.venv/bin/pip download --no-deps -d /tmp/ds pntos-python-datasets-lcm
cd /tmp/ds && unzip -o pntos_python_datasets_lcm-*.whl 'pntos_python_datasets_lcm/*.log'
```

The 60 s example log is a stationary segment (the filter aligns and settles; position and heading do not move), so
use the full recording whenever the track should visibly drive.

With the integrity build, `./build-integ/pnt-edge configs/pnt-edge-demo-protected.json` runs the protected
two-source filter instead: the tactical display then shows PACE, HPL and excluded sources, `$PPNT,INTEG` is in
the stream and the Integrity tab fills. To show a spoofing event live, replace the filter config path in that
file by one of the private repository's scenario configs (`configs/scenarios/ramp05.json` pulls GNSS at 0.5 m/s
from 600 s of the full recording; `cell_step.json` steps the alternate by 40 m) and use the full log.

## 4. Demo walk-through

1. Open the tactical display. Point out the health badge, the position and heading readouts, the track with its
   1-sigma ellipse, and the Sources panel counting messages per channel.
2. Press **Reset filter**. The health badge goes to `aligning` for about ten seconds (static alignment) and back
   to `ok`; the solution age counts up meanwhile. The reset lands in the audit log with the caller's address.
3. Open the maintenance UI, sign in. Overview: health, position, integrity, recent events (the reset is there).
   Inputs: per-channel rates and the gating counters. Solution: the track with follow mode, the horizontal sigma
   and HPL plot, speed and heading. Log: the filter's own lines (alignment, gate rejections) with a level filter.
4. Outputs tab: change the NMEA rate to 2 Hz and the sentences to `GGA RMC GST INTEG`, Apply. Watch the stream in
   a second terminal: `nc <host> 10110`. The change is saved to the config file.
5. In that terminal, type a command on the same connection and watch the acknowledgement arrive between the
   sentences (checksums are required; these are correct):
   ```
   $PPNT,STATUS,Q*5F
   $PPNT,VER,Q*0A
   $PPNT,OUT,S,5,GGA+HDT*00
   $PPNT,RESET,S,filter*30
   ```
6. Software tab: upload a release package (`pnt-edge-<tag>-linux-x86_64.tar.gz` and its `.sha256` from the Releases
   page). It is staged and checked, nothing changes yet. **Apply and restart** installs it, the application
   re-executes itself and the page asks you to sign in again; the Overview then shows the new version. **Roll back**
   brings the previous version back the same way. (In the development layout, running `build/pnt-edge`, only the
   executable and the web files are replaced, and a package whose libraries are not installed is refused with the
   reason; the installed layout of the binary tarball or `meson install` takes everything.)
   or by datagram: `printf '$PPNT,VER,Q*0A\r\n' | nc -u -w1 <host> 10111`.
6. Config tab: the live pnt-edge config (editable, saved on Save) and the filter config (read-only).
7. Fleet access: `curl -H 'Authorization: Bearer <token>' http://<host>:8081/api/v1/solution` once `web.api_token`
   is set in the config; control calls with the token answer 403.

## 5. Serial devices without hardware

The same demo through the serial drivers (VN-100 binary frames and u-blox NAV-PVT on two pseudo-terminals):

```
./build/pnt-edge-serial-sim subprojects/pntos-cpp/testdata/example_60s.log --loop --imu-link /tmp/pnt-edge/imu --gnss-link /tmp/pnt-edge/gnss
./build/pnt-edge configs/pnt-edge-serial-sim.json
```

Maintenance UI → Inputs shows the two devices streaming (≈100 Hz and 1 Hz for this log), their clock offsets and
error counters; unplugging is simulated by stopping the simulator (state `lost`, then `closed` until it is back).
Real devices: `configs/pnt-edge-serial.json` and docs/SERIAL.md.

Note on looping: a looped `lcm_log` input restarts the filter at the end of each pass (event `REPLAY`, about ten
seconds of `aligning`), because the filter cannot follow time jumping back to the start of the recording. The serial
simulator instead keeps time continuous across passes (TimeStartup keeps growing, the receiver's UTC never steps back),
so the filter runs through without a restart; the position jumps back to the start of the recording at each wrap. For
a continuous long demo use the full recording with `--input-log`.

## 6. Useful checks

- `curl -s http://<host>:8080/api/v1/status | jq .health`
- `curl -N http://<host>:8080/api/v1/stream` shows the live updates (Server-Sent Events).
- `journalctl -u pnt-edge -f` on the appliance; `tail -f /tmp/pnt-edge/audit.log` for the audit trail.
