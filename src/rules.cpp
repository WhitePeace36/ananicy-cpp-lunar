//
// Created by aviallon on 19/04/2021.
//

#include "core/rules.hpp"
#include "core/cgroups.hpp"

#ifdef ENABLE_REGEX_SUPPORT
#include "utility/regex_utils.hpp"
#endif

#include <algorithm>   // for all_of, any_of, sort
#include <filesystem>  // for path, recursive_directory_iterator
#include <fstream>     // for ifstream
#include <iostream>    // for cout
#include <string>      // for string, getline
#include <string_view> // for string_view

#include <fmt/format.h>

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

using nlohmann::json;

Rules::Rules(const fs::path &ruleset_directory, Config *config_src,
             const fs::path &vendor_directory)
    : m_config(config_src) {
  // Packaged rules first, then the configuration directory: whatever is
  // defined there replaces the packaged definition of the same name.
  std::error_code ec;
  const bool      has_vendor = !vendor_directory.empty() &&
                          fs::is_directory(vendor_directory, ec) &&
                          !fs::equivalent(vendor_directory, ruleset_directory, ec);
  if (has_vendor) {
    spdlog::info("Loading packaged rules from {}",
                 std::string_view{vendor_directory.c_str()});
    m_vendor_prefix = (vendor_directory / "").string();
    load_rules_from_directory(vendor_directory);
  }
  if (!exists(ruleset_directory)) {
    if (!has_vendor) {
      spdlog::error(fmt::format("{}: directory {} does not exist", __func__,
                                std::string_view{ruleset_directory.c_str()}));
    }
    return;
  }
  load_rules_from_directory(ruleset_directory);
}

bool Rules::load_rule_from_string(const std::string_view &line) {
  // NOTE: this technique makes it constexpr even in non constexpr context
  // Because since C++17 standard lambda functions are constexpr by default
  // @see https://en.cppreference.com/w/cpp/language/lambda
  const auto &is_invalid_str = [](auto &&s) {
    const auto &is_comment_str = [](auto &&s) {
      std::string_view hash_str{s};
      hash_str.remove_prefix(
          std::min(hash_str.find_first_not_of(" \t\r"), hash_str.size()));
      return hash_str.empty() || (hash_str[0] == '#');
    };
    return s.empty() || is_comment_str(s);
  };
  if (is_invalid_str(line)) {
    return false;
  }

  auto &&first_pair = line.find_first_of('{');
  auto &&second_pair = line.find_last_of('}');

  // Return if not found brackets in the given string.
  if (first_pair == std::string_view::npos ||
      second_pair == std::string_view::npos) {
    spdlog::error("Rule is invalid: pairs mismatch '}{'");
    return false;
  }

  // Remove trailing characters.
  auto &&clean_line =
      std::string_view{line.substr(first_pair, second_pair - first_pair + 1)};

  try {
    auto &&rule = json::parse(clean_line);

    const bool has_name = rule.contains("name") && rule["name"].is_string();

    if (rule.contains("cmdline_contains")) {
      /* "cmdline_contains" only narrows a "name" rule; on its own (or on a
       * type or cgroup line) it would otherwise silently redefine a type. */
      if (!has_name) {
        spdlog::warn("Ignoring rule: \"cmdline_contains\" requires \"name\": {}",
                     rule.dump());
        return false;
      }

      const auto &field = rule["cmdline_contains"];
      std::vector<std::string> needles;
      if (field.is_string()) {
        needles.push_back(field.get<std::string>());
      } else if (field.is_array()) {
        for (const auto &item : field) {
          if (!item.is_string()) {
            needles.clear();
            break;
          }
          needles.push_back(item.get<std::string>());
        }
      }
      if (needles.empty() ||
          std::any_of(needles.begin(), needles.end(),
                      [](const auto &n) { return n.empty(); })) {
        spdlog::error("Ignoring rule: \"cmdline_contains\" must be a non-empty "
                      "string or a non-empty list of non-empty strings: {}",
                      rule.dump());
        return false;
      }

      // A rule for the same name with the same texts (in any order) replaces
      // the earlier one, like a redefined plain rule does: the earlier one is
      // dropped as if it had never been loaded, and the new one takes its own
      // place in the load order. Without that, a rule in /etc/ananicy.d could
      // never override a packaged cmdline rule (the first match wins).
      std::sort(needles.begin(), needles.end());
      needles.erase(std::unique(needles.begin(), needles.end()), needles.end());
      const std::string name = rule["name"].get<std::string>();
      const std::string key =
          fmt::format("{} [cmdline: {}]", name, json(needles).dump());
      auto &candidates = m_cmdline_rules[name];
      const auto same = std::find_if(
          candidates.begin(), candidates.end(),
          [&](const auto &candidate) { return candidate.needles == needles; });
      if (same != candidates.end()) {
        note_definition("name", key);
        candidates.erase(same);
      } else {
        note_definition("", "name:" + key);
      }
      candidates.push_back({std::move(needles), rule});
      return true;
    }

    if (has_name) { /* Program rule */
      const auto &rule_name = rule["name"];
      if (m_program_rules.contains(rule_name)) {
        note_definition("name", rule_name);
      } else {
        note_definition("", "name:" + rule_name.get<std::string>());
      }
      m_program_rules.insert_or_assign(rule_name, rule);

#ifdef ENABLE_REGEX_SUPPORT
      // A redefinition replaces the old regex too, instead of adding a second
      // entry that still points at the same (now replaced) rule.
      std::erase_if(m_regex_rules, [&](const auto &entry) {
        return entry.first == rule_name.get<std::string>();
      });
      if (rule.contains("name_regex") && rule["name_regex"].is_string()) {
        m_regex_rules.emplace_back(rule_name,
                                   rule["name_regex"].get<std::string>());
      }
#endif
    } else if (rule.contains("type") &&
               rule["type"].is_string()) { /* Type rule */
      const auto &rule_type = rule["type"];
      if (m_type_rules.contains(rule_type)) {
        note_definition("type", rule_type);
      } else {
        note_definition("", "type:" + rule_type.get<std::string>());
      }
      m_type_rules.insert_or_assign(rule_type, rule);
    } else if (rule.contains("cgroup") &&
               rule["cgroup"].is_string()) { /* Cgroup rule */
      const auto &rule_cgroup = rule["cgroup"];
      if (m_cgroup_rules.contains(rule_cgroup)) {
        note_definition("cgroup", rule_cgroup);
      } else {
        note_definition("", "cgroup:" + rule_cgroup.get<std::string>());
      }
      m_cgroup_rules.insert_or_assign(rule_cgroup, rule);
    } else {
      spdlog::error(
          fmt::format("Rule does not have a name, or name is not a string: {}",
                      rule.dump()));
      return false;
    }
  } catch (const std::exception &e) {
    spdlog::warn(
        fmt::format("Error parsing JSON: {}, line: {}", e.what(), line));
    return false;
  }

  return true;
}

void Rules::note_definition(std::string_view kind, const std::string &key) {
  const std::string here =
      m_current_source.empty() ? std::string{"<string>"} : m_current_source;
  if (kind.empty()) { // first definition: remember where it came from
    m_rule_sources[key] = here;
    return;
  }
  const std::string full_key = fmt::format("{}:{}", kind, key);
  const auto        previous = m_rule_sources.find(full_key);
  const std::string before   = previous != m_rule_sources.end()
                                   ? previous->second
                                   : std::string{"<unknown>"};
  const auto in_vendor = [this](const std::string &source) {
    return !m_vendor_prefix.empty() && source.starts_with(m_vendor_prefix);
  };
  if (in_vendor(before) && !in_vendor(here)) {
    // the configuration directory changing a packaged definition: intended
    spdlog::info("{} rule '{}' from {} overrides the packaged one from {}",
                 kind, key, here, before);
  } else {
    spdlog::warn("{} rule '{}' from {} replaces the one from {} (the last "
                 "one loaded wins)",
                 kind, key, here, before);
  }
  m_rule_sources[full_key] = here;
}

void Rules::load_rules_from_file(const fs::path &path) {
  m_current_source = path.string();
  std::string   line{};
  std::ifstream rule_file{path};
  while (rule_file.good()) {
    std::getline(rule_file, line);
    // Rule files may be edited on Windows; tolerate CRLF line endings.
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    load_rule_from_string(line);
  }
  m_current_source.clear();
}

void Rules::show_all_rules() const noexcept {
  for (const auto &rule : m_cgroup_rules) {
    spdlog::debug(
        fmt::format("Cgroup: {}, {}", rule.first, rule.second.dump()));
  }
  for (const auto &rule : m_type_rules) {
    spdlog::debug(fmt::format("Type: {}, {}", rule.first, rule.second.dump()));
  }
  for (const auto &rule : m_program_rules) {
    spdlog::debug(fmt::format("Rule: {}, {}", rule.first, rule.second.dump()));
  }
  for (const auto &[name, candidates] : m_cmdline_rules) {
    for (const auto &candidate : candidates) {
      spdlog::debug(fmt::format("Cmdline rule: {}, {}", name,
                                candidate.rule.dump()));
    }
  }
}

void Rules::show_rules(rule_type type) const noexcept {
  const auto &rules = (type == rule_type::types)     ? m_type_rules
                      : (type == rule_type::cgroups) ? m_cgroup_rules
                                                     : m_program_rules;

  json rules_display;

  if (type == rule_type::rules) {
    for (const auto &rule : rules) {
      rules_display[rule.first] = get_rule(rule.first);
    }
    for (const auto &[name, candidates] : m_cmdline_rules) {
      for (const auto &candidate : candidates) {
        rules_display[fmt::format("{} [cmdline: {}]", name,
                                  json(candidate.needles).dump())] =
            apply_type(candidate.rule);
      }
    }
  } else {
    for (const auto &rule : rules) {
      rules_display[rule.first] = rule.second;
    }
  }

  std::cout << rules_display.dump(4) << std::endl;
}

void Rules::load_rules_from_directory(const fs::path &dir_path) {
  const auto &is_valid_file = [](auto &&config_ref, auto &&file_ext) {
    return ((config_ref->cgroup_load() && file_ext.compare(".cgroups") == 0) ||
            (config_ref->type_load() && file_ext.compare(".types") == 0) ||
            (config_ref->rule_load() && file_ext.compare(".rules") == 0));
  };

  // Load in sorted path order (00-default/... before 99-local/...), not in
  // the arbitrary order the filesystem returns entries: when a rule is
  // defined twice the last one loaded wins, so the order must be predictable.
  std::vector<fs::path> files;
  for (const auto &dir_entry : fs::recursive_directory_iterator{dir_path}) {
    if (fs::is_directory(dir_entry)) {
      spdlog::debug(fmt::format("Scanning directory {}",
                                std::string_view{dir_entry.path().c_str()}));
      continue;
    }

    if (is_valid_file(m_config, dir_entry.path().extension())) {
      files.push_back(dir_entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  for (const auto &file : files) {
    load_rules_from_file(file);
  }
}

void Rules::create_cgroups() const noexcept {
  spdlog::info("Creating Cgroups...");
  for (const auto &cgroup_rule : m_cgroup_rules) {
    control_groups::create_cgroup(cgroup_rule.first);
    if (cgroup_rule.second.contains("CPUQuota")) {
      control_groups::set_cgroup_cpu_quota(
          cgroup_rule.first, cgroup_rule.second["CPUQuota"].get<unsigned>());
    }
  }
  spdlog::info("Finished creating Cgroups...");
}

json Rules::apply_type(json rule) const noexcept {
  if (rule.contains("type") && m_type_rules.contains(rule["type"])) {
    json type_rule =
        m_type_rules.at(rule["type"]); // We create a copy of the type rule to
                                       // avoid overwriting it
    type_rule.merge_patch(
        rule); // We then merge the rule into it, overriding parameters from
               // the type with those explicitly specified in the rule
    rule.merge_patch(type_rule); // We then merge it back into the rule
  }
  return rule;
}

json Rules::get_rule(const std::string &name) const noexcept {
  json rule;

  if (m_program_rules.contains(name)) {
    rule = m_program_rules.at(name);
#ifdef ENABLE_REGEX_SUPPORT
  } else {
    // get rule if the name is matching some regex in the ruleset
    for (const auto &regex_rule : m_regex_rules) {
      if (utils::regex::is_str_matching_regex(name, regex_rule.second)) {
        rule = m_program_rules.at(regex_rule.first);
        break;
      }
    }
#endif
  }

  return apply_type(std::move(rule));
}

json Rules::get_rule(const std::string &name,
                     std::string_view   cmdline) const noexcept {
  if (const auto it = m_cmdline_rules.find(name);
      it != m_cmdline_rules.end()) {
    for (const auto &candidate : it->second) {
      const bool matches =
          std::all_of(candidate.needles.begin(), candidate.needles.end(),
                      [&](const std::string &needle) {
                        return cmdline.find(needle) != std::string_view::npos;
                      });
      if (matches) {
        return apply_type(candidate.rule);
      }
    }
  }
  return get_rule(name);
}

bool Rules::has_cmdline_rules(const std::string &name) const noexcept {
  return m_cmdline_rules.contains(name);
}

[[gnu::pure]] size_t Rules::size() const noexcept {
  size_t count = m_program_rules.size();
  for (const auto &entry : m_cmdline_rules) {
    count += entry.second.size();
  }
  return count;
}
