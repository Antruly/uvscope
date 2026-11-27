#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace uvscope {

// Result of asking the host firewall to admit a TCP port.
//
// The helper never prompts. It tries a non-interactive `sudo -n` command and,
// when that is not permitted, returns the exact command an administrator can
// run. Port numbers are validated by the caller and passed as separate argv
// entries, so no shell is involved.
struct FirewallAllowResult {
  bool supported = false;
  bool applied = false;
  bool requires_elevation = false;
  std::string tool;
  std::string command;
  std::string output;
  std::string error;
};

// Detects ufw/firewalld and its persisted enabled state. Read-only.
nlohmann::json firewall_status_json();

// Adds an allow rule for `port/tcp` and returns what happened.
FirewallAllowResult firewall_allow_tcp(int port);

}  // namespace uvscope
