#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "application.h"
#include "ipc.h"
#include "listener_child.h"
#include "proxy_child.h"
#include "socks5_child.h"
#include "terminal_child.h"
#include "util.h"
#include "version.h"

namespace {

void print_help() {
  std::printf(
      "uvscope %s — multi-protocol web debugging workbench\n\n"
      "Usage: uvscope [options]\n\n"
      "  --host <addr>       Listen address (default 127.0.0.1)\n"
      "  --port <port>       Listen port (default 8787; 0 chooses a free port)\n"
      "  --data-dir <path>   Data directory (SQLite, uploads, runtime state)\n"
      "  --web-root <path>   Built frontend directory\n"
      "  --tls-cert <pem>    Serve the workbench UI over HTTPS\n"
      "  --tls-key <pem>     Private key paired with --tls-cert\n"
      "  --open              Open the workbench in the default browser\n"
      "  --self-check        Run offline smoke tests and exit\n"
      "  --version           Print version and exit\n"
      "  -h, --help          Show this help\n\n"
      "Environment: UVSCOPE_DATA_DIR, UVSCOPE_WEB_ROOT, UVCPP_ROOT\n",
      UVSCOPE_VERSION);
}

bool parse_int(const std::string& text, int* value) {
  if (value == nullptr) return false;
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || parsed < 0 || parsed > 65535) return false;
  *value = static_cast<int>(parsed);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  SetConsoleOutputCP(CP_UTF8);
#endif

  uvscope::AppOptions options;
  options.data_dir = uvscope::default_data_dir();
  options.web_root = uvscope::default_web_root(uvscope::executable_dir(argv[0]));
  std::error_code self_path_error;
  options.self_path =
      std::filesystem::absolute(argv[0], self_path_error).string();
  if (self_path_error) options.self_path = argv[0];
  std::string ipc_child_endpoint;
  std::string proxy_child_endpoint;
  std::string socks5_child_endpoint;
  std::string listener_child_endpoint;
  std::string terminal_child_endpoint;
  bool show_help = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "uvscope: %s requires a value\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--host") {
      options.host = require_value("--host");
    } else if (arg == "--port") {
      const std::string value = require_value("--port");
      if (!parse_int(value, &options.port)) {
        std::fprintf(stderr, "uvscope: invalid port '%s'\n", value.c_str());
        return 2;
      }
    } else if (arg == "--data-dir") {
      options.data_dir = require_value("--data-dir");
    } else if (arg == "--web-root") {
      options.web_root = require_value("--web-root");
    } else if (arg == "--tls-cert") {
      options.tls_cert = require_value("--tls-cert");
    } else if (arg == "--tls-key") {
      options.tls_key = require_value("--tls-key");
    } else if (arg == "--open") {
      options.open_browser = true;
    } else if (arg == "--self-check") {
      options.self_check = true;
    } else if (arg == "--ipc-child") {
      ipc_child_endpoint = require_value("--ipc-child");
    } else if (arg == "--proxy-child") {
      proxy_child_endpoint = require_value("--proxy-child");
    } else if (arg == "--socks5-child") {
      socks5_child_endpoint = require_value("--socks5-child");
    } else if (arg == "--listener-child") {
      listener_child_endpoint = require_value("--listener-child");
    } else if (arg == "--terminal-child") {
      terminal_child_endpoint = require_value("--terminal-child");
    } else if (arg == "--version" || arg == "-v") {
      std::printf("uvscope %s\n", UVSCOPE_VERSION);
      return 0;
    } else if (arg == "--help" || arg == "-h") {
      show_help = true;
    } else {
      std::fprintf(stderr, "uvscope: unknown argument '%s'\n", arg.c_str());
      print_help();
      return 2;
    }
  }

  if (show_help) {
    print_help();
    return 0;
  }
  if (!ipc_child_endpoint.empty()) {
    return uvscope::run_ipc_child(ipc_child_endpoint);
  }
  if (!proxy_child_endpoint.empty()) {
    return uvscope::run_proxy_child(proxy_child_endpoint);
  }
  if (!socks5_child_endpoint.empty()) {
    return uvscope::run_socks5_child(socks5_child_endpoint);
  }
  if (!listener_child_endpoint.empty()) {
    return uvscope::run_listener_child(listener_child_endpoint);
  }
  if (!terminal_child_endpoint.empty()) {
    return uvscope::run_terminal_child(terminal_child_endpoint);
  }

  uvscope::Application app(std::move(options));
  return app.run();
}
