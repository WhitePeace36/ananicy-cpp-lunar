#pragma once

#include <cerrno>   // for errno, EINTR
#include <fcntl.h>  // for open
#include <string>   // for string
#include <unistd.h> // for read, close
#include <vector>   // for vector

#include <fmt/format.h>

namespace process_info {
static std::vector<std::string> get_cmdline_from_pid(std::int32_t pid) {
  // Read the whole file: arguments are separated by NUL and may themselves
  // contain newlines (e.g. "python3 -c $'import x\nmain()' myapp"), so a
  // std::getline() would cut the command line at the first '\n'.
  // Plain read(2), not iostreams: when the process exits mid-read, read()
  // fails with ESRCH, and libstdc++'s filebuf turns that into an exception
  // that an istreambuf_iterator lets escape (it crashed the event thread
  // under heavy process churn). Here it just ends the read.
  std::string cmdline_value;
  const int   fd = ::open(fmt::format("/proc/{}/cmdline", pid).c_str(),
                          O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    char buffer[4096];
    for (;;) {
      const ssize_t n = ::read(fd, buffer, sizeof(buffer));
      if (n > 0) {
        cmdline_value.append(buffer, static_cast<size_t>(n));
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else {
        break; // EOF, or the process is gone
      }
    }
    ::close(fd);
  }
  // Split on NUL, keeping empty arguments: skipping them made argv[1] look
  // like argv[0] (the program name) when argv[0] was empty.
  std::vector<std::string> cmdline;
  size_t                   start_pos = 0;
  while (start_pos < cmdline_value.size()) {
    size_t end_pos = cmdline_value.find('\0', start_pos);
    if (end_pos == std::string::npos) {
      end_pos = cmdline_value.size();
    }
    cmdline.push_back(cmdline_value.substr(start_pos, end_pos - start_pos));
    start_pos = end_pos + 1;
  }
  // Processes that rewrite their argv in place (setproctitle) leave NUL
  // padding behind: drop the trailing empty strings.
  while (!cmdline.empty() && cmdline.back().empty()) {
    cmdline.pop_back();
  }

  return cmdline;
}
} // namespace process_info
