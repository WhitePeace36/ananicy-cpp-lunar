//
// Created by aviallon on 13/04/2021.
//

#include "core/process.hpp"
#include "process_helpers.hpp"
#include "utility/synchronized_queue.hpp"
#include "utility/utils.hpp"

#if !defined(USE_BPF_PROC_IMPL)
#include <cerrno>
#endif

#include <atomic>     // for atomic
#include <chrono>     // for steady_clock
#include <filesystem> // for path
#include <fstream>    // for ifstream
#include <thread>     // for jthread
#include <unistd.h>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

#if defined(USE_BPF_PROC_IMPL)
static volatile std::atomic<bool>   g_exiting;
static synchronized_queue<Process> *g_process_queue;
/// Set when the perf buffer overflowed and events were dropped.
static std::atomic<std::uint64_t>   g_lost_events{0};

void ProcessQueue::init() noexcept {
  g_process_queue = &this->process_queue;
  this->full_scan();

  g_exiting.store(false, std::memory_order_relaxed);
  if (!m_bpf_program.initialize()) {
    spdlog::error("Failed to initialize the BPF program");
    m_status = false;
  }
}

extern "C" void handle_event(void *ctx, int32_t cpu, void *data,
                             uint32_t data_sz) {
  // Must match struct event in ananicy_cpp.bpf.c.
  struct event {
    pid_t pid;
  };

  if (data_sz < sizeof(event)) {
    return;
  }
  const auto *e = static_cast<const struct event *>(data);
  if (e->pid == 0 || !g_process_queue) {
    return;
  }

  // This is a C callback driven by libbpf: an exception escaping it
  // terminates the daemon, so a single vanished process must never do that.
  try {
    std::string &&command = get_command_from_pid(e->pid);

    Process process{.pid = e->pid, .name = std::move(command)};
    spdlog::trace("Pushing new process: pid = {}", e->pid);
    g_process_queue->push(std::move(process));
  } catch (const std::exception &ex) {
    spdlog::debug("{}: skipping pid {}: {}", __func__, e->pid, ex.what());
  }
}

extern "C" void handle_lost_events(void *ctx, int cpu, __u64 lost_cnt) {
  // Processes started in that window would keep their default priority
  // forever: remember it, the event loop then rescans /proc.
  spdlog::warn("Lost {} process events on CPU {}, a full scan will follow",
               lost_cnt, cpu);
  g_lost_events.fetch_add(lost_cnt, std::memory_order_relaxed);
}

static std::int32_t handle_events(BPFProgram   *bpf_program,
                                  ProcessQueue *process_queue,
                                  const std::stop_token &stop_token) {
  using clock = std::chrono::steady_clock;
  // At most one recovery scan per second, however many buffers overflow.
  static constexpr auto LOST_EVENTS_RESCAN_INTERVAL = std::chrono::seconds(1);
  clock::time_point     last_rescan{};

  std::int32_t status{};
  // while (!stop_token.stop_requested()) {
  while (!g_exiting.load(std::memory_order_consume)) {
    status = bpf_program->pool_event();
    if (status < 0 && status != -EINTR) {
      spdlog::error(fmt::format("Error received ({}): {}", status,
                                get_error_string(-status)));
      return EXIT_FAILURE;
    }
    /* reset err to return 0 if exiting */
    status = 0;

    // Drops counted by the BPF program itself: known right away, unlike the
    // kernel's lost-event records (handle_lost_events), which only arrive
    // with the next event written to the same ring.
    if (const auto dropped = bpf_program->take_dropped_events(); dropped != 0) {
      spdlog::warn("{} process events dropped (buffer full), a full scan will "
                   "follow",
                   dropped);
      g_lost_events.fetch_add(dropped, std::memory_order_relaxed);
    }

    if (g_lost_events.load(std::memory_order_relaxed) != 0 &&
        clock::now() - last_rescan >= LOST_EVENTS_RESCAN_INTERVAL) {
      const auto lost = g_lost_events.exchange(0, std::memory_order_relaxed);
      spdlog::info("Rescanning /proc after {} lost process events", lost);
      process_queue->full_scan();
      last_rescan = clock::now();
    }
  }

  return status;
}

void ProcessQueue::start() {
  this->event_thread =
      std::jthread([this](const std::stop_token &stop_token) -> void {
        int ret = EXIT_FAILURE;
        while (ret == EXIT_FAILURE && !stop_token.stop_requested()) {
          spdlog::trace("Starting handle_events");

          ret = handle_events(&m_bpf_program, this, stop_token);

          if (ret == EXIT_SUCCESS || stop_token.stop_requested())
            break;

          spdlog::warn("restarting socket connection!");
          g_exiting.store(true, std::memory_order_relaxed);
          m_bpf_program.destroy();
          this->init();
        }
      });
}

#else

static std::int32_t handle_events(NetlinkProgram              *netlink_program,
                                  synchronized_queue<Process> *queue,
                                  const std::stop_token       &stop_token) {
  ssize_t status = 0;
  while (!stop_token.stop_requested()) {
    errno = 0;
    status = netlink_program->pool_message();
    const std::int32_t errnum = errno;
    if (status == 0) {
      // An empty datagram: nothing to do. Returning here used to end the
      // event thread for good, silently, leaving only the startup scan.
      continue;
    } else if (status == -1) {
      if (errnum == EINTR || errnum == EAGAIN) {
        continue;
      }
      if (errnum == ENOBUFS) {
        spdlog::warn(fmt::format("Received message overrun - ({})",
                                 get_error_string(errnum)));
        return EXIT_FAILURE;
      }
      spdlog::error(fmt::format("Bad netlink status ({}): {}", errnum,
                                get_error_string(errnum)));
      return EXIT_FAILURE;
    }

    /// Don't add an exact repeat of the previous event to the queue.
    /// @see: https://gitlab.com/ananicy-cpp/ananicy-cpp/-/issues/20
    /// Compare the event type too: starting a program is a FORK of the child
    /// followed by an EXEC of the *same* pid, and only the exec carries the
    /// new program name. Deduplicating on the pid alone dropped that exec, so
    /// the process was matched under its parent's name (e.g. "bash").
    static pid_t         prev_pid;
    static std::uint32_t prev_type;

    const pid_t         pid = netlink_program->get_pid();
    const std::uint32_t type = netlink_program->get_event_type();
    if (pid == 0 || (pid == prev_pid && type == prev_type)) {
      continue;
    }
    prev_pid = pid;
    prev_type = type;

    try {
      Process process{.pid = pid, .name = get_command_from_pid(pid)};
      spdlog::trace("Pushing new process: pid = {}", pid);
      queue->push(std::move(process));
    } catch (const std::exception &ex) {
      spdlog::debug("{}: skipping pid {}: {}", __func__, pid, ex.what());
    }
  }

  return static_cast<std::int32_t>(status);
}

void ProcessQueue::init() noexcept {
  this->full_scan();

  if (!m_netlink_program.initialize()) {
    spdlog::error(fmt::format("Failed to initialize the NETLINK program : {}",
                              get_error_string(errno)));
    m_status = false;
  }
}

void ProcessQueue::start() {
  this->event_thread =
      std::jthread([this](const std::stop_token &stop_token) -> void {
        int ret = EXIT_FAILURE;
        while (ret == EXIT_FAILURE && !stop_token.stop_requested()) {
          spdlog::trace("Starting handle_events");

          ret = handle_events(&m_netlink_program, &this->process_queue,
                              stop_token);
          if (ret == EXIT_SUCCESS || stop_token.stop_requested())
            break;

          spdlog::warn("restarting socket connection!");
          m_netlink_program.destroy();
          this->init();
        };
      });
}

#endif

std::optional<std::string> title_program_name(std::string_view argv0,
                                              std::string_view exe_path) {
  if (argv0.ends_with(".exe")) // Wine path, e.g. "C:\\Program Files\\x.exe"
    return std::nullopt;
  const auto space = argv0.find(' ');
  if (space == std::string_view::npos)
    return std::nullopt;
  std::string_view head = argv0.substr(0, space);
  if (head.ends_with(':'))
    head.remove_suffix(1);
  if (head.empty())
    return std::nullopt;
  const auto slash = head.find_last_of('/');
  if (slash == std::string_view::npos)
    return std::string{head};
  // compare file names: argv[0] may reach the binary through a symlink
  const std::string_view name = head.substr(slash + 1);
  const auto exe_slash = exe_path.find_last_of('/');
  if (name.empty() || exe_slash == std::string_view::npos ||
      name != exe_path.substr(exe_slash + 1))
    return std::nullopt;
  return std::string{name};
}

static std::string exe_target(pid_t pid) {
  std::error_code ec;
  std::string     target =
      fs::read_symlink(fmt::format("/proc/{}/exe", pid), ec).string();
  if (ec)
    return {};
  if (const auto d = target.find(" (deleted)"); d != std::string::npos)
    target.resize(d);
  return target;
}

std::string get_command_from_pid(pid_t pid) {
  // Called from both the event thread and the full scan.
  static std::atomic<std::uint32_t> exe_fail_count = 0;
  static constexpr auto COMMAND_NAME_HEURISTIC_SKIP_EXE_FAILURES = 5;

  // Cmdline method
  try {
    std::vector<std::string> cmdline = process_info::get_cmdline_from_pid(pid);
    // An empty argv[0] (exec -a '' ...) carries no name: use the exe link.
    if (!cmdline.empty() && !cmdline[0].empty()) {
      // Rewritten process title: name the program, not the whole sentence.
      // /proc/PID/exe is only read when the title starts with a path.
      if (const auto space = cmdline[0].find(' '); space != std::string::npos) {
        const bool path_head =
            cmdline[0].find('/') < space && !cmdline[0].ends_with(".exe");
        if (auto name = title_program_name(
                cmdline[0], path_head ? exe_target(pid) : std::string{}))
          return *std::move(name);
      }

      // Wine/Proton processes show the Windows path of the image as argv[0]
      // (e.g. "C:\\Games\\game.exe"), so split on backslashes as well.
      // Done here so the full scan and the netlink backend match .exe rules
      // too, not only the BPF event path.
      auto exe_name_begin = cmdline[0].ends_with(".exe")
                                ? cmdline[0].find_last_of("/\\")
                                : cmdline[0].find_last_of('/');

      spdlog::trace(fmt::format("{}: exe_name_begin: {}, cmdline: {}", __func__,
                                exe_name_begin, cmdline));

      if (exe_name_begin != std::string::npos) {
        return cmdline[0].substr(exe_name_begin + 1);
      }
      return cmdline[0];
    }

  } catch (const fs::filesystem_error &e) {
    spdlog::warn(fmt::format("{}: filesystem error: {} (path: {})", __func__,
                             e.what(), std::string_view{e.path1().c_str()}));
  }

  // Exe method
  /**
   * If reading the exe file doesn't work several times in a row,
   * it probably means we don't have the permissions for that.
   * We should skip it entirely for performance reasons.
   */
  if (exe_fail_count < COMMAND_NAME_HEURISTIC_SKIP_EXE_FAILURES) {
    try {
      const fs::path proc_path{fmt::format("/proc/{}/exe", pid)};

      const fs::path exe_path = read_symlink(proc_path);
      auto           exe_name = exe_path.filename().string();
      // A replaced binary (e.g. after a package update) reads as
      // "name (deleted)"; strip the suffix *and* the space before it.
      const auto     exe_name_end = exe_name.find(" (deleted)");
      if (exe_name_end != std::string::npos) {
        exe_name.resize(exe_name_end);
      }
      spdlog::trace("{}: exe filename: {}", __func__, exe_name);

      exe_fail_count = 0;

      return exe_name;
    } catch (const fs::filesystem_error &e) {
      const auto errcode = e.code().value();
      if (errcode == ENOENT || errcode == ESRCH) // process already gone
        return "<unknown>";
      else if (errcode == EACCES || errcode == EPERM)
        exe_fail_count++;

      spdlog::trace(fmt::format("{}: read symlink failed: {}, code: {}",
                                __func__, e.code().message(), errcode));
    }
  }

  // Comm method
  try {
    std::ifstream comm_file(fmt::format("/proc/{}/comm", pid));
    std::string   comm;

    if (std::getline(comm_file, comm) && !comm.empty()) {
      return comm;
    }
  } catch (const fs::filesystem_error &e) {
    spdlog::error(fmt::format("{}: filesystem error: {} (path: {})", __func__,
                              e.what(), std::string_view{e.path1().c_str()}));
  }

  return {"<unknown>"};
}

void ProcessQueue::full_scan() {
  spdlog::info("Doing a full scan");
  std::uint32_t n_process_found{};
  for (const auto &dir_entry : fs::directory_iterator("/proc")) {
    const auto entry_filename = dir_entry.path().filename();
    const auto pid_str = entry_filename.string();
    if (!isdigit(pid_str[0])) {
      continue;
    }
    const auto pid = to_int<std::int32_t>(pid_str);
    // spdlog::trace("Pushing existing process: {}", pid);
    auto &&command = get_command_from_pid(pid);

    this->process_queue.push(Process{
        .pid = pid,
        .name = std::move(command),
    });
    n_process_found++;
  }

  spdlog::debug("Full scan found {:d} processes", n_process_found);
}

void ProcessQueue::stop() noexcept {
#if defined(USE_BPF_PROC_IMPL)
  g_exiting.store(true, std::memory_order_relaxed);
#endif
  // joinable(), not the stop token: when the listener failed to initialize,
  // start() never ran, and join() on that empty jthread threw
  // std::system_error from the noexcept destructor (std::terminate, core dump)
  // instead of the clean "initialization failed" exit.
  if (this->event_thread.joinable()) {
    this->event_thread.request_stop();
    this->event_thread.join();
  }
#if defined(USE_BPF_PROC_IMPL)
  m_bpf_program.destroy();
#else
  m_netlink_program.destroy();
#endif
}

pid_t get_pid() { return getpid(); }
