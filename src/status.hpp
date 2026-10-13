// Health / status snapshot of the daemon: the seed of the maintenance UI's data model. Written as JSON
// to a file at a fixed interval and printable on demand.
#pragma once

#include "inputs.hpp"
#include "nmea.hpp"

#include <nlohmann/json.hpp>
#include "integrity.hpp"

#include <pntos/cobra/app/Filter.hpp>

#include <chrono>
#include <optional>

namespace edge {

struct StatusSnapshot {
  std::chrono::system_clock::time_point wall;
  std::uint64_t uptime_sec = 0;
  InputCounters input;
  nlohmann::json devices = nlohmann::json::array();  ///< serial devices (input.devices in the JSON)
  std::optional<double> utc_correction_s;            ///< applied to solution times for UTC output, when known
  int host_clock_steps = 0;                          ///< host clock steps seen since start (each restarts a host-stamped filter)
  std::optional<double> last_host_clock_step_s;       ///< the most recent one, signed
  std::uint64_t solutions = 0, nmea_sentences = 0;
  std::optional<Pva> last_solution;
  double last_solution_age_sec = -1;    ///< wall seconds since the last published solution, -1 = none yet
  bool filter_error = false;
  nlohmann::json gating;                ///< registry group fusion/gating, if present
  IntegrityStatus integrity;            ///< registry group integrity/status, if an integrity plugin is linked
  std::size_t tcp_clients = 0;
};

nlohmann::json status_to_json(const StatusSnapshot& s);
/// Collects the gating counters from the filter's registry (empty object if the group does not exist).
nlohmann::json gating_from_registry(pntos::cobra::Filter& filter);
/// Writes JSON atomically (tmp file + rename).
void write_status_file(const std::string& path, const nlohmann::json& j);

/// Filter watchdog: true when the filter has published nothing for `threshold_s` although measurements kept
/// arriving (pushed grew since the last check) and it had already produced solutions. Cobra can stay in an error
/// state for good after a timeline break (an out-of-order fix, a gap); the daemon then re-aligns instead of
/// staying silent. threshold_s <= 0 disables it.
inline bool watchdog_due(double threshold_s, double last_solution_age_s, std::uint64_t solutions, std::uint64_t pushed_now, std::uint64_t pushed_before) {
  return threshold_s > 0 && solutions > 0 && last_solution_age_s > threshold_s && pushed_now > pushed_before;
}

}  // namespace edge
