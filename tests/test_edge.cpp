// Config parsing and an end-to-end replay through the daemon pieces (filter + log input + NMEA output).
#include "edge_config.hpp"
#include "inputs.hpp"
#include "nmea.hpp"
#include "outputs.hpp"
#include "status.hpp"
#include "integrity.hpp"

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

TEST(Edge, IntegrityAbsentWithoutThePlugin) {
  auto app = pntos::cobra::jsoncfg::load_app_config(cobra("configs/pos_ins.json"));
  pntos::cobra::app::RunOptions o;
  o.progress = false;
  pntos::cobra::Filter f(app, o);
  auto s = edge::integrity_from_registry(f);
  EXPECT_FALSE(s.present);
  EXPECT_EQ(edge::fix_quality_for(s, 1), 1);
  f.stop();
}

TEST(Status, WatchdogDue) {
  // off, never
  EXPECT_FALSE(edge::watchdog_due(0, 100, 10, 200, 100));
  // before the first solution the filter is aligning: not a watchdog case
  EXPECT_FALSE(edge::watchdog_due(20, 100, 0, 200, 100));
  // solutions flowing
  EXPECT_FALSE(edge::watchdog_due(20, 1.5, 10, 200, 100));
  // silent, but nothing arrives either (devices unplugged): the device health says so, no restart
  EXPECT_FALSE(edge::watchdog_due(20, 60, 10, 200, 200));
  // silent while measurements arrive: restart
  EXPECT_TRUE(edge::watchdog_due(20, 21, 10, 201, 200));
}

TEST(EdgeConfig, TheRoadVehicleFilterLoadsAndCarriesNothingOfTheExampleRecording) {
  // configs/filters/road_vehicle.json is Cobra's ground-vehicle example for real devices: the heading is found
  // while driving, and what belongs to one installation (how the IMU is mounted, where the antenna is, how old
  // the receiver's data are) is identity, zero, or a state of the filter. Cobra's own examples carry the values of
  // its recording, which on any other vehicle are wrong.
  std::ifstream in(src("configs/filters/road_vehicle.json"));
  ASSERT_TRUE(in.good());
  const auto j = nlohmann::json::parse(in);
  EXPECT_EQ(j.at("app").at("initialization"), "in_motion_align");
  int orchestrations = 0;
  for (const auto& c : j.at("configs")) {
    if (c.at("type") != "StandardOrchestrationConfig") continue;
    ++orchestrations;
    const auto identity = nlohmann::json::array({{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}});
    const auto zero = nlohmann::json::array({0.0, 0.0, 0.0});
    EXPECT_EQ(c.at("inertial_config").at("C_imu_to_platform"), identity);
    EXPECT_EQ(c.at("inertial_config").at("imu_gap_policy"), "hold");  // lost samples are the inertial's to handle
    EXPECT_EQ(c.at("alignment_config").at("lever_arm"), zero);
    EXPECT_FALSE(c.at("alignment_config").contains("heading"));
    for (const auto& p : c.at("preprocessor_configs")) {
      EXPECT_NE(p.at("type"), "TimeAdjusterConfig");  // it re-stamps every sample 10 ms after the last
      if (p.at("type") == "TimeBiasConfig") EXPECT_EQ(p.at("time_bias"), 0);
      if (p.at("type") == "ImuRotatorConfig") EXPECT_EQ(p.at("C_imu_to_platform"), identity);
    }
    for (const auto& m : c.at("mp_configs"))
      if (m.contains("lever_arm")) EXPECT_EQ(m.at("lever_arm"), zero);
  }
  EXPECT_EQ(orchestrations, 1);

  auto app = pntos::cobra::jsoncfg::load_app_config(src("configs/filters/road_vehicle.json"));
  app.app.logging_level = "WARN";
  pntos::cobra::app::RunOptions o;
  o.progress = false;
  EXPECT_NO_THROW({ pntos::cobra::Filter filter(app, o); });
}

TEST(EdgeConfig, TheProtectedRoadVehicleFilterIsTheSameFilterWithoutTheReceiversVelocity) {
  // Built only with the integrity plugin (meson.build): filters/road_vehicle_protected.json, written from
  // configs/filters/road_vehicle.json by the plugin's tool. The plugin tests and excludes the receiver's position;
  // a velocity of the same receiver that kept updating the filter would carry it along with a spoofer after the
  // exclusion (1.9 km on the example drive, reported as trusted). So the protected filter takes that velocity for
  // finding its heading at the start and for nothing after.
  const char* path = std::getenv("PNT_EDGE_PROTECTED_FILTER");
  if (!path || !*path) GTEST_SKIP() << "built without the integrity plugin";
  std::ifstream plain_in(src("configs/filters/road_vehicle.json")), protected_in(path);
  ASSERT_TRUE(plain_in.good());
  ASSERT_TRUE(protected_in.good()) << path;
  const auto plain = nlohmann::json::parse(plain_in), prot = nlohmann::json::parse(protected_in);
  const std::string position = "/sensor/ublox-ZED-F9T/position", velocity = "/sensor/ublox-ZED-F9T/velocity";
  auto orchestration = [](const nlohmann::json& j) {
    for (const auto& c : j.at("configs"))
      if (c.at("type") == "StandardOrchestrationConfig") return c;
    return nlohmann::json();
  };
  const auto a = orchestration(plain), b = orchestration(prot);
  ASSERT_FALSE(a.is_null());
  ASSERT_FALSE(b.is_null());

  EXPECT_EQ(prot.at("app").at("orchestration"), "protected");
  bool monitored = false;
  for (const auto& c : prot.at("configs"))
    if (c.value("group", "") == "config/integrity") monitored = c.at("values").at("sources") == nlohmann::json::array({position});
  EXPECT_TRUE(monitored) << "the integrity settings have to name the receiver's position as the one source";

  // the same filter and the same (neutral) installation
  for (const char* key : {"inertial_config", "alignment_config", "pinson_sb_config", "alignment_channels", "measurement_reorder_s"})
    EXPECT_EQ(b.at(key), a.at(key)) << key;
  // ... of which the receiver's velocity updates nothing and confirms no standstill
  bool plain_uses_velocity = false;
  for (const auto& m : a.at("mp_configs")) plain_uses_velocity |= m.at("channel") == velocity;
  EXPECT_TRUE(plain_uses_velocity) << "road_vehicle.json no longer uses the velocity: this test is void";
  for (const auto& m : b.at("mp_configs")) {
    EXPECT_NE(m.at("channel"), velocity);
    if (m.at("channel") == position) EXPECT_TRUE(m.contains("innovation_gate_probability"));
  }
  EXPECT_EQ(b.at("mp_configs").size() + 1, a.at("mp_configs").size());
  for (const auto& p : b.at("preprocessor_configs"))
    if (p.contains("confirmation_channels")) EXPECT_TRUE(p.at("confirmation_channels").empty());
  bool aligns_on_velocity = false;
  for (const auto& c : b.at("alignment_channels")) aligns_on_velocity |= c == velocity;
  EXPECT_TRUE(aligns_on_velocity);

  // Cobra reads it (running it takes the plugin, which the daemon registers and this test binary does not)
  EXPECT_NO_THROW({ pntos::cobra::jsoncfg::load_app_config(path); });
}
