#include "nmea_gnss.hpp"

#include "../nmea.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace edge::serial::nmea {

namespace {
constexpr double kDeg = M_PI / 180.0;
constexpr double kKnot = 0.514444;

bool to_double(const std::string& s, double& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  v = std::strtod(s.c_str(), &end);
  return end && *end == '\0';
}
double num(const std::string& s) {
  double v = 0;
  return to_double(s, v) ? v : 0.0;
}
}  // namespace

bool checksum_ok(const std::string& line_in) {
  std::string line = line_in;
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  if (line.size() < 4 || line[0] != '$') return false;
  const auto star = line.rfind('*');
  if (star == std::string::npos || star + 3 != line.size()) return false;
  return edge::nmea_checksum(line.substr(1, star - 1)) == line.substr(star + 1);
}

std::vector<std::string> fields(const std::string& body) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (true) {
    auto pos = body.find(',', start);
    out.push_back(body.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
    if (pos == std::string::npos) break;
    start = pos + 1;
  }
  return out;
}

double parse_angle(const std::string& ddmm, const std::string& hemisphere) {
  double v = 0;
  if (!to_double(ddmm, v)) return 0;
  const double deg = std::floor(v / 100.0);
  const double minutes = v - deg * 100.0;
  double out = (deg + minutes / 60.0) * kDeg;
  if (hemisphere == "S" || hemisphere == "W") out = -out;
  return out;
}

double parse_tod(const std::string& s) {
  double v = 0;
  if (!to_double(s, v)) return -1;
  const int hh = static_cast<int>(v / 10000.0);
  const int mm = static_cast<int>((v - hh * 10000.0) / 100.0);
  const double ss = v - hh * 10000.0 - mm * 100.0;
  return hh * 3600.0 + mm * 60.0 + ss;
}

void Parser::feed(const std::uint8_t* data, std::size_t len, std::vector<Fix>& out) {
  buf_.append(reinterpret_cast<const char*>(data), len);
  std::size_t pos = 0;
  while (true) {
    const auto nl = buf_.find('\n', pos);
    if (nl == std::string::npos) break;
    std::string line = buf_.substr(pos, nl - pos);
    pos = nl + 1;
    const auto dollar = line.find('$');
    if (dollar == std::string::npos) continue;
    line.erase(0, dollar);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (!checksum_ok(line)) {
      ++errors;
      continue;
    }
    ++sentences;
    handle(line.substr(1, line.rfind('*') - 1), out);
  }
  buf_.erase(0, pos);
  if (buf_.size() > 1024) {  // no line ending in a kilobyte: not NMEA
    buf_.clear();
    ++errors;
  }
}

void Parser::handle(const std::string& body, std::vector<Fix>& out) {
  const auto f = fields(body);
  if (f.empty() || f[0].size() < 5) return;
  const std::string talker = f[0].substr(0, f[0].size() - 3), type = f[0].substr(f[0].size() - 3);
  if (type == "RMC" && f.size() >= 10) {
    rmc_tod_ = parse_tod(f[1]);
    sog_ = num(f[7]) * kKnot;
    cog_ = num(f[8]) * kDeg;
    if (f[9].size() == 6) {
      day_ = std::atoi(f[9].substr(0, 2).c_str());
      month_ = std::atoi(f[9].substr(2, 2).c_str());
      year_ = 2000 + std::atoi(f[9].substr(4, 2).c_str());
    }
    if (f[2] != "A") rmc_tod_ = -1;
    return;
  }
  if (type == "ZDA" && f.size() >= 5) {
    day_ = std::atoi(f[2].c_str());
    month_ = std::atoi(f[3].c_str());
    year_ = std::atoi(f[4].c_str());
    return;
  }
  if (type == "GST" && f.size() >= 9) {
    gst_tod_ = parse_tod(f[1]);
    gst_lat_ = num(f[6]);
    gst_lon_ = num(f[7]);
    gst_alt_ = num(f[8]);
    return;
  }
  const bool gga = type == "GGA" && f.size() >= 12, gns = type == "GNS" && f.size() >= 11;
  if (!gga && !gns) return;
  Fix fix;
  fix.tod_s = parse_tod(f[1]);
  fix.lat_rad = parse_angle(f[2], f[3]);
  fix.lon_rad = parse_angle(f[4], f[5]);
  if (gga) {
    fix.quality = std::atoi(f[6].c_str());
    fix.satellites = std::atoi(f[7].c_str());
    fix.hdop = num(f[8]);
    fix.alt_hae_m = num(f[9]) + num(f[11]);
  } else {
    int q = 0;  // the best mode indicator among the constellations
    for (char c : f[6]) {
      int m = c == 'A' ? 1 : c == 'D' ? 2 : c == 'P' ? 3 : c == 'R' ? 4 : c == 'F' ? 5 : c == 'E' ? 6 : 0;
      if (m == 4 || (m > q && q != 4)) q = m;
    }
    fix.quality = q;
    fix.satellites = std::atoi(f[7].c_str());
    fix.hdop = num(f[8]);
    fix.alt_hae_m = num(f[9]) + num(f[10]);
  }
  if (fix.quality == 0 || f[2].empty()) return;
  if (fix.tod_s >= 0 && fix.tod_s == last_fix_tod_) return;  // GGA and GNS of the same epoch
  last_fix_tod_ = fix.tod_s;
  last_talker = talker;
  fix.year = year_;
  fix.month = month_;
  fix.day = day_;
  if (gst_tod_ >= 0 && std::fabs(gst_tod_ - fix.tod_s) < 1.5) {
    fix.sigma_lat_m = gst_lat_;
    fix.sigma_lon_m = gst_lon_;
    fix.sigma_alt_m = gst_alt_;
  }
  if (rmc_tod_ >= 0 && std::fabs(rmc_tod_ - fix.tod_s) < 1.5) {
    fix.have_velocity = true;
    fix.sog_mps = sog_;
    fix.cog_rad = cog_;
  }
  out.push_back(fix);
}

namespace {
std::string tod_str(double tod) {
  const int hh = static_cast<int>(tod / 3600), mm = static_cast<int>((tod - hh * 3600) / 60);
  const double ss = tod - hh * 3600 - mm * 60;
  char b[32];
  std::snprintf(b, sizeof b, "%02d%02d%05.2f", hh, mm, ss);
  return b;
}
}  // namespace

std::string encode_gga(const Fix& f, const std::string& talker) {
  char b[256];
  std::snprintf(b, sizeof b, "%sGGA,%s,%s,%s,%d,%02d,%.1f,%.3f,M,0.0,M,,", talker.c_str(), tod_str(f.tod_s).c_str(), edge::nmea_lat(f.lat_rad).c_str(),
                edge::nmea_lon(f.lon_rad).c_str(), f.quality, f.satellites, f.hdop, f.alt_hae_m);
  return edge::nmea_wrap(b);
}

std::string encode_gst(const Fix& f, const std::string& talker) {
  char b[256];
  std::snprintf(b, sizeof b, "%sGST,%s,%.2f,%.2f,%.2f,0.0,%.3f,%.3f,%.3f", talker.c_str(), tod_str(f.tod_s).c_str(), f.sigma_lat_m, f.sigma_lat_m, f.sigma_lon_m,
                f.sigma_lat_m, f.sigma_lon_m, f.sigma_alt_m);
  return edge::nmea_wrap(b);
}

std::string encode_rmc(const Fix& f, const std::string& talker) {
  char b[256];
  std::snprintf(b, sizeof b, "%sRMC,%s,A,%s,%s,%.3f,%.2f,%02d%02d%02d,,,A", talker.c_str(), tod_str(f.tod_s).c_str(), edge::nmea_lat(f.lat_rad).c_str(),
                edge::nmea_lon(f.lon_rad).c_str(), f.sog_mps / kKnot, f.cog_rad / kDeg, f.day, f.month, f.year % 100);
  return edge::nmea_wrap(b);
}

}  // namespace edge::serial::nmea
