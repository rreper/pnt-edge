// NMEA output: UDP datagrams to configured targets and a TCP line server on the tactical interface.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace edge {

class NmeaOutput {
 public:
  NmeaOutput(const std::vector<std::string>& udp_targets, const std::string& bind_address, int tcp_port);
  ~NmeaOutput();
  /// Sends one sentence (already wrapped with $, checksum and CRLF) to every UDP target and TCP client.
  void send(const std::string& sentence);
  std::uint64_t sentences_sent() const { return sent_.load(); }
  std::size_t tcp_clients() const;
  bool tcp_listening() const { return listen_fd_ >= 0; }

 private:
  void accept_loop();
  std::vector<std::pair<int, std::vector<unsigned char>>> udp_;  // socket, sockaddr bytes
  int listen_fd_ = -1;
  std::thread accept_thread_;
  std::atomic<bool> stop_{false};
  mutable std::mutex clients_mutex_;
  std::vector<int> clients_;
  std::atomic<std::uint64_t> sent_{0};
};

}  // namespace edge
