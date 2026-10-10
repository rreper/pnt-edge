#include "web/server.hpp"

#define CPPHTTPLIB_THREAD_POOL_COUNT 6
#include <httplib.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace edge {

using nlohmann::json;

namespace {
const char* mime_for(const std::string& path) {
  auto ends = [&](const char* ext) { return path.size() >= std::strlen(ext) && path.compare(path.size() - std::strlen(ext), std::strlen(ext), ext) == 0; };
  if (ends(".html")) return "text/html; charset=utf-8";
  if (ends(".js")) return "application/javascript; charset=utf-8";
  if (ends(".css")) return "text/css; charset=utf-8";
  if (ends(".json")) return "application/json";
  if (ends(".svg")) return "image/svg+xml";
  if (ends(".png")) return "image/png";
  if (ends(".ico")) return "image/x-icon";
  return "application/octet-stream";
}
void send_json(httplib::Response& res, const json& j, int status = 200) {
  res.status = status;
  res.set_content(j.dump(), "application/json");
}
std::string cookie_value(const httplib::Request& req, const std::string& name) {
  const std::string c = req.get_header_value("Cookie");
  std::size_t pos = 0;
  while (pos < c.size()) {
    std::size_t end = c.find(';', pos);
    if (end == std::string::npos) end = c.size();
    std::string kv = c.substr(pos, end - pos);
    const auto eq = kv.find('=');
    if (eq != std::string::npos) {
      std::string k = kv.substr(0, eq);
      k.erase(0, k.find_first_not_of(' '));
      if (k == name) return kv.substr(eq + 1);
    }
    pos = end + 1;
  }
  return "";
}
std::string url_decode(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') out += ' ';
    else if (s[i] == '%' && i + 2 < s.size()) { out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)); i += 2; }
    else out += s[i];
  }
  return out;
}
}  // namespace

WebServer::WebServer(Hub& hub, const WebConfig& cfg, std::string www_dir) : hub_(hub), cfg_(cfg), www_(std::move(www_dir)) {
  std::string err;
  if (!users_.load(cfg_.users_file, &err)) hub_.log("WARN", "web", "users file: " + err);
}

WebServer::~WebServer() { stop(); }

bool WebServer::serve_file(const std::string& rel, httplib::Response& res) const {
  if (rel.find("..") != std::string::npos) return false;
  const std::filesystem::path p = std::filesystem::path(www_) / rel;
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  res.set_content(ss.str(), mime_for(rel));
  res.set_header("Cache-Control", "no-cache");
  return true;
}

void WebServer::audit(const std::string& who, const std::string& address, const std::string& action, const std::string& detail) {
  hub_.event("CONTROL", who + "@" + address + " " + action + " " + detail);
  if (cfg_.audit_file.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(cfg_.audit_file).parent_path(), ec);
  std::ofstream out(cfg_.audit_file, std::ios::app);
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
  out << buf << ' ' << who << ' ' << address << ' ' << action << ' ' << detail << '\n';
}

WebServer::Identity WebServer::identify(const httplib::Request& req, Surface surface) {
  Identity id;
  if (surface == Surface::Tactical) {
    id.ok = true;
    id.who = "tactical";
    return id;
  }
  const std::string authz = req.get_header_value("Authorization");
  if (!cfg_.api_token.empty() && authz.rfind("Bearer ", 0) == 0 && authz.substr(7) == cfg_.api_token) {
    id.ok = true;
    id.read_only = true;
    id.who = "token";
    return id;
  }
  if (auto user = sessions_.user_for(cookie_value(req, "pnt_session"))) {
    id.ok = true;
    id.who = "user:" + *user;
  }
  return id;
}

void WebServer::configure(httplib::Server& srv, Surface surface) {
  const bool maint = surface == Surface::Maintenance;
  const std::string page_dir = maint ? "maintenance" : "tactical";

  // ---- pages
  srv.Get("/", [this, page_dir, surface](const httplib::Request& req, httplib::Response& res) {
    if (surface == Surface::Maintenance && !identify(req, surface).ok) {
      res.set_redirect("/login");
      return;
    }
    if (!serve_file(page_dir + "/index.html", res)) res.status = 404;
  });
  srv.Get(R"(/static/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
    if (!serve_file("common/" + req.matches[1].str(), res)) res.status = 404;
  });
  srv.Get(R"(/app/(.+))", [this, page_dir](const httplib::Request& req, httplib::Response& res) {
    if (!serve_file(page_dir + "/" + req.matches[1].str(), res)) res.status = 404;
  });
  if (maint) {
    srv.Get("/login", [this](const httplib::Request&, httplib::Response& res) {
      if (!serve_file("maintenance/login.html", res)) res.status = 404;
    });
    srv.Post("/login", [this](const httplib::Request& req, httplib::Response& res) {
      std::string user, password;
      if (req.has_param("user")) {
        user = req.get_param_value("user");
        password = req.get_param_value("password");
      } else {
        try {
          auto j = json::parse(req.body);
          user = j.value("user", "");
          password = j.value("password", "");
        } catch (const std::exception&) {
        }
      }
      if (users_.empty()) {
        hub_.log("WARN", "web", "login attempted but no users are configured (pnt-edge --set-password <user>)");
      }
      if (!user.empty() && users_.verify(user, password)) {
        const std::string token = sessions_.create(user);
        res.set_header("Set-Cookie", "pnt_session=" + token + "; HttpOnly; SameSite=Strict; Path=/");
        audit("user:" + user, req.remote_addr, "login", "");
        if (req.has_param("user")) res.set_redirect("/");
        else send_json(res, {{"ok", true}, {"user", user}});
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));  // slow down guessing
      hub_.log("WARN", "web", "failed login for \"" + user + "\" from " + req.remote_addr);
      if (req.has_param("user")) res.set_redirect("/login?failed=1");
      else send_json(res, {{"ok", false}, {"error", "invalid credentials"}}, 401);
    });
    srv.Post("/logout", [this](const httplib::Request& req, httplib::Response& res) {
      sessions_.revoke(cookie_value(req, "pnt_session"));
      res.set_header("Set-Cookie", "pnt_session=; Max-Age=0; Path=/");
      res.set_redirect("/login");
    });
  }

  // ---- API v1 (read)
  auto guard = [this, surface](const httplib::Request& req, httplib::Response& res, Identity* out) {
    Identity id = identify(req, surface);
    if (!id.ok) {
      send_json(res, {{"error", "unauthorized"}}, 401);
      return false;
    }
    if (out) *out = id;
    return true;
  };
  srv.Get("/api/v1/status", [this, guard](const httplib::Request& req, httplib::Response& res) {
    if (!guard(req, res, nullptr)) return;
    send_json(res, hub_.status());
  });
  srv.Get("/api/v1/solution", [this, guard](const httplib::Request& req, httplib::Response& res) {
    if (!guard(req, res, nullptr)) return;
    send_json(res, hub_.solution());
  });
  srv.Get("/api/v1/history", [this, guard](const httplib::Request& req, httplib::Response& res) {
    if (!guard(req, res, nullptr)) return;
    const double seconds = req.has_param("seconds") ? std::stod(req.get_param_value("seconds")) : 600.0;
    const int points = req.has_param("points") ? std::stoi(req.get_param_value("points")) : 1200;
    send_json(res, hub_.history(seconds, points));
  });
  srv.Get("/api/v1/events", [this, guard](const httplib::Request& req, httplib::Response& res) {
    if (!guard(req, res, nullptr)) return;
    send_json(res, hub_.events(req.has_param("n") ? std::stoi(req.get_param_value("n")) : 200));
  });
  srv.Get("/api/v1/log", [this, guard](const httplib::Request& req, httplib::Response& res) {
    if (!guard(req, res, nullptr)) return;
    send_json(res, hub_.log(req.has_param("n") ? std::stoi(req.get_param_value("n")) : 300, req.has_param("level") ? req.get_param_value("level") : "DEBUG"));
  });
  srv.Get("/api/v1/config", [this, guard, maint](const httplib::Request& req, httplib::Response& res) {
    if (!guard(req, res, nullptr)) return;
    if (!maint) {
      send_json(res, {{"error", "configuration is only readable on the maintenance surface"}}, 403);
      return;
    }
    send_json(res, hub_.config());
  });
  srv.Get("/api/v1/version", [this, guard](const httplib::Request& req, httplib::Response& res) {
    if (!guard(req, res, nullptr)) return;
    send_json(res, {{"pnt_edge", hub_.version()}, {"api", 1}});
  });
  srv.Get("/api/v1/whoami", [this, guard](const httplib::Request& req, httplib::Response& res) {
    Identity id;
    if (!guard(req, res, &id)) return;
    send_json(res, {{"who", id.who}, {"read_only", id.read_only}});
  });
  // server-sent events: status + solution whenever either changes (at most ~5 Hz), heartbeat every 5 s
  srv.Get("/api/v1/stream", [this, guard](const httplib::Request& req, httplib::Response& res) {
    if (!guard(req, res, nullptr)) return;
    res.set_header("Cache-Control", "no-cache");
    res.set_chunked_content_provider("text/event-stream", [this](std::size_t, httplib::DataSink& sink) {
      std::uint64_t seen = 0;
      while (running_) {
        const std::uint64_t now = hub_.wait_for_change(seen, std::chrono::milliseconds(5000));
        std::string payload;
        if (now != seen) {
          seen = now;
          payload = "event: update\ndata: " + json{{"status", hub_.status()}, {"solution", hub_.solution()}}.dump() + "\n\n";
        } else {
          payload = ": heartbeat\n\n";
        }
        if (!sink.write(payload.data(), payload.size())) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      sink.done();
      return true;
    });
  });

  // ---- control (POST); the tactical surface may reset the filter and nothing else
  auto control = [this, guard, surface](const char* kind, bool tactical_allowed) {
    return [this, guard, surface, kind, tactical_allowed](const httplib::Request& req, httplib::Response& res) {
      Identity id;
      if (!guard(req, res, &id)) return;
      if (id.read_only || (surface == Surface::Tactical && !tactical_allowed)) {
        send_json(res, {{"error", "not permitted on this surface"}}, 403);
        return;
      }
      json args = json::object();
      if (!req.body.empty()) {
        try {
          args = json::parse(req.body);
        } catch (const std::exception& e) {
          send_json(res, {{"error", std::string("bad JSON: ") + e.what()}}, 400);
          return;
        }
      }
      hub_.enqueue({kind, args, id.who, req.remote_addr});
      audit(id.who, req.remote_addr, kind, args.dump());
      send_json(res, {{"ok", true}, {"queued", kind}});
    };
  };
  srv.Post("/api/v1/control/reset", control("reset", true));          // {"what": "filter" | "app"}; tactical: filter only (checked in main)
  srv.Post("/api/v1/control/heading", control("set_heading", false));  // {"deg": .., "sigma_deg": ..}
  srv.Post("/api/v1/control/leverarm", control("set_leverarm", false));  // {"label": .., "x": .., "y": .., "z": ..}
  srv.Post("/api/v1/control/nmea", control("set_nmea", false));      // {"rate_hz": .., "sentences": [...], "udp_targets": [...]}
  srv.Put("/api/v1/config", control("save_config", false));          // the edge config object

  srv.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (res.body.empty()) res.set_content("{\"error\":\"not found\"}", "application/json");
  });
}

void WebServer::start() {
  running_ = true;
  auto launch = [this](const WebSurfaceConfig& c, Surface surface, std::unique_ptr<httplib::Server>& slot, int& port_out) {
    if (c.port <= 0 && c.port != -1) return;  // -1: ephemeral (tests)
    slot = std::make_unique<httplib::Server>();
    configure(*slot, surface);
    const int port = c.port == -1 ? slot->bind_to_any_port(c.bind) : (slot->bind_to_port(c.bind, c.port) ? c.port : 0);
    if (port <= 0) throw std::runtime_error("web: cannot bind " + c.bind + ":" + std::to_string(c.port));
    port_out = port;
    httplib::Server* s = slot.get();
    threads_.emplace_back([s] { s->listen_after_bind(); });
    hub_.log("INFO", "web", std::string(surface == Surface::Tactical ? "tactical" : "maintenance") + " surface on " + c.bind + ":" + std::to_string(port));
  };
  launch(cfg_.tactical, Surface::Tactical, tactical_, tactical_port_);
  launch(cfg_.maintenance, Surface::Maintenance, maintenance_, maintenance_port_);
  // wait until both listeners are up
  for (int i = 0; i < 100; ++i) {
    const bool t_ok = !tactical_ || tactical_->is_running(), m_ok = !maintenance_ || maintenance_->is_running();
    if (t_ok && m_ok) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void WebServer::stop() {
  if (!running_) return;
  running_ = false;
  if (tactical_) tactical_->stop();
  if (maintenance_) maintenance_->stop();
  for (auto& t : threads_) if (t.joinable()) t.join();
  threads_.clear();
}

}  // namespace edge
