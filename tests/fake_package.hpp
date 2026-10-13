// A release-shaped package for the update tests: the DESTDIR tree the release workflow tars up, with shell scripts
// standing in for the executables (so --version "runs" on any machine).
#pragma once

#include "web/update.hpp"

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fake_package {

inline void write(const std::filesystem::path& p, const std::string& s, bool exec = false) {
  std::filesystem::create_directories(p.parent_path());
  std::ofstream(p) << s;
  if (exec) ::chmod(p.c_str(), 0755);
}

inline std::string slurp(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/// Returns the .tar.gz bytes of a package whose pnt-edge reports `version`; "" if tar failed.
inline std::string make(const std::filesystem::path& scratch, const std::string& version) {
  namespace fs = std::filesystem;
  const fs::path tree = scratch / ("tree-" + version);
  fs::remove_all(tree);
  write(tree / "usr/local/bin/pnt-edge", "#!/bin/sh\necho \"pnt-edge " + version + " (Cobra 0.2.8)\"\n", true);
  write(tree / "usr/local/bin/pnt-edge-serial-sim", "#!/bin/sh\necho \"sim " + version + "\"\n", true);
  write(tree / "usr/local/share/pnt-edge/www/tactical/index.html", "<html>" + version + "</html>");
  write(tree / "usr/local/share/pnt-edge/www/maintenance/index.html", "<html>maint " + version + "</html>");
  write(tree / "usr/local/lib/x86_64-linux-gnu/libfake.so.1", "lib " + version);
  write(tree / "usr/local/lib/systemd/system/pnt-edge.service", "[Unit] " + version);
  write(tree / "usr/local/share/pntos-cobra/configs/pos_ins.json", "{\"v\": \"" + version + "\"}");
  write(tree / "usr/local/etc/pnt-edge/pnt-edge.json", "{\"packaged config\": \"" + version + "\"}");
  const fs::path tgz = scratch / ("pkg-" + version + ".tar.gz");
  std::string out;
  if (edge::run_command({"tar", "-C", tree.string(), "-czf", tgz.string(), "."}, &out) != 0) return "";
  return slurp(tgz);
}

/// A fake installed pnt-edge (script) with a www directory, as the updater's targets.
inline edge::UpdateTargets install(const std::filesystem::path& root, const std::string& version) {
  write(root / "install" / "pnt-edge", "#!/bin/sh\necho \"pnt-edge " + version + " (Cobra 0.2.8)\"\n", true);
  write(root / "install" / "pnt-edge-serial-sim", "#!/bin/sh\necho \"sim " + version + "\"\n", true);
  write(root / "install" / "www" / "tactical" / "index.html", "<html>" + version + "</html>");
  return {(root / "install" / "pnt-edge").string(), (root / "install" / "www").string(), (root / "updates").string(), version, "0.2.8"};
}

/// A fake installed tree <root>/prefix/{bin,lib,share,etc}: the appliance layout.
inline edge::UpdateTargets install_prefix(const std::filesystem::path& root, const std::string& version) {
  const std::filesystem::path prefix = root / "prefix";
  write(prefix / "bin" / "pnt-edge", "#!/bin/sh\necho \"pnt-edge " + version + " (Cobra 0.2.8)\"\n", true);
  write(prefix / "lib" / "x86_64-linux-gnu" / "libfake.so.1", "lib " + version);
  write(prefix / "lib" / "systemd" / "system" / "pnt-edge.service", "[Unit] operator");
  write(prefix / "share" / "pnt-edge" / "www" / "tactical" / "index.html", "<html>" + version + "</html>");
  write(prefix / "share" / "pnt-edge" / "www" / "tactical" / "stale.js", "goes away with the swap");
  write(prefix / "etc" / "pnt-edge" / "pnt-edge.json", "{\"operator config\": true}");
  return {(prefix / "bin" / "pnt-edge").string(), (prefix / "share" / "pnt-edge" / "www").string(), (root / "updates").string(), version, "0.2.8"};
}

}  // namespace fake_package
