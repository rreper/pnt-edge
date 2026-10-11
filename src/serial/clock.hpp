// Maps a device clock (IMU time since power-up, GNSS UTC or time of week) onto the host clock with the
// transport latency removed: the offset host - device is tracked by its running minimum (the samples with the
// least latency), and the applied offset follows that target with a bounded slew so stamped times never run
// backwards. A jump of more than `reset_jump_s` in the offset (device power cycle, week rollover) restarts it.
#pragma once

#include <cstdint>
#include <optional>

namespace edge::serial {

class ClockMapper {
 public:
  explicit ClockMapper(double window_s = 10.0, double max_slew = 1e-3, double reset_jump_s = 1.0)
      : window_ns_(static_cast<std::int64_t>(window_s * 1e9)), max_slew_(max_slew), reset_jump_ns_(static_cast<std::int64_t>(reset_jump_s * 1e9)) {}

  /// Returns the time of validity on the host timeline for a sample stamped `device_ns` that arrived at `host_ns`.
  std::int64_t map(std::int64_t device_ns, std::int64_t host_ns);
  /// The applied offset host - device (ns), once a sample was seen.
  std::optional<std::int64_t> offset_ns() const { return started_ ? std::optional(current_) : std::nullopt; }
  void reset() { started_ = false; }

 private:
  std::int64_t window_ns_;
  double max_slew_;
  std::int64_t reset_jump_ns_;
  bool started_ = false, settled_ = false;
  std::int64_t current_ = 0, window_min_ = 0, prev_min_ = 0, window_start_ = 0, start_ = 0, last_host_ = 0;
};

}  // namespace edge::serial
