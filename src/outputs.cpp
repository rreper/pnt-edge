#include "outputs.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace edge {

NmeaOutput::NmeaOutput(const std::vector<std::string>& udp_targets, const std::string& bind_address, int tcp_port) {
  for (const auto& t : udp_targets) {
    const auto colon = t.rfind(':');
    if (colon == std::string::npos) throw std::runtime_error("NMEA udp target must be host:port: " + t);
    const std::string host = t.substr(0, colon);
    const int port = std::stoi(t.substr(colon + 1));
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) throw std::runtime_error("cannot resolve NMEA udp target " + host);
    sockaddr_in addr = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) throw std::runtime_error("cannot open a UDP socket");
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    std::vector<unsigned char> bytes(sizeof addr);
    std::memcpy(bytes.data(), &addr, sizeof addr);
    udp_.push_back({fd, bytes});
  }
  if (tcp_port > 0) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(tcp_port));
    addr.sin_addr.s_addr = inet_addr(bind_address.c_str());
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || ::listen(listen_fd_, 8) < 0) {
      const std::string err = std::strerror(errno);
      ::close(listen_fd_);
      listen_fd_ = -1;
      throw std::runtime_error("cannot listen on " + bind_address + ":" + std::to_string(tcp_port) + ": " + err);
    }
    accept_thread_ = std::thread([this] { accept_loop(); });
  }
}

NmeaOutput::~NmeaOutput() {
  stop_ = true;
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  std::lock_guard lk(clients_mutex_);
  for (int fd : clients_) ::close(fd);
  for (auto& [fd, _] : udp_) ::close(fd);
}

void NmeaOutput::accept_loop() {
  while (!stop_) {
    sockaddr_in peer{};
    socklen_t len = sizeof peer;
    const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
    if (fd < 0) {
      if (stop_) break;
      continue;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    std::lock_guard lk(clients_mutex_);
    clients_.push_back(fd);
  }
}

std::size_t NmeaOutput::tcp_clients() const {
  std::lock_guard lk(clients_mutex_);
  return clients_.size();
}

void NmeaOutput::send(const std::string& s) {
  for (auto& [fd, addr] : udp_) ::sendto(fd, s.data(), s.size(), 0, reinterpret_cast<const sockaddr*>(addr.data()), static_cast<socklen_t>(addr.size()));
  std::lock_guard lk(clients_mutex_);
  for (auto it = clients_.begin(); it != clients_.end();) {
    if (::send(*it, s.data(), s.size(), MSG_NOSIGNAL) < 0) {
      ::close(*it);
      it = clients_.erase(it);
    } else {
      ++it;
    }
  }
  ++sent_;
}

}  // namespace edge
