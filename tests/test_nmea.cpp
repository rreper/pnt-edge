#include "integrity.hpp"
#include "nmea.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace edge;

TEST(Nmea, ChecksumAndFormatting) {
  // the classic example sentence: checksum 47
  EXPECT_EQ(nmea_checksum("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"), "47");
  EXPECT_EQ(nmea_wrap("GPHDT,90.00,T"), "$GPHDT,90.00,T*" + nmea_checksum("GPHDT,90.00,T") + "\r\n");
  const double d2r = M_PI / 180.0;
  EXPECT_EQ(nmea_lat(48.1173 * d2r), "4807.0380000,N");
  EXPECT_EQ(nmea_lat(-33.8688 * d2r), "3352.1280000,S");
  EXPECT_EQ(nmea_lon(11.5167 * d2r), "01131.0020000,E");
  EXPECT_EQ(nmea_lon(-84.1097484635 * d2r), "08406.5849078,W");
  // 2025-05-19 18:54:49.63 UTC
  const std::int64_t t = 1747680889'630'000'000LL;
  EXPECT_EQ(nmea_time(t), "185449.63");
  EXPECT_EQ(nmea_date(t), "190525");
}

TEST(Nmea, SentencesFromAPva) {
  Pva p;
  p.tov_ns = 1747680889'630'000'000LL;
  p.lat_rad = 39.7575 * M_PI / 180;
  p.lon_rad = -84.1097 * M_PI / 180;
  p.alt_hae_m = 225.16;
  p.vn = 10.0;
  p.ve = 10.0;  // course 45 deg, 14.14 m/s = 27.49 kn
  p.yaw_rad = -0.5 * M_PI / 180;  // wraps to 359.50
  p.roll_rad = 0.01;
  p.pitch_rad = -0.02;
  p.sigma_n = 0.5;
  p.sigma_e = 0.7;
  p.sigma_d = 1.2;
  p.sigma_heading_rad = 0.5 * M_PI / 180;
  NmeaOptions o;  // no geoid: GGA altitude is HAE with an empty separation field
  const std::string g = gga(p, o);
  EXPECT_EQ(g.substr(0, 16), "$GNGGA,185449.63");
  EXPECT_NE(g.find(",1,,,225.16,M,,M,,*"), std::string::npos);
  EXPECT_EQ(g.substr(g.size() - 2), "\r\n");
  const std::string r = rmc(p, o);
  EXPECT_NE(r.find(",A,3945.4500000,N,08406.5820000,W,27.49,45.00,190525,,,A*"), std::string::npos);
  EXPECT_NE(vtg(p, o).find("$GNVTG,45.00,T,,M,27.49,N,50.91,K,A*"), std::string::npos);
  EXPECT_NE(hdt(p, o).find("$GNHDT,359.50,T*"), std::string::npos);
  EXPECT_NE(gst(p, o).find(",0.85,0.70,0.50,90.0,0.50,0.70,1.20*"), std::string::npos) << gst(p, o);
  EXPECT_NE(zda(p, o).find("$GNZDA,185449.63,19,05,2025,00,00*"), std::string::npos);
  EXPECT_NE(pashr(p, o).find("$PASHR,185449.63,359.50,T,+0.57,-1.15,,,,0.500,1,1*"), std::string::npos);
  // every sentence verifies
  for (const auto& s : sentences(p, o)) {
    ASSERT_EQ(s.front(), '$');
    const auto star = s.rfind('*');
    EXPECT_EQ(s.substr(star + 1, 2), nmea_checksum(s.substr(1, star - 1)));
  }
  o.gga = o.rmc = false;
  EXPECT_EQ(sentences(p, o).size(), 5u);
  o.talker = "GP";
  EXPECT_EQ(hdt(p, o).substr(0, 6), "$GPHDT");
  // dead reckoning flag
  p.fix_quality = 6;
  EXPECT_NE(rmc(p, o).find(",E*"), std::string::npos);
}

TEST(Integrity, FixQualityAndIntegSentence) {
  edge::IntegrityStatus none;
  EXPECT_EQ(edge::fix_quality_for(none, 1), 1);
  edge::IntegrityStatus s;
  s.present = true;
  s.pace = "PRIMARY";
  s.hpl_m = 23.46;
  s.hal_m = 75.0;
  s.fault_detection = true;
  EXPECT_EQ(edge::fix_quality_for(s, 1), 1);
  s.pace = "ALTERNATE";
  s.excluded = {"gnss"};
  s.alarm = true;
  EXPECT_EQ(edge::fix_quality_for(s, 1), 6);
  const std::string sentence = edge::integ_sentence(s);
  EXPECT_EQ(sentence.substr(0, 40), "$PPNT,INTEG,ALTERNATE,23.5,75.0,1,1,gnss");
  EXPECT_EQ(sentence.substr(sentence.size() - 2), "\r\n");
  const auto star = sentence.find('*');
  EXPECT_EQ(sentence.substr(star + 1, 2), edge::nmea_checksum(sentence.substr(1, star - 1)));
  s.pace = "CONTINGENCY";
  EXPECT_EQ(edge::fix_quality_for(s, 1), 6);
  s.pace = "EMERGENCY";
  s.excluded = {"gnss", "cell"};
  EXPECT_EQ(edge::fix_quality_for(s, 1), 0);
  EXPECT_NE(edge::integ_sentence(s).find(",gnss+cell*"), std::string::npos);
  auto j = edge::integrity_to_json(s);
  EXPECT_EQ(j["pace"], "EMERGENCY");
  EXPECT_EQ(j["excluded"].size(), 2u);
  EXPECT_TRUE(edge::integrity_to_json(none).empty());
}
