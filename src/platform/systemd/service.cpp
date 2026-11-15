#include "service.hpp"

#include <cerrno>
#include <cstdlib>

#include <systemd/sd-daemon.h>
#include <systemd/sd-login.h>
#include <unistd.h>

namespace service {
void set_status(service::status status) {
  switch (status) {
  case STARTING:
    break;
  case STARTED:
    sd_notify(0, "READY=1");
    break;
  case STOPPING:
    sd_notify(0, "STOPPING=1");
    break;
  case STOPPED:
    break;
  }
}

std::string get_unit_name() {
  char *unit_name;
  errno = 0;
  sd_pid_get_unit(getpid(), &unit_name);
  if (errno == ENODATA) {
    return "<empty>";
  }

  auto ret = std::string(unit_name);
  free(unit_name);

  return ret;
}

} // namespace service
