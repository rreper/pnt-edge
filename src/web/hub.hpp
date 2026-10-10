// Shared, thread-safe state between the daemon's main loop and the HTTP surfaces: the latest status and solution,
// a solution history, the recent log, integrity events, and the queue of control commands the web pages issue.
#pragma once

#include "integrity.hpp"
#include "nmea.hpp"
#include "status.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace edge {

struct HistorySample {
  double t;                                 ///< solution time of validity, s
  double lat, lon, alt;                     ///< rad, rad, m
  double n, e, d;                           ///< m from the first sample (local NED)
  double speed_mps, heading_rad;
  double sigma_n, sigma_e, sigma_d;         ///< m
  double hpl_m;                             ///< 0 when no integrity plugin
  std::string pace;                         ///< "" when no integrity plugin
};

struct LogLine {
  double wall_s;
  std::string level, source, message;
};

struct Event {
  double wall_s;
  std::string kind, detail;  ///< PACE | EXCLUDE | READMIT | ALARM | CONTROL | ...
};

struct Command {
  std::string kind;         ///< reset_filter | reset_app | set_heading | set_leverarm | set_nmea | save_config
  nlohmann::json args;
  std::string source;       ///< "tactical" | "maintenance:<user>" | "token"
  std::string address;
};

class Hub {
 public:
  explicit Hub(double history_seconds = 7200, int log_lines = 2000);

  // --- producers (main loop)
  void set_status(const nlohmann::json& status);
  void push_solution(const Pva& p, const IntegrityStatus& integ);
  void log(const std::string& level, const std::string& source, const std::string& message);
  void event(const std::string& kind, const std::string& detail);
  void set_config(const nlohmann::json& edge_config, const nlohmann::json& filter_config);
  void set_version(const std::string& v) { std::lock_guard lk(m_); version_ = v; }

  // --- consumers (web)
  nlohmann::json status() const;
  nlohmann::json solution() const;                            ///< latest sample + integrity, or empty object
  nlohmann::json history(double seconds, int max_points) const;  ///< arrays, decimated to max_points
  nlohmann::json events(int max) const;
  nlohmann::json log(int max, const std::string& min_level) const;
  nlohmann::json config() const;
  std::string version() const { std::lock_guard lk(m_); return version_; }
  /// Blocks until a new status or solution arrives (or the timeout), for the SSE stream; returns the change counter.
  std::uint64_t wait_for_change(std::uint64_t seen, std::chrono::milliseconds timeout) const;

  // --- control
  void enqueue(Command c);
  std::optional<Command> next_command();
  std::size_t pending_commands() const { std::lock_guard lk(m_); return commands_.size(); }

 private:
  mutable std::mutex m_;
  mutable std::condition_variable cv_;
  std::uint64_t change_ = 0;
  double history_seconds_;
  std::size_t log_lines_;
  nlohmann::json status_ = nlohmann::json::object();
  std::optional<HistorySample> latest_;
  nlohmann::json latest_integrity_ = nlohmann::json::object();
  std::deque<HistorySample> history_;
  bool have_origin_ = false;
  double lat0_ = 0, lon0_ = 0, alt0_ = 0;
  std::deque<LogLine> log_;
  std::deque<Event> events_;
  std::deque<Command> commands_;
  nlohmann::json edge_config_ = nlohmann::json::object(), filter_config_ = nlohmann::json::object();
  std::string version_;
};

}  // namespace edge
