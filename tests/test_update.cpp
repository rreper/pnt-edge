// Software update: staging, checks (tarball, contents, checksum, the packaged binary runs), apply with backup, rollback,
// persistent history.
#include "fake_package.hpp"
#include "web/update.hpp"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>

using namespace edge;
namespace fs = std::filesystem;

namespace {
fs::path scratch() {
  const char* o = std::getenv("PNT_EDGE_TEST_OUT");
  return fs::path(o ? o : fs::temp_directory_path().string()) / "update-test";
}
std::string run(const std::string& exe) {
  std::string out;
  run_command({exe}, &out);
  return out;
}
}  // namespace

TEST(Updater, Sha256Helpers) {
  EXPECT_EQ(Updater::sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(Updater::parse_sha256("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  pnt-edge-v1-src.tar.gz\n"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(Updater::parse_sha256("not a checksum"), "");
  std::string out;
  EXPECT_EQ(run_command({"sh", "-c", "echo hi; exit 7"}, &out), 7);
  EXPECT_EQ(out, "hi\n");
  EXPECT_EQ(run_command({"/nonexistent/binary"}, &out), 127);
}

TEST(Updater, StageApplyRollbackHistory) {
  const fs::path root = scratch();
  fs::remove_all(root);
  const auto targets = fake_package::install(root, "0.1.0");
  const std::string exe = targets.exe_path, www_index = targets.www_dir + "/tactical/index.html";
  Updater u(targets);
  auto st = u.state();
  EXPECT_EQ(st["running"]["version"], "0.1.0");
  EXPECT_TRUE(st["staged"].is_null());
  EXPECT_TRUE(st["backup"].is_null());
  EXPECT_FALSE(st["restart_pending"].get<bool>());

  const std::string pkg = fake_package::make(root, "9.9.9");
  ASSERT_FALSE(pkg.empty());
  std::string err;
  // not a tarball: staged, reported, not applicable
  ASSERT_TRUE(u.stage("junk.tar.gz", "this is not a tarball", "", "user:admin", &err)) << err;
  ASSERT_TRUE(u.staged());
  EXPECT_FALSE(u.staged()->applicable());
  EXPECT_NE(u.staged()->problem.find("tar"), std::string::npos);
  EXPECT_FALSE(u.apply("user:admin", &err));
  // the source bundle shape (no bin/pnt-edge) is refused with a hint
  {
    const fs::path tree = root / "srcish";
    fake_package::write(tree / "pnt-edge-0.1.1" / "meson.build", "project()");
    std::string out;
    ASSERT_EQ(run_command({"tar", "-C", tree.string(), "-czf", (root / "src.tar.gz").string(), "."}, &out), 0);
    ASSERT_TRUE(u.stage("pnt-edge-v0.1.1-src.tar.gz", fake_package::slurp(root / "src.tar.gz"), "", "user:admin", &err));
    EXPECT_NE(u.staged()->problem.find("no bin/pnt-edge"), std::string::npos);
  }
  // right package, wrong checksum
  ASSERT_TRUE(u.stage("pnt-edge-v9.9.9-linux-x86_64.tar.gz", pkg, std::string(64, 'a') + "  pnt-edge-v9.9.9-linux-x86_64.tar.gz\n", "user:admin", &err));
  EXPECT_EQ(u.staged()->version, "pnt-edge 9.9.9 (Cobra 0.2.8)");
  EXPECT_NE(u.staged()->problem.find("sha256"), std::string::npos);
  EXPECT_FALSE(u.apply("user:admin", &err));
  // right package, right checksum
  ASSERT_TRUE(u.stage("pnt-edge-v9.9.9-linux-x86_64.tar.gz", pkg, Updater::sha256_hex(pkg) + "  pnt-edge-v9.9.9-linux-x86_64.tar.gz\n", "user:admin", &err));
  ASSERT_TRUE(u.staged()->applicable()) << u.staged()->problem;
  EXPECT_EQ(u.staged()->size, pkg.size());
  ASSERT_TRUE(u.apply("user:admin", &err)) << err;
  EXPECT_TRUE(u.restart_pending());
  EXPECT_EQ(run(exe), "pnt-edge 9.9.9 (Cobra 0.2.8)\n");
  EXPECT_EQ(run(targets.exe_path.substr(0, targets.exe_path.rfind('/')) + "/pnt-edge-serial-sim"), "sim 9.9.9\n");
  EXPECT_EQ(fake_package::slurp(www_index), "<html>9.9.9</html>");
  EXPECT_TRUE(fs::exists(targets.www_dir + "/maintenance/index.html"));
  st = u.state();
  EXPECT_TRUE(st["staged"].is_null());
  EXPECT_EQ(st["backup"]["version"], "0.1.0");
  EXPECT_GE(st["history"].size(), 5u);
  EXPECT_EQ(st["history"].back()["action"], "apply");
  EXPECT_EQ(st["history"].back()["to"], "pnt-edge 9.9.9 (Cobra 0.2.8)");
  // roll back
  ASSERT_TRUE(u.rollback("user:admin", &err)) << err;
  EXPECT_EQ(run(exe), "pnt-edge 0.1.0 (Cobra 0.2.8)\n");
  EXPECT_EQ(fake_package::slurp(www_index), "<html>0.1.0</html>");
  EXPECT_TRUE(u.state()["backup"].is_null());
  EXPECT_FALSE(u.rollback("user:admin", &err));
  // history survives a new instance (the restarted daemon)
  Updater u2(targets);
  EXPECT_EQ(u2.state()["history"].size(), u.state()["history"].size());
  EXPECT_FALSE(u2.restart_pending());
  // discard clears a staged package
  ASSERT_TRUE(u2.stage("x.tar.gz", pkg, "", "user:admin", &err));
  EXPECT_TRUE(u2.staged());
  ASSERT_TRUE(u2.discard(&err));
  EXPECT_FALSE(u2.staged());
}

TEST(Updater, InstalledLayoutInstallsLibsSparesConfig) {
  const fs::path root = scratch() / "prefix-layout";
  fs::remove_all(root);
  const auto targets = fake_package::install_prefix(root, "0.1.0");
  const fs::path prefix = root / "prefix";
  Updater u(targets);
  EXPECT_EQ(u.state()["running"]["layout"], "installed");
  EXPECT_EQ(u.state()["running"]["prefix"], prefix.string());
  const std::string pkg = fake_package::make(root, "2.0.0");
  ASSERT_FALSE(pkg.empty());
  std::string err;
  ASSERT_TRUE(u.stage("pnt-edge-v2.0.0-linux-x86_64.tar.gz", pkg, "", "token", &err)) << err;
  ASSERT_TRUE(u.staged()->applicable()) << u.staged()->problem;
  ASSERT_TRUE(u.apply("token", &err)) << err;
  // bin, lib and share came along; the www directory was swapped (stale file gone)
  EXPECT_EQ(run(targets.exe_path), "pnt-edge 2.0.0 (Cobra 0.2.8)\n");
  EXPECT_EQ(run((prefix / "bin" / "pnt-edge-serial-sim").string()), "sim 2.0.0\n");
  EXPECT_EQ(fake_package::slurp(prefix / "lib" / "x86_64-linux-gnu" / "libfake.so.1"), "lib 2.0.0");
  EXPECT_EQ(fake_package::slurp(prefix / "share" / "pntos-cobra" / "configs" / "pos_ins.json"), "{\"v\": \"2.0.0\"}");
  EXPECT_EQ(fake_package::slurp(prefix / "share" / "pnt-edge" / "www" / "tactical" / "index.html"), "<html>2.0.0</html>");
  EXPECT_FALSE(fs::exists(prefix / "share" / "pnt-edge" / "www" / "tactical" / "stale.js"));
  // the operator's configuration and the systemd unit are never touched
  EXPECT_EQ(fake_package::slurp(prefix / "etc" / "pnt-edge" / "pnt-edge.json"), "{\"operator config\": true}");
  EXPECT_EQ(fake_package::slurp(prefix / "lib" / "systemd" / "system" / "pnt-edge.service"), "[Unit] operator");
  auto st = u.state();
  EXPECT_EQ(st["backup"]["version"], "0.1.0");
  // pnt-edge-serial-sim and the cobra config did not exist before: recorded as added
  std::vector<std::string> added = st["backup"]["added"].get<std::vector<std::string>>();
  EXPECT_NE(std::find(added.begin(), added.end(), "bin/pnt-edge-serial-sim"), added.end());
  EXPECT_NE(std::find(added.begin(), added.end(), "share/pntos-cobra/configs/pos_ins.json"), added.end());
  // roll back: previous files back, added files gone, stale www file back
  ASSERT_TRUE(u.rollback("token", &err)) << err;
  EXPECT_EQ(run(targets.exe_path), "pnt-edge 0.1.0 (Cobra 0.2.8)\n");
  EXPECT_EQ(fake_package::slurp(prefix / "lib" / "x86_64-linux-gnu" / "libfake.so.1"), "lib 0.1.0");
  EXPECT_FALSE(fs::exists(prefix / "bin" / "pnt-edge-serial-sim"));
  EXPECT_FALSE(fs::exists(prefix / "share" / "pntos-cobra"));
  EXPECT_TRUE(fs::exists(prefix / "share" / "pnt-edge" / "www" / "tactical" / "stale.js"));
}

TEST(Updater, FailedInstallIsRestoredAtOnce) {
  if (::geteuid() == 0) GTEST_SKIP() << "root ignores directory permissions";
  const fs::path root = scratch() / "broken";
  fs::remove_all(root);
  const auto targets = fake_package::install_prefix(root, "0.1.0");
  Updater u(targets);
  const std::string pkg = fake_package::make(root, "3.0.0");
  std::string err;
  ASSERT_TRUE(u.stage("p.tar.gz", pkg, "", "user:admin", &err));
  ASSERT_TRUE(u.staged()->applicable()) << u.staged()->problem;
  // the www directory cannot be swapped (its parent is read-only): the executable and libraries already
  // installed must come back by themselves
  const fs::path www_parent = fs::path(targets.www_dir).parent_path();
  ::chmod(www_parent.c_str(), 0555);
  const bool applied = u.apply("user:admin", &err);
  ::chmod(www_parent.c_str(), 0755);
  EXPECT_FALSE(applied);
  EXPECT_NE(err.find("restored"), std::string::npos) << err;
  EXPECT_EQ(run(targets.exe_path), "pnt-edge 0.1.0 (Cobra 0.2.8)\n");
  EXPECT_EQ(fake_package::slurp(root / "prefix" / "lib" / "x86_64-linux-gnu" / "libfake.so.1"), "lib 0.1.0");
  EXPECT_FALSE(fs::exists(root / "prefix" / "bin" / "pnt-edge-serial-sim"));
  EXPECT_FALSE(u.restart_pending());
  EXPECT_EQ(u.state()["history"].back()["action"], "apply");
  EXPECT_NE(u.state()["history"].back()["result"].get<std::string>().find("restored"), std::string::npos);
}
