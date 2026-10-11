#include "ppnt.hpp"

#include "nmea.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace edge::ppnt {

using nlohmann::json;

namespace {
std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
  while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  return s;
}
std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}
std::string num(double x, int decimals) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, x);
  return buf;
}
std::string iso_utc(double unix_s) {
  const std::time_t t = static_cast<std::time_t>(unix_s);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
  return buf;
}
double wall_now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
}  // namespace

std::string ack(const std::string& command, const std::string& result, const std::string& message) {
  return nmea_wrap("PPNT,ACK," + command + "," + result + (message.empty() ? "" : "," + message));
}

std::optional<Sentence> parse(const std::string& line_in, std::string* error) {
  const std::string line = trim(line_in);
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return std::nullopt;
  };
  if (line.size() > 82) return fail("too long");
  if (line.rfind("$PPNT,", 0) != 0) return fail("not a $PPNT sentence");
  const auto star = line.find('*');
  if (star == std::string::npos || star + 3 != line.size()) return fail("missing checksum");
  const std::string body = line.substr(1, star - 1);
  const std::string given = line.substr(star + 1, 2);
  const std::string want = nmea_checksum(body);
  auto up = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; };
  if (up(given) != want) return fail("bad checksum");
  auto fields = split(body, ',');  // PPNT, command, kind, ...
  if (fields.size() < 3) return fail("missing command or kind");
  Sentence s;
  s.command = up(fields[1]);
  s.kind = up(fields[2]);
  s.fields.assign(fields.begin() + 3, fields.end());
  if (s.command.empty()) return fail("empty command");
  if (s.kind != "Q" && s.kind != "S" && s.kind != "R") return fail("kind must be Q, S or R");
  return s;
}

std::vector<std::string> handle(const std::string& line, const std::string& from, const Context& ctx) {
  if (trim(line).rfind("$PPNT,ACK,", 0) == 0) return {};  // acknowledgements are never answered
  std::string error;
  auto s = parse(line, &error);
  if (!s) {
    // name the command if the line at least looks like one
    std::string cmd = "?";
    const auto first = line.find(','), second = line.find(',', first + 1);
    if (line.rfind("$PPNT,", 0) == 0 && first != std::string::npos && second != std::string::npos) cmd = line.substr(first + 1, second - first - 1);
    for (auto& c : cmd) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return {ack(cmd, "ERR", error)};
  }
  Hub& hub = *ctx.hub;
  const std::string who = "ppnt";
  auto field = [&](std::size_t i) { return i < s->fields.size() ? s->fields[i] : std::string(); };
  auto number = [&](std::size_t i, double* out) {
    const std::string f = field(i);
    if (f.empty()) return false;
    try {
      *out = std::stod(f);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  };

  if (s->command == "STATUS" && s->kind == "Q") {
    const json st = hub.status();
    const json sol = hub.solution();
    const double age = st.value("last_solution_age_sec", -1.0);
    int fix = 0;
    if (st.contains("last_solution")) {
      fix = 1;
      if (st.contains("integrity") && st["integrity"].is_object() && st["integrity"].contains("pace")) {
        const std::string pace = st["integrity"]["pace"];
        fix = pace == "EMERGENCY" ? 0 : (pace == "ALTERNATE" || pace == "CONTINGENCY") ? 6 : 1;
      }
    }
    std::string sigma_h, sigma_v, sigma_hdg;
    if (st.contains("last_solution")) {
      const auto& ls = st["last_solution"];
      const auto sig = ls.value("sigma_ned_m", std::vector<double>{0, 0, 0});
      if (sig.size() >= 3) {
        sigma_h = num(std::hypot(sig[0], sig[1]), 2);
        sigma_v = num(sig[2], 2);
      }
      sigma_hdg = num(ls.value("sigma_heading_deg", 0.0), 2);
    }
    const std::string body = "PPNT,STATUS,R," + st.value("health", std::string("starting")) + "," + (age < 0 ? "" : num(age, 1)) + "," + std::to_string(fix) + "," +
                             "" /* satellites: not known to the filter */ + "," + sigma_h + "," + sigma_v + "," + sigma_hdg + "," + std::to_string(st.value("uptime_sec", 0ULL));
    return {nmea_wrap(body), ack("STATUS", "OK")};
  }
  if (s->command == "VER" && s->kind == "Q") {
    return {nmea_wrap("PPNT,VER,R," + ctx.version + "," + ctx.cobra_version + "," + (ctx.config_id ? ctx.config_id() : std::string())), ack("VER", "OK")};
  }
  if (s->command == "TIME" && s->kind == "Q") {
    const json sol = hub.solution();
    const double tov = sol.value("t", 0.0);
    const std::string source = ctx.time_source ? ctx.time_source() : (tov > 0 ? "gnss" : "free");
    const std::string utc = tov > 0 ? iso_utc(tov) : iso_utc(wall_now());
    const std::string offset = tov > 0 ? num((wall_now() - tov) * 1000.0, 0) : "";
    return {nmea_wrap("PPNT,TIME,R," + utc + "," + source + "," + offset), ack("TIME", "OK")};
  }
  if (s->command == "OUT" && s->kind == "S") {
    json args = json::object();
    double rate;
    if (number(0, &rate)) {
      if (rate < 0 || rate > 50) return {ack("OUT", "ERR", "rate must be 0..50 Hz")};
      args["rate_hz"] = rate;
    }
    if (!field(1).empty()) {
      std::vector<std::string> sentences;
      for (auto& x : split(field(1), '+'))
        if (!x.empty()) {
          for (auto& c : x) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
          sentences.push_back(x);
        }
      args["sentences"] = sentences;
    }
    if (args.empty()) return {ack("OUT", "ERR", "nothing to set")};
    hub.enqueue({"set_nmea", args, who, from});
    hub.audit(who, from, "set_nmea", args.dump());
    return {ack("OUT", "OK")};
  }
  if (s->command == "LEVERARM" && s->kind == "S") {
    double x, y, z;
    if (field(0).empty() || !number(1, &x) || !number(2, &y) || !number(3, &z)) return {ack("LEVERARM", "ERR", "need label,x,y,z")};
    const json args{{"label", field(0)}, {"x", x}, {"y", y}, {"z", z}};
    hub.enqueue({"set_leverarm", args, who, from});
    hub.audit(who, from, "set_leverarm", args.dump());
    return {ack("LEVERARM", "OK", "effective on the next filter reset")};
  }
  if (s->command == "HEADING" && s->kind == "S") {
    double deg, sigma = 1.0;
    if (!number(0, &deg)) return {ack("HEADING", "ERR", "need deg")};
    number(1, &sigma);
    const json args{{"deg", deg}, {"sigma_deg", sigma}};
    hub.enqueue({"set_heading", args, who, from});
    hub.audit(who, from, "set_heading", args.dump());
    return {ack("HEADING", "OK", "effective on the next filter reset")};
  }
  if (s->command == "RESET" && s->kind == "S") {
    const std::string what = field(0).empty() ? "filter" : field(0);
    if (what != "filter" && what != "app") return {ack("RESET", "ERR", "what must be filter or app")};
    const json args{{"what", what}};
    hub.enqueue({"reset", args, who, from});
    hub.audit(who, from, "reset", args.dump());
    return {ack("RESET", "OK")};
  }
  if (s->kind == "R") return {};  // replies are never answered
  return {ack(s->command, "ERR", "unknown")};
}

}  // namespace edge::ppnt
