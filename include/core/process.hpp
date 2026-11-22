//
// Created by aviallon on 13/04/2021.
//

#ifndef ANANICY_CPP_PROCESS_HPP
#define ANANICY_CPP_PROCESS_HPP

#if defined(USE_BPF_PROC_IMPL)
#include "bpf_program.hpp"
#else
#include "netlink_program.hpp"
#endif
#include "utility/synchronized_queue.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <thread>

pid_t get_pid();

std::string get_command_from_pid(pid_t pid);

// Programs that rewrite their process title (setproctitle: Chromium, avahi,
// sshd, postgres, ...) put a whole sentence into argv[0]; its first word is
// the program: "/opt/brave-bin/brave --type=zygote ..." -> "brave",
// "avahi-daemon: running [host]" -> "avahi-daemon". A first word that is a
// path only counts if its file name is that of the executable (exe_path, the
// target of /proc/PID/exe), so a real path with spaces ("/games/My Game/game") is left
// alone. nullopt: argv0 is not such a title, use its file name as before.
std::optional<std::string> title_program_name(std::string_view argv0,
                                              std::string_view exe_path);

struct Process {
  pid_t       pid{};
  std::string name{};
};

class ProcessQueue {
private:
  void init() noexcept;
#if defined(USE_BPF_PROC_IMPL)
  bool       m_verbose{false};
  BPFProgram m_bpf_program{};

#else
  NetlinkProgram m_netlink_program{};
#endif
  std::jthread event_thread;
  bool         m_status{true};

public:
#if defined(USE_BPF_PROC_IMPL)
  explicit ProcessQueue(bool verbose)
      : m_verbose(verbose), m_bpf_program(m_verbose) {
    this->init();
  }
#else
  ProcessQueue() { this->init(); }
#endif
  ~ProcessQueue() noexcept { this->stop(); }

  void start();
  void stop() noexcept;

  bool status() noexcept { return m_status; }

  void full_scan();

#ifdef USE_EXPERIMENTAL_IMPL
  synchronized_queue<Process> process_queue{INT16_MAX};
#else
  synchronized_queue<Process> process_queue;
#endif
};

#endif // ANANICY_CPP_PROCESS_HPP
