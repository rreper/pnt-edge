// VectorNav VN-100 serial protocol: ASCII async sentences ($VNDTV, $VNIMU, $VNYMR, $VNERR) and the binary output
// frames (sync 0xFA, groups 1..6, CRC16). Produces IMU samples: integrated (delta velocity, delta angle) when the
// device sends DeltaThetaVel, otherwise sampled (specific force, angular rate). Pure parser, no I/O.
#pragma once

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace edge::serial::vn100 {

struct Sample {
  bool integrated = false;  ///< true: a is delta velocity (m/s) and g delta angle (rad) over dt_s; false: m/s^2 and rad/s
  double dt_s = 0;          ///< integration interval reported by the device (0 if unknown)
  Eigen::Vector3d a = Eigen::Vector3d::Zero(), g = Eigen::Vector3d::Zero();
  std::optional<std::int64_t> device_ns;    ///< TimeStartup: ns since the device powered up
  std::optional<std::int64_t> absolute_ns;  ///< TimeGps: ns since the GPS epoch, when the device has it
};

class Parser {
 public:
  /// Consumes bytes and appends the complete samples found.
  void feed(const std::uint8_t* data, std::size_t len, std::vector<Sample>& out);
  std::uint64_t frames = 0;   ///< valid frames / sentences
  std::uint64_t errors = 0;   ///< checksum / CRC / framing failures
  std::uint64_t ignored = 0;  ///< valid frames without IMU content
  std::string last_error;     ///< the last $VNERR code seen

 private:
  bool parse_binary(std::vector<Sample>& out, std::size_t& consumed);
  bool parse_ascii(const std::string& line, std::vector<Sample>& out);
  std::vector<std::uint8_t> buf_;
};

/// VectorNav CRC16 (polynomial 0x1021, initial 0) over `len` bytes.
std::uint16_t crc16(const std::uint8_t* data, std::size_t len);
/// Wraps a command body ("VNWRG,75,1,4,01,0801", with or without a leading '$') as "$body*XX\r\n".
std::string ascii_command(const std::string& body);
/// Encodes a binary output frame carrying only group 1 fields `fields` with the given payload (tests, simulator).
std::vector<std::uint8_t> binary_frame(std::uint16_t group1_fields, const std::vector<std::uint8_t>& payload);
/// Encodes a sample as a group 1 frame: TimeStartup and/or TimeGps when present, DeltaThetaVel (integrated) or
/// Imu (sampled).
std::vector<std::uint8_t> encode(const Sample& s);
/// Encodes a sample as an ASCII sentence ($VNDTV for integrated, $VNIMU otherwise).
std::string encode_ascii(const Sample& s);

}  // namespace edge::serial::vn100
