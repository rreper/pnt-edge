#include "integrity.hpp"

#include "nmea.hpp"

#include <cstdio>

namespace edge {

using nlohmann::json;

IntegrityStatus integrity_from_registry(pntos::cobra::Filter& filter) {
  IntegrityStatus s;
  try {
    auto& reg = filter.registry();
    if (!reg.has_group("integrity/status")) return s;
    auto kv = reg.batch("integrity/status");
    auto keys = kv->keys();
    if (!keys) return s;
    s.present = true;
    for (const auto& k : *keys) {
      auto v = kv->get(k);
      if (!v) continue;
      if (auto d = std::get_if<double>(&*v)) s.raw[k] = *d;
      else if (auto b = std::get_if<bool>(&*v)) s.raw[k] = *b;
      else if (auto i = std::get_if<std::int64_t>(&*v)) s.raw[k] = *i;
      else if (auto str = std::get_if<std::string>(&*v)) s.raw[k] = *str;
      else if (auto arr = std::get_if<pntos::api::StringArray>(&*v)) s.raw[k] = *arr;
    }
    s = integrity_from_raw(json(s.raw));
  } catch (const std::exception&) {
    s = IntegrityStatus{};
  }
  return s;
}

IntegrityStatus integrity_from_raw(const json& raw) {
  IntegrityStatus s;
  s.present = true;
  s.raw = raw;
  s.pace = raw.value("pace", std::string());
  s.hpl_m = raw.value("hpl_m", 0.0);
  s.hal_m = raw.value("hal_m", 0.0);
  s.alarm = raw.value("alarm", false);
  s.fault_detection = raw.value("fault_detection_available", false);
  s.time_s = raw.value("time_s", 0.0);
  if (raw.contains("excluded") && raw["excluded"].is_array()) s.excluded = raw["excluded"].get<std::vector<std::string>>();
  // cobra-integrity v0.2: the vertical when it is monitored, and per source why it is out and whether it is heard
  s.vertical = raw.contains("vpl_m") && raw.contains("val_m") && raw["vpl_m"].is_number() && raw["val_m"].is_number();
  if (s.vertical) {
    s.vpl_m = raw["vpl_m"].get<double>();
    s.val_m = raw["val_m"].get<double>();
  }
  for (const auto& name : s.excluded) {
    const auto it = raw.find("cause_" + name);
    s.causes.push_back(it != raw.end() && it->is_string() ? it->get<std::string>() : std::string());
  }
  for (auto it = raw.begin(); it != raw.end(); ++it)
    if (it.key().rfind("silent_", 0) == 0 && it->is_boolean() && it->get<bool>()) s.silent.push_back(it.key().substr(7));
  return s;
}

int fix_quality_for(const IntegrityStatus& s, int base) {
  if (!s.present) return base;
  if (s.pace == "EMERGENCY") return 0;
  if (s.pace == "ALTERNATE" || s.pace == "CONTINGENCY") return 6;
  return base;
}

std::string integ_sentence(const IntegrityStatus& s) {
  char hpl[32], hal[32];
  std::snprintf(hpl, sizeof hpl, "%.1f", s.hpl_m);
  std::snprintf(hal, sizeof hal, "%.1f", s.hal_m);
  auto joined = [](const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) out += (i ? "+" : "") + names[i];
    return out;
  };
  // the vertical is empty when it is not monitored: a zero would read as a level
  char vpl[32] = "", val[32] = "";
  if (s.vertical) {
    std::snprintf(vpl, sizeof vpl, "%.1f", s.vpl_m);
    std::snprintf(val, sizeof val, "%.1f", s.val_m);
  }
  return nmea_wrap("PPNT,INTEG," + s.pace + "," + hpl + "," + hal + "," + (s.alarm ? "1" : "0") + "," + (s.fault_detection ? "1" : "0") + "," +
                   joined(s.excluded) + "," + vpl + "," + val + "," + joined(s.causes) + "," + joined(s.silent));
}

json integrity_to_json(const IntegrityStatus& s) {
  if (!s.present) return json::object();
  json j{{"pace", s.pace}, {"hpl_m", s.hpl_m}, {"hal_m", s.hal_m}, {"alarm", s.alarm}, {"fault_detection_available", s.fault_detection},
         {"excluded", s.excluded}, {"causes", s.causes}, {"silent", s.silent}, {"time_s", s.time_s}};
  for (auto it = s.raw.begin(); it != s.raw.end(); ++it)
    if (!j.contains(it.key())) j[it.key()] = it.value();
  return j;
}

}  // namespace edge
