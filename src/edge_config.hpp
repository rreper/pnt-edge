// pnt-edge daemon configuration (JSON). The filter itself is configured by a Cobra config file
// (`filter_config`), everything around it here: where measurements come from, where NMEA goes, status output.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace edge {

struct InputConfig {
  std::string type = "lcm_log";              ///< lcm_log | lcm_udp
  std::string path;                          ///< lcm_log: the log file
  double speed = 0;                          ///< lcm_log: 0 = as fast as possible, 1 = real time
  bool loop = false;                         ///< lcm_log: restart at the end (demo rigs)
  std::string url = "udpm://239.255.76.67:7667?ttl=0";  ///< lcm_udp
  std::string subscribe_to = "^/sensor/";    ///< lcm_udp: channel regex
  std::vector<std::string> channels;         ///< lcm_log: channels to push (empty = the filter config's transport list, else all)
};

struct NmeaConfig {
  std::vector<std::string> udp_targets;  ///< "host:port"
  int tcp_port = 0;                      ///< 0 = no TCP server
  std::string bind_address = "0.0.0.0";  ///< interface for the TCP server (the tactical interface)
  std::string talker = "GN";
  double rate_hz = 1.0;                  ///< at most this many epochs per second are emitted
  double utc_offset_sec = 0;             ///< added to the solution time (e.g. -18 for GPS time logs)
  std::vector<std::string> sentences{"GGA", "RMC", "VTG", "HDT", "GST", "ZDA", "PASHR"};
};

struct StatusConfig {
  std::string file = "/run/pnt-edge/status.json";  ///< rewritten every `interval_sec`; empty = off
  double interval_sec = 1.0;
};

struct EdgeConfig {
  std::string filter_config;              ///< path to the Cobra JSON config
  bool legacy_q_rotation = false;
  std::string logging_level = "INFO";
  InputConfig input;
  NmeaConfig nmea;
  StatusConfig status;
  std::string geoid_file;                 ///< empty: PNTOS_GEOID_FILE or data/egm96_15min.bin
};

EdgeConfig load_edge_config(const std::string& path);
EdgeConfig edge_config_from_json(const nlohmann::json& j, const std::string& base_dir = "");
nlohmann::json edge_config_to_json(const EdgeConfig& c);

}  // namespace edge
