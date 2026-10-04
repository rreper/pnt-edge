// Measurement inputs: each turns a source into pushes on a cobra::Filter.
#pragma once

#include "edge_config.hpp"

#include <pntos/cobra/app/Filter.hpp>
#include <pntos/cobra/transport/LcmUdpTransportPlugin.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace edge {

struct InputCounters {
  std::map<std::string, std::uint64_t> per_channel;
  std::uint64_t pushed = 0, decode_failures = 0;
  std::int64_t last_tov_ns = 0;
};

class Input {
 public:
  virtual ~Input() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual bool finished() const = 0;  ///< a log reached its end
  InputCounters counters() const {
    std::lock_guard lk(mutex_);
    return counters_;
  }

 protected:
  void count(const std::string& channel, std::int64_t tov_ns) {
    std::lock_guard lk(mutex_);
    ++counters_.per_channel[channel];
    ++counters_.pushed;
    counters_.last_tov_ns = tov_ns;
  }
  void count_failure() {
    std::lock_guard lk(mutex_);
    ++counters_.decode_failures;
  }
  mutable std::mutex mutex_;
  InputCounters counters_;
};

/// Replays an LCM event log (as fast as possible, paced, or looped).
class LogInput final : public Input {
 public:
  LogInput(pntos::cobra::Filter& filter, const InputConfig& config, std::set<std::string> channels);
  ~LogInput() override { stop(); }
  void start() override;
  void stop() override;
  bool finished() const override { return finished_; }
  /// Runs synchronously (what the thread does); public for tests.
  void run();

 private:
  pntos::cobra::Filter& filter_;
  InputConfig cfg_;
  std::set<std::string> channels_;
  std::thread thread_;
  std::atomic<bool> stop_{false}, finished_{false};
};

/// Receives ASPN-23 messages over LCM UDP multicast through Cobra's transport plugin, with a small
/// adapter mediator that forwards every message to the filter.
class LcmUdpInput final : public Input {
 public:
  LcmUdpInput(pntos::cobra::Filter& filter, const InputConfig& config);
  ~LcmUdpInput() override;
  void start() override;
  void stop() override;
  bool finished() const override { return false; }

 private:
  class ForwardingMediator;
  pntos::cobra::Filter& filter_;
  InputConfig cfg_;
  std::unique_ptr<ForwardingMediator> mediator_;
  std::unique_ptr<pntos::cobra::LcmUdpTransportPlugin> transport_;
};

std::unique_ptr<Input> make_input(pntos::cobra::Filter& filter, const InputConfig& config, std::set<std::string> default_channels);

}  // namespace edge
