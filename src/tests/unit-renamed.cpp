// Do rules for processes that rename themselves (setproctitle with all their
// start parameters packed into argv[0], prctl(PR_SET_NAME), named threads)
// resolve to the same rule as the plain exe / binary name?
//
// Needs the helper src/tests/proctitle-helper.c, built and passed in
// ANANICY_PROCTITLE_HELPER:
//   gcc -pthread -o /tmp/proctitle src/tests/proctitle-helper.c
//   ANANICY_PROCTITLE_HELPER=/tmp/proctitle ./test-renamed
// The "Daemon pipeline" case needs root (netlink proc connector).

#include "doctest_compatibility.h"

#include "config.hpp"
#include "core/process.hpp"
#include "core/rules.hpp"
#include "core/topology.hpp"
#include "core/worker.hpp"
#include "utility/process_info.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

const fs::path ROOT = fs::temp_directory_path() / "ananicy-renamed";

fs::path helper() {
  const char *p = std::getenv("ANANICY_PROCTITLE_HELPER");
  REQUIRE_MESSAGE(p != nullptr, "set ANANICY_PROCTITLE_HELPER");
  return p;
}

// Lay out the binaries the scenarios launch.
struct Layout {
  fs::path exe, long_exe, same_link, other_link, spaced_exe;
  Layout() {
    fs::remove_all(ROOT);
    exe        = ROOT / "opt/fakebrave/fakebrave";
    long_exe   = ROOT / "opt/beta/fakebrave-browser-beta";       // > 15 chars
    same_link  = ROOT / "bin/fakebrave";                         // -> exe
    other_link = ROOT / "bin/brave-browser";                     // -> exe
    spaced_exe = ROOT / "opt/My Apps/fakebrave";                 // space in path
    for (const auto &p : {exe, long_exe, spaced_exe}) {
      fs::create_directories(p.parent_path());
      fs::copy_file(helper(), p, fs::copy_options::overwrite_existing);
    }
    fs::create_directories(same_link.parent_path());
    fs::create_symlink(exe, same_link);
    fs::create_symlink(exe, other_link);
  }
  ~Layout() { fs::remove_all(ROOT); }
};

// Lots of Chromium/Electron-like start parameters (> 8 KiB), several of them
// containing '/' (paths), plus `extra` somewhere in the middle / at the end.
std::vector<std::string> many_params(const std::vector<std::string> &front,
                                     const std::vector<std::string> &back = {}) {
  std::vector<std::string> v = front;
  v.push_back("--user-data-dir=/home/user/.config/FakeBrave");
  v.push_back("--crashpad-handler-pid=35099");
  v.push_back("--enable-crash-reporter=,stable");
  v.push_back("--change-stack-guard-on-fork=enable");
  for (int i = 0; i < 300; ++i)
    v.push_back(fmt::format("--enable-features=SomeFeature{:04},Other{:04}", i, i));
  v.push_back("--shared-files=v8_context_snapshot_data:100");
  v.push_back("--field-trial-handle=3,i,4401185837417226718,9471346453282,262144");
  v.push_back("--log-file=/var/log/fake/chrome_debug.log");
  for (const auto &b : back) v.push_back(b);
  return v;
}

struct Child {
  pid_t pid{-1};
  pid_t forked{-1}; // zygote mode: the forked child
  ~Child() {
    for (pid_t p : {forked, pid})
      if (p > 0) kill(p, SIGKILL);
    if (pid > 0) waitpid(pid, nullptr, 0);
  }
};

// exec `path` with argv[0] = argv0, argv[1..] = params, extra env vars;
// returns once the helper said it is ready (title rewritten, threads up).
void spawn(Child &c, const fs::path &path, const std::string &argv0,
           const std::vector<std::string> &params,
           const std::map<std::string, std::string> &env) {
  int fds[2];
  REQUIRE(pipe(fds) == 0);
  c.pid = fork();
  REQUIRE(c.pid >= 0);
  if (c.pid == 0) {
    close(fds[0]);
    dup2(fds[1], 3);
    std::vector<std::string> envs;
    for (const auto &[k, v] : env) envs.push_back(k + "=" + v);
    std::vector<char *> a{const_cast<char *>(argv0.c_str())}, e;
    for (const auto &p : params) a.push_back(const_cast<char *>(p.c_str()));
    a.push_back(nullptr);
    for (auto &s : envs) e.push_back(s.data());
    e.push_back(nullptr);
    execve(path.c_str(), a.data(), e.data());
    _exit(127);
  }
  close(fds[1]);
  std::string out;
  char        buf[64];
  ssize_t     n;
  while (out.find("ready") == std::string::npos &&
         (n = read(fds[0], buf, sizeof buf)) > 0)
    out.append(buf, static_cast<size_t>(n));
  close(fds[0]);
  REQUIRE(out.find("ready") != std::string::npos);
  if (env.contains("PT_FORK_DELAY_MS")) c.forked = std::stoi(out);
}

std::string comm_of(pid_t pid) {
  std::string s;
  std::getline(std::ifstream(fmt::format("/proc/{}/comm", pid)), s);
  return s;
}

// Exactly what Worker::work() does to pick the rule for a queued process.
struct Resolved {
  std::string    name, comm;
  size_t         cmdline_len{};
  nlohmann::json rule;
};
Resolved resolve(const Rules &rules, pid_t pid) {
  Resolved r;
  r.name = get_command_from_pid(pid);
  r.comm = comm_of(pid);
  const auto cmdline = process_info::get_cmdline_string(static_cast<unsigned>(pid));
  r.cmdline_len = cmdline.size();
  r.rule = rules.has_cmdline_rules(r.name) ? rules.get_rule(r.name, cmdline)
                                           : rules.get_rule(r.name);
  return r;
}

const char *RULES =
    R"({"name": "fakebrave", "nice": 3}
{"name": "fakebrave", "cmdline_contains": "--type=renderer", "nice": 7}
{"name": "fakebrave", "cmdline_contains": ["--type=gpu-process", "--deep-needle-at-the-end"], "nice": 9}
{"name": "fakebrave-browser-beta", "nice": 4}
{"name": "brave-browser", "nice": 5}
{"name": "game.exe", "nice": -2}
)";

struct Fixture {
  Layout  layout;
  fs::path rules_dir = ROOT / "rules";
  Config   conf{TEST_DIR "test-rulesconfig.txt", ""};
  std::unique_ptr<Rules> rules;
  Fixture() {
    spdlog::set_level(spdlog::level::off);
    fs::create_directories(rules_dir);
    std::ofstream(rules_dir / "renamed.rules") << RULES;
    rules = std::make_unique<Rules>(rules_dir, &conf, "");
    REQUIRE(rules->size() == 6);
  }
};

void report(const char *scenario, const Resolved &r) {
  fmt::print(stderr, "  {:<58} name={:<24} comm={:<17} cmdline={:>5}B  -> nice {}\n",
             scenario, fmt::format("\"{}\"", r.name), fmt::format("\"{}\"", r.comm),
             r.cmdline_len, r.rule.contains("nice") ? r.rule["nice"].dump() : "(no rule)");
}

#define SCENARIO_CHECK(label, expr, expected_name, expected_nice)              \
  do {                                                                         \
    const Resolved res_ = (expr);                                                 \
    report(label, res_);                                                          \
    CHECK_MESSAGE(res_.name == expected_name, label, " -> name ", res_.name);        \
    CHECK_MESSAGE(res_.rule.contains("nice"), label, " -> no rule matched");      \
    if (res_.rule.contains("nice"))                                               \
      CHECK_MESSAGE(res_.rule["nice"] == expected_nice, label, " -> ", res_.rule.dump()); \
  } while (0)

} // namespace

TEST_CASE("Renamed processes map to the same rule as the binary name")
{
  Fixture f;
  const std::map<std::string, std::string> keep{{"PT_MODE", "keep"}};
  const std::map<std::string, std::string> join{{"PT_MODE", "join"}};
  const auto exe = f.layout.exe.string();

  SECTION("baseline: not renamed, plain exe rule") {
    Child c; spawn(c, f.layout.exe, exe, many_params({"--no-sandbox"}), keep);
    SCENARIO_CHECK("baseline, 300+ params, not renamed", resolve(*f.rules, c.pid), "fakebrave", 3);
  }
  SECTION("baseline: not renamed, cmdline_contains rule") {
    Child c; spawn(c, f.layout.exe, exe, many_params({"--type=renderer"}), keep);
    SCENARIO_CHECK("baseline, --type=renderer, not renamed", resolve(*f.rules, c.pid), "fakebrave", 7);
  }
  SECTION("setproctitle: all params joined into argv[0]") {
    Child c; spawn(c, f.layout.exe, exe, many_params({"--no-sandbox"}), join);
    SCENARIO_CHECK("renamed (params joined into argv[0])", resolve(*f.rules, c.pid), "fakebrave", 3);
  }
  SECTION("setproctitle + cmdline_contains") {
    Child c; spawn(c, f.layout.exe, exe, many_params({"--type=renderer"}), join);
    SCENARIO_CHECK("renamed, --type=renderer", resolve(*f.rules, c.pid), "fakebrave", 7);
  }
  SECTION("setproctitle + needle after > 8 KiB of params") {
    Child c;
    spawn(c, f.layout.exe, exe,
          many_params({"--type=gpu-process"}, {"--deep-needle-at-the-end"}), join);
    const auto r = resolve(*f.rules, c.pid);
    CHECK(r.cmdline_len > 8192);
    SCENARIO_CHECK("renamed, 2 needles, last one after >8KiB", r, "fakebrave", 9);
  }
  SECTION("setproctitle + comm renamed + renamed threads") {
    auto env = join;
    env["PT_COMM"] = "Renamed Main";
    env["PT_THREADS"] = "5";
    Child c; spawn(c, f.layout.exe, exe, many_params({"--type=renderer"}), env);
    SCENARIO_CHECK("renamed + PR_SET_NAME + 5 named threads", resolve(*f.rules, c.pid), "fakebrave", 7);
  }
  SECTION("only comm renamed (prctl), argv untouched") {
    auto env = keep;
    env["PT_COMM"] = "something-else";
    Child c; spawn(c, f.layout.exe, exe, many_params({"--no-sandbox"}), env);
    SCENARIO_CHECK("only PR_SET_NAME", resolve(*f.rules, c.pid), "fakebrave", 3);
  }
  SECTION("binary name longer than comm's 15 chars") {
    const auto p = f.layout.long_exe.string();
    Child c; spawn(c, f.layout.long_exe, p, many_params({"--no-sandbox"}), join);
    SCENARIO_CHECK("renamed, 22-char binary name", resolve(*f.rules, c.pid), "fakebrave-browser-beta", 4);
  }
  SECTION("started through a symlink with the same name") {
    const auto p = f.layout.same_link.string();
    Child c; spawn(c, f.layout.same_link, p, many_params({"--type=renderer"}), join);
    SCENARIO_CHECK("renamed, via same-name symlink", resolve(*f.rules, c.pid), "fakebrave", 7);
  }
  SECTION("title without a path (\"prog: what it does\")") {
    auto env = std::map<std::string, std::string>{
        {"PT_MODE", "title"}, {"PT_TITLE", "fakebrave: renderer --type=renderer --lang=de"}};
    Child c; spawn(c, f.layout.exe, exe, many_params({"--no-sandbox"}), env);
    SCENARIO_CHECK("title \"fakebrave: renderer ...\"", resolve(*f.rules, c.pid), "fakebrave", 7);
  }
}

// Cases where the renamed process should still get the same rule as when it
// is not renamed. These are expected to show differences.
TEST_CASE("Renamed processes: edge cases")
{
  Fixture f;
  const std::map<std::string, std::string> keep{{"PT_MODE", "keep"}};
  const std::map<std::string, std::string> join{{"PT_MODE", "join"}};

  SECTION("symlink with another name, not renamed") {
    const auto p = f.layout.other_link.string();
    Child c; spawn(c, f.layout.other_link, p, many_params({"--no-sandbox"}), keep);
    SCENARIO_CHECK("via symlink 'brave-browser', not renamed", resolve(*f.rules, c.pid), "brave-browser", 5);
  }
  SECTION("symlink with another name, renamed") {
    const auto p = f.layout.other_link.string();
    Child c; spawn(c, f.layout.other_link, p, many_params({"--no-sandbox"}), join);
    SCENARIO_CHECK("via symlink 'brave-browser', renamed", resolve(*f.rules, c.pid), "brave-browser", 5);
  }
  SECTION("space in the install path, not renamed") {
    const auto p = f.layout.spaced_exe.string();
    Child c; spawn(c, f.layout.spaced_exe, p, many_params({"--type=renderer"}), keep);
    SCENARIO_CHECK("'/opt/My Apps/fakebrave', not renamed", resolve(*f.rules, c.pid), "fakebrave", 7);
  }
  SECTION("space in the install path, renamed") {
    const auto p = f.layout.spaced_exe.string();
    Child c; spawn(c, f.layout.spaced_exe, p, many_params({"--type=renderer"}), join);
    SCENARIO_CHECK("'/opt/My Apps/fakebrave', renamed", resolve(*f.rules, c.pid), "fakebrave", 7);
  }
  SECTION("Wine path, args kept separate") {
    auto env = keep;
    env["PT_COMM"] = "game.exe";
    Child c;
    spawn(c, f.layout.exe, "C:\\Program Files\\Game\\game.exe", {"-dx12", "-skipintro"}, env);
    SCENARIO_CHECK("Wine 'C:\\Program Files\\Game\\game.exe' -dx12", resolve(*f.rules, c.pid), "game.exe", -2);
  }
  SECTION("Wine path, args joined into argv[0]") {
    auto env = join;
    env["PT_COMM"] = "game.exe";
    Child c;
    spawn(c, f.layout.exe, "C:\\Games\\game.exe", {"-dx12", "-skipintro"}, env);
    SCENARIO_CHECK("Wine 'C:\\Games\\game.exe -dx12 ...' joined", resolve(*f.rules, c.pid), "game.exe", -2);
  }
}

TEST_CASE("Worker applies the rule to every (renamed) thread")
{
  Fixture f;
  Child c;
  spawn(c, f.layout.exe, f.layout.exe.string(), many_params({"--type=renderer"}),
        {{"PT_MODE", "join"}, {"PT_COMM", "Renamed Main"}, {"PT_THREADS", "5"}});

  synchronized_queue<Process> queue{1024};
  Worker worker(f.rules.get(), &f.conf, &queue, topology::CpuTopology{});
  worker.start();
  queue.push(Process{.pid = c.pid, .name = get_command_from_pid(c.pid)});
  std::this_thread::sleep_for(800ms);
  worker.stop();

  int threads = 0;
  for (const auto &e : fs::directory_iterator(fmt::format("/proc/{}/task", c.pid))) {
    const pid_t tid = std::stoi(e.path().filename().string());
    const int   nice = getpriority(PRIO_PROCESS, static_cast<id_t>(tid));
    fmt::print(stderr, "  thread {:>7} comm=\"{}\" nice={}\n", tid, comm_of(tid), nice);
    CHECK(nice == 7);
    ++threads;
  }
  CHECK(threads == 6);
}

// The real event listener (netlink proc connector) + worker, like the daemon.
TEST_CASE("Daemon pipeline: events for renamed processes" * doctest::skip(getuid() != 0))
{
  Fixture f;
  ProcessQueue listener;
  REQUIRE(listener.status());
  Worker worker(f.rules.get(), &f.conf, &listener.process_queue, topology::CpuTopology{});
  listener.start();
  worker.start();
  std::this_thread::sleep_for(1500ms); // let the startup full scan drain

  const auto nice_of = [](pid_t p) { return getpriority(PRIO_PROCESS, static_cast<id_t>(p)); };

  SECTION("exec of a process that renames itself right away") {
    Child c;
    spawn(c, f.layout.exe, f.layout.exe.string(), many_params({"--type=renderer"}),
          {{"PT_MODE", "join"}, {"PT_THREADS", "3"}});
    std::this_thread::sleep_for(800ms);
    fmt::print(stderr, "  daemon: renamed at start             -> nice {}\n", nice_of(c.pid));
    CHECK(nice_of(c.pid) == 7);
  }
  SECTION("zygote: forked child rewrites its title later, no prctl") {
    for (const int delay : {0, 20, 100, 300, 700, 1500, 3000}) {
      Child c;
      spawn(c, f.layout.exe, f.layout.exe.string(), many_params({"--type=zygote"}),
            {{"PT_MODE", "join"}, {"PT_FORK_DELAY_MS", std::to_string(delay)},
             {"PT_CHILD_TITLE", f.layout.exe.string() + " --type=renderer --renderer-client-id=7"}});
      std::this_thread::sleep_for(std::chrono::milliseconds(delay + 1200));
      const auto r = resolve(*f.rules, c.forked);
      fmt::print(stderr, "  daemon: zygote child, title after {:>4}ms, no prctl -> nice {} (should be {})\n",
                 delay, nice_of(c.forked), r.rule["nice"].dump());
      CHECK_MESSAGE(nice_of(c.forked) == 7, "title rewritten after ", delay, "ms");
    }
  }
  SECTION("zygote: forked child rewrites its title later, then prctl") {
    Child c;
    spawn(c, f.layout.exe, f.layout.exe.string(), many_params({"--type=zygote"}),
          {{"PT_MODE", "join"}, {"PT_FORK_DELAY_MS", "1500"},
           {"PT_CHILD_TITLE", f.layout.exe.string() + " --type=renderer --renderer-client-id=7"},
           {"PT_CHILD_COMM", "fakebrave"}});
    std::this_thread::sleep_for(2700ms);
    fmt::print(stderr, "  daemon: zygote child, title after 1500ms + prctl -> nice {}\n", nice_of(c.forked));
    CHECK(nice_of(c.forked) == 7);
  }

  worker.stop();

}
