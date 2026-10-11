// A raw serial port (or any character device / pty) with poll-based reads. Baud rates are set through termios2
// (BOTHER), so non-standard rates work; on a pty or a non-tty file the rate is ignored.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace edge::serial {

class Port {
 public:
  Port() = default;
  ~Port();
  Port(const Port&) = delete;
  Port& operator=(const Port&) = delete;

  /// Opens `path` raw, 8N1, no flow control, at `baud`. On failure returns false and fills `error`.
  bool open(const std::string& path, int baud, std::string* error = nullptr);
  void close();
  bool is_open() const { return fd_ >= 0; }
  int fd() const { return fd_; }

  /// Waits up to `timeout_ms` for data. Returns the number of bytes read, 0 on timeout, -1 when the port is
  /// gone (unplugged, closed peer): the caller then closes and retries opening.
  int read(std::uint8_t* buf, std::size_t len, int timeout_ms);
  bool write(const std::uint8_t* data, std::size_t len);
  bool write(const std::string& s) { return write(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }

 private:
  int fd_ = -1;
};

/// CLOCK_REALTIME in ns since the Unix epoch.
std::int64_t host_now_ns();

}  // namespace edge::serial
