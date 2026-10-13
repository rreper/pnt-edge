// Orders messages from several devices before they reach the filter. Each device thread stamps and enqueues on
// its own schedule; the filter needs one timeline that never runs backwards (Cobra rejects a measurement older than
// its time, and an IMU gap behind a position fix can leave it unable to propagate). Release rule for the earliest
// pending message (time of validity T, from device d):
//   - it has waited at least `hold` since arrival (small reorder window for jitter within one device), and
//   - no other live device (one that delivered within `max_hold`) can still have a message before T on its way:
//     its last time of validity plus its message interval, less `hold` for jitter, is at or beyond T (a 1 Hz
//     receiver that just gave 1000 will give nothing before ~2000, so IMU frames up to 1950 need not wait; a 100 Hz
//     IMU at 990 may still bring 1000, so a fix at 1000 waits for the IMU to pass 1040),
//   - or it has waited `max_hold`: a stalled device must not stop the others for good.
// A message that arrives with a time of validity behind what was already released is dropped and counted, never
// pushed out of order. Pure (no threads, injected clock) so that it can be tested.
#pragma once

#include <pntos/api/api.hpp>

#include <cstdint>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace edge::serial {

struct MergeItem {
  std::int64_t tov_ns = 0;
  std::int64_t arrival_ns = 0;
  int source = 0;  ///< device index
  std::shared_ptr<pntos::api::AspnBase> message;
  std::string channel;
  bool operator>(const MergeItem& o) const { return tov_ns > o.tov_ns; }
};

class Merger {
 public:
  Merger(int sources, std::int64_t hold_ns, std::int64_t max_hold_ns)
      : last_tov_(sources, 0), last_arrival_(sources, 0), interval_(sources, 0), hold_(hold_ns), max_hold_(max_hold_ns) {}

  /// Returns false (and counts) when the item is already behind the released timeline.
  bool add(MergeItem item) {
    const int s = item.source;
    if (s >= 0 && s < static_cast<int>(last_tov_.size())) {
      if (last_tov_[s] != 0 && item.tov_ns > last_tov_[s]) {
        const std::int64_t d = item.tov_ns - last_tov_[s];
        interval_[s] = interval_[s] == 0 ? d : (interval_[s] * 4 + d) / 5;  // smoothed message interval
      }
      if (item.tov_ns > last_tov_[s]) last_tov_[s] = item.tov_ns;
      last_arrival_[s] = item.arrival_ns;
    }
    if (released_any_ && item.tov_ns < last_released_tov_) {
      ++late_dropped_;
      return false;
    }
    queue_.push(std::move(item));
    return true;
  }

  /// Everything releasable at `now`, in time order. With `flush` the ordering waits are skipped (shutdown).
  std::vector<MergeItem> release(std::int64_t now, bool flush = false) {
    std::vector<MergeItem> out;
    while (!queue_.empty()) {
      const MergeItem& top = queue_.top();
      const std::int64_t waited = now - top.arrival_ns;
      if (!flush) {
        if (waited < hold_) break;
        if (waited < max_hold_ && !others_caught_up(top, now)) break;
      }
      out.push_back(top);
      queue_.pop();
      last_released_tov_ = out.back().tov_ns;
      released_any_ = true;
    }
    return out;
  }

  std::size_t pending() const { return queue_.size(); }
  std::uint64_t late_dropped() const { return late_dropped_; }
  std::uint64_t held_for_peer() const { return held_for_peer_; }  ///< releases deferred because a live peer lagged (diagnostic)

 private:
  bool others_caught_up(const MergeItem& top, std::int64_t now) {
    for (std::size_t s = 0; s < last_tov_.size(); ++s) {
      if (static_cast<int>(s) == top.source) continue;
      const bool live = last_arrival_[s] != 0 && now - last_arrival_[s] < max_hold_;
      if (live && last_tov_[s] + interval_[s] - hold_ < top.tov_ns) {
        ++held_for_peer_;
        return false;
      }
    }
    return true;
  }

  std::priority_queue<MergeItem, std::vector<MergeItem>, std::greater<MergeItem>> queue_;
  std::vector<std::int64_t> last_tov_, last_arrival_, interval_;
  std::int64_t hold_, max_hold_;
  std::int64_t last_released_tov_ = 0;
  bool released_any_ = false;
  std::uint64_t late_dropped_ = 0, held_for_peer_ = 0;
};

}  // namespace edge::serial
