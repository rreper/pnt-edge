// The HTTP surfaces: API reads, SSE, login and bearer token, control routing and surface permissions.
#include "web/auth.hpp"
#include "web/hub.hpp"
#include "web/server.hpp"

#include <httplib.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <thread>

using namespace edge;
using json = nlohmann::json;

namespace {
std::string tmpdir() {
  auto d = std::filesystem::temp_directory_path() / "pnt_edge_web_tests";
  std::filesystem::create_directories(d);
  return d.string();
}
Pva sample(double t, double lat_deg, double lon_deg) {
  Pva p;
  p.tov_ns = static_cast<std::int64_t>(t * 1e9);
  p.lat_rad = lat_deg * M_PI / 180;
  p.lon_rad = lon_deg * M_PI / 180;
  p.alt_hae_m = 200;
  p.vn = 3; p.ve = 4; p.yaw_rad = 0.9;
  p.sigma_n = 1; p.sigma_e = 1.5; p.sigma_d = 2;
  return p;
}
}  // namespace

TEST(Auth, Pbkdf2AndUsersFile) {
  // RFC 6070-style sanity: known SHA-256 of "abc"
  EXPECT_EQ(auth::hex(auth::sha256("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // PBKDF2-HMAC-SHA256("password", "salt", 1) first 32 bytes (RFC 7914 test vector)
  EXPECT_EQ(auth::hex(auth::pbkdf2_sha256("password", std::vector<std::uint8_t>{'s', 'a', 'l', 't'}, 1)),
            "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
  auth::Users u;
  u.set_password("admin", "correct horse", 1000);
  EXPECT_TRUE(u.verify("admin", "correct horse"));
  EXPECT_FALSE(u.verify("admin", "wrong"));
  EXPECT_FALSE(u.verify("nobody", "correct horse"));
  const std::string path = tmpdir() + "/users.json";
  ASSERT_TRUE(u.save(path));
  auth::Users v;
  ASSERT_TRUE(v.load(path));
  EXPECT_TRUE(v.verify("admin", "correct horse"));
  auth::Sessions s(1.0);
  auto tok = s.create("admin");
  EXPECT_EQ(s.user_for(tok).value_or(""), "admin");
  s.revoke(tok);
  EXPECT_FALSE(s.user_for(tok));
}

TEST(Hub, HistoryEventsLog) {
  Hub hub(100, 5);
  IntegrityStatus none;
  for (int i = 0; i < 10; ++i) hub.push_solution(sample(1000 + i, 39.0 + i * 1e-5, -84.0), none);
  auto h = hub.history(600, 1000);
  ASSERT_EQ(h["t"].size(), 10u);
  EXPECT_NEAR(h["n"][9].get<double>(), 9 * 1e-5 * M_PI / 180 * 6.37e6, 2.0);
  EXPECT_DOUBLE_EQ(h["e"][0].get<double>(), 0.0);
  auto sol = hub.solution();
  EXPECT_NEAR(sol["speed_mps"].get<double>(), 5.0, 1e-9);
  for (int i = 0; i < 8; ++i) hub.log(i % 2 ? "WARN" : "DEBUG", "t", "line " + std::to_string(i));
  EXPECT_EQ(hub.log(100, "DEBUG").size(), 5u);  // ring of 5
  EXPECT_EQ(hub.log(100, "WARN").size(), 3u);
  hub.event("PACE", "PRIMARY -> ALTERNATE");
  EXPECT_EQ(hub.events(10).size(), 1u);
  hub.enqueue({"reset", json{{"what", "filter"}}, "tactical", "127.0.0.1"});
  auto c = hub.next_command();
  ASSERT_TRUE(c);
  EXPECT_EQ(c->kind, "reset");
  EXPECT_FALSE(hub.next_command());
}

TEST(Web, SurfacesApiLoginTokenAndControl) {
  Hub hub(100, 50);
  hub.set_status(json{{"health", "ok"}, {"solutions", 3}});
  hub.push_solution(sample(1000, 39.0, -84.0), IntegrityStatus{});
  hub.set_config(json{{"nmea", {{"rate_hz", 1.0}}}}, json{{"app", {{"name", "x"}}}});
  WebConfig cfg;
  cfg.tactical.port = -1;
  cfg.maintenance.port = -1;
  cfg.users_file = tmpdir() + "/users_web.json";
  cfg.api_token = "fleet-secret";
  cfg.audit_file = tmpdir() + "/audit.log";
  {
    auth::Users u;
    u.set_password("admin", "correct horse", 500);
    ASSERT_TRUE(u.save(cfg.users_file));
  }
  const std::string www = std::string(std::getenv("PNT_EDGE_SRCDIR") ? std::getenv("PNT_EDGE_SRCDIR") : "..") + "/www";
  WebServer web(hub, cfg, www);
  web.start();
  ASSERT_GT(web.tactical_port(), 0);
  ASSERT_GT(web.maintenance_port(), 0);
  httplib::Client tac("127.0.0.1", web.tactical_port()), maint("127.0.0.1", web.maintenance_port());

  // tactical: page and API without any login
  auto page = tac.Get("/");
  ASSERT_TRUE(page);
  EXPECT_EQ(page->status, 200);
  EXPECT_NE(page->body.find("Reset filter"), std::string::npos);
  auto st = tac.Get("/api/v1/status");
  ASSERT_TRUE(st);
  EXPECT_EQ(json::parse(st->body)["health"], "ok");
  auto js = tac.Get("/static/api.js");
  ASSERT_TRUE(js);
  EXPECT_EQ(js->status, 200);
  // tactical may reset the filter but not change NMEA or read the config
  auto r1 = tac.Post("/api/v1/control/reset", "{\"what\":\"filter\"}", "application/json");
  ASSERT_TRUE(r1);
  EXPECT_EQ(r1->status, 200);
  auto r2 = tac.Post("/api/v1/control/nmea", "{\"rate_hz\":5}", "application/json");
  ASSERT_TRUE(r2);
  EXPECT_EQ(r2->status, 403);
  auto r3 = tac.Get("/api/v1/config");
  ASSERT_TRUE(r3);
  EXPECT_EQ(r3->status, 403);

  // maintenance: unauthenticated API is 401, page redirects to /login
  auto u1 = maint.Get("/api/v1/status");
  ASSERT_TRUE(u1);
  EXPECT_EQ(u1->status, 401);
  auto u2 = maint.Get("/");
  ASSERT_TRUE(u2);
  EXPECT_EQ(u2->status, 302);
  // bad login
  auto bad = maint.Post("/login", "{\"user\":\"admin\",\"password\":\"nope\"}", "application/json");
  ASSERT_TRUE(bad);
  EXPECT_EQ(bad->status, 401);
  // good login -> cookie -> API and control work
  auto good = maint.Post("/login", "{\"user\":\"admin\",\"password\":\"correct horse\"}", "application/json");
  ASSERT_TRUE(good);
  EXPECT_EQ(good->status, 200);
  const std::string cookie = good->get_header_value("Set-Cookie").substr(0, good->get_header_value("Set-Cookie").find(';'));
  httplib::Headers hdr{{"Cookie", cookie}};
  auto s2 = maint.Get("/api/v1/config", hdr);
  ASSERT_TRUE(s2);
  EXPECT_EQ(s2->status, 200);
  EXPECT_EQ(json::parse(s2->body)["edge"]["nmea"]["rate_hz"], 1.0);
  auto who = maint.Get("/api/v1/whoami", hdr);
  EXPECT_EQ(json::parse(who->body)["who"], "user:admin");
  auto c1 = maint.Post("/api/v1/control/nmea", hdr, "{\"rate_hz\":5}", "application/json");
  ASSERT_TRUE(c1);
  EXPECT_EQ(c1->status, 200);
  // bearer token: read-only
  httplib::Headers tok{{"Authorization", "Bearer fleet-secret"}};
  auto t1 = maint.Get("/api/v1/solution", tok);
  ASSERT_TRUE(t1);
  EXPECT_EQ(t1->status, 200);
  EXPECT_NEAR(json::parse(t1->body)["lat_deg"].get<double>(), 39.0, 1e-9);
  auto t2 = maint.Post("/api/v1/control/reset", tok, "{\"what\":\"app\"}", "application/json");
  ASSERT_TRUE(t2);
  EXPECT_EQ(t2->status, 403);
  // the queued commands, in order
  auto q1 = hub.next_command();
  ASSERT_TRUE(q1);
  EXPECT_EQ(q1->kind, "reset");
  EXPECT_EQ(q1->source, "tactical");
  auto q2 = hub.next_command();
  ASSERT_TRUE(q2);
  EXPECT_EQ(q2->kind, "set_nmea");
  EXPECT_EQ(q2->source, "user:admin");
  EXPECT_FALSE(hub.next_command());
  // audit log written
  std::ifstream audit(cfg.audit_file);
  std::string line, all;
  while (std::getline(audit, line)) all += line + "\n";
  EXPECT_NE(all.find("user:admin"), std::string::npos);
  EXPECT_NE(all.find("set_nmea"), std::string::npos);
  // SSE delivers an update
  std::string got;
  httplib::Client sse("127.0.0.1", web.tactical_port());
  sse.set_read_timeout(3, 0);
  auto res = sse.Get("/api/v1/stream", [&](const char* data, size_t n) {
    got.append(data, n);
    return got.find("event: update") == std::string::npos;  // stop after the first update
  });
  EXPECT_NE(got.find("event: update"), std::string::npos);
  EXPECT_NE(got.find("\"health\":\"ok\""), std::string::npos);
  web.stop();
}
