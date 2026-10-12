#include "input.hpp"

#include <pntos/cobra/utils/logging.hpp>

#include <aspn23/eigen/MeasurementImu.hpp>
#include <aspn23/eigen/MeasurementPosition.hpp>
#include <aspn23/eigen/MeasurementVelocity.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>

namespace edge {

namespace {
using pntos::api::LoggingLevel;
using RowMajorMatrix = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
using DynVector = Eigen::Matrix<double, Eigen::Dynamic, 1>;
constexpr double kDeg = M_PI / 180.0;

aspn23_eigen::TypeHeader header(Aspn23MessageType type) { return aspn23_eigen::TypeHeader(type, 0, 0, 0, 0); }

void say(LoggingLevel level, const std::string& m) { pntos::cobra::utils::print_message(level, "SerialInput", m); }

std::string timestamp_for_filename() {
  const auto t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char b[32];
  std::strftime(b, sizeof b, "%Y%m%d_%H%M%S", &tm);
  return b;
}

/// Civil date of `ns` (UTC) as days since the epoch for the midnight before it.
std::int64_t midnight_ns(std::int64_t ns) {
  const std::int64_t day = 86400LL * 1'000'000'000LL;
  return (ns / day) * day;
}
std::int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
}  // namespace

// ----------------------------------------------------------------------------- messages

std::shared_ptr<pntos::api::AspnBase> SerialInput::imu_message(std::int64_t tov_ns, const serial::vn100::Sample& s) {
  return std::make_shared<aspn23_eigen::MeasurementImu>(header(ASPN_MEASUREMENT_IMU), aspn23_eigen::TypeTimestamp(tov_ns),
                                                        s.integrated ? ASPN23_MEASUREMENT_IMU_IMU_TYPE_INTEGRATED : ASPN23_MEASUREMENT_IMU_IMU_TYPE_SAMPLED,
                                                        DynVector(s.a), DynVector(s.g), std::vector<aspn23_eigen::TypeIntegrity>{});
}

std::shared_ptr<pntos::api::AspnBase> SerialInput::position_message(std::int64_t tov_ns, double lat, double lon, double alt, double sn, double se, double sd) {
  RowMajorMatrix cov = RowMajorMatrix::Zero(3, 3);
  cov(0, 0) = sn * sn;
  cov(1, 1) = se * se;
  cov(2, 2) = sd * sd;
  return std::make_shared<aspn23_eigen::MeasurementPosition>(header(ASPN_MEASUREMENT_POSITION), aspn23_eigen::TypeTimestamp(tov_ns),
                                                             ASPN23_MEASUREMENT_POSITION_REFERENCE_FRAME_GEODETIC, lat, lon, alt, cov,
                                                             ASPN23_MEASUREMENT_POSITION_ERROR_MODEL_NONE, DynVector(0), std::vector<aspn23_eigen::TypeIntegrity>{});
}

std::shared_ptr<pntos::api::AspnBase> SerialInput::velocity_message(std::int64_t tov_ns, double vn, double ve, double vd, double sh, double sv) {
  RowMajorMatrix cov = RowMajorMatrix::Zero(3, 3);
  cov(0, 0) = sh * sh;
  cov(1, 1) = sh * sh;
  cov(2, 2) = sv * sv;
  return std::make_shared<aspn23_eigen::MeasurementVelocity>(header(ASPN_MEASUREMENT_VELOCITY), aspn23_eigen::TypeTimestamp(tov_ns),
                                                             ASPN23_MEASUREMENT_VELOCITY_REFERENCE_FRAME_NED, vn, ve, vd, cov,
                                                             ASPN23_MEASUREMENT_VELOCITY_ERROR_MODEL_NONE, DynVector(0), std::vector<aspn23_eigen::TypeIntegrity>{});
}

// ----------------------------------------------------------------------------- lifecycle

SerialInput::SerialInput(pntos::cobra::Filter& filter, const InputConfig& config) : filter_(filter), cfg_(config) {
  for (const auto& dc : cfg_.devices) {
    auto d = std::make_unique<Device>();
    d->cfg = dc;
    if (d->cfg.name.empty()) d->cfg.name = d->cfg.driver;
    devices_.push_back(std::move(d));
  }
}

SerialInput::~SerialInput() { stop(); }

void SerialInput::start() {
  stop_ = false;
  pusher_ = std::thread([this] { pusher(); });
  for (auto& d : devices_) d->thread = std::thread([this, dev = d.get()] { run_device(*dev); });
}

void SerialInput::stop() {
  stop_ = true;
  queue_cv_.notify_all();
  for (auto& d : devices_)
    if (d->thread.joinable()) d->thread.join();
  if (pusher_.joinable()) pusher_.join();
}

// ----------------------------------------------------------------------------- device threads

void SerialInput::run_device(Device& d) {
  auto set_state = [&](const std::string& s, const std::string& detail = "") {
    std::lock_guard lk(mutex_);
    if (d.state != s || d.detail != detail) {
      d.state = s;
      d.detail = detail;
      say(s == "lost" || s == "no data" ? LoggingLevel::WARN : LoggingLevel::INFO, d.cfg.name + " (" + d.cfg.port + "): " + s + (detail.empty() ? "" : " - " + detail));
    }
  };
  std::vector<std::uint8_t> buf(4096);
  while (!stop_) {
    set_state("opening");
    std::string err;
    if (!d.port.open(d.cfg.port, d.cfg.baud, &err)) {
      set_state("closed", err);
      for (int i = 0; i < 10 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    set_state("open");
    d.mapper.reset();
    d.utc_mapper.reset();
    if (!cfg_.record_dir.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(cfg_.record_dir, ec);
      const std::string path = (std::filesystem::path(cfg_.record_dir) / (d.cfg.name + "_" + timestamp_for_filename() + ".raw")).string();
      d.record.open(path, std::ios::binary | std::ios::app);
      if (!d.record) say(LoggingLevel::WARN, "cannot record to " + path);
    }
    // device configuration on open
    if (d.cfg.driver == "ubx" && d.cfg.configure) {
      std::string port = d.cfg.ubx_port;
      if (port.empty()) port = d.cfg.port.find("ttyACM") != std::string::npos ? "usb" : "uart1";
      const int rate_ms = d.cfg.rate_hz > 0 ? static_cast<int>(std::lround(1000.0 / d.cfg.rate_hz)) : 200;
      for (const auto& f : serial::ubx::configure(port, rate_ms)) d.port.write(f.data(), f.size());
    }
    for (const auto& line : d.cfg.init) {
      if (d.cfg.driver == "vn100") d.port.write(serial::vn100::ascii_command(line));
      else if (d.cfg.driver == "ubx") {  // hex bytes "B5 62 ..."
        std::vector<std::uint8_t> bytes;
        std::size_t i = 0;
        while (i < line.size()) {
          while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
          if (i + 2 > line.size()) break;
          bytes.push_back(static_cast<std::uint8_t>(std::strtoul(line.substr(i, 2).c_str(), nullptr, 16)));
          i += 2;
        }
        d.port.write(bytes.data(), bytes.size());
      } else d.port.write(line + "\r\n");
    }
    serial::vn100::Parser imu;
    serial::ubx::Parser ubx;
    serial::nmea::Parser nmea;
    std::int64_t last_data_ns = serial::host_now_ns();
    while (!stop_) {
      const int n = d.port.read(buf.data(), buf.size(), 200);
      if (n < 0) {
        set_state("lost", "port closed or device unplugged");
        break;
      }
      const std::int64_t now = serial::host_now_ns();
      if (n == 0) {
        if (now - last_data_ns > 5'000'000'000LL) set_state("no data", "nothing received for 5 s");
        continue;
      }
      last_data_ns = now;
      if (d.record) d.record.write(reinterpret_cast<const char*>(buf.data()), n);
      {
        std::lock_guard lk(mutex_);
        d.bytes += static_cast<std::uint64_t>(n);
      }
      // decode
      if (d.cfg.driver == "vn100") {
        std::vector<serial::vn100::Sample> samples;
        imu.feed(buf.data(), static_cast<std::size_t>(n), samples);
        {
          std::lock_guard lk(mutex_);
          d.errors = imu.errors;
          if (!imu.last_error.empty()) d.detail = "$VNERR," + imu.last_error;
        }
        for (const auto& s : samples) {
          note_frame(d, now);
          const std::int64_t tov = stamp(d, s.absolute_ns, s.device_ns, now);
          enqueue(d, tov, now, imu_message(tov, s), d.cfg.channel);
        }
        if (!samples.empty()) set_state("streaming");
      } else if (d.cfg.driver == "ubx") {
        std::vector<serial::ubx::Frame> frames;
        ubx.feed(buf.data(), static_cast<std::size_t>(n), frames);
        {
          std::lock_guard lk(mutex_);
          d.errors = ubx.errors;
        }
        for (const auto& f : frames) {
          serial::ubx::NavPvt pvt;
          serial::ubx::TimeLs ls;
          if (serial::ubx::decode_nav_timels(f, ls)) {
            if (ls.valid) {
              std::lock_guard lk(mutex_);
              d.leap_seconds = ls.current_ls;
            }
            continue;
          }
          if (!serial::ubx::decode_nav_pvt(f, pvt)) continue;
          note_frame(d, now);
          std::optional<std::int64_t> utc;
          if (pvt.valid_date && pvt.valid_time) {
            utc = serial::ubx::utc_ns(pvt);
            d.utc_mapper.map(*utc, now);
            std::lock_guard lk(mutex_);
            d.utc_correction_ns = d.cfg.time_source == "device" ? 0 : -*d.utc_mapper.offset_ns();
          }
          if (serial::ubx::fix_quality(pvt) == 0) continue;
          const std::int64_t tov = stamp(d, utc, static_cast<std::int64_t>(pvt.itow_ms) * 1'000'000LL, now);
          const double sh = std::max(pvt.hacc_m / std::sqrt(2.0), d.cfg.sigma_floor_m);
          const double sv = std::max(pvt.vacc_m, 1.5 * d.cfg.sigma_floor_m);
          enqueue(d, tov, now, position_message(tov, pvt.lat_deg * kDeg, pvt.lon_deg * kDeg, pvt.height_m, sh, sh, sv), d.cfg.channel);
          if (!d.cfg.velocity_channel.empty()) {
            const double svel = std::max(pvt.sacc / std::sqrt(3.0), 0.05);
            enqueue(d, tov, now, velocity_message(tov, pvt.vel_n, pvt.vel_e, pvt.vel_d, svel, svel), d.cfg.velocity_channel);
          }
        }
        if (!frames.empty()) set_state("streaming");
      } else {  // nmea
        std::vector<serial::nmea::Fix> fixes;
        nmea.feed(buf.data(), static_cast<std::size_t>(n), fixes);
        {
          std::lock_guard lk(mutex_);
          d.errors = nmea.errors;
        }
        for (const auto& f : fixes) {
          note_frame(d, now);
          std::optional<std::int64_t> utc;
          if (f.tod_s >= 0) {
            std::int64_t base;
            if (f.year > 0) base = days_from_civil(f.year, f.month, f.day) * 86400LL * 1'000'000'000LL;
            else {  // no date sentence yet: the host's day, adjusted around midnight
              base = midnight_ns(now);
              const std::int64_t cand = base + static_cast<std::int64_t>(f.tod_s * 1e9);
              if (cand - now > 43200LL * 1'000'000'000LL) base -= 86400LL * 1'000'000'000LL;
              else if (now - cand > 43200LL * 1'000'000'000LL) base += 86400LL * 1'000'000'000LL;
            }
            utc = base + static_cast<std::int64_t>(std::llround(f.tod_s * 1e9));
            d.utc_mapper.map(*utc, now);
            std::lock_guard lk(mutex_);
            d.utc_correction_ns = d.cfg.time_source == "device" ? 0 : -*d.utc_mapper.offset_ns();
          }
          const std::int64_t tov = stamp(d, utc, std::nullopt, now);
          double sh, sv;
          if (f.sigma_lat_m > 0 || f.sigma_lon_m > 0) {
            sh = std::max(std::max(f.sigma_lat_m, f.sigma_lon_m), d.cfg.sigma_floor_m);
            sv = std::max(f.sigma_alt_m, 1.5 * d.cfg.sigma_floor_m);
          } else {
            const double uere = f.quality == 4 ? 0.02 : f.quality == 5 ? 0.5 : f.quality == 2 ? 1.0 : d.cfg.uere_m;
            sh = std::max((f.hdop > 0 ? f.hdop : 1.0) * uere, d.cfg.sigma_floor_m);
            sv = 1.5 * sh;
          }
          enqueue(d, tov, now, position_message(tov, f.lat_rad, f.lon_rad, f.alt_hae_m, sh, sh, sv), d.cfg.channel);
          if (!d.cfg.velocity_channel.empty() && f.have_velocity)
            enqueue(d, tov, now, velocity_message(tov, f.sog_mps * std::cos(f.cog_rad), f.sog_mps * std::sin(f.cog_rad), 0.0, 0.5, 1.0), d.cfg.velocity_channel);
        }
        if (!fixes.empty()) set_state("streaming");
      }
    }
    d.port.close();
    if (d.record.is_open()) d.record.close();
  }
  d.port.close();
}

std::int64_t SerialInput::stamp(Device& d, std::optional<std::int64_t> absolute_ns, std::optional<std::int64_t> relative_ns, std::int64_t arrival_ns) {
  const std::int64_t latency = static_cast<std::int64_t>(d.cfg.latency_ms * 1e6);
  if (d.cfg.time_source == "device" && absolute_ns) return *absolute_ns - latency;
  if (!absolute_ns && !relative_ns) return arrival_ns - latency;
  const std::int64_t mapped = d.mapper.map(absolute_ns ? *absolute_ns : *relative_ns, arrival_ns);
  {
    std::lock_guard lk(mutex_);
    d.clock_offset_ns = d.mapper.offset_ns();
  }
  return mapped - latency;
}

void SerialInput::note_frame(Device& d, std::int64_t arrival_ns) {
  std::lock_guard lk(mutex_);
  ++d.frames;
  ++d.window_frames;
  d.last_frame_host_ns = arrival_ns;
  if (d.window_start_ns == 0) d.window_start_ns = arrival_ns;
  else if (arrival_ns - d.window_start_ns >= 1'000'000'000LL) {
    d.rate_hz = static_cast<double>(d.window_frames) * 1e9 / static_cast<double>(arrival_ns - d.window_start_ns);
    d.window_frames = 0;
    d.window_start_ns = arrival_ns;
  }
}

void SerialInput::enqueue(const Device&, std::int64_t tov_ns, std::int64_t arrival_ns, std::shared_ptr<pntos::api::AspnBase> m, const std::string& channel) {
  {
    std::lock_guard lk(queue_mutex_);
    queue_.push(Pending{tov_ns, arrival_ns, std::move(m), channel});
  }
  queue_cv_.notify_one();
}

void SerialInput::pusher() {
  const std::int64_t hold = static_cast<std::int64_t>(cfg_.reorder_ms * 1e6);
  std::unique_lock lk(queue_mutex_);
  while (!stop_) {
    queue_cv_.wait_for(lk, std::chrono::milliseconds(5));
    const std::int64_t now = serial::host_now_ns();
    while (!queue_.empty() && (stop_ || now - queue_.top().arrival_ns >= hold)) {
      Pending p = queue_.top();
      queue_.pop();
      lk.unlock();
      filter_.push(pntos::api::Message(p.message, p.channel));
      count(p.channel, p.tov_ns);
      lk.lock();
    }
  }
  // drain in order on stop so a test that wrote everything sees it all
  while (!queue_.empty()) {
    Pending p = queue_.top();
    queue_.pop();
    lk.unlock();
    filter_.push(pntos::api::Message(p.message, p.channel));
    count(p.channel, p.tov_ns);
    lk.lock();
  }
}

// ----------------------------------------------------------------------------- status

nlohmann::json SerialInput::devices() const {
  std::lock_guard lk(mutex_);
  nlohmann::json out = nlohmann::json::array();
  const std::int64_t now = serial::host_now_ns();
  for (const auto& d : devices_) {
    nlohmann::json j{{"name", d->cfg.name}, {"driver", d->cfg.driver}, {"port", d->cfg.port}, {"baud", d->cfg.baud}, {"channel", d->cfg.channel},
                     {"state", d->state}, {"detail", d->detail}, {"bytes", d->bytes}, {"frames", d->frames}, {"errors", d->errors}, {"rate_hz", d->rate_hz},
                     {"last_frame_age_s", d->last_frame_host_ns ? static_cast<double>(now - d->last_frame_host_ns) * 1e-9 : -1.0}};
    if (d->clock_offset_ns) j["clock_offset_s"] = static_cast<double>(*d->clock_offset_ns) * 1e-9;
    if (d->leap_seconds) j["leap_seconds"] = *d->leap_seconds;
    if (d->utc_correction_ns) j["utc_correction_s"] = static_cast<double>(*d->utc_correction_ns) * 1e-9;
    out.push_back(std::move(j));
  }
  return out;
}

std::optional<std::int64_t> SerialInput::utc_correction_ns() const {
  std::lock_guard lk(mutex_);
  for (const auto& d : devices_)
    if (d->utc_correction_ns) return d->utc_correction_ns;
  return std::nullopt;
}

bool SerialInput::uses_host_clock() const {
  for (const auto& d : cfg_.devices)
    if (d.time_source != "device") return true;
  return false;
}

}  // namespace edge
