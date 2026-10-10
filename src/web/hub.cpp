#include "web/hub.hpp"

#include <pntos/cobra/utils/navutils.hpp>

#include <algorithm>
#include <cmath>

namespace edge {

using nlohmann::json;

namespace {
double wall_now() {
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
int level_rank(const std::string& l) {
  if (l == "ERROR") return 3;
  if (l == "WARN") return 2;
  if (l == "INFO") return 1;
  return 0;
}
}  // namespace

Hub::Hub(double history_seconds, int log_lines) : history_seconds_(history_seconds), log_lines_(static_cast<std::size_t>(std::max(1, log_lines))) {}

void Hub::set_status(const json& status) {
  std::lock_guard lk(m_);
  status_ = status;
  ++change_;
  cv_.notify_all();
}

void Hub::push_solution(const Pva& p, const IntegrityStatus& integ) {
  namespace nav = pntos::cobra::nav;
  std::lock_guard lk(m_);
  if (!have_origin_) {
    lat0_ = p.lat_rad;
    lon0_ = p.lon_rad;
    alt0_ = p.alt_hae_m;
    have_origin_ = true;
  }
  HistorySample s;
  s.t = p.tov_ns * 1e-9;
  s.lat = p.lat_rad;
  s.lon = p.lon_rad;
  s.alt = p.alt_hae_m;
  s.n = nav::delta_lat_to_north(p.lat_rad - lat0_, lat0_, alt0_);
  s.e = nav::delta_lon_to_east(p.lon_rad - lon0_, lat0_, alt0_);
  s.d = -(p.alt_hae_m - alt0_);
  s.speed_mps = std::hypot(p.vn, p.ve);
  s.heading_rad = p.yaw_rad;
  s.sigma_n = p.sigma_n;
  s.sigma_e = p.sigma_e;
  s.sigma_d = p.sigma_d;
  s.hpl_m = integ.present ? integ.hpl_m : 0.0;
  s.pace = integ.present ? integ.pace : "";
  latest_ = s;
  latest_integrity_ = integrity_to_json(integ);
  history_.push_back(s);
  while (!history_.empty() && s.t - history_.front().t > history_seconds_) history_.pop_front();
  ++change_;
  cv_.notify_all();
}

void Hub::log(const std::string& level, const std::string& source, const std::string& message) {
  std::lock_guard lk(m_);
  log_.push_back({wall_now(), level, source, message});
  while (log_.size() > log_lines_) log_.pop_front();
}

void Hub::event(const std::string& kind, const std::string& detail) {
  std::lock_guard lk(m_);
  events_.push_back({wall_now(), kind, detail});
  while (events_.size() > 500) events_.pop_front();
  ++change_;
  cv_.notify_all();
}

void Hub::set_config(const json& edge_config, const json& filter_config) {
  std::lock_guard lk(m_);
  edge_config_ = edge_config;
  filter_config_ = filter_config;
}

json Hub::status() const {
  std::lock_guard lk(m_);
  return status_;
}

json Hub::solution() const {
  std::lock_guard lk(m_);
  if (!latest_) return json::object();
  const auto& s = *latest_;
  return json{{"t", s.t}, {"lat_deg", s.lat * 180 / M_PI}, {"lon_deg", s.lon * 180 / M_PI}, {"alt_m", s.alt}, {"ned_m", {s.n, s.e, s.d}},
              {"speed_mps", s.speed_mps}, {"heading_deg", s.heading_rad * 180 / M_PI}, {"sigma_m", {s.sigma_n, s.sigma_e, s.sigma_d}},
              {"hpl_m", s.hpl_m}, {"pace", s.pace}, {"integrity", latest_integrity_}};
}

json Hub::history(double seconds, int max_points) const {
  std::lock_guard lk(m_);
  json out{{"t", json::array()}, {"n", json::array()}, {"e", json::array()}, {"d", json::array()}, {"speed", json::array()}, {"heading_deg", json::array()},
           {"sigma_h", json::array()}, {"sigma_d", json::array()}, {"hpl", json::array()}, {"pace", json::array()}};
  if (history_.empty()) return out;
  const double t_end = history_.back().t;
  std::vector<const HistorySample*> sel;
  for (const auto& s : history_)
    if (seconds <= 0 || t_end - s.t <= seconds) sel.push_back(&s);
  const std::size_t step = max_points > 0 && sel.size() > static_cast<std::size_t>(max_points) ? sel.size() / max_points + 1 : 1;
  for (std::size_t i = 0; i < sel.size(); i += step) {
    const auto& s = *sel[i];
    out["t"].push_back(s.t);
    out["n"].push_back(s.n);
    out["e"].push_back(s.e);
    out["d"].push_back(s.d);
    out["speed"].push_back(s.speed_mps);
    out["heading_deg"].push_back(s.heading_rad * 180 / M_PI);
    out["sigma_h"].push_back(std::hypot(s.sigma_n, s.sigma_e));
    out["sigma_d"].push_back(s.sigma_d);
    out["hpl"].push_back(s.hpl_m);
    out["pace"].push_back(s.pace);
  }
  out["origin"] = {{"lat_deg", lat0_ * 180 / M_PI}, {"lon_deg", lon0_ * 180 / M_PI}, {"alt_m", alt0_}};
  return out;
}

json Hub::events(int max) const {
  std::lock_guard lk(m_);
  json out = json::array();
  const std::size_t n = events_.size();
  const std::size_t from = max > 0 && n > static_cast<std::size_t>(max) ? n - max : 0;
  for (std::size_t i = from; i < n; ++i) out.push_back({{"t", events_[i].wall_s}, {"kind", events_[i].kind}, {"detail", events_[i].detail}});
  return out;
}

json Hub::log(int max, const std::string& min_level) const {
  std::lock_guard lk(m_);
  json out = json::array();
  const int rank = level_rank(min_level);
  int taken = 0;
  for (auto it = log_.rbegin(); it != log_.rend() && (max <= 0 || taken < max); ++it) {
    if (level_rank(it->level) < rank) continue;
    out.push_back({{"t", it->wall_s}, {"level", it->level}, {"source", it->source}, {"message", it->message}});
    ++taken;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

json Hub::config() const {
  std::lock_guard lk(m_);
  return json{{"edge", edge_config_}, {"filter", filter_config_}};
}

std::uint64_t Hub::wait_for_change(std::uint64_t seen, std::chrono::milliseconds timeout) const {
  std::unique_lock lk(m_);
  cv_.wait_for(lk, timeout, [&] { return change_ != seen; });
  return change_;
}

void Hub::enqueue(Command c) {
  std::lock_guard lk(m_);
  commands_.push_back(std::move(c));
}

std::optional<Command> Hub::next_command() {
  std::lock_guard lk(m_);
  if (commands_.empty()) return std::nullopt;
  Command c = std::move(commands_.front());
  commands_.pop_front();
  return c;
}

}  // namespace edge
