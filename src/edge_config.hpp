// pnt-edge daemon configuration (JSON). The filter itself is configured by a Cobra config file
// (`filter_config`), everything around it here: where measurements come from, where NMEA goes, status output.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace edge {

/// One serial device of a "serial" input (docs/SERIAL.md).
struct SerialDeviceConfig {
  std::string name;                  ///< label in the status (default: the driver name)
  std::string driver;                ///< vn100 | ubx | nmea
  std::string port;                  ///< /dev/ttyUSB0, /dev/ttyACM0, /dev/ttyTHS1 ... (a pty for the simulator)
  int baud = 115200;
  std::string channel;               ///< ASPN channel the filter config expects (IMU or position)
  std::string velocity_channel;      ///< GNSS: also push NED velocity on this channel (empty = no)
  std::string time_source = "host";  ///< host: device clock mapped onto the host clock; device: the receiver's UTC as is
  double latency_ms = 0;             ///< fixed transport latency subtracted from the stamped time
  std::vector<std::string> init;     ///< sent on open: vn100 ASCII commands (checksum added), ubx hex frames, nmea lines
  bool configure = false;            ///< ubx: send CFG-VALSET (UBX only, NAV-PVT + NAV-TIMELS, rate_hz) on open
  double rate_hz = 5;                ///< ubx configure: navigation rate
  std::string ubx_port;              ///< ubx configure: uart1 | uart2 | usb (default from the device path)
  double uere_m = 2.5;               ///< nmea without GST: sigma_h = hdop * uere
  double sigma_floor_m = 0.3;        ///< horizontal position sigma floor (vertical 1.5x)
};

struct InputConfig {
  std::string type = "lcm_log";              ///< lcm_log | lcm_udp | serial
  std::string path;                          ///< lcm_log: the log file
  double speed = 0;                          ///< lcm_log: 0 = as fast as possible, 1 = real time
  bool loop = false;                         ///< lcm_log: at the end, restart filter and replay from the top (demo rigs)
  std::string url = "udpm://239.255.76.67:7667?ttl=0";  ///< lcm_udp
  std::string subscribe_to = "^/sensor/";    ///< lcm_udp: channel regex
  std::vector<std::string> channels;         ///< lcm_log: channels to push (empty = the filter config's transport list, else all)
  std::vector<SerialDeviceConfig> devices;   ///< serial
  std::string record_dir;                    ///< serial: record the raw bytes of every device here (empty = off)
  double reorder_ms = 50;                    ///< serial: messages are held this long so devices are pushed in time order
};

struct NmeaConfig {
  std::vector<std::string> udp_targets;  ///< "host:port"
  int tcp_port = 0;                      ///< 0 = no TCP server (commands are also read on it)
  int command_udp_port = 10111;          ///< $PPNT commands by UDP datagram, replies to the sender; 0 = off
  std::string bind_address = "0.0.0.0";  ///< address for the TCP server and the command port (the one Ethernet port: 0.0.0.0)
  std::string talker = "GN";
  double rate_hz = 1.0;                  ///< at most this many epochs per second are emitted
  double utc_offset_sec = 0;             ///< added to the solution time (e.g. -18 for GPS time logs)
  bool utc_from_receiver = true;         ///< serial input: use the receiver's UTC to correct the host-stamped solution time instead
  std::vector<std::string> sentences{"GGA", "RMC", "VTG", "HDT", "GST", "ZDA", "PASHR"};
};

struct StatusConfig {
  std::string file = "/run/pnt-edge/status.json";  ///< rewritten every `interval_sec`; empty = off
  double interval_sec = 1.0;
};

/// One HTTP surface served by the daemon (see docs/WEB_UI.md).
struct WebSurfaceConfig {
  std::string bind = "0.0.0.0";
  int port = 0;                 ///< 0 = not served
};
struct WebConfig {
  WebSurfaceConfig tactical{"0.0.0.0", 8080};     ///< no login: the tactical display with its reset button
  WebSurfaceConfig maintenance{"0.0.0.0", 8081};  ///< login (users_file) or bearer api_token (read-only) for a fleet server
  std::string www;                                ///< directory with tactical/, maintenance/, common/ (empty: the installed one)
  std::string users_file = "/etc/pnt-edge/users.json";
  std::string api_token;                          ///< empty: no token access
  std::string audit_file = "/var/log/pnt-edge/audit.log";
  std::string update_dir = "/var/lib/pnt-edge/updates";  ///< staged packages, backup of the replaced version, history
  double history_seconds = 7200;                  ///< solution history kept in memory
  int log_lines = 2000;                           ///< log lines kept in memory
};

struct EdgeConfig {
  std::string filter_config;              ///< path to the Cobra JSON config
  bool legacy_q_rotation = false;
  std::string logging_level = "INFO";
  InputConfig input;
  NmeaConfig nmea;
  StatusConfig status;
  std::string geoid_file;                 ///< empty: PNTOS_GEOID_FILE or data/egm96_15min.bin
  WebConfig web;
  std::string path;                       ///< the file this config was loaded from (for saving)
};
/// Writes the config back to `path` (pretty JSON, comments dropped). Throws on failure.
void save_edge_config(const EdgeConfig& c, const std::string& path);

EdgeConfig load_edge_config(const std::string& path);
EdgeConfig edge_config_from_json(const nlohmann::json& j, const std::string& base_dir = "");
nlohmann::json edge_config_to_json(const EdgeConfig& c);

}  // namespace edge
