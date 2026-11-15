#pragma once

#include <string>
namespace service {
enum status {
  STARTING,
  STARTED,
  STOPPING,
  STOPPED,
};

void set_status(status status);

std::string get_unit_name();
} // namespace service
