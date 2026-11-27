#include "firewall.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "util.h"

#if defined(__linux__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace uvscope {
namespace {

#if defined(__linux__)

constexpr std::size_t kMaxOutput = 16 * 1024;

struct CommandResult {
  bool started = false;
  int exit_code = -1;
  std::string output;
};

struct FirewallTool {
  std::string name;
  std::string path;
};

std::optional<std::string> find_executable(std::initializer_list<const char*> candidates) {
  for (const char* candidate : candidates) {
    if (candidate != nullptr && access(candidate, X_OK) == 0) {
      return std::string(candidate);
    }
  }
  return std::nullopt;
}

CommandResult run_command(const std::vector<std::string>& argv) {
  CommandResult result;
  if (argv.empty()) {
    result.output = "empty command";
    return result;
  }
  int pipe_fd[2] = {-1, -1};
  if (pipe(pipe_fd) != 0) {
    result.output = std::strerror(errno);
    return result;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    result.output = std::strerror(errno);
    return result;
  }
  if (pid == 0) {
    close(pipe_fd[0]);
    dup2(pipe_fd[1], STDOUT_FILENO);
    dup2(pipe_fd[1], STDERR_FILENO);
    close(pipe_fd[1]);
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& item : argv) {
      args.push_back(const_cast<char*>(item.c_str()));
    }
    args.push_back(nullptr);
    // A stuck firewall tool must not pin a worker thread forever. The alarm is
    // preserved across execv, so the tool itself receives SIGALRM after 10 s.
    alarm(10);
    execv(args[0], args.data());
    _exit(127);
  }
  close(pipe_fd[1]);
  char buffer[4096];
  for (;;) {
    const ssize_t count = read(pipe_fd[0], buffer, sizeof(buffer));
    if (count > 0) {
      const std::size_t room =
          result.output.size() < kMaxOutput ? kMaxOutput - result.output.size() : 0;
      if (room > 0) {
        result.output.append(buffer, std::min<std::size_t>(static_cast<std::size_t>(count), room));
      }
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  close(pipe_fd[0]);
  int status = 0;
  pid_t waited = -1;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited == pid) {
    result.started = true;
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }
  result.output = trim(result.output);
  return result;
}

std::optional<FirewallTool> detect_tool() {
  if (const auto ufw =
          find_executable({"/usr/sbin/ufw", "/usr/bin/ufw", "/sbin/ufw", "/bin/ufw"})) {
    return FirewallTool{"ufw", *ufw};
  }
  if (const auto firewalld = find_executable(
          {"/usr/bin/firewall-cmd", "/usr/sbin/firewall-cmd", "/bin/firewall-cmd"})) {
    return FirewallTool{"firewalld", *firewalld};
  }
  return std::nullopt;
}

std::optional<std::string> find_sudo() {
  return find_executable({"/usr/bin/sudo", "/bin/sudo", "/usr/sbin/sudo", "/sbin/sudo"});
}

bool credential_prompt(const CommandResult& result) {
  // sudo capitalizes the first word of its diagnostics, so match lowercased.
  const std::string text = lower(result.output);
  return text.find("password is required") != std::string::npos ||
         text.find("a terminal is required") != std::string::npos ||
         text.find("no tty present") != std::string::npos ||
         text.find("not in the sudoers file") != std::string::npos;
}

#endif  // __linux__

}  // namespace

nlohmann::json firewall_status_json() {
  nlohmann::json out = {{"ok", true}, {"supported", false}, {"tool", "none"}};
#if defined(__linux__)
  const auto tool = detect_tool();
  if (!tool) {
    out["error"] = "no supported firewall tool found (ufw or firewalld)";
    return out;
  }
  out["supported"] = true;
  out["tool"] = tool->name;
  if (tool->name == "ufw") {
    // ufw persists its enabled flag in a world-readable file; `ufw status`
    // itself requires root and cannot be used from the daemon's user.
    const std::string conf = read_file("/etc/ufw/ufw.conf", 64 * 1024);
    out["enabled"] = conf.find("ENABLED=yes") != std::string::npos;
  } else {
    const auto systemctl = find_executable({"/usr/bin/systemctl", "/bin/systemctl"});
    out["enabled"] = false;
    if (systemctl) {
      const auto result = run_command({*systemctl, "is-active", "firewalld"});
      out["enabled"] = result.exit_code == 0;
    }
  }
#else
  out["error"] = "the firewall helper is only available on Linux";
#endif
  return out;
}

FirewallAllowResult firewall_allow_tcp(int port) {
  FirewallAllowResult result;
  if (port < 1 || port > 65535) {
    result.error = "port must be between 1 and 65535";
    return result;
  }
#if defined(__linux__)
  const auto tool = detect_tool();
  if (!tool) {
    result.error = "no supported firewall tool found (ufw or firewalld)";
    return result;
  }
  result.supported = true;
  result.tool = tool->name;

  const std::string port_spec = std::to_string(port) + "/tcp";
  const bool is_root = geteuid() == 0;
  const bool firewalld = tool->name == "firewalld";

  std::string display;
  if (firewalld) {
    display = (is_root ? "" : "sudo ") + tool->path + " --add-port=" + port_spec +
              " --permanent && " + (is_root ? "" : "sudo ") + tool->path + " --reload";
  } else {
    display = (is_root ? "" : "sudo ") + tool->path + " allow " + port_spec;
  }
  result.command = display;

  std::optional<std::string> sudo;
  if (!is_root) {
    sudo = find_sudo();
    if (!sudo) {
      result.requires_elevation = true;
      result.error = "sudo was not found; run the command as root";
      return result;
    }
  }

  auto elevated = [&](const std::vector<std::string>& args) {
    std::vector<std::string> argv;
    if (!is_root) argv.push_back(*sudo);
    argv.insert(argv.end(), args.begin(), args.end());
    return run_command(argv);
  };

  const auto run = firewalld
                       ? elevated({tool->path, "--add-port=" + port_spec, "--permanent"})
                       : elevated({tool->path, "allow", port_spec});
  result.output = run.output;
  if (run.exit_code != 0) {
    if (!is_root && credential_prompt(run)) {
      result.requires_elevation = true;
      result.error = "elevated privileges are required";
    } else {
      result.error = run.output.empty() ? "firewall command failed" : run.output;
    }
    return result;
  }

  if (firewalld) {
    const auto reload = elevated({tool->path, "--reload"});
    if (!reload.output.empty()) {
      result.output = result.output.empty() ? reload.output : result.output + "\n" + reload.output;
    }
    if (reload.exit_code != 0) {
      result.error = "the port was added but firewalld reload failed";
      return result;
    }
  }

  result.applied = true;
  return result;
#else
  result.error = "the firewall helper is only available on Linux";
  return result;
#endif
}

}  // namespace uvscope
