// The daemon's HTTP surfaces (cpp-httplib): the tactical display (no login) and the maintenance UI (login or
// bearer token), both serving static pages from the www directory and the same JSON API (docs/WEB_UI.md).
#pragma once

#include "edge_config.hpp"
#include "web/auth.hpp"
#include "web/hub.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace httplib {
class Server;
class Request;
class Response;
}  // namespace httplib

namespace edge {

class WebServer {
 public:
  WebServer(Hub& hub, const WebConfig& cfg, std::string www_dir);
  ~WebServer();
  WebServer(const WebServer&) = delete;
  WebServer& operator=(const WebServer&) = delete;

  /// Binds both surfaces (those with a port > 0) and serves them on background threads. Throws if a bind fails.
  void start();
  void stop();
  int tactical_port() const { return tactical_port_; }
  int maintenance_port() const { return maintenance_port_; }
  bool running() const { return running_; }
  auth::Users& users() { return users_; }

 private:
  enum class Surface { Tactical, Maintenance };
  struct Identity {
    bool ok = false;
    bool read_only = false;  ///< bearer token
    std::string who;         ///< "tactical" | "user:<name>" | "token"
  };
  void configure(httplib::Server& srv, Surface surface);
  Identity identify(const httplib::Request& req, Surface surface);
  bool serve_file(const std::string& rel, httplib::Response& res) const;
  void audit(const std::string& who, const std::string& address, const std::string& action, const std::string& detail);

  Hub& hub_;
  WebConfig cfg_;
  std::string www_;
  auth::Users users_;
  auth::Sessions sessions_;
  std::unique_ptr<httplib::Server> tactical_, maintenance_;
  std::vector<std::thread> threads_;
  std::atomic<bool> running_{false};
  int tactical_port_ = 0, maintenance_port_ = 0;
};

}  // namespace edge
