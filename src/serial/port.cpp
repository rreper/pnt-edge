#include "port.hpp"

#include <asm/termbits.h>  // termios2 / BOTHER (no <termios.h> in this TU: the two headers clash)
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace edge::serial {

Port::~Port() { close(); }

bool Port::open(const std::string& path, int baud, std::string* error) {
  close();
  int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    if (error) *error = path + ": " + std::strerror(errno);
    return false;
  }
  termios2 t{};
  if (::ioctl(fd, TCGETS2, &t) == 0) {
    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
    t.c_oflag &= ~OPOST;
    t.c_lflag &= ~(ECHO | ECHOE | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS | CBAUD);
    t.c_cflag |= CS8 | CLOCAL | CREAD | BOTHER;
    t.c_ispeed = t.c_ospeed = static_cast<unsigned>(baud > 0 ? baud : 115200);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (::ioctl(fd, TCSETS2, &t) != 0) {
      if (error) *error = path + ": cannot set " + std::to_string(baud) + " baud: " + std::strerror(errno);
      ::close(fd);
      return false;
    }
    ::ioctl(fd, TCFLSH, TCIFLUSH);
  } else if (errno != ENOTTY) {  // a FIFO or a plain file is fine (tests, recordings); other errors are not
    if (error) *error = path + ": " + std::strerror(errno);
    ::close(fd);
    return false;
  }
  fd_ = fd;
  return true;
}

void Port::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

int Port::read(std::uint8_t* buf, std::size_t len, int timeout_ms) {
  if (fd_ < 0) return -1;
  pollfd p{fd_, POLLIN, 0};
  int r = ::poll(&p, 1, timeout_ms);
  if (r < 0) return errno == EINTR ? 0 : -1;
  if (r == 0) return 0;
  if (p.revents & (POLLERR | POLLNVAL)) return -1;
  ssize_t n = ::read(fd_, buf, len);
  if (n > 0) return static_cast<int>(n);
  if (n == 0) return (p.revents & POLLHUP) ? -1 : 0;
  if (errno == EAGAIN || errno == EINTR) return (p.revents & POLLHUP) ? -1 : 0;
  return -1;  // EIO on a pty whose master closed, ENODEV/ENXIO on unplug
}

bool Port::write(const std::uint8_t* data, std::size_t len) {
  std::size_t done = 0;
  while (fd_ >= 0 && done < len) {
    ssize_t n = ::write(fd_, data + done, len - done);
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) {
        pollfd p{fd_, POLLOUT, 0};
        ::poll(&p, 1, 200);
        continue;
      }
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return done == len;
}

std::int64_t host_now_ns() {
  timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

}  // namespace edge::serial
