#include "vn100.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace edge::serial::vn100 {

namespace {

constexpr double kDeg = M_PI / 180.0;

// Field sizes in bytes per group (VN-100 / VN-200 / VN-300 binary output protocol). Groups with variable-size
// fields (GPS satellite info, raw measurements) are listed up to the last fixed field.
const std::vector<std::vector<int>> kGroupSizes = {
    {8, 8, 8, 12, 16, 12, 24, 12, 12, 24, 20, 28, 2, 4, 8},     // 1 common
    {8, 8, 8, 2, 8, 8, 8, 4, 4, 1},                             // 2 time
    {2, 12, 12, 12, 4, 4, 16, 12, 12, 12, 12, 2},               // 3 imu
    {8, 8, 2, 1, 1, 24, 24, 12, 12, 12, 4, 4, 8, 28},           // 4 gps
    {2, 12, 16, 36, 12, 12, 12, 12, 12},                        // 5 attitude
    {2, 24, 24, 12, 12, 12, 12, 12, 12, 4, 4},                  // 6 ins
};

template <class T>
T le(const std::uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}
Eigen::Vector3d vec3f(const std::uint8_t* p) { return {le<float>(p), le<float>(p + 4), le<float>(p + 8)}; }
template <class T>
void put(std::vector<std::uint8_t>& out, T v) {
  std::uint8_t b[sizeof v];
  std::memcpy(b, &v, sizeof v);
  out.insert(out.end(), b, b + sizeof v);
}
void put3f(std::vector<std::uint8_t>& out, const Eigen::Vector3d& v) {
  for (int k = 0; k < 3; ++k) put<float>(out, static_cast<float>(v(k)));
}

std::uint8_t xor_checksum(const std::string& s) {
  std::uint8_t c = 0;
  for (unsigned char ch : s) c ^= ch;
  return c;
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (true) {
    auto pos = s.find(',', start);
    out.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
    if (pos == std::string::npos) break;
    start = pos + 1;
  }
  return out;
}

bool to_double(const std::string& s, double& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  v = std::strtod(s.c_str(), &end);
  return end && *end == '\0';
}

}  // namespace

std::uint16_t crc16(const std::uint8_t* data, std::size_t len) {
  std::uint16_t crc = 0;
  for (std::size_t i = 0; i < len; ++i) {
    crc = static_cast<std::uint16_t>((crc >> 8) | (crc << 8));
    crc ^= data[i];
    crc ^= static_cast<std::uint16_t>((crc & 0xff) >> 4);
    crc ^= static_cast<std::uint16_t>((crc << 8) << 4);
    crc ^= static_cast<std::uint16_t>(((crc & 0xff) << 4) << 1);
  }
  return crc;
}

std::string ascii_command(const std::string& body_in) {
  std::string body = body_in;
  if (!body.empty() && body[0] == '$') body.erase(0, 1);
  while (!body.empty() && (body.back() == '\r' || body.back() == '\n')) body.pop_back();
  if (auto star = body.find('*'); star != std::string::npos) body.erase(star);
  char cs[4];
  std::snprintf(cs, sizeof cs, "%02X", xor_checksum(body));
  return "$" + body + "*" + cs + "\r\n";
}

std::vector<std::uint8_t> binary_frame(std::uint16_t group1_fields, const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> f{0xFA, 0x01};
  put<std::uint16_t>(f, group1_fields);
  f.insert(f.end(), payload.begin(), payload.end());
  const std::uint16_t crc = crc16(f.data() + 1, f.size() - 1);
  f.push_back(static_cast<std::uint8_t>(crc >> 8));
  f.push_back(static_cast<std::uint8_t>(crc & 0xff));
  return f;
}

std::vector<std::uint8_t> encode(const Sample& s) {
  std::uint16_t fields = 0;
  std::vector<std::uint8_t> p;
  if (s.device_ns) {
    fields |= 0x0001;
    put<std::uint64_t>(p, static_cast<std::uint64_t>(*s.device_ns));
  }
  if (s.absolute_ns) {
    fields |= 0x0002;
    put<std::uint64_t>(p, static_cast<std::uint64_t>(*s.absolute_ns));
  }
  if (s.integrated) {
    fields |= 0x0800;  // DeltaThetaVel: dtime, dtheta (deg), dvel
    put<float>(p, static_cast<float>(s.dt_s));
    put3f(p, s.g / kDeg);
    put3f(p, s.a);
  } else {
    fields |= 0x0200;  // Imu: uncompensated accel, gyro
    put3f(p, s.a);
    put3f(p, s.g);
  }
  return binary_frame(fields, p);
}

std::string encode_ascii(const Sample& s) {
  char buf[256];
  if (s.integrated)
    std::snprintf(buf, sizeof buf, "VNDTV,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f", s.dt_s, s.g(0) / kDeg, s.g(1) / kDeg, s.g(2) / kDeg, s.a(0), s.a(1), s.a(2));
  else
    std::snprintf(buf, sizeof buf, "VNIMU,0,0,0,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,0,0", s.a(0), s.a(1), s.a(2), s.g(0), s.g(1), s.g(2));
  return ascii_command(buf);
}

void Parser::feed(const std::uint8_t* data, std::size_t len, std::vector<Sample>& out) {
  buf_.insert(buf_.end(), data, data + len);
  std::size_t pos = 0;
  while (pos < buf_.size()) {
    const std::uint8_t b = buf_[pos];
    if (b == 0xFA) {
      std::size_t consumed = 0;
      // parse at the current position: rotate so the frame starts at buf_[0] (cheap: pos is usually 0)
      if (pos) buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos)), pos = 0;
      if (!parse_binary(out, consumed)) break;  // incomplete: wait for more bytes
      if (consumed == 0) {
        ++pos;  // not a frame after all
        continue;
      }
      pos = consumed;
      continue;
    }
    if (b == '$') {
      auto nl = std::find(buf_.begin() + static_cast<std::ptrdiff_t>(pos), buf_.end(), '\n');
      if (nl == buf_.end()) {
        if (buf_.size() - pos > 512) {  // no sentence is that long: drop the '$'
          ++errors;
          ++pos;
          continue;
        }
        break;
      }
      std::string line(buf_.begin() + static_cast<std::ptrdiff_t>(pos), nl);
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
      if (parse_ascii(line, out)) ++frames;
      else ++errors;
      pos = static_cast<std::size_t>(nl - buf_.begin()) + 1;
      continue;
    }
    ++pos;  // noise between frames
  }
  buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos));
}

bool Parser::parse_binary(std::vector<Sample>& out, std::size_t& consumed) {
  consumed = 0;
  if (buf_.size() < 2) return false;
  const std::uint8_t groups = buf_[1];
  if (groups == 0 || (groups & 0x80)) {
    consumed = 0;
    return true;  // not a frame
  }
  int ngroups = 0;
  for (int g = 0; g < 7; ++g)
    if (groups & (1u << g)) ++ngroups;
  const std::size_t header = 2 + 2 * static_cast<std::size_t>(ngroups);
  if (buf_.size() < header) return false;
  std::size_t payload = 0;
  std::size_t mask_pos = 2;
  for (int g = 0; g < 7; ++g) {
    if (!(groups & (1u << g))) continue;
    const std::uint16_t fields = le<std::uint16_t>(buf_.data() + mask_pos);
    mask_pos += 2;
    if (g >= static_cast<int>(kGroupSizes.size())) return consumed = 0, true;
    for (int f = 0; f < 16; ++f) {
      if (!(fields & (1u << f))) continue;
      if (f >= static_cast<int>(kGroupSizes[static_cast<std::size_t>(g)].size())) return consumed = 0, true;
      payload += static_cast<std::size_t>(kGroupSizes[static_cast<std::size_t>(g)][static_cast<std::size_t>(f)]);
    }
  }
  const std::size_t total = header + payload + 2;
  if (buf_.size() < total) return false;
  if (crc16(buf_.data() + 1, total - 1) != 0) {
    ++errors;
    consumed = 0;
    return true;
  }
  ++frames;
  Sample s;
  bool have_delta = false, have_accel = false, have_gyro = false;
  Eigen::Vector3d accel = Eigen::Vector3d::Zero(), gyro = Eigen::Vector3d::Zero();
  const std::uint8_t* p = buf_.data() + header;
  mask_pos = 2;
  for (int g = 0; g < 7; ++g) {
    if (!(groups & (1u << g))) continue;
    const std::uint16_t fields = le<std::uint16_t>(buf_.data() + mask_pos);
    mask_pos += 2;
    for (int f = 0; f < 16; ++f) {
      if (!(fields & (1u << f))) continue;
      const int size = kGroupSizes[static_cast<std::size_t>(g)][static_cast<std::size_t>(f)];
      if (g == 0) {
        if (f == 0) s.device_ns = static_cast<std::int64_t>(le<std::uint64_t>(p));
        else if (f == 1) s.absolute_ns = static_cast<std::int64_t>(le<std::uint64_t>(p));
        else if (f == 5) gyro = vec3f(p), have_gyro = true;
        else if (f == 8) accel = vec3f(p), have_accel = true;
        else if (f == 9) {
          if (!have_accel) accel = vec3f(p), have_accel = true;
          if (!have_gyro) gyro = vec3f(p + 12), have_gyro = true;
        } else if (f == 11) {
          s.dt_s = le<float>(p);
          s.g = vec3f(p + 4) * kDeg;
          s.a = vec3f(p + 16);
          have_delta = true;
        }
      } else if (g == 1) {
        if (f == 0 && !s.device_ns) s.device_ns = static_cast<std::int64_t>(le<std::uint64_t>(p));
        else if (f == 1 && !s.absolute_ns) s.absolute_ns = static_cast<std::int64_t>(le<std::uint64_t>(p));
      } else if (g == 2) {
        if (f == 6) {
          s.dt_s = le<float>(p);
          s.g = vec3f(p + 4) * kDeg;
          have_delta = true;
        } else if (f == 7) s.a = vec3f(p);
        else if (f == 2 && !have_accel) accel = vec3f(p), have_accel = true;
        else if (f == 3 && !have_gyro) gyro = vec3f(p), have_gyro = true;
        else if (f == 9) accel = vec3f(p), have_accel = true;
        else if (f == 10) gyro = vec3f(p), have_gyro = true;
      }
      p += size;
    }
  }
  if (have_delta) {
    s.integrated = true;
    out.push_back(s);
  } else if (have_accel && have_gyro) {
    s.integrated = false;
    s.a = accel;
    s.g = gyro;
    out.push_back(s);
  } else {
    ++ignored;
  }
  consumed = total;
  return true;
}

bool Parser::parse_ascii(const std::string& line, std::vector<Sample>& out) {
  if (line.size() < 6 || line[0] != '$') return false;
  const auto star = line.rfind('*');
  if (star == std::string::npos || star + 3 != line.size()) return false;
  const std::string body = line.substr(1, star - 1);
  const unsigned want = static_cast<unsigned>(std::strtoul(line.c_str() + star + 1, nullptr, 16));
  if (want != xor_checksum(body)) return false;
  const auto f = split(body);
  if (f.empty()) return false;
  const std::string& tag = f[0];
  if (tag == "VNDTV" && f.size() >= 8) {
    Sample s;
    s.integrated = true;
    double v[7];
    for (int k = 0; k < 7; ++k)
      if (!to_double(f[static_cast<std::size_t>(k + 1)], v[k])) return false;
    s.dt_s = v[0];
    s.g = Eigen::Vector3d(v[1], v[2], v[3]) * kDeg;
    s.a = Eigen::Vector3d(v[4], v[5], v[6]);
    out.push_back(s);
    return true;
  }
  if ((tag == "VNIMU" && f.size() >= 10) || (tag == "VNYMR" && f.size() >= 13)) {
    const std::size_t a0 = tag == "VNIMU" ? 4 : 7;  // VNIMU: mag(3) accel(3) gyro(3); VNYMR: ypr(3) mag(3) accel(3) gyro(3)
    Sample s;
    double v[6];
    for (std::size_t k = 0; k < 6; ++k)
      if (!to_double(f[a0 + k], v[k])) return false;
    s.a = Eigen::Vector3d(v[0], v[1], v[2]);
    s.g = Eigen::Vector3d(v[3], v[4], v[5]);
    out.push_back(s);
    return true;
  }
  if (tag == "VNERR") {
    last_error = f.size() > 1 ? f[1] : "?";
    ++ignored;
    return true;
  }
  ++ignored;  // other async output or command replies ($VNWRG, $VNRRG, ...)
  return true;
}

}  // namespace edge::serial::vn100
