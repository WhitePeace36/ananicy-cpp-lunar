#include "doctest_compatibility.h"

#include "config.hpp"
#include "core/priority.hpp"
#include "core/cgroups.hpp"
#include "core/rules.hpp"

#include "core/process.hpp"
#include "utility/process_info.hpp"
#include "utility/utils.hpp"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

static fs::path get_cgroup_path(const std::string_view &cgroup_name) {
  const auto &cgroup_info = control_groups::get_cgroup_version();
  const auto &base_path = cgroup_info.path;

  fs::path cgroup_path{base_path};
  switch (cgroup_info.version) {
  case control_groups::cgroup_info::none:
    break;
  case control_groups::cgroup_info::v1:
    cgroup_path /= fs::path{"cpu"} / cgroup_name.data();
    break;
  case control_groups::cgroup_info::v2:
    cgroup_path /= cgroup_name;
    break;
  }
  return cgroup_path;
}

/// BEGIN Test Cgroups
TEST_SUITE_BEGIN("cgroups");

const bool amiuser = (getuid() > 0);

TEST_CASE("Check if cgroup is valid")
{
  spdlog::set_level(spdlog::level::off);
  const auto &cgroup_info = control_groups::get_cgroup_version(true);
  const bool is_known_version = (cgroup_info.version == control_groups::cgroup_info::none) || (cgroup_info.version == control_groups::cgroup_info::v1) || (cgroup_info.version == control_groups::cgroup_info::v2);
  REQUIRE(is_known_version);
}

TEST_CASE("the cgroup version is v2" * doctest::may_fail(true) * doctest::no_output(true) * doctest::skip(amiuser))
{
  spdlog::set_level(spdlog::level::off);
  const auto &cgroup_info = control_groups::get_cgroup_version(true);
  CHECK(cgroup_info.version != control_groups::cgroup_info::none);
  CHECK(cgroup_info.version != control_groups::cgroup_info::v1);
  CHECK(cgroup_info.version == control_groups::cgroup_info::v2);
}

TEST_CASE("the cgroup version is v1" * doctest::may_fail(true) * doctest::no_output(true) * doctest::skip(amiuser))
{
  spdlog::set_level(spdlog::level::off);
  const auto &cgroup_info = control_groups::get_cgroup_version(true);
  CHECK(cgroup_info.version != control_groups::cgroup_info::none);
  CHECK(cgroup_info.version != control_groups::cgroup_info::v2);
  CHECK(cgroup_info.version == control_groups::cgroup_info::v1);
}

TEST_CASE("the cgroup version is none" * doctest::may_fail(true) * doctest::no_output(true) * doctest::skip(amiuser))
{
  spdlog::set_level(spdlog::level::off);
  const auto &cgroup_info = control_groups::get_cgroup_version(true);
  CHECK(cgroup_info.version != control_groups::cgroup_info::v2);
  CHECK(cgroup_info.version != control_groups::cgroup_info::v1);
  CHECK(cgroup_info.version == control_groups::cgroup_info::none);
}

TEST_CASE("create cgroup" * doctest::may_fail(true) * doctest::no_output(true) * doctest::skip(amiuser))
{
  spdlog::set_level(spdlog::level::off);

  static constexpr std::string_view cgroup_name = "UNIT_TEST_ANANICY";
  const auto& cgroup_path = get_cgroup_path(cgroup_name);
  std::error_code err{};
  if (fs::exists(cgroup_path)) {
    fs::remove(cgroup_path, err);
  }
  CHECK(control_groups::create_cgroup(cgroup_name));
  fs::remove(cgroup_path, err);
}

TEST_CASE("add pid to cgroup" * doctest::may_fail(true) * doctest::no_output(true) * doctest::skip(amiuser))
{
  spdlog::set_level(spdlog::level::off);

  static constexpr std::string_view cgroup_name = "UNIT_TEST_ANANICY";
  const auto& cgroup_path = get_cgroup_path(cgroup_name);
  std::error_code err{};
  if (fs::exists(cgroup_path)) {
    fs::remove(cgroup_path, err);
  }
  CHECK(control_groups::create_cgroup(cgroup_name));

  // Get cgroup of current proc.
  const pid_t current_proc = getpid();
  const auto saved_cgroup = control_groups::get_cgroup_for_pid(current_proc);

  // Check that current proc is located in some cgroup.
  CHECK(!saved_cgroup.empty());

  // Move current proc to root cgroup.
  CHECK(control_groups::add_pid_to_cgroup(current_proc, "") == 0);
  CHECK(control_groups::get_cgroup_for_pid(current_proc) == "/");

  // Move current proc to created cgroup.
  CHECK(control_groups::add_pid_to_cgroup(current_proc, cgroup_name.data()) == 0);
  CHECK(control_groups::get_cgroup_for_pid(current_proc) == cgroup_name);

  fs::remove(cgroup_path, err);
}

TEST_CASE("add pid to cgroup" * doctest::may_fail(true) * doctest::no_output(true) * doctest::skip(amiuser))
{
  spdlog::set_level(spdlog::level::off);

  static constexpr std::string_view cgroup_name = "UNIT_TEST_ANANICY";
  const auto& cgroup_path = get_cgroup_path(cgroup_name);
  std::error_code err{};
  if (fs::exists(cgroup_path)) {
    fs::remove(cgroup_path, err);
  }
  CHECK(control_groups::create_cgroup(cgroup_name));

  // Set cpu quota of created cgroup.
  CHECK(control_groups::set_cgroup_cpu_quota(cgroup_name.data(), 90) == 0);

  fs::remove(cgroup_path, err);
}

TEST_SUITE_END();
/// END Test Cgroups

TEST_CASE("Config")
{
  static constexpr std::string_view config_path = (TEST_DIR "test-sampleconfig.txt");

  SECTION("Check if config is correctly set according to the file")
  {
    Config conf(config_path);

    const bool is_latnice_supported = priority::test_latnice_support() == "true";
    const bool apply_latnice = is_latnice_supported ? conf.apply_latnice() : false;

    // Check "apply" options.
    CHECK(!conf.apply_nice());
    CHECK(!apply_latnice);
    CHECK(!conf.apply_sched());
    CHECK(!conf.apply_ionice());
    CHECK(!conf.apply_oom_score_adj());
    CHECK(!conf.apply_cgroups());
    CHECK(!conf.apply_cpuset());

    // Check "load" options.
    CHECK(!conf.cgroup_load());
    CHECK(!conf.type_load());
    CHECK(!conf.rule_load());

    // Check logging.
    CHECK(conf.log_applied_rule());
    CHECK(conf.loglevel() == spdlog::level::err);

    // Check frequency.
    const auto conf_check_freq = conf.check_freq().count();
    CHECK(conf_check_freq == 5);

    // Check x3d.
    CHECK(conf.x3d_mode() == "frequency");

    // Check workaround.
    CHECK(!conf.cgroup_realtime_workaround());
  }
}

TEST_CASE("Rules")
{
  static constexpr std::string_view config_path = (TEST_DIR "test-rulesconfig.txt");
  spdlog::set_level(spdlog::level::off);

  Config conf(config_path);
  SECTION("Load rule from string")
  {
    Rules rules("", &conf);
    REQUIRE(rules.size() == 0);

    // Check if type was loaded successfully.
    CHECK(rules.load_rule_from_string("{ \"type\": \"Doc-View\", \"nice\": -4, \"latency_nice\": 5 }"));
    CHECK(rules.load_rule_from_string("{ \"type\": \"Player-Audio\", \"nice\": 6, \"ioclass\": \"realtime\", \"latency_nice\": 8 }"));

    // Check if rule was loaded successfully.
    CHECK(rules.load_rule_from_string("{ \"name\": \"icecat\", \"type\":\"Doc-View\" }"));
    CHECK(!rules.get_rule("icecat").is_null());
    CHECK(rules.size() == 1);

    // Check if more rules were loaded successfully.
    CHECK(rules.load_rule_from_string("{ \"name\": \"mpd\", \"type\": \"Player-Audio\" }"));
    CHECK(!rules.get_rule("mpd").is_null());
    CHECK(rules.size() == 2);
    // comment(#) after the rule
    CHECK(rules.load_rule_from_string("{ \"name\": \"someprogram\", \"type\":\"Dc-Vw\" } # hey"));
    CHECK(!rules.get_rule("someprogram").is_null());
    CHECK(rules.size() == 3);
    // should be trimmed
    CHECK(rules.load_rule_from_string("      \t\t   { \"name\": \"someprogram2\", \"type\":\"Dc-Vw\" }         \t\t  "));
    CHECK(!rules.get_rule("someprogram2").is_null());
    CHECK(rules.size() == 4);

    // These rules must fail.

    // no field called "name"
    CHECK(!rules.load_rule_from_string("{ \"nm\": \"icct\", \"tp\":\"Dc-Vw\" }"));
    // comment(#) before the rule
    CHECK(!rules.load_rule_from_string("# { \"nm\": \"icct\", \"tp\":\"Dc-Vw\" }"));
    // missing closing bracket
    CHECK(!rules.load_rule_from_string("{ \"name\": \"icct\", \"type\":\"Dc-Vw\" "));
    // missing open bracket
    CHECK(!rules.load_rule_from_string("\"name\": \"icct\", \"type\":\"Dc-Vw\" }"));
    // missing both brackets
    CHECK(!rules.load_rule_from_string("\"name\": \"icct\", \"type\":\"Dc-Vw\" "));
    CHECK(rules.size() == 4);
  }

  SECTION("Whitespace-only lines are skipped")
  {
    Rules rules("", &conf);
    CHECK(!rules.load_rule_from_string("      "));
    CHECK(!rules.load_rule_from_string("\t\t\t"));
    CHECK(!rules.load_rule_from_string("  \t  \t  "));
    CHECK(!rules.load_rule_from_string(""));
    CHECK(rules.size() == 0);
  }

  SECTION("Trailing carriage return is handled")
  {
    Rules rules("", &conf);
    CHECK(!rules.load_rule_from_string("\r"));
    CHECK(!rules.load_rule_from_string("   \r"));
    CHECK(!rules.load_rule_from_string("# this is a comment\r"));
    // \r after } simulates std::getline on a CRLF file
    CHECK(rules.load_rule_from_string("{ \"name\": \"crlftest\", \"type\": \"Game\" }\r"));
    CHECK(!rules.get_rule("crlftest").is_null());
    CHECK(rules.size() == 1);
  }

  SECTION("Nested JSON objects parse correctly")
  {
    Rules rules("", &conf);
    CHECK(rules.load_rule_from_string("{ \"name\": \"nested\", \"type\": \"Game\", \"extra\": { \"key\": \"val\" } }"));
    CHECK(!rules.get_rule("nested").is_null());
    CHECK(rules.size() == 1);
  }

  SECTION("Load rules from CRLF file")
  {
    Rules rules("", &conf);
    // Write a temp rules file with Windows line endings
    const fs::path tmp_path = fs::temp_directory_path() / "test-crlf.rules";
    {
      std::ofstream f(tmp_path, std::ios::binary);
      f << "# comment line\r\n"
        << "\r\n"
        << "{ \"type\": \"TestType\", \"nice\": -1 }\r\n"
        << "   \r\n"
        << "{ \"name\": \"crlfprog\", \"type\": \"TestType\" }\r\n";
    }
    rules.load_rules_from_file(tmp_path);
    CHECK(!rules.get_rule("crlfprog").is_null());
    CHECK(rules.size() == 1);
    fs::remove(tmp_path);
  }

  SECTION("cmdline_contains rules")
  {
    Rules rules("", &conf);
    CHECK(rules.load_rule_from_string("{ \"type\": \"Game\", \"nice\": -5, \"ioclass\": \"best-effort\" }"));
    CHECK(rules.load_rule_from_string("{ \"name\": \"java\", \"nice\": 3 }"));
    CHECK(rules.load_rule_from_string("{ \"name\": \"java\", \"cmdline_contains\": \"minecraft\", \"type\": \"Game\" }"));
    CHECK(rules.load_rule_from_string("{ \"name\": \"java\", \"cmdline_contains\": [\"-jar\", \"jenkins.war\"], \"nice\": 10 }"));
    CHECK(rules.size() == 3);

    // A cmdline rule must not overwrite the plain name rule.
    CHECK(rules.get_rule("java")["nice"] == 3);
    CHECK(rules.has_cmdline_rules("java"));
    CHECK(!rules.has_cmdline_rules("python3"));

    // Single needle, with type merging.
    const auto mc = rules.get_rule("java", "java -Xmx4G -cp minecraft-1.21.jar net.minecraft.Main");
    CHECK(mc["nice"] == -5);
    CHECK(mc["ioclass"] == "best-effort");

    // All needles must match.
    CHECK(rules.get_rule("java", "java -jar /opt/jenkins.war")["nice"] == 10);
    CHECK(rules.get_rule("java", "java -cp jenkins.war Main")["nice"] == 3);

    // No match falls back to the plain name rule.
    CHECK(rules.get_rule("java", "java -jar other.jar")["nice"] == 3);

    // Rules only apply for their own name.
    CHECK(rules.get_rule("python3", "python3 minecraft.py").is_null());

    // First matching rule in load order wins.
    CHECK(rules.get_rule("java", "java -jar jenkins.war minecraft")["nice"] == -5);
  }

  SECTION("cmdline_contains without a plain name rule")
  {
    Rules rules("", &conf);
    CHECK(rules.load_rule_from_string("{ \"name\": \"python3\", \"cmdline_contains\": \"jupyter-lab\", \"nice\": 5 }"));
    CHECK(rules.get_rule("python3", "/usr/bin/python3 /usr/bin/jupyter-lab")["nice"] == 5);
    CHECK(rules.get_rule("python3", "python3 script.py").is_null());
    CHECK(rules.get_rule("python3").is_null());
  }

  SECTION("cmdline_contains alone does nothing")
  {
    Rules rules("", &conf);
    CHECK(!rules.load_rule_from_string("{ \"cmdline_contains\": \"minecraft\", \"nice\": -5 }"));
    // Must not be taken as a (re)definition of type Game.
    CHECK(!rules.load_rule_from_string("{ \"type\": \"Game\", \"cmdline_contains\": \"minecraft\", \"nice\": -5 }"));
    CHECK(!rules.load_rule_from_string("{ \"cgroup\": \"cpu80\", \"cmdline_contains\": \"minecraft\" }"));
    CHECK(rules.size() == 0);
    CHECK(rules.get_rule("java", "java minecraft").is_null());
  }

  SECTION("cmdline_contains invalid values")
  {
    Rules rules("", &conf);
    CHECK(!rules.load_rule_from_string("{ \"name\": \"java\", \"cmdline_contains\": \"\" }"));
    CHECK(!rules.load_rule_from_string("{ \"name\": \"java\", \"cmdline_contains\": [] }"));
    CHECK(!rules.load_rule_from_string("{ \"name\": \"java\", \"cmdline_contains\": [\"a\", 1] }"));
    CHECK(!rules.load_rule_from_string("{ \"name\": \"java\", \"cmdline_contains\": [\"a\", \"\"] }"));
    CHECK(!rules.load_rule_from_string("{ \"name\": \"java\", \"cmdline_contains\": 42 }"));
    CHECK(rules.size() == 0);
    CHECK(!rules.has_cmdline_rules("java"));
  }
}


TEST_CASE("Process command line")
{
  // A real child whose arguments contain a newline: the command line must be
  // read in full, not cut at the first '\n'.
  const pid_t child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    execl("/bin/sh", "sh", "-c", "sleep 5; :", "first\nline", "cctest-tail",
          static_cast<char *>(nullptr));
    _exit(127);
  }

  const std::string expected = "sh -c sleep 5; : first\nline cctest-tail";
  std::string       got;
  for (int i = 0; i < 200 && got != expected; ++i) { // until it has exec'd
    got = process_info::get_cmdline_string(static_cast<unsigned>(child));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(got == expected);
  CHECK(get_command_from_pid(child) == "sh");

  kill(child, SIGKILL);
  waitpid(child, nullptr, 0);

  // Reaped: nothing to read, and no crash.
  CHECK(process_info::get_cmdline_string(static_cast<unsigned>(child)).empty());
}
