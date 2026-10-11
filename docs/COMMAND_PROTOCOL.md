# `$PPNT` command protocol (tactical interface)

Proprietary NMEA 0183 sentences, talker `P`, mnemonic `PNT`. Commands arrive on the same TCP port that serves
NMEA output (and on a UDP command port); every command is answered with an acknowledgement sentence on the
connection it arrived on. Standard NMEA framing: `$`, comma-separated fields, `*hh` checksum, CRLF, at most 82
characters per sentence. Fields never contain commas; empty fields mean "unchanged" in SET commands.

## Sentences

| Sentence | Direction | Meaning |
|---|---|---|
| `$PPNT,STATUS,Q*hh` | in | query health |
| `$PPNT,STATUS,R,<health>,<age_s>,<fix>,<sats>,<sigma_h_m>,<sigma_v_m>,<sigma_hdg_deg>,<uptime_s>*hh` | out | health reply; `health` is one of `ok`, `aligning`, `stale`, `error`, or with an integrity plugin `alternate`, `contingency`, `emergency` |
| `$PPNT,INTEG,<pace>,<hpl_m>,<hal_m>,<alarm>,<fault_detection>,<excluded>*hh` | out | integrity status, once per NMEA epoch when an integrity plugin is linked: PACE state (`PRIMARY`, `ALTERNATE`, `CONTINGENCY`, `EMERGENCY`), horizontal protection level and alert limit in metres, `alarm` 1 while a source disagreement is being evaluated, `fault_detection` 1 while two or more trusted sources allow one, excluded source names joined by `+` |
| `$PPNT,VER,Q*hh` / `$PPNT,VER,R,<pnt-edge>,<cobra>,<config_id>*hh` | in / out | versions and the SHA-256 prefix of the active config |
| `$PPNT,OUT,S,<rate_hz>,<sentence list>*hh` | in | set NMEA rate and sentence selection, e.g. `$PPNT,OUT,S,5,GGA+HDT+GST` |
| `$PPNT,LEVERARM,S,<label>,<x>,<y>,<z>*hh` | in | set a lever arm (platform frame, metres) of the named processor; takes effect on the next filter restart |
| `$PPNT,HEADING,S,<deg>,<sigma_deg>*hh` | in | provide an initial heading for manual-heading alignment |
| `$PPNT,RESET,S,<what>*hh` | in | `filter` re-aligns from scratch, `app` restarts the service (the same two actions as the web surfaces' reset, see docs/WEB_UI.md) |
| `$PPNT,TIME,Q*hh` / `$PPNT,TIME,R,<utc>,<source>,<offset_ms>*hh` | in / out | time status (source: gnss, ptp, ntp, free) |
| `$PPNT,ACK,<command>,<result>[,<message>]*hh` | out | acknowledgement: result `OK`, `ERR` or `BUSY` |

The GGA fix quality follows the PACE state when an integrity plugin is linked: `PRIMARY` keeps 1 (GPS),
`ALTERNATE` and `CONTINGENCY` give 6 (estimated: non-GNSS aiding or dead reckoning), `EMERGENCY` gives 0
(invalid) and RMC reports `V`. Without the plugin the quality is 1 whenever a solution exists.

Sentence fields: `Q` query, `S` set, `R` reply. Unknown commands get `$PPNT,ACK,<command>,ERR,unknown`.
Commands that change configuration are logged in the maintenance audit log with the source address.

## Why `$P`

There is no vendor-neutral command standard for PNT devices; what integrators already drive is NMEA both ways
with proprietary `$P` sentences, next to vendor ASCII dialects. `$PPNT` keeps the parser every tactical client
already has, stays within 82 characters, and leaves the full interface (ASPN-23 over LCM) for integrators who
want more than NMEA carries.

## Transport and configuration

Commands are read on two paths, both on the NMEA bind address (`nmea.bind_address`, the tactical interface):

- the NMEA TCP server (`nmea.tcp_port`): any connected client may write `$PPNT` lines on the same connection
  it receives the stream on; replies go to that client only, interleaved with the stream;
- a UDP command port (`nmea.command_udp_port`, default 10111, 0 = off): one or more sentences per datagram,
  replies by datagram to the sender.

Every sentence is acknowledged on the path it arrived on. Queries (`STATUS`, `VER`, `TIME`) answer from the
daemon's live state; sets are queued to the main loop exactly like the web surfaces' control calls
(docs/WEB_UI.md) and written to the audit log as `ppnt <ip:port> <action> <args>`. `OUT` takes effect
immediately and is saved to the config file; `HEADING` and `LEVERARM` are written to the filter config and take
effect on the next filter reset (the ACK says so); `RESET,S,filter` re-aligns, `RESET,S,app` exits with code 3
for the service manager to restart.

Reply formats: `STATUS,R` carries `health`, the solution age in seconds, the GGA fix quality (0/1/6 as above),
an empty satellite field (not known to the filter), horizontal and vertical 1-sigma in metres, heading sigma in
degrees, uptime in seconds. `TIME,R` carries the last solution's UTC as ISO 8601, the source (`gnss` while
solutions flow, else `free`) and the wall-clock minus solution-time offset in milliseconds. `VER,R` carries the
pnt-edge version, the Cobra library version and the first eight hex digits of the SHA-256 of the active filter
config file.

Example session over TCP (`nc <device> 10110`):

```
$PPNT,STATUS,Q*0F
$PPNT,STATUS,R,ok,0.2,1,,1.41,2.00,0.50,3617*5B
$PPNT,ACK,STATUS,OK*6F
$PPNT,OUT,S,5,GGA+HDT+GST*2E
$PPNT,ACK,OUT,OK*35
$PPNT,HEADING,S,123.4,1*1C
$PPNT,ACK,HEADING,OK,effective on the next filter reset*2A
$PPNT,RESET,S,filter*1F
$PPNT,ACK,RESET,OK*2E
```

A malformed line is answered with `$PPNT,ACK,<command or ?>,ERR,<reason>` (`bad checksum`, `too long`,
`missing checksum`, ...). Sentences of kind `R` and `ACK` sentences are never answered, so two devices wired
back to back cannot loop.

## Phase A status

Delivered: the output half (GGA, RMC, VTG, HDT, GST, ZDA, PASHR, INTEG) and the command half (parser,
acknowledgements, STATUS / VER / TIME / OUT / LEVERARM / HEADING / RESET) on both transports, sharing the
command layer and audit log with the web surfaces. Open: serial drivers (A.2) and a `$PPNT,SRC` family for
enabling and disabling individual aiding sources from the tactical side.
