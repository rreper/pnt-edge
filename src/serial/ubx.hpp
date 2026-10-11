// u-blox UBX protocol: frame parser (sync, class, id, length, Fletcher checksum), NAV-PVT and NAV-TIMELS
// decoders, and CFG-VALSET generation to put a receiver into the mode pnt-edge wants (UBX only, NAV-PVT and
// NAV-TIMELS at a given rate). Pure parser, no I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace edge::serial::ubx {

struct Frame {
  std::uint8_t cls = 0, id = 0;
  std::vector<std::uint8_t> payload;
};

struct NavPvt {
  std::uint32_t itow_ms = 0;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  std::int32_t nano = 0;
  bool valid_date = false, valid_time = false, fully_resolved = false;
  std::uint32_t tacc_ns = 0;
  int fix_type = 0;             ///< 0 none, 2 2D, 3 3D, 4 GNSS+DR, 5 time only
  bool gnss_fix_ok = false, diff_soln = false;
  int carrier_solution = 0;     ///< 0 none, 1 float, 2 fixed
  int num_sv = 0;
  double lon_deg = 0, lat_deg = 0, height_m = 0, hmsl_m = 0, hacc_m = 0, vacc_m = 0;
  double vel_n = 0, vel_e = 0, vel_d = 0, gspeed = 0, head_mot_deg = 0, sacc = 0, headacc_deg = 0, pdop = 0;
};

struct TimeLs {
  int current_ls = 0;
  bool valid = false;
};

class Parser {
 public:
  void feed(const std::uint8_t* data, std::size_t len, std::vector<Frame>& out);
  std::uint64_t frames = 0, errors = 0;

 private:
  std::vector<std::uint8_t> buf_;
};

bool decode_nav_pvt(const Frame& f, NavPvt& out);
bool decode_nav_timels(const Frame& f, TimeLs& out);
/// UTC time of the fix in ns since the Unix epoch (date/time fields plus nano).
std::int64_t utc_ns(const NavPvt& p);
/// GGA fix quality for the solution type (0, 1, 2, 4, 5, 6).
int fix_quality(const NavPvt& p);

std::vector<std::uint8_t> frame(std::uint8_t cls, std::uint8_t id, const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_nav_pvt(const NavPvt& p);
/// CFG-VALSET for the RAM layer: items are (key, little-endian value bytes sized by the key's size bits).
std::vector<std::uint8_t> cfg_valset(const std::vector<std::pair<std::uint32_t, std::uint64_t>>& items, bool ram = true, bool flash = false);
/// The configuration frames for a receiver on `port` ("uart1", "uart2", "usb"): UBX out, NMEA off, NAV-PVT and
/// NAV-TIMELS every navigation epoch, measurement period `rate_ms`.
std::vector<std::vector<std::uint8_t>> configure(const std::string& port, int rate_ms, bool persist = false);
/// Hex dump "B5 62 ..." for logs and docs.
std::string hex(const std::vector<std::uint8_t>& bytes);

}  // namespace edge::serial::ubx
