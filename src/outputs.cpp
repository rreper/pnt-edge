#include "outputs.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <thread>
#include <cstring>
#include <stdexcept>

namespace edge {

NmeaOutput::NmeaOutput(const std::vector<std::string>& udp_targets, const std::string& bind_address, int tcp_port, int command_udp_port) {
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
  if (tcp_port != 0) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(tcp_port > 0 ? tcp_port : 0));
    addr.sin_addr.s_addr = inet_addr(bind_address.c_str());
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || ::listen(listen_fd_, 8) < 0) {
      const std::string err = std::strerror(errno);
      ::close(listen_fd_);
      listen_fd_ = -1;
      throw std::runtime_error("cannot listen on " + bind_address + ":" + std::to_string(tcp_port) + ": " + err);
    }
    socklen_t alen = sizeof addr;
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &alen);
    tcp_port_ = ntohs(addr.sin_port);
    accept_thread_ = std::thread([this] { accept_loop(); });
  }
  if (command_udp_port != 0) {
    command_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    ::setsockopt(command_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(command_udp_port > 0 ? command_udp_port : 0));
    addr.sin_addr.s_addr = inet_addr(bind_address.c_str());
    if (::bind(command_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
      const std::string err = std::strerror(errno);
      ::close(command_fd_);
      command_fd_ = -1;
      throw std::runtime_error("cannot bind the command port " + bind_address + ":" + std::to_string(command_udp_port) + ": " + err);
    }
    socklen_t alen = sizeof addr;
    ::getsockname(command_fd_, reinterpret_cast<sockaddr*>(&addr), &alen);
    command_port_ = ntohs(addr.sin_port);
  }
  if (listen_fd_ >= 0 || command_fd_ >= 0) io_thread_ = std::thread([this] { io_loop(); });
}

void NmeaOutput::set_command_handler(CommandHandler h) {
  std::lock_guard lk(handler_mutex_);
  handler_ = std::move(h);
}

void NmeaOutput::handle_line(const std::string& line, const std::string& from, int reply_fd, const sockaddr_in* reply_to) {
  CommandHandler h;
  {
    std::lock_guard lk(handler_mutex_);
    h = handler_;
  }
  if (!h) return;
  ++commands_;
  for (const auto& reply : h(line, from)) {
    if (reply_fd >= 0) ::send(reply_fd, reply.data(), reply.size(), MSG_NOSIGNAL);
    else if (reply_to && command_fd_ >= 0) ::sendto(command_fd_, reply.data(), reply.size(), 0, reinterpret_cast<const sockaddr*>(reply_to), sizeof *reply_to);
  }
}

void NmeaOutput::io_loop() {
  while (!stop_) {
    std::vector<pollfd> fds;
    std::vector<int> client_fds;
    if (command_fd_ >= 0) fds.push_back({command_fd_, POLLIN, 0});
    {
      std::lock_guard lk(clients_mutex_);
      client_fds = clients_;
    }
    for (int fd : client_fds) fds.push_back({fd, POLLIN, 0});
    if (fds.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    if (::poll(fds.data(), fds.size(), 200) <= 0) continue;
    char buf[2048];
    for (const auto& p : fds) {
      if (!(p.revents & (POLLIN | POLLHUP | POLLERR))) continue;
      if (p.fd == command_fd_) {
        sockaddr_in peer{};
        socklen_t len = sizeof peer;
        const ssize_t n = ::recvfrom(command_fd_, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr*>(&peer), &len);
        if (n <= 0) continue;
        std::string data(buf, static_cast<std::size_t>(n));
        const std::string from = std::string(inet_ntoa(peer.sin_addr)) + ":" + std::to_string(ntohs(peer.sin_port));
        std::size_t pos = 0;
        while (pos < data.size()) {
          std::size_t end = data.find('\n', pos);
          if (end == std::string::npos) end = data.size();
          const std::string line = data.substr(pos, end - pos);
          if (line.find('$') != std::string::npos) handle_line(line, from, -1, &peer);
          pos = end + 1;
        }
        continue;
      }
      const ssize_t n = ::recv(p.fd, buf, sizeof buf, 0);
      if (n <= 0) {  // closed by the peer
        std::lock_guard lk(clients_mutex_);
        auto it = std::find(clients_.begin(), clients_.end(), p.fd);
        if (it != clients_.end()) {
          ::close(*it);
          clients_.erase(it);
        }
        rx_.erase(p.fd);
        continue;
      }
      std::string& rx = rx_[p.fd];
      rx.append(buf, static_cast<std::size_t>(n));
      if (rx.size() > 4096) rx.erase(0, rx.size() - 4096);
      std::size_t nl;
      while ((nl = rx.find('\n')) != std::string::npos) {
        const std::string line = rx.substr(0, nl);
        rx.erase(0, nl + 1);
        sockaddr_in peer{};
        socklen_t len = sizeof peer;
        ::getpeername(p.fd, reinterpret_cast<sockaddr*>(&peer), &len);
        const std::string from = std::string(inet_ntoa(peer.sin_addr)) + ":" + std::to_string(ntohs(peer.sin_port));
        if (line.find('$') != std::string::npos) handle_line(line, from, p.fd, nullptr);
      }
    }
  }
}

NmeaOutput::~NmeaOutput() {
  stop_ = true;
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  if (io_thread_.joinable()) io_thread_.join();
  if (command_fd_ >= 0) ::close(command_fd_);
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
