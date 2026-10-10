#include "web/auth.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <fstream>
#include <random>

namespace edge::auth {

// ----------------------------------------------------------------------------- SHA-256 (FIPS 180-4)
namespace {
constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::uint8_t buf[64];
  std::size_t buf_len = 0;
  std::uint64_t total = 0;
  void block(const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) | (std::uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
      const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = S0 + maj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const std::uint8_t* p, std::size_t n) {
    total += n;
    while (n > 0) {
      const std::size_t take = std::min(n, 64 - buf_len);
      std::memcpy(buf + buf_len, p, take);
      buf_len += take; p += take; n -= take;
      if (buf_len == 64) { block(buf); buf_len = 0; }
    }
  }
  std::vector<std::uint8_t> finish() {
    const std::uint64_t bits = total * 8;
    std::uint8_t pad = 0x80;
    update(&pad, 1);
    std::uint8_t zero = 0;
    while (buf_len != 56) update(&zero, 1);
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(len, 8);
    std::vector<std::uint8_t> out(32);
    for (int i = 0; i < 8; ++i) {
      out[4 * i] = h[i] >> 24; out[4 * i + 1] = h[i] >> 16; out[4 * i + 2] = h[i] >> 8; out[4 * i + 3] = h[i];
    }
    return out;
  }
};
}  // namespace

std::vector<std::uint8_t> sha256(const std::string& data) {
  Sha256 s;
  s.update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
  return s.finish();
}

std::vector<std::uint8_t> hmac_sha256(const std::vector<std::uint8_t>& key_in, const std::string& data) {
  std::vector<std::uint8_t> key = key_in;
  if (key.size() > 64) key = sha256(std::string(key.begin(), key.end()));
  key.resize(64, 0);
  std::string ipad(64, 0), opad(64, 0);
  for (int i = 0; i < 64; ++i) {
    ipad[i] = static_cast<char>(key[i] ^ 0x36);
    opad[i] = static_cast<char>(key[i] ^ 0x5c);
  }
  auto inner = sha256(ipad + data);
  return sha256(opad + std::string(inner.begin(), inner.end()));
}

std::vector<std::uint8_t> pbkdf2_sha256(const std::string& password, const std::vector<std::uint8_t>& salt, int iterations, std::size_t dklen) {
  std::vector<std::uint8_t> key(password.begin(), password.end()), out;
  for (std::uint32_t block = 1; out.size() < dklen; ++block) {
    std::string msg(salt.begin(), salt.end());
    msg += static_cast<char>(block >> 24); msg += static_cast<char>(block >> 16); msg += static_cast<char>(block >> 8); msg += static_cast<char>(block);
    auto u = hmac_sha256(key, msg);
    auto t = u;
    for (int i = 1; i < iterations; ++i) {
      u = hmac_sha256(key, std::string(u.begin(), u.end()));
      for (std::size_t k = 0; k < t.size(); ++k) t[k] ^= u[k];
    }
    out.insert(out.end(), t.begin(), t.end());
  }
  out.resize(dklen);
  return out;
}

std::string hex(const std::vector<std::uint8_t>& b) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (auto x : b) { s += d[x >> 4]; s += d[x & 15]; }
  return s;
}

std::vector<std::uint8_t> unhex(const std::string& h) {
  std::vector<std::uint8_t> out;
  auto v = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0; };
  for (std::size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<std::uint8_t>(v(h[i]) * 16 + v(h[i + 1])));
  return out;
}

std::string random_hex(std::size_t bytes) {
  std::random_device rd;
  std::vector<std::uint8_t> b(bytes);
  for (auto& x : b) x = static_cast<std::uint8_t>(rd());
  return hex(b);
}

// ----------------------------------------------------------------------------- users
bool Users::load(const std::string& path, std::string* error) {
  users_.clear();
  std::ifstream in(path);
  if (!in) return true;  // absent: no users yet
  try {
    auto j = nlohmann::json::parse(in);
    for (const auto& u : j.value("users", nlohmann::json::array())) {
      User user;
      user.name = u.at("name").get<std::string>();
      user.salt_hex = u.at("salt").get<std::string>();
      user.hash_hex = u.at("hash").get<std::string>();
      user.iterations = u.value("iterations", 100000);
      users_[user.name] = user;
    }
    return true;
  } catch (const std::exception& e) {
    if (error) *error = path + ": " + e.what();
    return false;
  }
}

bool Users::save(const std::string& path, std::string* error) const {
  nlohmann::json j{{"users", nlohmann::json::array()}};
  for (const auto& [n, u] : users_) j["users"].push_back({{"name", u.name}, {"salt", u.salt_hex}, {"iterations", u.iterations}, {"hash", u.hash_hex}});
  std::ofstream out(path);
  if (!out) {
    if (error) *error = "cannot write " + path;
    return false;
  }
  out << j.dump(2) << "\n";
  return true;
}

void Users::set_password(const std::string& name, const std::string& password, int iterations) {
  User u;
  u.name = name;
  u.iterations = iterations;
  u.salt_hex = random_hex(16);
  u.hash_hex = hex(pbkdf2_sha256(password, unhex(u.salt_hex), iterations));
  users_[name] = u;
}

bool Users::verify(const std::string& name, const std::string& password) const {
  auto it = users_.find(name);
  if (it == users_.end()) return false;
  const auto computed = hex(pbkdf2_sha256(password, unhex(it->second.salt_hex), it->second.iterations));
  // constant-time compare
  if (computed.size() != it->second.hash_hex.size()) return false;
  unsigned diff = 0;
  for (std::size_t i = 0; i < computed.size(); ++i) diff |= static_cast<unsigned>(computed[i] ^ it->second.hash_hex[i]);
  return diff == 0;
}

std::vector<std::string> Users::names() const {
  std::vector<std::string> out;
  for (const auto& [n, u] : users_) out.push_back(n);
  return out;
}

// ----------------------------------------------------------------------------- sessions
namespace {
double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

std::string Sessions::create(const std::string& user) {
  std::lock_guard lk(m_);
  const std::string token = random_hex(24);
  entries_[token] = {user, now_s() + ttl_};
  for (auto it = entries_.begin(); it != entries_.end();)
    it = it->second.expires < now_s() ? entries_.erase(it) : std::next(it);
  return token;
}

std::optional<std::string> Sessions::user_for(const std::string& token) {
  std::lock_guard lk(m_);
  auto it = entries_.find(token);
  if (it == entries_.end() || it->second.expires < now_s()) return std::nullopt;
  it->second.expires = now_s() + ttl_;
  return it->second.user;
}

void Sessions::revoke(const std::string& token) {
  std::lock_guard lk(m_);
  entries_.erase(token);
}

}  // namespace edge::auth
