#include "status.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace edge {

using nlohmann::json;

json status_to_json(const StatusSnapshot& s) {
  json j;
  j["time_utc"] = std::chrono::duration_cast<std::chrono::milliseconds>(s.wall.time_since_epoch()).count() / 1000.0;
  j["uptime_sec"] = s.uptime_sec;
  j["input"] = {{"pushed", s.input.pushed}, {"decode_failures", s.input.decode_failures}, {"last_tov_s", s.input.last_tov_ns * 1e-9},
                {"per_channel", s.input.per_channel}};
  j["solutions"] = s.solutions;
  j["nmea_sentences"] = s.nmea_sentences;
  j["last_solution_age_sec"] = s.last_solution_age_sec;
  j["filter_error"] = s.filter_error;
  j["tcp_clients"] = s.tcp_clients;
  j["gating"] = s.gating;
  j["integrity"] = integrity_to_json(s.integrity);
  if (s.last_solution) {
    const Pva& p = *s.last_solution;
    j["last_solution"] = {{"tov_s", p.tov_ns * 1e-9}, {"lat_deg", p.lat_rad * 180 / M_PI}, {"lon_deg", p.lon_rad * 180 / M_PI},
                          {"alt_hae_m", p.alt_hae_m}, {"vel_ned_mps", {p.vn, p.ve, p.vd}},
                          {"rpy_deg", {p.roll_rad * 180 / M_PI, p.pitch_rad * 180 / M_PI, p.yaw_rad * 180 / M_PI}},
                          {"sigma_ned_m", {p.sigma_n, p.sigma_e, p.sigma_d}}, {"sigma_heading_deg", p.sigma_heading_rad * 180 / M_PI}};
  }
  // a one-word health verdict for the UI header
  std::string health = "starting";
  if (s.filter_error) health = "error";
  else if (s.last_solution_age_sec >= 0 && s.last_solution_age_sec < 5) health = "ok";
  else if (s.input.pushed > 0 && s.last_solution_age_sec < 0) health = "aligning";
  else if (s.last_solution_age_sec >= 5) health = "stale";
  if (s.integrity.present && health == "ok") {
    if (s.integrity.pace == "EMERGENCY") health = "emergency";
    else if (s.integrity.pace == "CONTINGENCY") health = "contingency";
    else if (s.integrity.pace == "ALTERNATE") health = "alternate";
  }
  j["health"] = health;
  return j;
}

json gating_from_registry(pntos::cobra::Filter& filter) {
  json out = json::object();
  try {
    auto& reg = filter.registry();
    if (!reg.has_group("fusion/gating")) return out;
    auto kv = reg.batch("fusion/gating");
    auto keys = kv->keys();
    if (!keys) return out;
    for (const auto& k : *keys) {
      if (auto v = kv->get_value<double>(k)) out[k] = *v;
    }
  } catch (const std::exception&) {
  }
  return out;
}

void write_status_file(const std::string& path, const json& j) {
  if (path.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp);
    if (!out) return;
    out << j.dump(2) << '\n';
  }
  std::rename(tmp.c_str(), path.c_str());
}

}  // namespace edge
