#include "nmea.hpp"

#include <pntos/cobra/utils/aspn.hpp>
#include <pntos/cobra/utils/navutils.hpp>

#include <cmath>
#include <cstdio>
#include <ctime>

namespace edge {

namespace {
constexpr double kR2D = 180.0 / M_PI;
constexpr double kMps2Knots = 1.943844492;

std::string fmt(const char* f, double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, f, v);
  return buf;
}
std::string dm(double deg_abs, int deg_width) {
  const int d = static_cast<int>(deg_abs);
  const double m = (deg_abs - d) * 60.0;
  char buf[48];
  std::snprintf(buf, sizeof buf, "%0*d%010.7f", deg_width, d, m);
  return buf;
}
double wrap360(double deg) {
  deg = std::fmod(deg, 360.0);
  return deg < 0 ? deg + 360.0 : deg;
}
std::tm utc_tm(std::int64_t ns, double* frac) {
  const std::time_t s = static_cast<std::time_t>(ns / 1000000000);
  if (frac) *frac = (ns % 1000000000) * 1e-9;
  std::tm t{};
  gmtime_r(&s, &t);
  return t;
}
}  // namespace

bool pva_from_message(const pntos::api::Message& message, Pva& out, std::int64_t utc_offset_ns) {
  auto pva = message.as<pntos::cobra::utils::PVA>();
  if (!pva) return false;
  auto q = pntos::cobra::utils::quaternion(*pva);
  out = Pva{};
  out.tov_ns = pva->get_time_of_validity().get_elapsed_nsec() + utc_offset_ns;
  out.lat_rad = pva->get_p1();
  out.lon_rad = pva->get_p2();
  out.alt_hae_m = pva->get_p3();
  out.vn = pva->get_v1();
  out.ve = pva->get_v2();
  out.vd = pva->get_v3();
  if (q) {
    const auto rpy = pntos::cobra::nav::dcm_to_rpy(pntos::cobra::nav::quat_to_dcm(*q));
    out.roll_rad = rpy(0);
    out.pitch_rad = rpy(1);
    out.yaw_rad = rpy(2);
  }
  const auto c = pva->get_covariance();
  auto sig = [&](int i) { return c.rows() > i && c(i, i) > 0 ? std::sqrt(c(i, i)) : 0.0; };
  out.sigma_n = sig(0);
  out.sigma_e = sig(1);
  out.sigma_d = sig(2);
  out.sigma_heading_rad = sig(8);
  out.fix_quality = 1;
  return true;
}

std::string nmea_checksum(const std::string& body) {
  unsigned char x = 0;
  for (unsigned char ch : body) x ^= ch;
  char buf[4];
  std::snprintf(buf, sizeof buf, "%02X", x);
  return buf;
}

std::string nmea_wrap(const std::string& body) { return "$" + body + "*" + nmea_checksum(body) + "\r\n"; }

std::string nmea_lat(double lat_rad) {
  const double deg = lat_rad * kR2D;
  return dm(std::fabs(deg), 2) + (deg < 0 ? ",S" : ",N");
}
std::string nmea_lon(double lon_rad) {
  const double deg = lon_rad * kR2D;
  return dm(std::fabs(deg), 3) + (deg < 0 ? ",W" : ",E");
}
std::string nmea_time(std::int64_t utc_ns) {
  double frac = 0;
  const std::tm t = utc_tm(utc_ns, &frac);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d%02d%05.2f", t.tm_hour, t.tm_min, t.tm_sec + frac);
  return buf;
}
std::string nmea_date(std::int64_t utc_ns) {
  const std::tm t = utc_tm(utc_ns, nullptr);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d%02d%02d", t.tm_mday, t.tm_mon + 1, t.tm_year % 100);
  return buf;
}

std::string gga(const Pva& p, const NmeaOptions& o) {
  const double undulation = o.geoid ? o.geoid->undulation(p.lat_rad, p.lon_rad) : 0.0;
  const double msl = p.alt_hae_m - undulation;
  std::string b = o.talker + "GGA," + nmea_time(p.tov_ns) + "," + nmea_lat(p.lat_rad) + "," + nmea_lon(p.lon_rad) + "," +
                  std::to_string(p.fix_quality) + "," + (p.satellites > 0 ? fmt("%02.0f", p.satellites) : std::string("")) + "," +
                  (p.hdop > 0 ? fmt("%.1f", p.hdop) : std::string("")) + "," + fmt("%.2f", msl) + ",M," +
                  (o.geoid ? fmt("%.2f", undulation) : std::string("")) + ",M,,";
  return nmea_wrap(b);
}

std::string rmc(const Pva& p, const NmeaOptions& o) {
  const double speed = std::hypot(p.vn, p.ve) * kMps2Knots;
  const double course = wrap360(std::atan2(p.ve, p.vn) * kR2D);
  std::string b = o.talker + "RMC," + nmea_time(p.tov_ns) + "," + (p.fix_quality > 0 ? "A" : "V") + "," + nmea_lat(p.lat_rad) + "," +
                  nmea_lon(p.lon_rad) + "," + fmt("%.2f", speed) + "," + fmt("%.2f", course) + "," + nmea_date(p.tov_ns) + ",,," +
                  (p.fix_quality == 6 ? "E" : "A");
  return nmea_wrap(b);
}

std::string vtg(const Pva& p, const NmeaOptions& o) {
  const double speed_mps = std::hypot(p.vn, p.ve);
  const double course = wrap360(std::atan2(p.ve, p.vn) * kR2D);
  std::string b = o.talker + "VTG," + fmt("%.2f", course) + ",T,,M," + fmt("%.2f", speed_mps * kMps2Knots) + ",N," +
                  fmt("%.2f", speed_mps * 3.6) + ",K," + (p.fix_quality == 6 ? "E" : "A");
  return nmea_wrap(b);
}

std::string hdt(const Pva& p, const NmeaOptions& o) {
  return nmea_wrap(o.talker + "HDT," + fmt("%.2f", wrap360(p.yaw_rad * kR2D)) + ",T");
}

std::string gst(const Pva& p, const NmeaOptions& o) {
  const double rms = std::sqrt((p.sigma_n * p.sigma_n + p.sigma_e * p.sigma_e + p.sigma_d * p.sigma_d) / 3.0);
  const double major = std::max(p.sigma_n, p.sigma_e), minor = std::min(p.sigma_n, p.sigma_e);
  const double orient = p.sigma_n >= p.sigma_e ? 0.0 : 90.0;
  std::string b = o.talker + "GST," + nmea_time(p.tov_ns) + "," + fmt("%.2f", rms) + "," + fmt("%.2f", major) + "," + fmt("%.2f", minor) +
                  "," + fmt("%.1f", orient) + "," + fmt("%.2f", p.sigma_n) + "," + fmt("%.2f", p.sigma_e) + "," + fmt("%.2f", p.sigma_d);
  return nmea_wrap(b);
}

std::string zda(const Pva& p, const NmeaOptions& o) {
  const std::tm t = utc_tm(p.tov_ns, nullptr);
  char buf[48];
  std::snprintf(buf, sizeof buf, ",%02d,%02d,%04d,00,00", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
  return nmea_wrap(o.talker + "ZDA," + nmea_time(p.tov_ns) + buf);
}

std::string pashr(const Pva& p, const NmeaOptions&) {
  // $PASHR,hhmmss.sss,HHH.HH,T,RRR.RR,PPP.PP,heave,roll acc,pitch acc,heading acc,flag1,flag2
  const double heading_acc = p.sigma_heading_rad * kR2D;
  std::string b = "PASHR," + nmea_time(p.tov_ns) + "," + fmt("%.2f", wrap360(p.yaw_rad * kR2D)) + ",T," + fmt("%+.2f", p.roll_rad * kR2D) + "," +
                  fmt("%+.2f", p.pitch_rad * kR2D) + ",,," + "," + fmt("%.3f", heading_acc) + ",1,1";
  return nmea_wrap(b);
}

std::vector<std::string> sentences(const Pva& p, const NmeaOptions& o) {
  std::vector<std::string> out;
  if (o.gga) out.push_back(gga(p, o));
  if (o.rmc) out.push_back(rmc(p, o));
  if (o.vtg) out.push_back(vtg(p, o));
  if (o.hdt) out.push_back(hdt(p, o));
  if (o.gst) out.push_back(gst(p, o));
  if (o.zda) out.push_back(zda(p, o));
  if (o.pashr) out.push_back(pashr(p, o));
  return out;
}

}  // namespace edge
