#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace uvscope {

// Outbound SOCKS5 settings shared by HTTP/TCP/UDP probes and persistent
// sessions. An absent proxy means a direct connection; a configured proxy is
// mandatory and never falls back to direct when the handshake fails.
struct Socks5Proxy {
  std::string host;
  int port = 0;
  std::string username;
  std::string password;

  bool enabled() const {
    return !host.empty() || port != 0 || !username.empty() || !password.empty();
  }

  bool valid(std::string* error) const;
};

// API-safe view: never includes the password.
nlohmann::json socks5_proxy_json(const Socks5Proxy& proxy);

// Structured failure. `code` is the libuv error and `detail` the proxy's own
// message (`socks5_last_error()`).
nlohmann::json socks5_failure_json(const Socks5Proxy& proxy, int code,
                                   const std::string& detail,
                                   bool proxy_connect_failed = false);

std::string socks5_error_class(int code, const std::string& detail,
                               bool proxy_connect_failed);

}  // namespace uvscope
