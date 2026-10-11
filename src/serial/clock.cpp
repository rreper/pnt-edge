#include "clock.hpp"

#include <algorithm>
#include <cstdlib>

namespace edge::serial {

std::int64_t ClockMapper::map(std::int64_t device_ns, std::int64_t host_ns) {
  const std::int64_t offset = host_ns - device_ns;
  if (!started_ || std::llabs(offset - current_) > reset_jump_ns_) {
    started_ = true;
    settled_ = false;
    current_ = window_min_ = prev_min_ = offset;
    window_start_ = start_ = last_host_ = host_ns;
    return device_ns + current_;
  }
  window_min_ = std::min(window_min_, offset);
  if (host_ns - window_start_ >= window_ns_) {
    prev_min_ = window_min_;
    window_min_ = offset;
    window_start_ = host_ns;
    settled_ = true;
  }
  // Follow the least latency seen with a bounded slew: fast (0.1 s/s) during the first window so the start-up
  // latency is shed within a fraction of a second, slow afterwards; both keep consecutive stamps monotonic.
  const std::int64_t target = settled_ ? std::min(prev_min_, window_min_) : window_min_;
  const double slew = settled_ ? max_slew_ : 0.1;
  const double max_step = slew * static_cast<double>(std::max<std::int64_t>(host_ns - last_host_, 0));
  const double step = std::clamp(static_cast<double>(target - current_), -max_step, max_step);
  current_ += static_cast<std::int64_t>(step);
  last_host_ = host_ns;
  return device_ns + current_;
}

}  // namespace edge::serial
