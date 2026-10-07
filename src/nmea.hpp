// NMEA 0183 sentence generation from a Cobra PVA solution (tactical interface output).
// Sentences: GGA, RMC, VTG, HDT, GST, ZDA and the Ashtech-style PASHR attitude sentence. Every sentence is
// returned with its checksum and CRLF, ready to send. Talker id is configurable (GN by default).
#pragma once

#include <pntos/api/api.hpp>
#include <pntos/cobra/utils/geoid.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace edge {

struct Pva {
  std::int64_t tov_ns = 0;  ///< UTC time of validity in ns since the Unix epoch (GPS-UTC offset applied by the caller)
  double lat_rad = 0, lon_rad = 0, alt_hae_m = 0;
  double vn = 0, ve = 0, vd = 0;          ///< m/s NED
  double roll_rad = 0, pitch_rad = 0, yaw_rad = 0;
  double sigma_n = 0, sigma_e = 0, sigma_d = 0;  ///< m, 1-sigma
  double sigma_heading_rad = 0;
  int fix_quality = 1;                    ///< GGA field 6: 0 invalid, 1 GPS, 2 DGPS, 4 RTK fixed, 5 RTK float, 6 estimated (dead reckoning)
  int satellites = 0;                     ///< 0 if unknown
  double hdop = 0;                        ///< 0 if unknown
};

/// Builds Pva from a Cobra PVA message (geodetic, with quaternion). Returns false if the message is not a PVA.
bool pva_from_message(const pntos::api::Message& message, Pva& out, std::int64_t utc_offset_ns = 0);

std::string nmea_checksum(const std::string& body);  ///< two upper-case hex digits of the XOR of `body`
std::string nmea_wrap(const std::string& body);      ///< "$" + body + "*" + checksum + CRLF

struct NmeaOptions {
  std::string talker = "GN";
  std::shared_ptr<const pntos::cobra::nav::Geoid> geoid;  ///< for the MSL altitude and geoid separation in GGA
  bool gga = true, rmc = true, vtg = true, hdt = true, gst = true, zda = true, pashr = true;
  bool integ = true;  ///< $PPNT,INTEG after the epoch's sentences, when an integrity status exists (see integrity.hpp)
};

std::string gga(const Pva& p, const NmeaOptions& o);
std::string rmc(const Pva& p, const NmeaOptions& o);
std::string vtg(const Pva& p, const NmeaOptions& o);
std::string hdt(const Pva& p, const NmeaOptions& o);
std::string gst(const Pva& p, const NmeaOptions& o);
std::string zda(const Pva& p, const NmeaOptions& o);
std::string pashr(const Pva& p, const NmeaOptions& o);
/// All enabled sentences for one epoch, in the order above.
std::vector<std::string> sentences(const Pva& p, const NmeaOptions& o);

// helpers exposed for tests
std::string nmea_lat(double lat_rad);   ///< "ddmm.mmmmmmm,N"
std::string nmea_lon(double lon_rad);   ///< "dddmm.mmmmmmm,E"
std::string nmea_time(std::int64_t utc_ns);  ///< "hhmmss.ss"
std::string nmea_date(std::int64_t utc_ns);  ///< "ddmmyy"

}  // namespace edge
