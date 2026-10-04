// Config parsing and an end-to-end replay through the daemon pieces (filter + log input + NMEA output).
#include "edge_config.hpp"
#include "inputs.hpp"
#include "nmea.hpp"
#include "outputs.hpp"
#include "status.hpp"

#include <pntos/cobra/app/AppBuilder.hpp>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
std::string src(const char* rel) {
  const char* s = std::getenv("PNT_EDGE_SRCDIR");
  return std::string(s ? s : "..") + "/" + rel;
}
std::string cobra(const char* rel) { return src("subprojects/pntos-cpp/") + rel; }
}  // namespace

TEST(EdgeConfig, ParsesAndResolvesPaths) {
  auto j = nlohmann::json::parse(R"({"filter_config":"filters/pos_ins.json","input":{"type":"lcm_log","path":"logs/a.log","speed":2},
      "nmea":{"udp_targets":["10.0.0.5:10110"],"tcp_port":10110,"rate_hz":5,"sentences":["GGA","HDT"]},"status":{"file":"/tmp/s.json"}})");
  auto c = edge::edge_config_from_json(j, "/etc/pnt-edge");
  EXPECT_EQ(c.filter_config, "/etc/pnt-edge/filters/pos_ins.json");
  EXPECT_EQ(c.input.path, "/etc/pnt-edge/logs/a.log");
  EXPECT_EQ(c.input.speed, 2.0);
  EXPECT_EQ(c.nmea.tcp_port, 10110);
  EXPECT_EQ(c.nmea.sentences.size(), 2u);
  EXPECT_EQ(c.status.file, "/tmp/s.json");
  auto back = edge::edge_config_from_json(edge::edge_config_to_json(c));
  EXPECT_EQ(back.nmea.udp_targets, c.nmea.udp_targets);
  EXPECT_THROW(edge::edge_config_from_json(nlohmann::json::parse(R"({"input":{}})")), std::runtime_error);
  EXPECT_THROW(edge::edge_config_from_json(nlohmann::json::parse(R"({"filter_config":"x","input":{"type":"serial"}})")), std::runtime_error);
  EXPECT_THROW(edge::load_edge_config("/nonexistent.json"), std::runtime_error);
}

TEST(Edge, ReplayProducesNmeaAndStatus) {
  // a UDP listener for the NMEA output
  int rx = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ASSERT_EQ(::bind(rx, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
  socklen_t len = sizeof addr;
  ::getsockname(rx, reinterpret_cast<sockaddr*>(&addr), &len);
  const int port = ntohs(addr.sin_port);
  timeval tv{2, 0};
  ::setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  auto app = pntos::cobra::jsoncfg::load_app_config(cobra("configs/pos_ins.json"));
  app.app.logging_level = "WARN";
  pntos::cobra::app::RunOptions o;
  o.progress = false;
  pntos::cobra::Filter filter(app, o);
  edge::NmeaOutput out({"127.0.0.1:" + std::to_string(port)}, "127.0.0.1", 0);
  edge::NmeaOptions nopt;
  std::size_t solutions = 0;
  edge::Pva last;
  filter.set_solution_callback([&](const pntos::api::Message& m) {
    edge::Pva p;
    ASSERT_TRUE(edge::pva_from_message(m, p));
    ++solutions;
    last = p;
    for (const auto& s : edge::sentences(p, nopt)) out.send(s);
  });
  edge::InputConfig ic;
  ic.type = "lcm_log";
  ic.path = cobra("testdata/example_60s.log");
  auto input = edge::make_input(filter, ic, {"/sensor/vn-100/imu", "/sensor/ublox-ZED-F9T/position"});
  static_cast<edge::LogInput*>(input.get())->run();
  EXPECT_TRUE(input->finished());
  EXPECT_GE(solutions, 40u);
  EXPECT_EQ(out.sentences_sent(), solutions * 7);
  auto counters = input->counters();
  EXPECT_GT(counters.per_channel["/sensor/vn-100/imu"], 5000u);
  EXPECT_EQ(counters.decode_failures, 0u);
  // the first datagram is a GGA at the filter's first epoch
  char buf[256];
  const ssize_t n = ::recv(rx, buf, sizeof buf - 1, 0);
  ASSERT_GT(n, 0);
  buf[n] = 0;
  EXPECT_EQ(std::string(buf).substr(0, 6), "$GNGGA");
  ::close(rx);
  // status snapshot
  edge::StatusSnapshot snap;
  snap.wall = std::chrono::system_clock::now();
  snap.input = counters;
  snap.solutions = solutions;
  snap.last_solution = last;
  snap.last_solution_age_sec = 0.5;
  snap.gating = edge::gating_from_registry(filter);
  auto j = edge::status_to_json(snap);
  EXPECT_EQ(j["health"], "ok");
  EXPECT_NEAR(j["last_solution"]["lat_deg"].get<double>(), 39.7575, 1e-3);
  const std::string path = std::filesystem::temp_directory_path() / "pnt_edge_status_test.json";
  edge::write_status_file(path, j);
  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  EXPECT_EQ(filter.stop(), 0);
}
