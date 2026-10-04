// Health / status snapshot of the daemon: the seed of the maintenance UI's data model. Written as JSON
// to a file at a fixed interval and printable on demand.
#pragma once

#include "inputs.hpp"
#include "nmea.hpp"

#include <nlohmann/json.hpp>
#include <pntos/cobra/app/Filter.hpp>

#include <chrono>
#include <optional>

namespace edge {

struct StatusSnapshot {
  std::chrono::system_clock::time_point wall;
  std::uint64_t uptime_sec = 0;
  InputCounters input;
  std::uint64_t solutions = 0, nmea_sentences = 0;
  std::optional<Pva> last_solution;
  double last_solution_age_sec = -1;    ///< wall seconds since the last published solution, -1 = none yet
  bool filter_error = false;
  nlohmann::json gating;                ///< registry group fusion/gating, if present
  std::size_t tcp_clients = 0;
};

nlohmann::json status_to_json(const StatusSnapshot& s);
/// Collects the gating counters from the filter's registry (empty object if the group does not exist).
nlohmann::json gating_from_registry(pntos::cobra::Filter& filter);
/// Writes JSON atomically (tmp file + rename).
void write_status_file(const std::string& path, const nlohmann::json& j);

}  // namespace edge
