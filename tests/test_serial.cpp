// Serial drivers: parsers, encoders, clock mapping, and the serial input end to end over pseudo-terminals.
#include "serial/clock.hpp"
#include "serial/input.hpp"
#include "serial/nmea_gnss.hpp"
#include "serial/port.hpp"
#include "serial/ubx.hpp"
#include "serial/vn100.hpp"

#include <pntos/cobra/app/Filter.hpp>
#include <pntos/cobra/config/JsonConfig.hpp>
#include <pntos/cobra/transport/LcmConversions.hpp>
#include <pntos/cobra/transport/LcmLog.hpp>
#include <pntos/cobra/utils/aspn.hpp>

#include <aspn23/eigen/MeasurementImu.hpp>
#include <aspn23/eigen/MeasurementPosition.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

using namespace edge::serial;

namespace {
std::string srcdir() {
  const char* e = std::getenv("PNT_EDGE_SRCDIR");
  return e ? e : ".";
}
std::string cobra(const std::string& rel) { return srcdir() + "/subprojects/pntos-cpp/" + rel; }

struct Pty {
  int master = -1;
  std::string path;
  Port keep;  // the slave, raw, kept open so the line discipline never mangles bytes
  bool open() {
    master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || ::grantpt(master) || ::unlockpt(master)) return false;
    char name[256];
    if (::ptsname_r(master, name, sizeof name)) return false;
    path = name;
    return keep.open(path, 115200);
  }
  void write(const std::vector<std::uint8_t>& b) { ASSERT_EQ(::write(master, b.data(), b.size()), static_cast<ssize_t>(b.size())); }
  void write(const std::string& s) { ASSERT_EQ(::write(master, s.data(), s.size()), static_cast<ssize_t>(s.size())); }
  ~Pty() {
    if (master >= 0) ::close(master);
  }
};
}  // namespace

// ----------------------------------------------------------------------------- VN-100

TEST(Vn100, AsciiDeltaSentenceAndChecksum) {
  vn100::Sample s;
  s.integrated = true;
  s.dt_s = 0.005;
  s.g = Eigen::Vector3d(0.001, -0.002, 0.0005);
  s.a = Eigen::Vector3d(0.01, 0.02, -0.049);
  const std::string line = vn100::encode_ascii(s);
  EXPECT_EQ(line.substr(0, 6), "$VNDTV");
  vn100::Parser p;
  std::vector<vn100::Sample> out;
  // in two pieces, with noise before it
  const std::string stream = "\xff\x00" + line + "$VNERR,12*";
  p.feed(reinterpret_cast<const std::uint8_t*>(stream.data()), 7, out);
  EXPECT_TRUE(out.empty());
  p.feed(reinterpret_cast<const std::uint8_t*>(stream.data()) + 7, stream.size() - 7, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].integrated);
  EXPECT_NEAR(out[0].dt_s, 0.005, 1e-9);
  EXPECT_NEAR(out[0].g(1), -0.002, 1e-7);
  EXPECT_NEAR(out[0].a(2), -0.049, 1e-7);
  EXPECT_EQ(p.frames, 1u);
  EXPECT_EQ(p.errors, 0u);
  // a corrupted checksum is counted, not delivered
  std::string bad = line;
  bad[10] = bad[10] == '1' ? '2' : '1';
  p.feed(reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size(), out);
  EXPECT_EQ(out.size(), 1u);
  EXPECT_EQ(p.errors, 1u);
  EXPECT_EQ(vn100::ascii_command("VNWRG,06,0"), "$VNWRG,06,0*6C\r\n");
  EXPECT_EQ(vn100::ascii_command("$VNWRG,06,0*XX\r\n"), "$VNWRG,06,0*6C\r\n");
}

TEST(Vn100, BinaryFrameRoundTripAndResync) {
  vn100::Sample s;
  s.integrated = true;
  s.dt_s = 0.005;
  s.g = Eigen::Vector3d(0.001, -0.002, 0.0005);
  s.a = Eigen::Vector3d(0.01, 0.02, -0.049);
  s.device_ns = 123456789012LL;
  s.absolute_ns = 1747680886'500000000LL;
  const auto f = vn100::encode(s);
  EXPECT_EQ(f[0], 0xFA);
  EXPECT_EQ(vn100::crc16(f.data() + 1, f.size() - 1), 0);  // CRC over the frame including its CRC is zero
  std::vector<std::uint8_t> stream{0x00, 0xFA, 0x7F};  // noise, including a fake sync
  stream.insert(stream.end(), f.begin(), f.end());
  auto corrupt = f;
  corrupt[8] ^= 0x01;
  stream.insert(stream.end(), corrupt.begin(), corrupt.end());
  stream.insert(stream.end(), f.begin(), f.end());
  vn100::Parser p;
  std::vector<vn100::Sample> out;
  for (std::size_t i = 0; i < stream.size(); i += 5) p.feed(stream.data() + i, std::min<std::size_t>(5, stream.size() - i), out);  // byte dribble
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(p.errors, 1u);
  EXPECT_EQ(p.frames, 2u);
  EXPECT_TRUE(out[1].integrated);
  EXPECT_EQ(*out[1].device_ns, 123456789012LL);
  EXPECT_EQ(*out[1].absolute_ns, 1747680886'500000000LL);
  EXPECT_NEAR(out[1].g(0), 0.001, 1e-7);
  EXPECT_NEAR(out[1].a(2), -0.049, 1e-7);
  // sampled frame (Imu field)
  vn100::Sample r;
  r.a = Eigen::Vector3d(0.1, 0.2, -9.8);
  r.g = Eigen::Vector3d(0.01, 0.02, 0.03);
  const auto g = vn100::encode(r);
  out.clear();
  p.feed(g.data(), g.size(), out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_FALSE(out[0].integrated);
  EXPECT_NEAR(out[0].a(2), -9.8, 1e-5);
  EXPECT_NEAR(out[0].g(2), 0.03, 1e-7);
}

// ----------------------------------------------------------------------------- NMEA GNSS

TEST(NmeaGnss, GgaWithGstRmcAndDate) {
  nmea::Fix f;
  f.tod_s = 12 * 3600 + 34 * 60 + 56.5;
  f.year = 2025;
  f.month = 5;
  f.day = 19;
  f.lat_rad = 39.5 * M_PI / 180;
  f.lon_rad = -104.9 * M_PI / 180;
  f.alt_hae_m = 1650.25;
  f.quality = 2;
  f.satellites = 11;
  f.hdop = 0.9;
  f.sigma_lat_m = 0.8;
  f.sigma_lon_m = 0.6;
  f.sigma_alt_m = 1.4;
  f.sog_mps = 10;
  f.cog_rad = M_PI / 2;
  const std::string stream = nmea::encode_rmc(f) + nmea::encode_gga(f) + nmea::encode_gst(f) + "$GPGSV,1,1,00*79\r\n$GPGGA,bad*00\r\n";
  nmea::Parser p;
  std::vector<nmea::Fix> out;
  p.feed(reinterpret_cast<const std::uint8_t*>(stream.data()), stream.size(), out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(p.errors, 1u);
  const auto& o = out[0];
  EXPECT_NEAR(o.tod_s, f.tod_s, 1e-3);
  EXPECT_EQ(o.year, 2025);
  EXPECT_EQ(o.day, 19);
  EXPECT_NEAR(o.lat_rad, f.lat_rad, 1e-9);
  EXPECT_NEAR(o.lon_rad, f.lon_rad, 1e-9);
  EXPECT_NEAR(o.alt_hae_m, 1650.25, 1e-3);
  EXPECT_EQ(o.quality, 2);
  EXPECT_EQ(o.satellites, 11);
  EXPECT_EQ(o.sigma_lat_m, 0.0) << "GST arrives after GGA: it tags the next epoch";
  EXPECT_TRUE(o.have_velocity);
  EXPECT_NEAR(o.sog_mps, 10, 1e-2);
  EXPECT_NEAR(o.cog_rad, M_PI / 2, 1e-4);
  // next epoch: the GST of the previous one is within 1.5 s, so it is used
  f.tod_s += 1.0;
  const std::string next = nmea::encode_gga(f);
  p.feed(reinterpret_cast<const std::uint8_t*>(next.data()), next.size(), out);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_NEAR(out[1].sigma_lat_m, 0.8, 1e-6);
  EXPECT_NEAR(out[1].sigma_alt_m, 1.4, 1e-6);
}

// ----------------------------------------------------------------------------- UBX

TEST(Ubx, NavPvtRoundTripValsetAndResync) {
  ubx::NavPvt p;
  p.itow_ms = 123456789;
  p.year = 2025;
  p.month = 5;
  p.day = 19;
  p.hour = 18;
  p.minute = 54;
  p.second = 46;
  p.nano = -250000;
  p.valid_date = p.valid_time = p.fully_resolved = true;
  p.fix_type = 3;
  p.gnss_fix_ok = true;
  p.carrier_solution = 1;
  p.num_sv = 14;
  p.lat_deg = 39.5000001;
  p.lon_deg = -104.9000002;
  p.height_m = 1650.123;
  p.hacc_m = 1.234;
  p.vacc_m = 2.345;
  p.vel_n = 1.5;
  p.vel_e = -0.5;
  p.vel_d = 0.1;
  p.sacc = 0.25;
  p.pdop = 1.8;
  const auto f = ubx::encode_nav_pvt(p);
  EXPECT_EQ(f.size(), 92u + 8u);
  std::vector<std::uint8_t> stream{0xB5, 0x00, 0xB5, 0x62, 0x01};  // a false start then a truncated header
  stream.insert(stream.end(), f.begin(), f.end());
  ubx::Parser parser;
  std::vector<ubx::Frame> frames;
  parser.feed(stream.data(), 3, frames);
  parser.feed(stream.data() + 3, stream.size() - 3, frames);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(parser.errors, 1u) << "the stray sync in front of the frame";
  ubx::NavPvt o;
  ASSERT_TRUE(ubx::decode_nav_pvt(frames[0], o));
  EXPECT_EQ(o.itow_ms, 123456789u);
  EXPECT_EQ(o.second, 46);
  EXPECT_EQ(o.nano, -250000);
  EXPECT_TRUE(o.fully_resolved);
  EXPECT_EQ(o.carrier_solution, 1);
  EXPECT_EQ(ubx::fix_quality(o), 5);
  EXPECT_NEAR(o.lat_deg, 39.5000001, 1e-7);
  EXPECT_NEAR(o.lon_deg, -104.9000002, 1e-7);
  EXPECT_NEAR(o.height_m, 1650.123, 1e-3);
  EXPECT_NEAR(o.hacc_m, 1.234, 1e-3);
  EXPECT_NEAR(o.vel_e, -0.5, 1e-3);
  EXPECT_NEAR(o.pdop, 1.8, 1e-2);
  EXPECT_EQ(ubx::utc_ns(o), 1747680886LL * 1'000'000'000LL - 250000);  // 2025-05-19 18:54:46 UTC
  // a frame with a bad checksum is skipped and counted
  auto bad = f;
  bad[20] ^= 0xFF;
  parser.feed(bad.data(), bad.size(), frames);
  EXPECT_EQ(frames.size(), 1u);
  EXPECT_EQ(parser.errors, 2u);
  // CFG-VALSET: header, layer, key sizes
  const auto v = ubx::cfg_valset({{0x10740002, 0}, {0x30210001, 200}});
  std::vector<ubx::Frame> vf;
  ubx::Parser vp;
  vp.feed(v.data(), v.size(), vf);
  ASSERT_EQ(vf.size(), 1u) << "the generated frame checks out";
  EXPECT_EQ(vf[0].cls, 0x06);
  EXPECT_EQ(vf[0].id, 0x8A);
  EXPECT_EQ(ubx::hex(vf[0].payload), "00 01 00 00 02 00 74 10 00 01 00 21 30 C8 00");
  const auto cfg = ubx::configure("usb", 200);
  EXPECT_EQ(cfg.size(), 3u);
  EXPECT_EQ(cfg[0][6 + 4], 0x01);  // first key byte of CFG-USBOUTPROT-UBX (0x10780001, little-endian)
  EXPECT_EQ(cfg[0][6 + 7], 0x10);
}

// ----------------------------------------------------------------------------- clock mapping

TEST(ClockMapper, ShedsLatencyKeepsMonotonic) {
  ClockMapper m(2.0, 1e-3);
  const std::int64_t host0 = 1'000'000'000'000LL;
  std::int64_t prev = 0;
  std::uint32_t seed = 1;
  auto noise = [&] {  // 0..20 ms of arrival jitter, with a floor of 2 ms transport latency
    seed = seed * 1103515245u + 12345u;
    return 2'000'000 + static_cast<std::int64_t>((seed >> 8) % 20'000'000);
  };
  std::int64_t last_err = 0;
  for (int i = 0; i < 2000; ++i) {  // 200 Hz for 10 s; the device clock is 3 s behind the host
    const std::int64_t device = static_cast<std::int64_t>(i) * 5'000'000LL;
    const std::int64_t truth = host0 + device + 3'000'000'000LL;
    const std::int64_t tov = m.map(device, truth + noise());
    if (i) EXPECT_GT(tov, prev) << "stamps must never run backwards (sample " << i << ")";
    prev = tov;
    last_err = tov - truth;
  }
  EXPECT_NEAR(static_cast<double>(last_err), 2e6, 1.5e6) << "settles on the minimum latency (2 ms), within the jitter quantisation";
  EXPECT_NEAR(static_cast<double>(*m.offset_ns() - host0), 3e9 + 2e6, 1.5e6);
  // a device power cycle (clock jump) restarts the mapping without a glitch in the sign
  const std::int64_t tov = m.map(0, host0 + 20'000'000'000LL + 2'000'000);
  EXPECT_EQ(tov, host0 + 20'000'000'000LL + 2'000'000);
}

// ----------------------------------------------------------------------------- end to end over ptys

TEST(SerialInput, LogPlayedAsVn100AndUbxOverPtysDrivesTheFilter) {
  Pty imu_pty, gnss_pty;
  ASSERT_TRUE(imu_pty.open());
  ASSERT_TRUE(gnss_pty.open());

  auto app = pntos::cobra::jsoncfg::load_app_config(cobra("configs/pos_ins.json"));
  app.app.logging_level = "WARN";
  pntos::cobra::app::RunOptions o;
  o.progress = false;
  pntos::cobra::Filter filter(app, o);
  std::atomic<int> solutions{0};
  filter.set_solution_callback([&](const pntos::api::Message&) { ++solutions; });

  edge::InputConfig ic;
  ic.type = "serial";
  ic.reorder_ms = 30;
  edge::SerialDeviceConfig imu;
  imu.name = "imu";
  imu.driver = "vn100";
  imu.port = imu_pty.path;
  imu.channel = "/sensor/vn-100/imu";
  imu.time_source = "device";
  edge::SerialDeviceConfig gnss;
  gnss.name = "gnss";
  gnss.driver = "ubx";
  gnss.port = gnss_pty.path;
  gnss.channel = "/sensor/ublox-ZED-F9T/position";
  gnss.time_source = "device";
  ic.devices = {imu, gnss};
  edge::SerialInput input(filter, ic);
  input.start();
  // opening a port flushes stale input (as on a real serial port): write only once both devices are open
  for (int i = 0; i < 100; ++i) {
    const auto d = input.devices();
    if (d[0]["state"] == "open" && d[1]["state"] == "open") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  // play the 60 s log at 20x through the two masters, in log order
  std::uint64_t imu_sent = 0, gnss_sent = 0;
  {
    pntos::cobra::lcm::LcmLogReader reader(cobra("testdata/example_60s.log"));
    const auto wall0 = std::chrono::steady_clock::now();
    std::optional<std::int64_t> t0, first;
    while (auto ev = reader.next()) {
      std::shared_ptr<pntos::api::AspnBase> msg;
      try {
        msg = pntos::cobra::lcm::decode(ev->data);
      } catch (const std::exception&) {
        continue;
      }
      if (!msg) continue;
      auto tov = pntos::cobra::utils::time_of_validity(*msg);
      if (!tov) continue;
      if (!t0) t0 = ev->timestamp_us;
      std::this_thread::sleep_until(wall0 + std::chrono::microseconds((ev->timestamp_us - *t0) / 20));
      if (!first) first = tov->elapsed_nsec;
      if (auto* m = dynamic_cast<const aspn23_eigen::MeasurementImu*>(msg.get()); m && ev->channel == imu.channel) {
        vn100::Sample s;
        s.integrated = true;
        s.a = m->get_meas_accel().head<3>();
        s.g = m->get_meas_gyro().head<3>();
        s.dt_s = 0.01;
        s.device_ns = tov->elapsed_nsec - *first;
        s.absolute_ns = tov->elapsed_nsec;
        imu_pty.write(vn100::encode(s));
        ++imu_sent;
      } else if (auto* p = dynamic_cast<const aspn23_eigen::MeasurementPosition*>(msg.get()); p && ev->channel == gnss.channel) {
        const std::time_t secs = static_cast<std::time_t>(tov->elapsed_nsec / 1'000'000'000LL);
        std::tm tm{};
        gmtime_r(&secs, &tm);
        ubx::NavPvt pvt;
        pvt.year = tm.tm_year + 1900;
        pvt.month = tm.tm_mon + 1;
        pvt.day = tm.tm_mday;
        pvt.hour = tm.tm_hour;
        pvt.minute = tm.tm_min;
        pvt.second = tm.tm_sec;
        pvt.nano = static_cast<std::int32_t>(tov->elapsed_nsec % 1'000'000'000LL);
        pvt.valid_date = pvt.valid_time = pvt.fully_resolved = true;
        pvt.fix_type = 3;
        pvt.gnss_fix_ok = true;
        pvt.num_sv = 12;
        pvt.lat_deg = p->get_term1() * 180 / M_PI;
        pvt.lon_deg = p->get_term2() * 180 / M_PI;
        pvt.height_m = p->get_term3();
        const auto cov = p->get_covariance();
        pvt.hacc_m = std::sqrt(cov(0, 0) + cov(1, 1));
        pvt.vacc_m = std::sqrt(cov(2, 2));
        gnss_pty.write(ubx::encode_nav_pvt(pvt));
        ++gnss_sent;
      }
    }
  }
  ASSERT_GT(imu_sent, 5000u);  // 100 Hz in the example log
  ASSERT_GT(gnss_sent, 50u);   // 1 Hz in the example log
  {
    const auto devs = input.devices();
    ASSERT_EQ(devs.size(), 2u);
    EXPECT_EQ(devs[0]["state"].get<std::string>(), "streaming");
    EXPECT_EQ(devs[1]["state"].get<std::string>(), "streaming");
  }
  // everything written must be pushed: wait, then stop (which drains the reorder buffer)
  for (int i = 0; i < 100 && input.counters().pushed < imu_sent + gnss_sent; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  input.stop();
  const auto c = input.counters();
  EXPECT_EQ(c.pushed, imu_sent + gnss_sent);
  EXPECT_EQ(c.per_channel.at(imu.channel), imu_sent);
  EXPECT_EQ(c.per_channel.at(gnss.channel), gnss_sent);
  EXPECT_EQ(c.decode_failures, 0u);
  EXPECT_GT(solutions.load(), 40) << "the filter aligned and published solutions from the serial path";
  const auto devs = input.devices();
  ASSERT_EQ(devs.size(), 2u);
  EXPECT_EQ(devs[0]["errors"], 0);
  EXPECT_EQ(devs[0]["frames"], imu_sent);
  EXPECT_EQ(devs[1]["frames"], gnss_sent);
  ASSERT_TRUE(input.utc_correction_ns().has_value());
  EXPECT_EQ(*input.utc_correction_ns(), 0) << "device time source: solution times are already UTC";
}

TEST(SerialInput, DeviceComesAndGoes) {
  // no such port: the device reports closed with the reason, and the input keeps running
  pntos::cobra::app::RunOptions o;
  o.progress = false;
  auto app = pntos::cobra::jsoncfg::load_app_config(cobra("configs/pos_ins.json"));
  app.app.logging_level = "ERROR";
  pntos::cobra::Filter filter(app, o);
  edge::InputConfig ic;
  ic.type = "serial";
  edge::SerialDeviceConfig d;
  d.driver = "nmea";
  d.port = "/dev/does-not-exist-pnt-edge";
  d.channel = "/sensor/ublox-ZED-F9T/position";
  ic.devices = {d};
  edge::SerialInput input(filter, ic);
  input.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  auto devs = input.devices();
  ASSERT_EQ(devs.size(), 1u);
  EXPECT_EQ(devs[0]["state"], "closed");
  EXPECT_NE(devs[0]["detail"].get<std::string>().find("does-not-exist"), std::string::npos);
  input.stop();
}
