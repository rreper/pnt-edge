#include "edge_config.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace edge {

using nlohmann::json;

namespace {
std::string resolve(const std::string& base, const std::string& p) {
  if (p.empty() || base.empty() || std::filesystem::path(p).is_absolute()) return p;
  return (std::filesystem::path(base) / p).lexically_normal().string();
}
}  // namespace

EdgeConfig edge_config_from_json(const json& j, const std::string& base_dir) {
  if (!j.is_object()) throw std::runtime_error("pnt-edge config: top level must be an object");
  EdgeConfig c;
  c.filter_config = resolve(base_dir, j.value("filter_config", ""));
  if (c.filter_config.empty()) throw std::runtime_error("pnt-edge config: \"filter_config\" is required");
  c.legacy_q_rotation = j.value("legacy_q_rotation", false);
  c.logging_level = j.value("logging_level", "INFO");
  c.solution_watchdog_s = j.value("solution_watchdog_s", 20.0);
  c.geoid_file = resolve(base_dir, j.value("geoid_file", ""));
  if (j.contains("input")) {
    const json& i = j.at("input");
    c.input.type = i.value("type", "lcm_log");
    c.input.path = resolve(base_dir, i.value("path", ""));
    c.input.speed = i.value("speed", 0.0);
    c.input.loop = i.value("loop", false);
    c.input.url = i.value("url", c.input.url);
    c.input.subscribe_to = i.value("subscribe_to", c.input.subscribe_to);
    c.input.channels = i.value("channels", std::vector<std::string>{});
    c.input.record_dir = resolve(base_dir, i.value("record_dir", ""));
    c.input.reorder_ms = i.value("reorder_ms", 50.0);
    c.input.reorder_max_ms = i.value("reorder_max_ms", 1000.0);
    for (const json& d : i.value("devices", json::array())) {
      SerialDeviceConfig s;
      s.driver = d.value("driver", "");
      s.name = d.value("name", s.driver);
      s.port = d.value("port", "");
      s.baud = d.value("baud", 115200);
      s.channel = d.value("channel", "");
      s.velocity_channel = d.value("velocity_channel", "");
      s.time_source = d.value("time_source", "host");
      s.latency_ms = d.value("latency_ms", 0.0);
      s.init = d.value("init", std::vector<std::string>{});
      s.configure = d.value("configure", false);
      s.rate_hz = d.value("rate_hz", 5.0);
      s.ubx_port = d.value("ubx_port", "");
      s.uere_m = d.value("uere_m", 2.5);
      s.sigma_floor_m = d.value("sigma_floor_m", 0.3);
      if (s.driver != "vn100" && s.driver != "ubx" && s.driver != "nmea") throw std::runtime_error("pnt-edge config: device driver must be vn100, ubx or nmea");
      if (s.port.empty() || s.channel.empty()) throw std::runtime_error("pnt-edge config: every serial device needs a port and a channel");
      if (s.time_source != "host" && s.time_source != "device") throw std::runtime_error("pnt-edge config: time_source must be host or device");
      c.input.devices.push_back(std::move(s));
    }
    if (c.input.type != "lcm_log" && c.input.type != "lcm_udp" && c.input.type != "serial")
      throw std::runtime_error("pnt-edge config: input.type must be lcm_log, lcm_udp or serial");
    if (c.input.type == "serial" && c.input.devices.empty()) throw std::runtime_error("pnt-edge config: a serial input needs at least one device");
  }
  if (j.contains("nmea")) {
    const json& n = j.at("nmea");
    c.nmea.udp_targets = n.value("udp_targets", std::vector<std::string>{});
    c.nmea.tcp_port = n.value("tcp_port", 0);
    c.nmea.command_udp_port = n.value("command_udp_port", 10111);
    c.nmea.bind_address = n.value("bind_address", "0.0.0.0");
    c.nmea.talker = n.value("talker", "GN");
    c.nmea.rate_hz = n.value("rate_hz", 1.0);
    c.nmea.utc_offset_sec = n.value("utc_offset_sec", 0.0);
    c.nmea.utc_from_receiver = n.value("utc_from_receiver", true);
    c.nmea.sentences = n.value("sentences", c.nmea.sentences);
  }
  if (j.contains("status")) {
    const json& s = j.at("status");
    c.status.file = s.value("file", c.status.file);
    c.status.interval_sec = s.value("interval_sec", 1.0);
  }
  if (j.contains("web")) {
    const json& w = j.at("web");
    auto surface = [&](const char* key, WebSurfaceConfig& out) {
      if (!w.contains(key)) return;
      const json& x = w.at(key);
      if (x.is_boolean()) {
        if (!x.get<bool>()) out.port = 0;
        return;
      }
      out.bind = x.value("bind", out.bind);
      out.port = x.value("port", out.port);
    };
    surface("tactical", c.web.tactical);
    surface("maintenance", c.web.maintenance);
    c.web.www = resolve(base_dir, w.value("www", ""));
    c.web.users_file = resolve(base_dir, w.value("users_file", c.web.users_file));
    c.web.api_token = w.value("api_token", "");
    c.web.audit_file = resolve(base_dir, w.value("audit_file", c.web.audit_file));
    c.web.update_dir = resolve(base_dir, w.value("update_dir", c.web.update_dir));
    c.web.history_seconds = w.value("history_seconds", c.web.history_seconds);
    c.web.log_lines = w.value("log_lines", c.web.log_lines);
  }
  return c;
}

void save_edge_config(const EdgeConfig& c, const std::string& path) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp);
    if (!out) throw std::runtime_error("cannot write " + tmp);
    out << edge_config_to_json(c).dump(2) << "\n";
  }
  std::filesystem::rename(tmp, path);
}

EdgeConfig load_edge_config(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open " + path);
  json j;
  try {
    j = json::parse(in, nullptr, true, true);
  } catch (const std::exception& e) {
    throw std::runtime_error(path + ": " + e.what());
  }
  auto c = edge_config_from_json(j, std::filesystem::path(path).parent_path().string());
  c.path = path;
  return c;
}

json edge_config_to_json(const EdgeConfig& c) {
  json devices = json::array();
  for (const auto& d : c.input.devices)
    devices.push_back({{"name", d.name}, {"driver", d.driver}, {"port", d.port}, {"baud", d.baud}, {"channel", d.channel}, {"velocity_channel", d.velocity_channel},
                       {"time_source", d.time_source}, {"latency_ms", d.latency_ms}, {"init", d.init}, {"configure", d.configure}, {"rate_hz", d.rate_hz},
                       {"ubx_port", d.ubx_port}, {"uere_m", d.uere_m}, {"sigma_floor_m", d.sigma_floor_m}});
  return json{{"filter_config", c.filter_config},
              {"legacy_q_rotation", c.legacy_q_rotation},
              {"logging_level", c.logging_level},
              {"solution_watchdog_s", c.solution_watchdog_s},
              {"geoid_file", c.geoid_file},
              {"input", {{"type", c.input.type}, {"path", c.input.path}, {"speed", c.input.speed}, {"loop", c.input.loop}, {"url", c.input.url},
                         {"subscribe_to", c.input.subscribe_to}, {"channels", c.input.channels}, {"devices", devices}, {"record_dir", c.input.record_dir},
                         {"reorder_ms", c.input.reorder_ms}, {"reorder_max_ms", c.input.reorder_max_ms}}},
              {"nmea", {{"udp_targets", c.nmea.udp_targets}, {"tcp_port", c.nmea.tcp_port}, {"command_udp_port", c.nmea.command_udp_port}, {"bind_address", c.nmea.bind_address},
                        {"talker", c.nmea.talker}, {"rate_hz", c.nmea.rate_hz}, {"utc_offset_sec", c.nmea.utc_offset_sec}, {"utc_from_receiver", c.nmea.utc_from_receiver},
                        {"sentences", c.nmea.sentences}}},
              {"status", {{"file", c.status.file}, {"interval_sec", c.status.interval_sec}}},
              {"web", {{"tactical", {{"bind", c.web.tactical.bind}, {"port", c.web.tactical.port}}},
                       {"maintenance", {{"bind", c.web.maintenance.bind}, {"port", c.web.maintenance.port}}},
                       {"www", c.web.www}, {"users_file", c.web.users_file}, {"api_token", c.web.api_token}, {"audit_file", c.web.audit_file}, {"update_dir", c.web.update_dir},
                       {"history_seconds", c.web.history_seconds}, {"log_lines", c.web.log_lines}}}};
}

}  // namespace edge
