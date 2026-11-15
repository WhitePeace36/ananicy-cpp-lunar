#pragma once

// Helpers to apply a per-thread setting to a whole process and to report the
// result the way the worker expects:
//   true  -> carry on with the rule's next attribute
//   false -> the process is gone, skip the rest of the rule
//
// Linux nice, latency nice, I/O priority (IOPRIO_WHO_PROCESS), scheduling
// policy and CPU affinity are all *per thread*: calling them with the pid only
// changes the main thread, so they have to be applied to /proc/<pid>/task/*.

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/types.h>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace task_helpers {

/// Runs `op(tid)` for every thread of `pid`; `op` returns 0 or an errno.
/// Returns 0 if at least one thread was changed and no thread failed for a
/// reason other than having exited meanwhile, ESRCH if the process (or every
/// one of its threads) is gone, otherwise the first real error.
template <typename Op>
std::int32_t for_each_thread(pid_t pid, Op &&op) noexcept {
  std::error_code                     ec;
  std::filesystem::directory_iterator it{fmt::format("/proc/{}/task", pid), ec};
  if (ec) {
    return ESRCH;
  }

  bool         applied = false;
  std::int32_t error = 0;
  for (; it != std::filesystem::directory_iterator{}; it.increment(ec)) {
    if (ec) {
      break;
    }
    const std::string name = it->path().filename().string();
    char             *end = nullptr;
    const long        tid = std::strtol(name.c_str(), &end, 10);
    if (end == name.c_str() || *end != '\0' || tid <= 0) {
      continue;
    }
    const std::int32_t err = op(static_cast<pid_t>(tid));
    if (err == 0) {
      applied = true;
    } else if (err != ESRCH && error == 0) { // ESRCH: thread exited meanwhile
      error = err;
    }
  }
  if (error != 0) {
    return error;
  }
  return applied ? 0 : ESRCH;
}

/// Logs `err` for `what` on `pid` and converts it to the worker's convention.
/// Only a vanished process stops the rule; a refused operation (permissions,
/// unsupported value) is logged and the remaining attributes still apply.
inline bool report(std::int32_t err, std::string_view what, pid_t pid) {
  if (err == 0) {
    spdlog::debug("{}: Successfully applied to {}", what, pid);
    return true;
  }
  if (err == ESRCH) {
    spdlog::warn("{}: Process not found: {}", what, pid);
    return false;
  }
  if (err == EACCES || err == EPERM) {
    spdlog::error("{}: Insufficient permission for process {} ({})", what, pid,
                  std::strerror(err));
    return true;
  }
  spdlog::error("{}: Failed for process {}: {} ({})", what, pid,
                std::strerror(err), err);
  return true;
}

} // namespace task_helpers
