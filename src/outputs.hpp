// NMEA output: UDP datagrams to configured targets and a TCP line server; $PPNT commands come back the same ways.
#pragma once

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace edge {

class NmeaOutput {
 public:
  /// Answers one incoming command line from `from` ("ip:port"); the returned sentences go back to the sender.
  using CommandHandler = std::function<std::vector<std::string>(const std::string& line, const std::string& from)>;

  /// `tcp_port` -1 and `command_udp_port` -1 pick an ephemeral port (tests); 0 disables.
  NmeaOutput(const std::vector<std::string>& udp_targets, const std::string& bind_address, int tcp_port, int command_udp_port = 0);
  ~NmeaOutput();
  /// Sends one sentence (already wrapped with $, checksum and CRLF) to every UDP target and TCP client.
  void send(const std::string& sentence);
  std::uint64_t sentences_sent() const { return sent_.load(); }
  std::size_t tcp_clients() const;
  bool tcp_listening() const { return listen_fd_ >= 0; }
  int tcp_port() const { return tcp_port_; }
  int command_udp_port() const { return command_port_; }
  /// Commands arrive as lines on the TCP connections and as datagrams on the command UDP port.
  void set_command_handler(CommandHandler h);
  std::uint64_t commands_received() const { return commands_.load(); }

 private:
  void accept_loop();
  void io_loop();  ///< reads commands from TCP clients and the UDP command socket
  void handle_line(const std::string& line, const std::string& from, int reply_fd, const sockaddr_in* reply_to);
  std::vector<std::pair<int, std::vector<unsigned char>>> udp_;  // socket, sockaddr bytes
  int listen_fd_ = -1;
  int tcp_port_ = 0;
  int command_fd_ = -1;
  int command_port_ = 0;
  std::thread accept_thread_, io_thread_;
  std::map<int, std::string> rx_;  ///< per-client receive buffers (io thread only)
  CommandHandler handler_;
  mutable std::mutex handler_mutex_;
  std::atomic<std::uint64_t> commands_{0};
  std::atomic<bool> stop_{false};
  mutable std::mutex clients_mutex_;
  std::vector<int> clients_;
  std::atomic<std::uint64_t> sent_{0};
};

}  // namespace edge
