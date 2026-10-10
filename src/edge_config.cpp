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
    if (c.input.type != "lcm_log" && c.input.type != "lcm_udp") throw std::runtime_error("pnt-edge config: input.type must be lcm_log or lcm_udp");
  }
  if (j.contains("nmea")) {
    const json& n = j.at("nmea");
    c.nmea.udp_targets = n.value("udp_targets", std::vector<std::string>{});
    c.nmea.tcp_port = n.value("tcp_port", 0);
    c.nmea.bind_address = n.value("bind_address", "0.0.0.0");
    c.nmea.talker = n.value("talker", "GN");
    c.nmea.rate_hz = n.value("rate_hz", 1.0);
    c.nmea.utc_offset_sec = n.value("utc_offset_sec", 0.0);
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
  return json{{"filter_config", c.filter_config},
              {"legacy_q_rotation", c.legacy_q_rotation},
              {"logging_level", c.logging_level},
              {"geoid_file", c.geoid_file},
              {"input", {{"type", c.input.type}, {"path", c.input.path}, {"speed", c.input.speed}, {"loop", c.input.loop}, {"url", c.input.url},
                         {"subscribe_to", c.input.subscribe_to}, {"channels", c.input.channels}}},
              {"nmea", {{"udp_targets", c.nmea.udp_targets}, {"tcp_port", c.nmea.tcp_port}, {"bind_address", c.nmea.bind_address},
                        {"talker", c.nmea.talker}, {"rate_hz", c.nmea.rate_hz}, {"utc_offset_sec", c.nmea.utc_offset_sec}, {"sentences", c.nmea.sentences}}},
              {"status", {{"file", c.status.file}, {"interval_sec", c.status.interval_sec}}},
              {"web", {{"tactical", {{"bind", c.web.tactical.bind}, {"port", c.web.tactical.port}}},
                       {"maintenance", {{"bind", c.web.maintenance.bind}, {"port", c.web.maintenance.port}}},
                       {"www", c.web.www}, {"users_file", c.web.users_file}, {"api_token", c.web.api_token}, {"audit_file", c.web.audit_file},
                       {"history_seconds", c.web.history_seconds}, {"log_lines", c.web.log_lines}}}};
}

}  // namespace edge
