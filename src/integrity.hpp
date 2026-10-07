// Integrity status surfaced on the tactical interface: read from the filter's registry group `integrity/status`
// (written by an integrity-monitoring orchestration plugin when one is linked), mapped to the GGA fix quality and
// to the $PPNT,INTEG sentence. Without such a plugin the group does not exist and everything here is a no-op.
#pragma once

#include <pntos/cobra/app/Filter.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace edge {

struct IntegrityStatus {
  bool present = false;                 ///< the registry group exists
  std::string pace;                     ///< PRIMARY | ALTERNATE | CONTINGENCY | EMERGENCY
  double hpl_m = 0, hal_m = 0;          ///< horizontal protection level and alert limit
  bool alarm = false;                   ///< a source disagreement is being evaluated
  bool fault_detection = false;         ///< two or more fresh trusted sources: a fault can be detected
  std::vector<std::string> excluded;    ///< source names currently excluded
  double time_s = 0;                    ///< filter time of the status
  nlohmann::json raw;                   ///< every key of the group as read
};

/// Reads `integrity/status` (present = false if the group does not exist or cannot be read).
IntegrityStatus integrity_from_registry(pntos::cobra::Filter& filter);

/// GGA fix quality from the PACE state: PRIMARY keeps `base` (1 = GPS), ALTERNATE and CONTINGENCY give 6
/// (estimated, non-GNSS aiding or dead reckoning), EMERGENCY gives 0 (invalid). Absent status: `base`.
int fix_quality_for(const IntegrityStatus& s, int base = 1);

/// `$PPNT,INTEG,<pace>,<hpl_m>,<hal_m>,<alarm 0|1>,<fault_detection 0|1>,<excluded joined by '+'>*hh`
std::string integ_sentence(const IntegrityStatus& s);

nlohmann::json integrity_to_json(const IntegrityStatus& s);

}  // namespace edge
