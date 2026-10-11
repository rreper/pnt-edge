// $PPNT: parsing and checksums, every command's reply, and the two transports (TCP connection, UDP datagram).
#include "nmea.hpp"
#include "outputs.hpp"
#include "ppnt.hpp"
#include "web/hub.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>

using namespace edge;

namespace {
std::string wrap(const std::string& body) { return nmea_wrap(body); }
std::string strip(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
  return s;
}
}  // namespace

TEST(Ppnt, ParseAndChecksum) {
  auto s = ppnt::parse(wrap("PPNT,OUT,S,5,GGA+HDT"));
  ASSERT_TRUE(s);
  EXPECT_EQ(s->command, "OUT");
  EXPECT_EQ(s->kind, "S");
  ASSERT_EQ(s->fields.size(), 2u);
  EXPECT_EQ(s->fields[1], "GGA+HDT");
  std::string err;
  EXPECT_FALSE(ppnt::parse("$PPNT,OUT,S,5*00", &err));
  EXPECT_EQ(err, "bad checksum");
  EXPECT_FALSE(ppnt::parse("$GPGGA,1*00", &err));
  EXPECT_FALSE(ppnt::parse(wrap("PPNT,STATUS"), &err));
  EXPECT_FALSE(ppnt::parse(wrap("PPNT,STATUS,X"), &err));
  EXPECT_FALSE(ppnt::parse("$" + std::string(90, 'A') + "*00", &err));
  EXPECT_EQ(err, "too long");
  EXPECT_TRUE(ppnt::parse(wrap("PPNT,ver,q")));  // command and kind are case-insensitive
}

TEST(Ppnt, RepliesAndQueuedCommands) {
  Hub hub(100, 50);
  hub.set_status(nlohmann::json{{"health", "ok"}, {"uptime_sec", 42}, {"last_solution_age_sec", 0.3},
                                {"last_solution", {{"sigma_ned_m", {1.0, 1.0, 2.0}}, {"sigma_heading_deg", 0.5}}},
                                {"integrity", {{"pace", "ALTERNATE"}}}});
  ppnt::Context ctx;
  ctx.hub = &hub;
  ctx.version = "0.1.0";
  ctx.cobra_version = "0.2.6";
  ctx.config_id = [] { return std::string("deadbeef"); };
  auto r = ppnt::handle(wrap("PPNT,STATUS,Q"), "1.2.3.4:5", ctx);
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(strip(r[0]).substr(0, 42), "$PPNT,STATUS,R,ok,0.3,6,,1.41,2.00,0.50,42");
  EXPECT_EQ(strip(r[1]), strip(ppnt::ack("STATUS", "OK")));
  r = ppnt::handle(wrap("PPNT,VER,Q"), "", ctx);
  EXPECT_EQ(strip(r[0]), strip(wrap("PPNT,VER,R,0.1.0,0.2.6,deadbeef")));
  r = ppnt::handle(wrap("PPNT,TIME,Q"), "", ctx);
  EXPECT_EQ(strip(r[0]).substr(0, 11), "$PPNT,TIME,");
  EXPECT_NE(strip(r[0]).find(",free,"), std::string::npos);
  r = ppnt::handle(wrap("PPNT,OUT,S,5,gga+hdt+integ"), "1.2.3.4:5", ctx);
  EXPECT_EQ(strip(r.back()), strip(ppnt::ack("OUT", "OK")));
  auto c = hub.next_command();
  ASSERT_TRUE(c);
  EXPECT_EQ(c->kind, "set_nmea");
  EXPECT_EQ(c->args["rate_hz"], 5.0);
  EXPECT_EQ(c->args["sentences"], (std::vector<std::string>{"GGA", "HDT", "INTEG"}));
  EXPECT_EQ(c->source, "ppnt");
  EXPECT_EQ(c->address, "1.2.3.4:5");
  r = ppnt::handle(wrap("PPNT,OUT,S,,"), "", ctx);
  EXPECT_NE(strip(r[0]).find("ERR"), std::string::npos);
  r = ppnt::handle(wrap("PPNT,LEVERARM,S,pos,-0.5,0.38,-0.05"), "", ctx);
  EXPECT_NE(strip(r[0]).find("LEVERARM,OK"), std::string::npos);
  c = hub.next_command();
  ASSERT_TRUE(c);
  EXPECT_EQ(c->kind, "set_leverarm");
  EXPECT_DOUBLE_EQ(c->args["y"], 0.38);
  r = ppnt::handle(wrap("PPNT,HEADING,S,123.4"), "", ctx);
  EXPECT_NE(strip(r[0]).find("HEADING,OK"), std::string::npos);
  c = hub.next_command();
  EXPECT_DOUBLE_EQ(c->args["sigma_deg"], 1.0);
  r = ppnt::handle(wrap("PPNT,RESET,S,filter"), "", ctx);
  EXPECT_EQ(strip(r[0]), strip(ppnt::ack("RESET", "OK")));
  c = hub.next_command();
  EXPECT_EQ(c->kind, "reset");
  r = ppnt::handle(wrap("PPNT,RESET,S,everything"), "", ctx);
  EXPECT_NE(strip(r[0]).find("ERR"), std::string::npos);
  r = ppnt::handle(wrap("PPNT,FROB,S,1"), "", ctx);
  EXPECT_EQ(strip(r[0]), strip(ppnt::ack("FROB", "ERR", "unknown")));
  r = ppnt::handle("$PPNT,OUT,S,5*00\r\n", "", ctx);
  EXPECT_EQ(strip(r[0]), strip(ppnt::ack("OUT", "ERR", "bad checksum")));
  EXPECT_TRUE(ppnt::handle(wrap("PPNT,ACK,X,OK"), "", ctx).empty());  // a reply is never answered
  EXPECT_FALSE(hub.next_command());
  EXPECT_GE(hub.events(10).size(), 3u);  // audit entries on the timeline
}

TEST(Ppnt, TcpAndUdpTransports) {
  NmeaOutput out({}, "127.0.0.1", -1, -1);
  ASSERT_GT(out.tcp_port(), 0);
  ASSERT_GT(out.command_udp_port(), 0);
  std::vector<std::string> seen;
  out.set_command_handler([&](const std::string& line, const std::string& from) {
    seen.push_back(line + " from " + from);
    return std::vector<std::string>{ppnt::ack("VER", "OK", "tcp-or-udp")};
  });
  // TCP: connect, send two lines in one packet, read the two acks
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(out.tcp_port()));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
  const std::string two = wrap("PPNT,VER,Q") + wrap("PPNT,VER,Q");
  ASSERT_EQ(::send(fd, two.data(), two.size(), 0), static_cast<ssize_t>(two.size()));
  timeval tv{3, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  std::string got;
  char buf[256];
  while (got.size() < 2 * ppnt::ack("VER", "OK", "tcp-or-udp").size()) {
    const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    got.append(buf, static_cast<std::size_t>(n));
  }
  EXPECT_EQ(got, ppnt::ack("VER", "OK", "tcp-or-udp") + ppnt::ack("VER", "OK", "tcp-or-udp"));
  // the client is still a client of the output stream
  out.send("$GNGGA,test*00\r\n");
  const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
  EXPECT_GT(n, 0);
  ::close(fd);
  // UDP datagram, reply to the sender
  int u = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(static_cast<std::uint16_t>(out.command_udp_port()));
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const std::string q = wrap("PPNT,VER,Q");
  ASSERT_EQ(::sendto(u, q.data(), q.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to), static_cast<ssize_t>(q.size()));
  ::setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  const ssize_t m = ::recv(u, buf, sizeof buf, 0);
  ASSERT_GT(m, 0);
  EXPECT_EQ(std::string(buf, static_cast<std::size_t>(m)), ppnt::ack("VER", "OK", "tcp-or-udp"));
  ::close(u);
  EXPECT_EQ(out.commands_received(), 3u);
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_NE(seen[2].find("from 127.0.0.1:"), std::string::npos);
}
