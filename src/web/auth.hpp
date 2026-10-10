// Maintenance login: a users file with PBKDF2-HMAC-SHA256 password hashes (self-contained SHA-256, no OpenSSL),
// in-memory sessions, and a bearer token for read-only fleet access.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace edge::auth {

std::vector<std::uint8_t> sha256(const std::string& data);
std::vector<std::uint8_t> hmac_sha256(const std::vector<std::uint8_t>& key, const std::string& data);
std::vector<std::uint8_t> pbkdf2_sha256(const std::string& password, const std::vector<std::uint8_t>& salt, int iterations, std::size_t dklen = 32);
std::string hex(const std::vector<std::uint8_t>& b);
std::vector<std::uint8_t> unhex(const std::string& h);
std::string random_hex(std::size_t bytes);  ///< from std::random_device

struct User {
  std::string name;
  std::string salt_hex, hash_hex;
  int iterations = 100000;
};

/// The users file: {"users": [{"name": ..., "salt": hex, "iterations": n, "hash": hex}]}.
class Users {
 public:
  bool load(const std::string& path, std::string* error = nullptr);  ///< false if unreadable (an absent file is empty)
  bool save(const std::string& path, std::string* error = nullptr) const;
  void set_password(const std::string& name, const std::string& password, int iterations = 100000);
  bool verify(const std::string& name, const std::string& password) const;
  bool empty() const { return users_.empty(); }
  std::vector<std::string> names() const;

 private:
  std::map<std::string, User> users_;
};

class Sessions {
 public:
  explicit Sessions(double ttl_seconds = 12 * 3600) : ttl_(ttl_seconds) {}
  std::string create(const std::string& user);
  std::optional<std::string> user_for(const std::string& token);  ///< refreshes the expiry
  void revoke(const std::string& token);

 private:
  struct Entry {
    std::string user;
    double expires;
  };
  double ttl_;
  std::mutex m_;
  std::map<std::string, Entry> entries_;
};

}  // namespace edge::auth
