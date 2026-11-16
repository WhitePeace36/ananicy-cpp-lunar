//
// Created by aviallon on 20/04/2021.
//

#include "core/priority.hpp"
#include "syscalls.h"
#include "task_helpers.hpp"
#include "utility/utils.hpp"

#include <filesystem>  // for directory_iterator
#include <fstream>     // for ofstream
#include <string_view> // for string_view

#include <fmt/format.h>

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace priority {

bool set_priority(pid_t pid, std::int32_t nice_value) {
  const std::int32_t err = task_helpers::for_each_thread(pid, [&](pid_t tid) {
    errno = 0;
    return setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice_value) == 0
               ? 0
               : errno;
  });
  return task_helpers::report(err, __func__, pid);
}

bool set_latency_nice(pid_t pid, std::int32_t latency_nice_value) {
  const std::int32_t err = task_helpers::for_each_thread(pid, [&](pid_t tid) {
    errno = 0;
    return set_latnice(tid, latency_nice_value) == 0 ? 0 : errno;
  });
  return task_helpers::report(err, __func__, pid);
}


bool set_io_priority(pid_t pid, const std::string_view &io_class,
                     std::int32_t value) {
  std::int32_t io_class_value = 0;
  if (io_class == "best-effort") {
    io_class_value = IOPRIO_CLASS_BE;
  } else if (io_class == "realtime") {
    io_class_value = IOPRIO_CLASS_RT;
  } else if (io_class == "idle") {
    io_class_value = IOPRIO_CLASS_IDLE;
  } else if (io_class == "none") {
    // No class of its own: the I/O priority follows the CPU priority again
    // (best-effort at level (nice + 20) / 5, idle for SCHED_IDLE tasks), like
    // `ionice -c none`. The kernel only accepts it with level 0.
    io_class_value = IOPRIO_CLASS_NONE;
    if (value != 0) {
      spdlog::debug("ioclass \"none\" has no level, ignoring ionice {}",
                    value);
      value = 0;
    }
  } else {
    spdlog::error("Unknown io class {}, skipping...", io_class);
    return true;
  }

  const std::int32_t io_prio = IOPRIO_PRIO_VALUE(io_class_value, value);

  // ioprio_valid() rejects class "none", which is valid to set (see above).
  if (io_class_value != IOPRIO_CLASS_NONE && !ioprio_valid(io_prio)) {
    spdlog::error("IO priority is invalid, skipping...");
    return true;
  }

  // IOPRIO_WHO_PROCESS takes a thread id: apply it to every thread, not
  // only the main one (I/O done by worker threads kept its old priority).
  const std::int32_t err = task_helpers::for_each_thread(pid, [&](pid_t tid) {
    errno = 0;
    return ioprio_set(IOPRIO_WHO_PROCESS, static_cast<id_t>(tid), io_prio) == 0
               ? 0
               : errno;
  });
  return task_helpers::report(err, __func__, pid);
}

bool set_sched(pid_t pid, const std::string_view &sched_name,
               unsigned rt_prio) {
  std::int32_t sched = 0;
  sched_param  param = {};
  if (sched_name == "idle") {
    sched = SCHED_IDLE;
  } else if (sched_name == "normal" || sched_name == "other") {
    sched = SCHED_OTHER;
  } else if (sched_name == "rr") {
    sched = SCHED_RR;
    param.sched_priority = static_cast<std::int32_t>(rt_prio);
  } else if (sched_name == "fifo") {
    sched = SCHED_FIFO;
    param.sched_priority = static_cast<std::int32_t>(rt_prio);
  } else if (sched_name == "deadline") {
    spdlog::warn("deadline scheduler is not available yet (due to its "
                 "different behavior)");
    //    sched = SCHED_DEADLINE;
    //    param.sched_priority = 1;
    // Leave the policy alone: falling through would call
    // sched_setscheduler() with sched == 0, i.e. reset it to SCHED_OTHER.
    return true;
  } else if (sched_name == "batch") {
    sched = SCHED_BATCH;
  } else {
    spdlog::error("{}: Invalid scheduler '{}'", __func__, sched_name);
    // A bad rule value is not a vanished process: returning false would make
    // the worker skip the remaining attributes (ioclass, cgroup, cpuset...).
    return true;
  }
  // sched_setscheduler() is per thread as well.
  const std::int32_t err = task_helpers::for_each_thread(pid, [&](pid_t tid) {
    errno = 0;
    return sched_setscheduler(tid, sched, &param) == 0 ? 0 : errno;
  });
  if (err != 0 && err != ESRCH) {
    spdlog::error("{}: Error info: sched_name: {}, pid: {}", __func__,
                  sched_name, pid);
  }
  return task_helpers::report(err, __func__, pid);
}

bool set_oom_score_adjust(pid_t pid, std::int32_t value) {
  // oom_score_adj is per process. Only a vanished process may stop the rule:
  // a refused or invalid value is logged and the remaining attributes apply.
  const auto    oom_score_path{fmt::format("/proc/{}/oom_score_adj", pid)};
  std::ofstream oom_score_file{oom_score_path};
  if (oom_score_file.fail()) {
    const std::int32_t err = errno;
    std::error_code    ec;
    if (!fs::exists(fmt::format("/proc/{}", pid), ec)) {
      return task_helpers::report(ESRCH, __func__, pid);
    }
    spdlog::error("{}: Couldn't open {} to set {}: {}", __func__,
                  oom_score_path, value, std::strerror(err));
    return true;
  }

  // The kernel validates the value on write, so check the flush too (it
  // used to fail silently when the stream was destroyed).
  errno = 0;
  oom_score_file << value << std::flush;
  if (oom_score_file.fail()) {
    spdlog::error("{}: Couldn't set OOM score adjustment to {} for {}: {}",
                  __func__, value, pid, std::strerror(errno));
  }
  return true;
}

std::string_view test_latnice_support() noexcept {
  pid_t        pid{0};
  std::int32_t latency_nice{-20};

  const std::int32_t saved_latnice = get_latnice(pid);
  errno = 0;

  // Use this here instead of function,
  // to suppress errors from function call.
  struct ananicy_sched_attr attr = {
      .size = sizeof(struct ananicy_sched_attr),
      .sched_flags = SCHED_FLAG_LATENCY_NICE | SCHED_FLAG_KEEP_PARAMS,
      .sched_latency_nice = latency_nice,
  };
  const std::int32_t err = ananicy_sched_setattr(pid, &attr, 0);
  bool               is_supported{};
  if (err == 0 && errno == 0) {
    set_latnice(pid, saved_latnice);
    is_supported = true;
  }
  return (is_supported) ? "true" : "false";
}

} // namespace priority
