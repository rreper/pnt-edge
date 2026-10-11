#include "ubx.hpp"

#include <cstdio>
#include <cstring>

namespace edge::serial::ubx {

namespace {
template <class T>
T le(const std::uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}
template <class T>
void put(std::vector<std::uint8_t>& out, T v) {
  std::uint8_t b[sizeof v];
  std::memcpy(b, &v, sizeof v);
  out.insert(out.end(), b, b + sizeof v);
}
void fletcher(const std::uint8_t* p, std::size_t n, std::uint8_t& a, std::uint8_t& b) {
  for (std::size_t i = 0; i < n; ++i) {
    a = static_cast<std::uint8_t>(a + p[i]);
    b = static_cast<std::uint8_t>(b + a);
  }
}
std::int64_t days_from_civil(int y, int m, int d) {  // Howard Hinnant
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
}  // namespace

void Parser::feed(const std::uint8_t* data, std::size_t len, std::vector<Frame>& out) {
  buf_.insert(buf_.end(), data, data + len);
  std::size_t pos = 0;
  while (pos < buf_.size()) {
    if (buf_[pos] != 0xB5) {
      ++pos;
      continue;
    }
    if (buf_.size() - pos < 2) break;
    if (buf_[pos + 1] != 0x62) {
      ++pos;
      continue;
    }
    if (buf_.size() - pos < 6) break;
    const std::uint16_t n = le<std::uint16_t>(buf_.data() + pos + 4);
    if (n > 4096) {  // not a frame
      ++errors;
      ++pos;
      continue;
    }
    const std::size_t total = 6 + n + 2;
    if (buf_.size() - pos < total) {
      // Incomplete: either the frame is still arriving, or this is a stray sync whose "length" came from the
      // bytes of a real frame behind it. If a complete, valid frame starts later in the buffer, take that one.
      std::size_t q = pos + 2;
      bool found = false;
      while (q + 8 <= buf_.size()) {
        if (buf_[q] == 0xB5 && buf_[q + 1] == 0x62) {
          const std::uint16_t m = le<std::uint16_t>(buf_.data() + q + 4);
          if (buf_.size() - q >= 8u + m) {
            std::uint8_t a = 0, b = 0;
            fletcher(buf_.data() + q + 2, 4u + m, a, b);
            if (a == buf_[q + 6 + m] && b == buf_[q + 7 + m]) {
              found = true;
              break;
            }
          }
        }
        ++q;
      }
      if (!found) break;
      ++errors;
      pos = q;
      continue;
    }
    std::uint8_t a = 0, b = 0;
    fletcher(buf_.data() + pos + 2, 4 + n, a, b);
    if (a != buf_[pos + 6 + n] || b != buf_[pos + 7 + n]) {
      ++errors;
      ++pos;
      continue;
    }
    Frame f;
    f.cls = buf_[pos + 2];
    f.id = buf_[pos + 3];
    f.payload.assign(buf_.begin() + static_cast<std::ptrdiff_t>(pos + 6), buf_.begin() + static_cast<std::ptrdiff_t>(pos + 6 + n));
    out.push_back(std::move(f));
    ++frames;
    pos += total;
  }
  buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos));
}

bool decode_nav_pvt(const Frame& f, NavPvt& o) {
  if (f.cls != 0x01 || f.id != 0x07 || f.payload.size() < 92) return false;
  const std::uint8_t* p = f.payload.data();
  o.itow_ms = le<std::uint32_t>(p);
  o.year = le<std::uint16_t>(p + 4);
  o.month = p[6];
  o.day = p[7];
  o.hour = p[8];
  o.minute = p[9];
  o.second = p[10];
  o.valid_date = p[11] & 1;
  o.valid_time = p[11] & 2;
  o.fully_resolved = p[11] & 4;
  o.tacc_ns = le<std::uint32_t>(p + 12);
  o.nano = le<std::int32_t>(p + 16);
  o.fix_type = p[20];
  o.gnss_fix_ok = p[21] & 1;
  o.diff_soln = p[21] & 2;
  o.carrier_solution = (p[21] >> 6) & 3;
  o.num_sv = p[23];
  o.lon_deg = le<std::int32_t>(p + 24) * 1e-7;
  o.lat_deg = le<std::int32_t>(p + 28) * 1e-7;
  o.height_m = le<std::int32_t>(p + 32) * 1e-3;
  o.hmsl_m = le<std::int32_t>(p + 36) * 1e-3;
  o.hacc_m = le<std::uint32_t>(p + 40) * 1e-3;
  o.vacc_m = le<std::uint32_t>(p + 44) * 1e-3;
  o.vel_n = le<std::int32_t>(p + 48) * 1e-3;
  o.vel_e = le<std::int32_t>(p + 52) * 1e-3;
  o.vel_d = le<std::int32_t>(p + 56) * 1e-3;
  o.gspeed = le<std::int32_t>(p + 60) * 1e-3;
  o.head_mot_deg = le<std::int32_t>(p + 64) * 1e-5;
  o.sacc = le<std::uint32_t>(p + 68) * 1e-3;
  o.headacc_deg = le<std::uint32_t>(p + 72) * 1e-5;
  o.pdop = le<std::uint16_t>(p + 76) * 0.01;
  return true;
}

bool decode_nav_timels(const Frame& f, TimeLs& o) {
  if (f.cls != 0x01 || f.id != 0x26 || f.payload.size() < 24) return false;
  o.current_ls = static_cast<std::int8_t>(f.payload[9]);
  o.valid = f.payload[23] & 1;
  return true;
}

std::int64_t utc_ns(const NavPvt& p) {
  const std::int64_t days = days_from_civil(p.year, p.month, p.day);
  const std::int64_t s = days * 86400 + p.hour * 3600 + p.minute * 60 + p.second;
  return s * 1'000'000'000LL + p.nano;
}

int fix_quality(const NavPvt& p) {
  if (!p.gnss_fix_ok || p.fix_type == 0 || p.fix_type == 5) return 0;
  if (p.carrier_solution == 2) return 4;
  if (p.carrier_solution == 1) return 5;
  if (p.fix_type == 1) return 6;
  return p.diff_soln ? 2 : 1;
}

std::vector<std::uint8_t> frame(std::uint8_t cls, std::uint8_t id, const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> f{0xB5, 0x62, cls, id};
  put<std::uint16_t>(f, static_cast<std::uint16_t>(payload.size()));
  f.insert(f.end(), payload.begin(), payload.end());
  std::uint8_t a = 0, b = 0;
  fletcher(f.data() + 2, f.size() - 2, a, b);
  f.push_back(a);
  f.push_back(b);
  return f;
}

std::vector<std::uint8_t> encode_nav_pvt(const NavPvt& p) {
  std::vector<std::uint8_t> d;
  put<std::uint32_t>(d, p.itow_ms);
  put<std::uint16_t>(d, static_cast<std::uint16_t>(p.year));
  d.push_back(static_cast<std::uint8_t>(p.month));
  d.push_back(static_cast<std::uint8_t>(p.day));
  d.push_back(static_cast<std::uint8_t>(p.hour));
  d.push_back(static_cast<std::uint8_t>(p.minute));
  d.push_back(static_cast<std::uint8_t>(p.second));
  d.push_back(static_cast<std::uint8_t>((p.valid_date ? 1 : 0) | (p.valid_time ? 2 : 0) | (p.fully_resolved ? 4 : 0)));
  put<std::uint32_t>(d, p.tacc_ns);
  put<std::int32_t>(d, p.nano);
  d.push_back(static_cast<std::uint8_t>(p.fix_type));
  d.push_back(static_cast<std::uint8_t>((p.gnss_fix_ok ? 1 : 0) | (p.diff_soln ? 2 : 0) | ((p.carrier_solution & 3) << 6)));
  d.push_back(0);
  d.push_back(static_cast<std::uint8_t>(p.num_sv));
  put<std::int32_t>(d, static_cast<std::int32_t>(p.lon_deg * 1e7));
  put<std::int32_t>(d, static_cast<std::int32_t>(p.lat_deg * 1e7));
  put<std::int32_t>(d, static_cast<std::int32_t>(p.height_m * 1e3));
  put<std::int32_t>(d, static_cast<std::int32_t>(p.hmsl_m * 1e3));
  put<std::uint32_t>(d, static_cast<std::uint32_t>(p.hacc_m * 1e3));
  put<std::uint32_t>(d, static_cast<std::uint32_t>(p.vacc_m * 1e3));
  put<std::int32_t>(d, static_cast<std::int32_t>(p.vel_n * 1e3));
  put<std::int32_t>(d, static_cast<std::int32_t>(p.vel_e * 1e3));
  put<std::int32_t>(d, static_cast<std::int32_t>(p.vel_d * 1e3));
  put<std::int32_t>(d, static_cast<std::int32_t>(p.gspeed * 1e3));
  put<std::int32_t>(d, static_cast<std::int32_t>(p.head_mot_deg * 1e5));
  put<std::uint32_t>(d, static_cast<std::uint32_t>(p.sacc * 1e3));
  put<std::uint32_t>(d, static_cast<std::uint32_t>(p.headacc_deg * 1e5));
  put<std::uint16_t>(d, static_cast<std::uint16_t>(p.pdop * 100));
  put<std::uint16_t>(d, 0);  // flags3
  put<std::uint32_t>(d, 0);  // reserved
  put<std::int32_t>(d, 0);   // headVeh
  put<std::int16_t>(d, 0);   // magDec
  put<std::uint16_t>(d, 0);  // magAcc
  return frame(0x01, 0x07, d);
}

std::vector<std::uint8_t> cfg_valset(const std::vector<std::pair<std::uint32_t, std::uint64_t>>& items, bool ram, bool flash) {
  std::vector<std::uint8_t> d{0x00, static_cast<std::uint8_t>((ram ? 1 : 0) | (flash ? 4 : 0)), 0x00, 0x00};
  for (const auto& [key, value] : items) {
    put<std::uint32_t>(d, key);
    const int size_bits = (key >> 28) & 7;
    const int n = size_bits <= 2 ? 1 : size_bits == 3 ? 2 : size_bits == 4 ? 4 : 8;
    for (int k = 0; k < n; ++k) d.push_back(static_cast<std::uint8_t>(value >> (8 * k)));
  }
  return frame(0x06, 0x8A, d);
}

std::vector<std::vector<std::uint8_t>> configure(const std::string& port, int rate_ms, bool persist) {
  // CFG-MSGOUT-UBX_NAV_PVT_* and _TIMELS_*, CFG-*OUTPROT-UBX/NMEA per port, CFG-RATE-MEAS/NAV.
  std::uint32_t pvt, timels, prot_ubx, prot_nmea;
  if (port == "usb") pvt = 0x20910009, timels = 0x20910028, prot_ubx = 0x10780001, prot_nmea = 0x10780002;
  else if (port == "uart2") pvt = 0x20910008, timels = 0x20910027, prot_ubx = 0x10760001, prot_nmea = 0x10760002;
  else pvt = 0x20910007, timels = 0x20910026, prot_ubx = 0x10740001, prot_nmea = 0x10740002;
  std::vector<std::vector<std::uint8_t>> out;
  out.push_back(cfg_valset({{prot_ubx, 1}, {prot_nmea, 0}}, true, persist));
  out.push_back(cfg_valset({{0x30210001, static_cast<std::uint64_t>(rate_ms)}, {0x30210002, 1}}, true, persist));
  out.push_back(cfg_valset({{pvt, 1}, {timels, 1}}, true, persist));
  return out;
}

std::string hex(const std::vector<std::uint8_t>& bytes) {
  std::string s;
  char b[4];
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    std::snprintf(b, sizeof b, "%02X", bytes[i]);
    if (i) s += ' ';
    s += b;
  }
  return s;
}

}  // namespace edge::serial::ubx
