// Software update from the maintenance UI (roadmap B.3): a release package (pnt-edge-<tag>-linux-<arch>.tar.gz as
// published by .github/workflows/release.yml, a DESTDIR tree with bin/pnt-edge and share/pnt-edge/www) is uploaded,
// staged and checked (sha256 against the published .sha256 when given, contents, and the packaged binary is run
// with --version to prove it executes here), then applied in place: the running executable and the www directory
// are backed up and replaced by rename, and the application re-executes itself. Roll back restores the backup.
// Two layouts: installed (`<prefix>/bin/pnt-edge`): everything the package carries under its prefix is installed
// (bin/, lib/, share/; never etc/ and never lib/systemd/), the previous files are kept in backup/; development
// (`build/pnt-edge`): only the executable, pnt-edge-serial-sim and the www directory. After installing, the new
// executable is run once in place; if it does not start there, the backup is put back at once.
// Everything lives under `dir`: staged/, extract/, backup/, history.json.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace edge {

struct UpdateTargets {
  std::string exe_path;   ///< the running executable (replaced on apply); canonical path taken at startup
  std::string www_dir;    ///< the served www directory (replaced on apply)
  std::string dir;        ///< state directory (created)
  std::string version;    ///< running pnt-edge version
  std::string cobra;      ///< running Cobra version
};

struct StagedPackage {
  std::string file;              ///< original file name
  std::string path;              ///< where it is staged
  std::uint64_t size = 0;
  std::string sha256;            ///< computed
  std::string sha256_expected;   ///< from the uploaded .sha256, empty if none was given
  std::string version;           ///< what the packaged binary reports with --version ("" if it would not run)
  std::string binary;            ///< path of bin/pnt-edge inside the extracted tree
  std::string www;               ///< path of share/pnt-edge/www inside the extracted tree
  std::string pkg_prefix;        ///< the package's install prefix inside the extracted tree (parent of bin/)
  std::string problem;           ///< empty when the package can be applied
  std::string uploaded_by;
  double uploaded_t = 0;
  bool applicable() const { return problem.empty(); }
};

class Updater {
 public:
  explicit Updater(UpdateTargets t);

  /// Stores and inspects an uploaded package; replaces any previously staged one. False with *error on failure.
  bool stage(const std::string& filename, const std::string& data, const std::string& sha256_text, const std::string& who, std::string* error);
  bool discard(std::string* error);
  /// Replaces the executable and the www directory by the staged package (backup kept). Does not restart.
  bool apply(const std::string& who, std::string* error);
  /// Puts the backup back.
  bool rollback(const std::string& who, std::string* error);

  /// {"running": {...}, "staged": {...}|null, "backup": {...}|null, "restart_pending": bool, "history": [...]}
  nlohmann::json state() const;
  bool restart_pending() const { std::lock_guard lk(m_); return restart_pending_; }
  std::optional<StagedPackage> staged() const { std::lock_guard lk(m_); return staged_; }

  /// `tar -tzf`-free helpers, exposed for tests.
  static std::string sha256_hex(const std::string& data);
  static std::string parse_sha256(const std::string& text);  ///< first 64 hex digits, lower case, or ""

 private:
  bool inspect(StagedPackage& p, std::string* error);
  bool installed_layout() const;  ///< exe lives in a bin/ directory
  std::string prefix_dir() const; ///< the install prefix (installed layout) or the exe directory
  bool restore_backup(std::string* error);
  void record(const std::string& action, const std::string& who, const std::string& from, const std::string& to, const std::string& result);
  nlohmann::json backup_info() const;

  UpdateTargets t_;
  mutable std::mutex m_;
  std::optional<StagedPackage> staged_;
  bool restart_pending_ = false;
  nlohmann::json history_ = nlohmann::json::array();
};

/// Runs argv (no shell), captures stdout+stderr, returns the exit status (-1 if it could not run, 127 if not found).
/// `env` entries (NAME=value) are set in the child. Exposed for tests.
int run_command(const std::vector<std::string>& argv, std::string* output, int timeout_s = 30, const std::vector<std::string>& env = {});

}  // namespace edge
