# `$PPNT` command protocol (tactical interface)

Proprietary NMEA 0183 sentences, talker `P`, mnemonic `PNT`. Commands arrive on the same TCP port that serves
NMEA output (and on a UDP command port); every command is answered with an acknowledgement sentence on the
connection it arrived on. Standard NMEA framing: `$`, comma-separated fields, `*hh` checksum, CRLF, at most 82
characters per sentence. Fields never contain commas; empty fields mean "unchanged" in SET commands.

## Sentences

| Sentence | Direction | Meaning |
|---|---|---|
| `$PPNT,STATUS,Q*hh` | in | query health |
| `$PPNT,STATUS,R,<health>,<age_s>,<fix>,<sats>,<sigma_h_m>,<sigma_v_m>,<sigma_hdg_deg>,<uptime_s>*hh` | out | health reply; `health` is one of `ok`, `aligning`, `stale`, `error` |
| `$PPNT,VER,Q*hh` / `$PPNT,VER,R,<pnt-edge>,<cobra>,<config_id>*hh` | in / out | versions and the SHA-256 prefix of the active config |
| `$PPNT,OUT,S,<rate_hz>,<sentence list>*hh` | in | set NMEA rate and sentence selection, e.g. `$PPNT,OUT,S,5,GGA+HDT+GST` |
| `$PPNT,LEVERARM,S,<label>,<x>,<y>,<z>*hh` | in | set a lever arm (platform frame, metres) of the named processor; takes effect on the next filter restart |
| `$PPNT,HEADING,S,<deg>,<sigma_deg>*hh` | in | provide an initial heading for manual-heading alignment |
| `$PPNT,RESET,S,<what>*hh` | in | `filter` re-aligns from scratch, `app` restarts the service |
| `$PPNT,TIME,Q*hh` / `$PPNT,TIME,R,<utc>,<source>,<offset_ms>*hh` | in / out | time status (source: gnss, ptp, ntp, free) |
| `$PPNT,ACK,<command>,<result>[,<message>]*hh` | out | acknowledgement: result `OK`, `ERR` or `BUSY` |

Sentence fields: `Q` query, `S` set, `R` reply. Unknown commands get `$PPNT,ACK,<command>,ERR,unknown`.
Commands that change configuration are logged in the maintenance audit log with the source address.

## Why `$P`

There is no vendor-neutral command standard for PNT devices; what integrators already drive is NMEA both ways
with proprietary `$P` sentences, next to vendor ASCII dialects. `$PPNT` keeps the parser every tactical client
already has, stays within 82 characters, and leaves the full interface (ASPN-23 over LCM) for integrators who
want more than NMEA carries.

## Phase A status

The output half (GGA, RMC, VTG, HDT, GST, ZDA, PASHR) is implemented. The command parser, acknowledgement and
the STATUS / VER / OUT / RESET handlers are Phase A.3; LEVERARM and HEADING need the config write-back of Phase B.
