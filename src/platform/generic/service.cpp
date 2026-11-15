#include "service.hpp"

namespace service {
// Without systemd there is no service manager to notify.
void set_status(service::status) {}

std::string get_unit_name() { return "<not using systemd>"; }
} // namespace service
