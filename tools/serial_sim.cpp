// pnt-edge-serial-sim: plays an LCM log as serial devices on pseudo-terminals, so the serial input path of
// pnt-edge (VN-100 and u-blox / NMEA drivers, clock mapping, reordering) runs end to end without hardware.
//
//   pnt-edge-serial-sim <log> [--speed 1] [--loop] [--imu binary|ascii] [--gnss ubx|nmea] [--utc-offset 0]
//                       [--imu-link /tmp/pnt-imu] [--gnss-link /tmp/pnt-gnss] [--imu-channel c] [--gnss-channel c]
//
// Prints the two pty paths (and creates the symlinks when asked) and streams until the log ends or forever.
#include "nmea.hpp"
#include "serial/nmea_gnss.hpp"
#include "serial/port.hpp"
#include "serial/ubx.hpp"
#include "serial/vn100.hpp"

#include <pntos/cobra/transport/LcmConversions.hpp>
#include <pntos/cobra/transport/LcmLog.hpp>
#include <pntos/cobra/utils/aspn.hpp>

#include <aspn23/eigen/MeasurementImu.hpp>
#include <aspn23/eigen/MeasurementPosition.hpp>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t g_stop = 0;

struct Pty {
  int master = -1, slave = -1;
  std::string path;
  bool open() {
    master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) return false;
    char name[256];
    if (::ptsname_r(master, name, sizeof name) != 0) return false;
    path = name;
    // keep the slave open and raw so bytes written before the reader connects are neither mangled nor lost to EIO
    edge::serial::Port p;
    if (!p.open(path, 115200)) return false;
    slave = ::dup(p.fd());
    return true;
  }
  std::uint64_t dropped = 0;
  void write(const std::uint8_t* data, std::size_t n) {
    ssize_t r = ::write(master, data, n);
    if (r < 0 || static_cast<std::size_t>(r) != n) ++dropped;  // no reader keeping up: a real device would lose it too
  }
  void write(const std::string& s) { write(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }
};

void link(const std::string& link_path, const std::string& target) {
  if (link_path.empty()) return;
  ::unlink(link_path.c_str());
  if (::symlink(target.c_str(), link_path.c_str()) != 0) std::cerr << "cannot create " << link_path << ": " << std::strerror(errno) << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: pnt-edge-serial-sim <lcm log> [--speed 1] [--loop] [--imu binary|ascii] [--gnss ubx|nmea] [--utc-offset 0]\n"
                 "                           [--imu-link path] [--gnss-link path] [--imu-channel c] [--gnss-channel c]\n";
    return 2;
  }
  std::string log = argv[1], imu_mode = "binary", gnss_mode = "ubx", imu_link, gnss_link;
  std::string imu_channel = "/sensor/vn-100/imu", gnss_channel = "/sensor/ublox-ZED-F9T/position";
  double speed = 1, utc_offset = 0;
  bool loop = false;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--speed") speed = std::atof(next().c_str());
    else if (a == "--loop") loop = true;
    else if (a == "--imu") imu_mode = next();
    else if (a == "--gnss") gnss_mode = next();
    else if (a == "--utc-offset") utc_offset = std::atof(next().c_str());
    else if (a == "--imu-link") imu_link = next();
    else if (a == "--gnss-link") gnss_link = next();
    else if (a == "--imu-channel") imu_channel = next();
    else if (a == "--gnss-channel") gnss_channel = next();
    else {
      std::cerr << "unknown option " << a << "\n";
      return 2;
    }
  }
  Pty imu, gnss;
  if (!imu.open() || !gnss.open()) {
    std::cerr << "cannot open pseudo-terminals: " << std::strerror(errno) << "\n";
    return 1;
  }
  link(imu_link, imu.path);
  link(gnss_link, gnss.path);
  std::cout << "imu  " << imu_mode << "  " << imu.path << (imu_link.empty() ? "" : "  -> " + imu_link) << "\n"
            << "gnss " << gnss_mode << "  " << gnss.path << (gnss_link.empty() ? "" : "  -> " + gnss_link) << "\n"
            << std::flush;
  std::signal(SIGINT, [](int) { g_stop = 1; });
  std::signal(SIGTERM, [](int) { g_stop = 1; });

  const std::int64_t offset_ns = static_cast<std::int64_t>(utc_offset * 1e9);
  std::uint64_t imu_n = 0, gnss_n = 0;
  do {
    pntos::cobra::lcm::LcmLogReader reader(log);
    const auto wall0 = std::chrono::steady_clock::now();
    std::optional<std::int64_t> t0, first_tov;
    while (!g_stop) {
      auto ev = reader.next();
      if (!ev) break;
      if (ev->channel != imu_channel && ev->channel != gnss_channel) continue;
      std::shared_ptr<pntos::api::AspnBase> msg;
      try {
        msg = pntos::cobra::lcm::decode(ev->data);
      } catch (const std::exception&) {
        continue;
      }
      if (!msg) continue;
      auto tov = pntos::cobra::utils::time_of_validity(*msg);
      if (!tov) continue;
      if (speed > 0) {
        if (!t0) t0 = ev->timestamp_us;
        std::this_thread::sleep_until(wall0 + std::chrono::microseconds(static_cast<std::int64_t>((ev->timestamp_us - *t0) / speed)));
      }
      const std::int64_t t = tov->elapsed_nsec + offset_ns;
      if (!first_tov) first_tov = t;
      if (auto* m = dynamic_cast<const aspn23_eigen::MeasurementImu*>(msg.get())) {
        edge::serial::vn100::Sample s;
        s.integrated = m->get_imu_type() == ASPN23_MEASUREMENT_IMU_IMU_TYPE_INTEGRATED;
        s.a = m->get_meas_accel().head<3>();
        s.g = m->get_meas_gyro().head<3>();
        s.dt_s = 0.01;
        s.device_ns = t - *first_tov;
        s.absolute_ns = t;
        if (imu_mode == "ascii") imu.write(edge::serial::vn100::encode_ascii(s));
        else {
          const auto f = edge::serial::vn100::encode(s);
          imu.write(f.data(), f.size());
        }
        ++imu_n;
      } else if (auto* p = dynamic_cast<const aspn23_eigen::MeasurementPosition*>(msg.get())) {
        const auto cov = p->get_covariance();
        const double sh = std::sqrt(std::max(cov(0, 0), cov(1, 1))), sv = std::sqrt(cov(2, 2));
        const std::time_t secs = static_cast<std::time_t>(t / 1'000'000'000LL);
        std::tm tm{};
        gmtime_r(&secs, &tm);
        if (gnss_mode == "nmea") {
          edge::serial::nmea::Fix f;
          f.tod_s = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec + static_cast<double>(t % 1'000'000'000LL) * 1e-9;
          f.year = tm.tm_year + 1900;
          f.month = tm.tm_mon + 1;
          f.day = tm.tm_mday;
          f.lat_rad = p->get_term1();
          f.lon_rad = p->get_term2();
          f.alt_hae_m = p->get_term3();
          f.quality = 1;
          f.satellites = 12;
          f.hdop = 1.0;
          f.sigma_lat_m = f.sigma_lon_m = sh;
          f.sigma_alt_m = sv;
          char zda[64];
          std::snprintf(zda, sizeof zda, "GPZDA,%02d%02d%06.3f,%02d,%02d,%04d,00,00", tm.tm_hour, tm.tm_min, tm.tm_sec + static_cast<double>(t % 1'000'000'000LL) * 1e-9, tm.tm_mday,
                        tm.tm_mon + 1, tm.tm_year + 1900);
          gnss.write(edge::nmea_wrap(zda));
          gnss.write(edge::serial::nmea::encode_gga(f));
          gnss.write(edge::serial::nmea::encode_gst(f));
        } else {
          edge::serial::ubx::NavPvt pvt;
          const std::int64_t gps_s = secs + 18 - 315964800;
          pvt.itow_ms = static_cast<std::uint32_t>((gps_s % 604800) * 1000 + (t % 1'000'000'000LL) / 1'000'000);
          pvt.year = tm.tm_year + 1900;
          pvt.month = tm.tm_mon + 1;
          pvt.day = tm.tm_mday;
          pvt.hour = tm.tm_hour;
          pvt.minute = tm.tm_min;
          pvt.second = tm.tm_sec;
          pvt.nano = static_cast<std::int32_t>(t % 1'000'000'000LL);
          pvt.valid_date = pvt.valid_time = pvt.fully_resolved = true;
          pvt.tacc_ns = 30;
          pvt.fix_type = 3;
          pvt.gnss_fix_ok = true;
          pvt.num_sv = 12;
          pvt.lat_deg = p->get_term1() * 180.0 / M_PI;
          pvt.lon_deg = p->get_term2() * 180.0 / M_PI;
          pvt.height_m = p->get_term3();
          pvt.hmsl_m = p->get_term3();
          pvt.hacc_m = sh * std::sqrt(2.0);
          pvt.vacc_m = sv;
          pvt.pdop = 1.5;
          const auto f = edge::serial::ubx::encode_nav_pvt(pvt);
          gnss.write(f.data(), f.size());
        }
        ++gnss_n;
      }
    }
  } while (loop && !g_stop);
  std::cerr << "serial-sim: " << imu_n << " IMU frames, " << gnss_n << " GNSS fixes; dropped (no reader) " << imu.dropped << " / " << gnss.dropped << "\n";
  return 0;
}
