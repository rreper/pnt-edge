// pnt-edge: the Cobra navigation filter as an edge appliance process.
//   pnt-edge config.json [--input-log file] [--once] [--print-status]
// Reads measurements (LCM log or LCM UDP multicast), runs the filter through the push API, emits NMEA 0183
// on the tactical interface and keeps a JSON status snapshot for the maintenance UI.
#include "edge_config.hpp"
#include "inputs.hpp"
#include "nmea.hpp"
#include "outputs.hpp"
#include "status.hpp"
#include "integrity.hpp"
#ifdef PNT_EDGE_HAS_INTEGRITY
#include <cobra_integrity/register.hpp>
#endif

#include <pntos/cobra/app/AppBuilder.hpp>
#include <pntos/cobra/app/Filter.hpp>
#include <pntos/cobra/utils/geoid.hpp>

#include <atomic>
#include <csignal>
#include <iostream>
#include <set>
#include <thread>

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
#ifdef PNT_EDGE_HAS_INTEGRITY
  cobra_integrity::register_plugins();  // makes "orchestration": "protected" available to the filter config
#endif
  if (argc < 2 || std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
    std::cerr << "usage: pnt-edge config.json [--input-log file] [--once] [--print-status]\n";
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
      } else {
        throw std::runtime_error("unknown option " + a);
      }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // the filter
    pntos::cobra::AppConfig app = pntos::cobra::jsoncfg::load_app_config(cfg.filter_config);
    app.app.logging_level = cfg.logging_level;
    pntos::cobra::app::RunOptions options;
    options.progress = false;
    options.legacy_q_rotation = cfg.legacy_q_rotation;
    std::set<std::string> default_channels;
    for (const auto& c : app.configs)
      if (auto* t = dynamic_cast<const pntos::cobra::LcmLogTransportConfig*>(c.get()); t && t->channels_to_process)
        default_channels.insert(t->channels_to_process->begin(), t->channels_to_process->end());
    pntos::cobra::Filter filter(app, options);

    // NMEA
    edge::NmeaOptions nopt;
    nopt.talker = cfg.nmea.talker;
    if (!cfg.geoid_file.empty()) {
      std::string err;
      if (auto g = pntos::cobra::nav::Geoid::load(cfg.geoid_file, &err)) nopt.geoid = std::make_shared<const pntos::cobra::nav::Geoid>(std::move(*g));
      else std::cerr << "pnt-edge: " << err << " (GGA altitude will be HAE)\n";
    } else {
      nopt.geoid = pntos::cobra::nav::default_geoid();
    }
    auto enabled = [&](const char* s) { return std::find(cfg.nmea.sentences.begin(), cfg.nmea.sentences.end(), s) != cfg.nmea.sentences.end(); };
    nopt.gga = enabled("GGA"); nopt.rmc = enabled("RMC"); nopt.vtg = enabled("VTG"); nopt.hdt = enabled("HDT");
    nopt.gst = enabled("GST"); nopt.zda = enabled("ZDA"); nopt.pashr = enabled("PASHR");
    nopt.integ = enabled("INTEG");
    edge::NmeaOutput out(cfg.nmea.udp_targets, cfg.nmea.bind_address, cfg.nmea.tcp_port);
    const std::int64_t utc_offset_ns = static_cast<std::int64_t>(cfg.nmea.utc_offset_sec * 1e9);
    const std::int64_t min_gap_ns = cfg.nmea.rate_hz > 0 ? static_cast<std::int64_t>(1e9 / cfg.nmea.rate_hz) : 0;

    std::mutex state_mutex;
    edge::StatusSnapshot status;
    const auto start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_solution_wall;
    std::int64_t last_emitted_tov = 0;
    filter.set_solution_callback([&](const pntos::api::Message& m) {
      edge::Pva p;
      if (!edge::pva_from_message(m, p, utc_offset_ns)) return;
      std::lock_guard lk(state_mutex);
      ++status.solutions;
      status.last_solution = p;
      last_solution_wall = std::chrono::steady_clock::now();
      if (min_gap_ns > 0 && last_emitted_tov != 0 && p.tov_ns - last_emitted_tov < min_gap_ns - 1'000'000) return;
      last_emitted_tov = p.tov_ns;
      const auto integ = edge::integrity_from_registry(filter);
      p.fix_quality = edge::fix_quality_for(integ, p.fix_quality);
      for (const auto& s : edge::sentences(p, nopt)) out.send(s);
      if (nopt.integ && integ.present) out.send(edge::integ_sentence(integ));
      status.nmea_sentences = out.sentences_sent();
    });

    auto input = edge::make_input(filter, cfg.input, default_channels);
    input->start();
    std::cerr << "pnt-edge: running (" << cfg.input.type << (cfg.input.type == "lcm_log" ? " " + cfg.input.path : " " + cfg.input.url) << "), NMEA to "
              << cfg.nmea.udp_targets.size() << " UDP target(s)" << (cfg.nmea.tcp_port ? " and TCP port " + std::to_string(cfg.nmea.tcp_port) : "") << "\n";

    // status loop
    auto next_status = std::chrono::steady_clock::now();
    while (!g_stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (once && input->finished()) break;
      if (std::chrono::steady_clock::now() < next_status) continue;
      next_status += std::chrono::duration<double>(cfg.status.interval_sec) > std::chrono::duration<double>(0)
                         ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(cfg.status.interval_sec))
                         : std::chrono::seconds(1);
      edge::StatusSnapshot snap;
      {
        std::lock_guard lk(state_mutex);
        snap = status;
        snap.last_solution_age_sec = status.solutions ? std::chrono::duration<double>(std::chrono::steady_clock::now() - last_solution_wall).count() : -1;
      }
      snap.wall = std::chrono::system_clock::now();
      snap.uptime_sec = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count());
      snap.input = input->counters();
      snap.filter_error = filter.error_logged();
      snap.gating = edge::gating_from_registry(filter);
      snap.integrity = edge::integrity_from_registry(filter);
      snap.tcp_clients = out.tcp_clients();
      const auto j = edge::status_to_json(snap);
      edge::write_status_file(cfg.status.file, j);
      if (print_status) std::cerr << j.dump() << "\n";
    }
    input->stop();
    const int code = filter.stop();
    std::cerr << "pnt-edge: stopped; " << status.solutions << " solutions, " << out.sentences_sent() << " NMEA sentences\n";
    return code;
  } catch (const std::exception& e) {
    std::cerr << "pnt-edge: " << e.what() << "\n";
    return 2;
  }
}
