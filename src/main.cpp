// pnt-edge: the Cobra navigation filter as an edge appliance process.
//   pnt-edge config.json [--input-log file] [--once] [--print-status]
//   pnt-edge config.json --set-password <user>        (reads the password from PNT_EDGE_PASSWORD or the terminal)
// Reads measurements (LCM log or LCM UDP multicast), runs the filter through the push API, emits NMEA 0183
// on the tactical interface, keeps a JSON status snapshot, and serves the tactical display and the maintenance
// UI over HTTP (docs/WEB_UI.md).
#include "edge_config.hpp"
#include "inputs.hpp"
#include "integrity.hpp"
#include "nmea.hpp"
#include "outputs.hpp"
#include "status.hpp"
#include "web/hub.hpp"
#include "web/server.hpp"
#ifdef PNT_EDGE_HAS_INTEGRITY
#include <cobra_integrity/register.hpp>
#endif

#include <pntos/cobra/app/AppBuilder.hpp>
#include <pntos/cobra/app/Filter.hpp>
#include <pntos/cobra/utils/geoid.hpp>
#include <pntos/cobra/utils/logging.hpp>

#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PNT_EDGE_VERSION
#define PNT_EDGE_VERSION "dev"
#endif
#ifndef PNT_EDGE_WWW
#define PNT_EDGE_WWW "/usr/local/share/pnt-edge/www"
#endif

namespace {
std::atomic<bool> g_stop{false};
std::atomic<int> g_exit_code{0};
void on_signal(int) { g_stop = true; }

std::string www_dir(const edge::EdgeConfig& cfg) {
  if (!cfg.web.www.empty()) return cfg.web.www;
  if (std::filesystem::is_directory(PNT_EDGE_WWW)) return PNT_EDGE_WWW;
  // development: <build>/pnt-edge next to <source>/www
  std::error_code ec;
  auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    for (auto dir = exe.parent_path(); !dir.empty() && dir != dir.root_path(); dir = dir.parent_path())
      if (std::filesystem::is_directory(dir / "www" / "tactical")) return (dir / "www").string();
  }
  return PNT_EDGE_WWW;
}

/// Everything that is torn down and rebuilt on a filter reset.
struct Runtime {
  std::unique_ptr<pntos::cobra::Filter> filter;
  std::unique_ptr<edge::Input> input;
  std::unique_ptr<edge::NmeaOutput> out;
  std::mutex mutex;
  edge::StatusSnapshot status;
  std::chrono::steady_clock::time_point last_solution_wall;
  std::int64_t last_emitted_tov = 0;
};

std::unique_ptr<Runtime> start_runtime(const edge::EdgeConfig& cfg, edge::Hub& hub, const edge::NmeaOptions& nopt) {
  auto rt = std::make_unique<Runtime>();
  pntos::cobra::AppConfig app = pntos::cobra::jsoncfg::load_app_config(cfg.filter_config);
  app.app.logging_level = cfg.logging_level;
  pntos::cobra::app::RunOptions options;
  options.progress = false;
  options.legacy_q_rotation = cfg.legacy_q_rotation;
  std::set<std::string> default_channels;
  for (const auto& c : app.configs)
    if (auto* t = dynamic_cast<const pntos::cobra::LcmLogTransportConfig*>(c.get()); t && t->channels_to_process)
      default_channels.insert(t->channels_to_process->begin(), t->channels_to_process->end());
  hub.set_config(edge::edge_config_to_json(cfg), pntos::cobra::jsoncfg::app_config_to_json(app));
  rt->filter = std::make_unique<pntos::cobra::Filter>(app, options);
  rt->out = std::make_unique<edge::NmeaOutput>(cfg.nmea.udp_targets, cfg.nmea.bind_address, cfg.nmea.tcp_port);
  const std::int64_t utc_offset_ns = static_cast<std::int64_t>(cfg.nmea.utc_offset_sec * 1e9);
  const std::int64_t min_gap_ns = cfg.nmea.rate_hz > 0 ? static_cast<std::int64_t>(1e9 / cfg.nmea.rate_hz) : 0;
  Runtime* r = rt.get();
  rt->filter->set_solution_callback([r, &hub, nopt, utc_offset_ns, min_gap_ns](const pntos::api::Message& m) {
    edge::Pva p;
    if (!edge::pva_from_message(m, p, utc_offset_ns)) return;
    std::lock_guard lk(r->mutex);
    ++r->status.solutions;
    r->status.last_solution = p;
    r->last_solution_wall = std::chrono::steady_clock::now();
    const auto integ = edge::integrity_from_registry(*r->filter);
    p.fix_quality = edge::fix_quality_for(integ, p.fix_quality);
    hub.push_solution(p, integ);
    if (min_gap_ns > 0 && r->last_emitted_tov != 0 && p.tov_ns - r->last_emitted_tov < min_gap_ns - 1'000'000) return;
    r->last_emitted_tov = p.tov_ns;
    for (const auto& s : edge::sentences(p, nopt)) r->out->send(s);
    if (nopt.integ && integ.present) r->out->send(edge::integ_sentence(integ));
    r->status.nmea_sentences = r->out->sentences_sent();
  });
  rt->input = edge::make_input(*rt->filter, cfg.input, default_channels);
  rt->input->start();
  return rt;
}

void stop_runtime(Runtime& rt) {
  if (rt.input) rt.input->stop();
  if (rt.filter) rt.filter->stop();
}

/// Patches the Cobra filter config file: heading (rad) of every ManualHeadingAlignmentConfig, or a lever arm.
void patch_filter_config(const std::string& path, const std::function<void(nlohmann::json&)>& edit) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open " + path);
  auto j = nlohmann::json::parse(in, nullptr, true, true);
  edit(j);
  const std::string tmp = path + ".tmp";
  std::ofstream out(tmp);
  out << j.dump(2) << "\n";
  out.close();
  std::filesystem::rename(tmp, path);
}

int set_password(const edge::EdgeConfig& cfg, const std::string& user) {
  std::string password;
  if (const char* env = std::getenv("PNT_EDGE_PASSWORD")) password = env;
  else {
    std::cerr << "password for " << user << ": " << std::flush;
    std::getline(std::cin, password);
  }
  if (password.size() < 8) {
    std::cerr << "pnt-edge: password must have at least 8 characters\n";
    return 2;
  }
  edge::auth::Users users;
  std::string err;
  if (!users.load(cfg.web.users_file, &err)) std::cerr << "pnt-edge: " << err << " (starting a new users file)\n";
  users.set_password(user, password);
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(cfg.web.users_file).parent_path(), ec);
  if (!users.save(cfg.web.users_file, &err)) {
    std::cerr << "pnt-edge: " << err << "\n";
    return 2;
  }
  ::chmod(cfg.web.users_file.c_str(), 0600);
  std::cerr << "pnt-edge: password set for " << user << " in " << cfg.web.users_file << "\n";
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
#ifdef PNT_EDGE_HAS_INTEGRITY
  cobra_integrity::register_plugins();  // makes "orchestration": "protected" available to the filter config
#endif
  if (argc < 2 || std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
    std::cerr << "usage: pnt-edge config.json [--input-log file] [--once] [--print-status]\n"
                 "       pnt-edge config.json --set-password <user>\n";
    return argc < 2 ? 2 : 0;
  }
  try {
    edge::EdgeConfig cfg = edge::load_edge_config(argv[1]);
    bool once = false, print_status = false;
    for (int i = 2; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--input-log" && i + 1 < argc) {
        cfg.input.type = "lcm_log";
        cfg.input.path = argv[++i];
      } else if (a == "--once") {
        once = true;
      } else if (a == "--print-status") {
        print_status = true;
      } else if (a == "--set-password" && i + 1 < argc) {
        return set_password(cfg, argv[++i]);
      } else {
        throw std::runtime_error("unknown option " + a);
      }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    edge::Hub hub(cfg.web.history_seconds, cfg.web.log_lines);
    hub.set_version(PNT_EDGE_VERSION);
    pntos::cobra::utils::set_log_sink([&hub](pntos::api::LoggingLevel l, const std::string& id, const std::string& m) {
      hub.log(pntos::api::to_string(l), id, m);
    });
    auto say = [&](const std::string& level, const std::string& m) {
      hub.log(level, "pnt-edge", m);
      std::cerr << "pnt-edge: " << m << "\n";
    };

    // NMEA options (shared by every runtime)
    edge::NmeaOptions nopt;
    nopt.talker = cfg.nmea.talker;
    if (!cfg.geoid_file.empty()) {
      std::string err;
      if (auto g = pntos::cobra::nav::Geoid::load(cfg.geoid_file, &err)) nopt.geoid = std::make_shared<const pntos::cobra::nav::Geoid>(std::move(*g));
      else say("WARN", err + " (GGA altitude will be HAE)");
    } else {
      nopt.geoid = pntos::cobra::nav::default_geoid();
    }
    auto apply_sentences = [&]() {
      auto enabled = [&](const char* s) { return std::find(cfg.nmea.sentences.begin(), cfg.nmea.sentences.end(), s) != cfg.nmea.sentences.end(); };
      nopt.gga = enabled("GGA"); nopt.rmc = enabled("RMC"); nopt.vtg = enabled("VTG"); nopt.hdt = enabled("HDT");
      nopt.gst = enabled("GST"); nopt.zda = enabled("ZDA"); nopt.pashr = enabled("PASHR"); nopt.integ = enabled("INTEG");
    };
    apply_sentences();

    // web surfaces
    edge::WebServer web(hub, cfg.web, www_dir(cfg));
    web.start();

    auto rt = start_runtime(cfg, hub, nopt);
    say("INFO", "running (" + cfg.input.type + (cfg.input.type == "lcm_log" ? " " + cfg.input.path : " " + cfg.input.url) + "), NMEA to " +
                    std::to_string(cfg.nmea.udp_targets.size()) + " UDP target(s)" + (cfg.nmea.tcp_port ? " and TCP port " + std::to_string(cfg.nmea.tcp_port) : "") +
                    ", web " + www_dir(cfg) + " on tactical :" + std::to_string(web.tactical_port()) + " / maintenance :" + std::to_string(web.maintenance_port()));

    const auto start = std::chrono::steady_clock::now();
    auto next_status = std::chrono::steady_clock::now();
    std::string last_pace, last_excluded, last_health;
    while (!g_stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (once && rt->input->finished()) break;

      // control commands from the web surfaces
      while (auto cmd = hub.next_command()) {
        try {
          if (cmd->kind == "reset") {
            const std::string what = cmd->args.value("what", "filter");
            if (what == "app") {
              if (cmd->source == "tactical") throw std::runtime_error("the tactical display may only reset the filter");
              say("WARN", "application restart requested by " + cmd->source + " from " + cmd->address);
              g_exit_code = 3;
              g_stop = true;
              break;
            }
            say("WARN", "filter reset requested by " + cmd->source + " from " + cmd->address);
            stop_runtime(*rt);
            rt.reset();
            rt = start_runtime(cfg, hub, nopt);
            hub.event("RESET", "filter re-aligned on request of " + cmd->source);
          } else if (cmd->kind == "set_nmea") {
            if (cmd->args.contains("rate_hz")) cfg.nmea.rate_hz = cmd->args["rate_hz"].get<double>();
            if (cmd->args.contains("sentences")) cfg.nmea.sentences = cmd->args["sentences"].get<std::vector<std::string>>();
            if (cmd->args.contains("udp_targets")) cfg.nmea.udp_targets = cmd->args["udp_targets"].get<std::vector<std::string>>();
            apply_sentences();
            // the solution callback holds the sentence selection and the output by value: rebuild the runtime
            stop_runtime(*rt);
            rt.reset();
            rt = start_runtime(cfg, hub, nopt);
            if (!cfg.path.empty()) edge::save_edge_config(cfg, cfg.path);
            say("INFO", "NMEA output changed by " + cmd->source + ": " + cmd->args.dump());
          } else if (cmd->kind == "set_heading") {
            const double deg = cmd->args.at("deg").get<double>(), sigma = cmd->args.value("sigma_deg", 1.0);
            patch_filter_config(cfg.filter_config, [&](nlohmann::json& j) {
              for (auto& c : j["configs"])
                if (c.value("type", "") == "StandardOrchestrationConfig" && c.contains("alignment_config")) {
                  c["alignment_config"]["heading"] = deg * M_PI / 180.0;
                  c["alignment_config"]["heading_sigma"] = sigma * M_PI / 180.0;
                }
            });
            say("INFO", "initial heading set to " + std::to_string(deg) + " deg by " + cmd->source + " (takes effect on the next filter reset)");
          } else if (cmd->kind == "set_leverarm") {
            const std::string label = cmd->args.at("label").get<std::string>();
            const std::vector<double> arm{cmd->args.at("x").get<double>(), cmd->args.at("y").get<double>(), cmd->args.at("z").get<double>()};
            bool found = false;
            patch_filter_config(cfg.filter_config, [&](nlohmann::json& j) {
              for (auto& c : j["configs"])
                if (c.value("type", "") == "StandardOrchestrationConfig")
                  for (auto& mp : c["mp_configs"])
                    if (mp.value("label", "") == label) {
                      mp["lever_arm"] = arm;
                      found = true;
                    }
            });
            if (!found) throw std::runtime_error("no measurement processor labelled " + label);
            say("INFO", "lever arm of " + label + " set by " + cmd->source + " (takes effect on the next filter reset)");
          } else if (cmd->kind == "save_config") {
            edge::EdgeConfig next = edge::edge_config_from_json(cmd->args, std::filesystem::path(cfg.path).parent_path().string());
            next.path = cfg.path;
            const bool nmea_changed = edge::edge_config_to_json(next)["nmea"] != edge::edge_config_to_json(cfg)["nmea"];
            cfg = next;
            apply_sentences();
            if (!cfg.path.empty()) edge::save_edge_config(cfg, cfg.path);
            if (nmea_changed) {
              stop_runtime(*rt);
              rt.reset();
              rt = start_runtime(cfg, hub, nopt);
            }
            say("INFO", "configuration saved by " + cmd->source + (nmea_changed ? " (NMEA applied)" : " (other changes apply on restart)"));
          } else {
            throw std::runtime_error("unknown command " + cmd->kind);
          }
        } catch (const std::exception& e) {
          say("ERROR", "command " + cmd->kind + " from " + cmd->source + " failed: " + e.what());
          hub.event("CONTROL", "failed " + cmd->kind + ": " + e.what());
        }
      }
      if (g_stop) break;

      if (std::chrono::steady_clock::now() < next_status) continue;
      next_status += cfg.status.interval_sec > 0 ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(cfg.status.interval_sec))
                                                 : std::chrono::seconds(1);
      edge::StatusSnapshot snap;
      {
        std::lock_guard lk(rt->mutex);
        snap = rt->status;
        snap.last_solution_age_sec = rt->status.solutions ? std::chrono::duration<double>(std::chrono::steady_clock::now() - rt->last_solution_wall).count() : -1;
        snap.tcp_clients = rt->out->tcp_clients();
        snap.nmea_sentences = rt->out->sentences_sent();
      }
      snap.wall = std::chrono::system_clock::now();
      snap.uptime_sec = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count());
      snap.input = rt->input->counters();
      snap.filter_error = rt->filter->error_logged();
      snap.gating = edge::gating_from_registry(*rt->filter);
      snap.integrity = edge::integrity_from_registry(*rt->filter);
      const auto j = edge::status_to_json(snap);
      hub.set_status(j);
      // events worth a line on the timeline
      const std::string health = j.value("health", "");
      if (health != last_health) {
        if (!last_health.empty()) hub.event("HEALTH", last_health + " -> " + health);
        last_health = health;
      }
      if (snap.integrity.present) {
        if (snap.integrity.pace != last_pace) {
          if (!last_pace.empty()) hub.event("PACE", last_pace + " -> " + snap.integrity.pace + " (HPL " + std::to_string(snap.integrity.hpl_m) + " m)");
          last_pace = snap.integrity.pace;
        }
        std::string ex;
        for (const auto& e : snap.integrity.excluded) ex += (ex.empty() ? "" : "+") + e;
        if (ex != last_excluded) {
          hub.event(ex.empty() ? "READMIT" : "EXCLUDE", ex.empty() ? "all sources trusted again" : "excluded: " + ex);
          last_excluded = ex;
        }
      }
      edge::write_status_file(cfg.status.file, j);
      if (print_status) std::cerr << j.dump() << "\n";
    }
    stop_runtime(*rt);
    const int code = rt->filter->stop();
    web.stop();
    pntos::cobra::utils::set_log_sink({});
    std::cerr << "pnt-edge: stopped; " << rt->status.solutions << " solutions, " << rt->out->sentences_sent() << " NMEA sentences\n";
    return g_exit_code ? g_exit_code.load() : code;
  } catch (const std::exception& e) {
    std::cerr << "pnt-edge: " << e.what() << "\n";
    return 2;
  }
}
