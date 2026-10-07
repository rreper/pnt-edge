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
    s.pace = s.raw.value("pace", std::string());
    s.hpl_m = s.raw.value("hpl_m", 0.0);
    s.hal_m = s.raw.value("hal_m", 0.0);
    s.alarm = s.raw.value("alarm", false);
    s.fault_detection = s.raw.value("fault_detection_available", false);
    s.time_s = s.raw.value("time_s", 0.0);
    if (s.raw.contains("excluded") && s.raw["excluded"].is_array()) s.excluded = s.raw["excluded"].get<std::vector<std::string>>();
  } catch (const std::exception&) {
    s = IntegrityStatus{};
  }
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
  std::string ex;
  for (const auto& e : s.excluded) ex += (ex.empty() ? "" : "+") + e;
  return nmea_wrap("PPNT,INTEG," + s.pace + "," + hpl + "," + hal + "," + (s.alarm ? "1" : "0") + "," + (s.fault_detection ? "1" : "0") + "," + ex);
}

json integrity_to_json(const IntegrityStatus& s) {
  if (!s.present) return json::object();
  json j{{"pace", s.pace}, {"hpl_m", s.hpl_m}, {"hal_m", s.hal_m}, {"alarm", s.alarm}, {"fault_detection_available", s.fault_detection},
         {"excluded", s.excluded}, {"time_s", s.time_s}};
  for (auto it = s.raw.begin(); it != s.raw.end(); ++it)
    if (!j.contains(it.key())) j[it.key()] = it.value();
  return j;
}

}  // namespace edge
