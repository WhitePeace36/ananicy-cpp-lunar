//
// Created by aviallon on 19/04/2021.
//

#ifndef ANANICY_CPP_RULES_HPP
#define ANANICY_CPP_RULES_HPP

#include "config.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef ENABLE_REGEX_SUPPORT
#include <utility>
#include <vector>
#endif

#include <nlohmann/json.hpp>

class Rules {
private:
  using ruleset = std::unordered_map<std::string, nlohmann::json>;

  Config *m_config;
  ruleset m_program_rules{};
  ruleset m_type_rules{};
  ruleset m_cgroup_rules{};

  /// Program rules that also carry "cmdline_contains", keyed by "name".
  /// Every needle must occur in the process command line for the rule to
  /// apply. They are tried in load order before the plain "name" rule.
  struct cmdline_rule {
    std::vector<std::string> needles;
    nlohmann::json           rule;
  };
  std::unordered_map<std::string, std::vector<cmdline_rule>> m_cmdline_rules{};

  nlohmann::json apply_type(nlohmann::json rule) const noexcept;

  /// File currently being loaded (empty for rules given as strings), and
  /// where each name/type/cgroup rule came from, to report redefinitions.
  std::string                                  m_current_source{};
  std::unordered_map<std::string, std::string> m_rule_sources{};
  void note_definition(std::string_view kind, const std::string &key);

#ifdef ENABLE_REGEX_SUPPORT
  std::vector<std::pair<std::string, std::string>> m_regex_rules{};
#endif

public:
  explicit Rules(const std::filesystem::path &ruleset_directory,
                 Config                      *config_src);
  bool load_rule_from_string(const std::string_view &line);
  void load_rules_from_file(const std::filesystem::path &path);
  void load_rules_from_directory(const std::filesystem::path &dir_path);
  nlohmann::json get_rule(const std::string &name) const noexcept;
  /// Like get_rule(name), but first tries the "cmdline_contains" rules for
  /// that name against `cmdline` (the arguments joined by spaces).
  nlohmann::json get_rule(const std::string &name,
                          std::string_view   cmdline) const noexcept;
  /// Whether `name` has any "cmdline_contains" rule, i.e. whether the caller
  /// needs to read the process command line at all.
  bool has_cmdline_rules(const std::string &name) const noexcept;

  void create_cgroups() const noexcept;

  size_t size() const noexcept;

  void show_all_rules() const noexcept;

  enum class rule_type { rules, types, cgroups };
  void show_rules(rule_type type) const noexcept;
};

#endif // ANANICY_CPP_RULES_HPP
