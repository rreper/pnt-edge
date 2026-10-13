#include "web/update.hpp"

#include "web/auth.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace edge {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

double now_s() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

bool write_file(const fs::path& p, const std::string& data, std::string* error) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!f) {
    if (error) *error = "cannot write " + p.string();
    return false;
  }
  return true;
}

std::string read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/// First match of `rel` (a relative path) at most `depth` directories below root, or "".
fs::path find_below(const fs::path& root, const fs::path& rel, int depth) {
  std::error_code ec;
  if (fs::exists(root / rel, ec)) return root / rel;
  if (depth == 0) return {};
  for (const auto& e : fs::directory_iterator(root, ec)) {
    if (!e.is_directory(ec)) continue;
    auto p = find_below(e.path(), rel, depth - 1);
    if (!p.empty()) return p;
  }
  return {};
}

/// Copies a regular file to a temporary name next to `dest` and renames it over `dest` (atomic for a running binary).
bool replace_file(const fs::path& src, const fs::path& dest, std::string* error) {
  std::error_code ec;
  const fs::path tmp = dest.string() + ".new";
  fs::remove(tmp, ec);
  fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    if (error) *error = "cannot write " + tmp.string() + ": " + ec.message();
    return false;
  }
  ::chmod(tmp.c_str(), 0755);
  fs::rename(tmp, dest, ec);
  if (ec) {
    if (error) *error = "cannot replace " + dest.string() + ": " + ec.message();
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

/// Replaces directory `dest` by a copy of `src` (copy next to it, swap by rename, remove the old one).
bool replace_dir(const fs::path& src, const fs::path& dest, std::string* error) {
  std::error_code ec;
  const fs::path tmp = dest.string() + ".new", old = dest.string() + ".old";
  fs::remove_all(tmp, ec);
  fs::remove_all(old, ec);
  fs::copy(src, tmp, fs::copy_options::recursive, ec);
  if (ec) {
    if (error) *error = "cannot write " + tmp.string() + ": " + ec.message();
    return false;
  }
  if (fs::exists(dest, ec)) fs::rename(dest, old, ec);
  if (ec) {
    if (error) *error = "cannot move " + dest.string() + " aside: " + ec.message();
    fs::remove_all(tmp, ec);
    return false;
  }
  fs::rename(tmp, dest, ec);
  if (ec) {
    if (error) *error = "cannot install " + dest.string() + ": " + ec.message();
    fs::rename(old, dest, ec);
    return false;
  }
  fs::remove_all(old, ec);
  return true;
}

}  // namespace

int run_command(const std::vector<std::string>& argv, std::string* output, int timeout_s, const std::vector<std::string>& env) {
  int fds[2];
  if (::pipe(fds) != 0) return -1;
  const pid_t pid = ::fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    ::dup2(fds[1], 1);
    ::dup2(fds[1], 2);
    ::close(fds[0]);
    ::close(fds[1]);
    for (const auto& e : env) {
      const auto eq = e.find('=');
      if (eq != std::string::npos) ::setenv(e.substr(0, eq).c_str(), e.substr(eq + 1).c_str(), 1);
    }
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    ::execvp(args[0], args.data());
    _exit(127);
  }
  ::close(fds[1]);
  std::string out;
  char buf[4096];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
  for (;;) {
    pollfd p{fds[0], POLLIN, 0};
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) {
      ::kill(pid, SIGKILL);
      break;
    }
    const int r = ::poll(&p, 1, static_cast<int>(left));
    if (r <= 0) continue;
    const ssize_t n = ::read(fds[0], buf, sizeof buf);
    if (n <= 0) break;
    if (out.size() < 65536) out.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fds[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  if (output) *output = out;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
}

std::string Updater::sha256_hex(const std::string& data) { return auth::hex(auth::sha256(data)); }

std::string Updater::parse_sha256(const std::string& text) {
  static const std::regex re("[0-9a-fA-F]{64}");
  std::smatch m;
  if (!std::regex_search(text, m, re)) return "";
  std::string s = m.str();
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

Updater::Updater(UpdateTargets t) : t_(std::move(t)) {
  std::error_code ec;
  fs::create_directories(fs::path(t_.dir) / "staged", ec);
  const auto hist = fs::path(t_.dir) / "history.json";
  if (fs::exists(hist, ec)) {
    try {
      history_ = json::parse(read_file(hist));
      if (!history_.is_array()) history_ = json::array();
    } catch (const std::exception&) {
      history_ = json::array();
    }
  }
}

bool Updater::installed_layout() const { return fs::path(t_.exe_path).parent_path().filename() == "bin"; }

std::string Updater::prefix_dir() const {
  const fs::path exe(t_.exe_path);
  return installed_layout() ? exe.parent_path().parent_path().string() : exe.parent_path().string();
}

void Updater::record(const std::string& action, const std::string& who, const std::string& from, const std::string& to, const std::string& result) {
  history_.push_back({{"t", now_s()}, {"action", action}, {"who", who}, {"from", from}, {"to", to}, {"result", result}});
  while (history_.size() > 50) history_.erase(history_.begin());
  std::string err;
  write_file(fs::path(t_.dir) / "history.json", history_.dump(1), &err);
}

namespace {
/// lib/ and lib/<multiarch>/ below a prefix, for LD_LIBRARY_PATH.
std::string lib_dirs(const fs::path& prefix) {
  std::error_code ec;
  std::string out;
  const fs::path lib = prefix / "lib";
  if (!fs::is_directory(lib, ec)) return out;
  out = lib.string();
  for (const auto& e : fs::directory_iterator(lib, ec))
    if (e.is_directory(ec) && e.path().filename() != "systemd") out += ":" + e.path().string();
  return out;
}
/// Package files to install under the prefix, relative to it: bin/, lib/ (not systemd units), share/ (not the www
/// directory, which is swapped whole); never etc/ (configuration belongs to the operator).
std::vector<fs::path> package_files(const fs::path& pkg_prefix) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const char* top : {"bin", "lib", "share"}) {
    const fs::path dir = pkg_prefix / top;
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
      if (!e.is_regular_file(ec)) continue;
      const fs::path rel = fs::relative(e.path(), pkg_prefix, ec);
      const std::string s = rel.generic_string();
      if (s.rfind("lib/systemd/", 0) == 0 || s.rfind("share/pnt-edge/www/", 0) == 0) continue;
      out.push_back(rel);
    }
  }
  return out;
}
}  // namespace

namespace {
/// Runs `exe --version`; a release before 0.2.0 has no such flag and prints the usage (exit 2) but did start, so it
/// counts as running and its version comes from the package file name. Returns the exit status (0 = runs).
int probe_version(const std::string& exe, const std::vector<std::string>& env, const std::string& filename, std::string* version_line) {
  std::string out;
  int rc = run_command({exe, "--version"}, &out, 20, env);
  if (rc == 2 && out.find("usage: pnt-edge") != std::string::npos) {
    static const std::regex tag("v?([0-9]+\\.[0-9]+\\.[0-9]+)");
    std::smatch m;
    out = "pnt-edge " + (std::regex_search(filename, m, tag) ? m[1].str() : std::string("(release before 0.2.0)")) + " (older release)\n";
    rc = 0;
  }
  if (version_line) *version_line = out.substr(0, out.find('\n'));
  return rc;
}
}  // namespace

bool Updater::inspect(StagedPackage& p, std::string* error) {
  std::error_code ec;
  const fs::path extract = fs::path(t_.dir) / "extract";
  fs::remove_all(extract, ec);
  fs::create_directories(extract, ec);
  std::string out;
  const int rc = run_command({"tar", "-xzf", p.path, "-C", extract.string()}, &out, 120);
  if (rc != 0) {
    p.problem = "not a readable .tar.gz (tar exit " + std::to_string(rc) + "): " + out.substr(0, 200);
    return true;
  }
  const fs::path bin = find_below(extract, fs::path("bin") / "pnt-edge", 4);
  if (bin.empty()) {
    p.problem = "no bin/pnt-edge in the package (is this the release binary tarball, not the source bundle?)";
    return true;
  }
  p.binary = bin.string();
  p.pkg_prefix = bin.parent_path().parent_path().string();
  const fs::path www = fs::path(p.pkg_prefix) / "share" / "pnt-edge" / "www";
  if (fs::is_directory(www, ec)) p.www = www.string();
  ::chmod(bin.c_str(), 0755);
  // run the packaged binary once: in the installed layout with the libraries it brings (they are installed too), in
  // the development layout as the system finds them (only the executable is replaced there)
  std::vector<std::string> env;
  if (installed_layout()) {
    std::string lp = lib_dirs(p.pkg_prefix);
    if (const char* cur = std::getenv("LD_LIBRARY_PATH")) lp += std::string(lp.empty() ? "" : ":") + cur;
    if (!lp.empty()) env.push_back("LD_LIBRARY_PATH=" + lp);
  }
  std::string ver;
  const int vrc = probe_version(bin.string(), env, p.file, &ver);
  if (vrc != 0) {
    p.problem = "the packaged binary does not run on this machine (exit " + std::to_string(vrc) + "): " + ver.substr(0, 300);
    if (!installed_layout() && ver.find("shared librar") != std::string::npos)
      p.problem += " (development layout: only the executable and www are replaced; install under a prefix with bin/ and lib/ for full updates)";
    return true;
  }
  p.version = ver;
  if (!p.sha256_expected.empty() && p.sha256_expected != p.sha256) p.problem = "sha256 does not match the published checksum";
  else if (p.www.empty()) p.problem = "no share/pnt-edge/www in the package";
  (void)error;
  return true;
}

bool Updater::stage(const std::string& filename, const std::string& data, const std::string& sha256_text, const std::string& who, std::string* error) {
  std::lock_guard lk(m_);
  if (data.empty()) {
    if (error) *error = "empty upload";
    return false;
  }
  StagedPackage p;
  p.file = fs::path(filename).filename().string();
  if (p.file.empty()) p.file = "package.tar.gz";
  p.path = (fs::path(t_.dir) / "staged" / "package.tar.gz").string();
  p.size = data.size();
  p.sha256 = sha256_hex(data);
  p.sha256_expected = parse_sha256(sha256_text);
  p.uploaded_by = who;
  p.uploaded_t = now_s();
  if (!write_file(p.path, data, error)) return false;
  inspect(p, error);
  staged_ = p;
  record("stage", who, t_.version, p.version.empty() ? p.file : p.version, p.problem.empty() ? "ok" : p.problem);
  return true;
}

bool Updater::discard(std::string* error) {
  std::lock_guard lk(m_);
  std::error_code ec;
  fs::remove_all(fs::path(t_.dir) / "extract", ec);
  fs::remove(fs::path(t_.dir) / "staged" / "package.tar.gz", ec);
  staged_.reset();
  (void)error;
  return true;
}

nlohmann::json Updater::backup_info() const {
  std::error_code ec;
  const fs::path info = fs::path(t_.dir) / "backup" / "version.json";
  if (!fs::exists(info, ec)) return nullptr;
  try {
    return json::parse(read_file(info));
  } catch (const std::exception&) {
    return nullptr;
  }
}

/// Puts every file of backup/files/ back under the prefix, removes the files the update added, swaps the www
/// directory back. Caller holds the mutex.
bool Updater::restore_backup(std::string* error) {
  std::error_code ec;
  const fs::path backup = fs::path(t_.dir) / "backup";
  const auto info = backup_info();
  if (info.is_null()) {
    if (error) *error = "no backup to roll back to";
    return false;
  }
  const fs::path base = prefix_dir();
  std::string err;
  const fs::path files = backup / "files";
  if (fs::is_directory(files, ec))
    for (const auto& e : fs::recursive_directory_iterator(files, ec)) {
      if (!e.is_regular_file(ec)) continue;
      const fs::path rel = fs::relative(e.path(), files, ec);
      fs::create_directories((base / rel).parent_path(), ec);
      if (!replace_file(e.path(), base / rel, &err)) {
        if (error) *error = err;
        return false;
      }
    }
  for (const auto& a : info.value("added", json::array())) {
    const fs::path f = base / a.get<std::string>();
    fs::remove(f, ec);
    // directories the update created for them go too, while they are empty and below the prefix
    for (fs::path d = f.parent_path(); d != base && d.string().size() > base.string().size() && fs::is_directory(d, ec) && fs::is_empty(d, ec); d = d.parent_path())
      fs::remove(d, ec);
  }
  if (fs::is_directory(backup / "www", ec) && !replace_dir(backup / "www", t_.www_dir, &err)) {
    if (error) *error = err;
    return false;
  }
  return true;
}

bool Updater::apply(const std::string& who, std::string* error) {
  std::lock_guard lk(m_);
  if (!staged_ || !staged_->applicable()) {
    if (error) *error = staged_ ? staged_->problem : "nothing staged";
    return false;
  }
  const std::string to = staged_->version;
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    record("apply", who, t_.version, to, why);
    std::error_code ec2;
    fs::remove_all(fs::path(t_.dir) / "backup", ec2);  // whatever was restored is back in place; nothing to roll back to
    return false;
  };
  std::error_code ec;
  const fs::path backup = fs::path(t_.dir) / "backup", files = backup / "files";
  fs::remove_all(backup, ec);
  fs::create_directories(files, ec);
  if (!fs::exists(t_.exe_path, ec)) return fail("the running executable " + t_.exe_path + " is not where it was started from");
  // what goes where
  const fs::path base = prefix_dir(), pkg_prefix = staged_->pkg_prefix;
  std::vector<std::pair<fs::path, fs::path>> plan;  // package file -> destination
  if (installed_layout()) {
    for (const auto& rel : package_files(pkg_prefix)) plan.emplace_back(pkg_prefix / rel, base / rel);
  } else {
    plan.emplace_back(staged_->binary, t_.exe_path);
    const fs::path sim = fs::path(staged_->binary).parent_path() / "pnt-edge-serial-sim", sim_dest = base / "pnt-edge-serial-sim";
    if (fs::exists(sim, ec) && fs::exists(sim_dest, ec)) plan.emplace_back(sim, sim_dest);
  }
  // 1. back up what will be replaced; note what is new
  json added = json::array();
  std::string err;
  for (const auto& [src, dest] : plan) {
    const fs::path rel = fs::relative(dest, base, ec);
    if (fs::exists(dest, ec)) {
      fs::create_directories((files / rel).parent_path(), ec);
      fs::copy_file(dest, files / rel, fs::copy_options::overwrite_existing, ec);
      if (ec) return fail("cannot back up " + dest.string() + ": " + ec.message());
    } else {
      added.push_back(rel.generic_string());
    }
  }
  if (fs::is_directory(t_.www_dir, ec)) fs::copy(t_.www_dir, backup / "www", fs::copy_options::recursive, ec);
  write_file(backup / "version.json", json{{"version", t_.version}, {"cobra", t_.cobra}, {"t", now_s()}, {"by", who}, {"added", added}, {"files", plan.size()}}.dump(), &err);
  // 2. install, executable last so a half-done install never leaves a new binary with old libraries
  std::stable_partition(plan.begin(), plan.end(), [&](const auto& pr) { return pr.second != fs::path(t_.exe_path); });
  for (const auto& [src, dest] : plan) {
    fs::create_directories(dest.parent_path(), ec);
    if (!replace_file(src, dest, &err)) {
      std::string e2;
      restore_backup(&e2);
      return fail(err + " (is the install location writable by the service user?); previous files restored" + (e2.empty() ? "" : ", restore reported: " + e2));
    }
  }
  if (!replace_dir(staged_->www, t_.www_dir, &err)) {
    std::string e2;
    restore_backup(&e2);
    return fail(err + "; previous files restored");
  }
  if (installed_layout() && (base == "/usr" || base == "/usr/local")) {
    std::string out;
    run_command({"ldconfig"}, &out, 60);  // new sonames reach the loader cache; harmless when not root
  }
  // 3. the installed executable must start where it is (library path, loader) before we ever re-exec into it
  std::string ver;
  const int rc = probe_version(t_.exe_path, {}, staged_->file, &ver);
  if (rc != 0) {
    std::string e2;
    restore_backup(&e2);
    return fail("the installed executable does not start in " + base.string() + " (exit " + std::to_string(rc) + "): " + ver.substr(0, 300) + "; previous version restored");
  }
  record("apply", who, t_.version, to, "ok, restart pending");
  restart_pending_ = true;
  fs::remove_all(fs::path(t_.dir) / "extract", ec);
  fs::remove(fs::path(staged_->path), ec);
  staged_.reset();
  return true;
}

bool Updater::rollback(const std::string& who, std::string* error) {
  std::lock_guard lk(m_);
  const auto info = backup_info();
  const std::string to = info.is_null() ? "" : info.value("version", "");
  std::string err;
  if (!restore_backup(&err)) {
    if (error) *error = err;
    record("rollback", who, t_.version, to, err);
    return false;
  }
  record("rollback", who, t_.version, to, "ok, restart pending");
  restart_pending_ = true;
  std::error_code ec;
  fs::remove_all(fs::path(t_.dir) / "backup", ec);
  return true;
}

nlohmann::json Updater::state() const {
  std::lock_guard lk(m_);
  json j;
  j["running"] = {{"version", t_.version}, {"cobra", t_.cobra}, {"executable", t_.exe_path}, {"www", t_.www_dir}, {"dir", t_.dir},
                  {"layout", installed_layout() ? "installed" : "development"}, {"prefix", prefix_dir()}};
  if (staged_) {
    const auto& p = *staged_;
    j["staged"] = {{"file", p.file}, {"size", p.size}, {"sha256", p.sha256}, {"sha256_expected", p.sha256_expected}, {"version", p.version},
                   {"problem", p.problem}, {"applicable", p.applicable()}, {"uploaded_by", p.uploaded_by}, {"uploaded_t", p.uploaded_t}};
  } else {
    j["staged"] = nullptr;
  }
  j["backup"] = backup_info();
  j["restart_pending"] = restart_pending_;
  j["history"] = history_;
  return j;
}

}  // namespace edge
