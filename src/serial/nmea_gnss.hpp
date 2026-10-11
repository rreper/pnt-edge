// NMEA 0183 GNSS receiver input: GGA / GNS (position), GST (sigmas), RMC (date, velocity), ZDA (date). One Fix
// is produced per GGA or GNS sentence with a valid fix, carrying the latest GST and RMC of the same epoch.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace edge::serial::nmea {

struct Fix {
  double tod_s = -1;                 ///< UTC time of day in seconds from the sentence, -1 if absent
  int year = 0, month = 0, day = 0;  ///< from RMC / ZDA when seen (0 = unknown)
  double lat_rad = 0, lon_rad = 0, alt_hae_m = 0;
  int quality = 0;                   ///< GGA fix quality (GNS mode mapped onto it)
  int satellites = 0;
  double hdop = 0;
  double sigma_lat_m = 0, sigma_lon_m = 0, sigma_alt_m = 0;  ///< from GST (0 = none in this epoch)
  bool have_velocity = false;
  double sog_mps = 0, cog_rad = 0;   ///< from RMC
};

class Parser {
 public:
  void feed(const std::uint8_t* data, std::size_t len, std::vector<Fix>& out);
  std::uint64_t sentences = 0, errors = 0;
  std::string last_talker;

 private:
  void handle(const std::string& body, std::vector<Fix>& out);
  std::string buf_;
  int year_ = 0, month_ = 0, day_ = 0;
  double gst_tod_ = -1, gst_lat_ = 0, gst_lon_ = 0, gst_alt_ = 0;
  double rmc_tod_ = -1, sog_ = 0, cog_ = 0;
  double last_fix_tod_ = -1;
};

bool checksum_ok(const std::string& line);                 ///< "$...*XX" with optional CR/LF
std::vector<std::string> fields(const std::string& body);  ///< comma-separated, keeps empty fields
double parse_angle(const std::string& ddmm, const std::string& hemisphere);  ///< ddmm.mmmm / dddmm.mmmm -> rad
double parse_tod(const std::string& hhmmss);  ///< "hhmmss.ss" -> seconds of day, -1 if empty

/// Encoders for the simulator and the tests.
std::string encode_gga(const Fix& f, const std::string& talker = "GP");
std::string encode_gst(const Fix& f, const std::string& talker = "GP");
std::string encode_rmc(const Fix& f, const std::string& talker = "GP");

}  // namespace edge::serial::nmea
