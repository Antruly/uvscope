#include "socks5_proxy.h"

#include <uv.h>

#include "util.h"

namespace uvscope {

bool Socks5Proxy::valid(std::string* error) const {
  if (!enabled()) return true;
  if (host.empty() || host.size() > 255) {
    if (error) *error = "SOCKS5 proxy host is required";
    return false;
  }
  if (port <= 0 || port > 65535) {
    if (error) *error = "SOCKS5 proxy port must be between 1 and 65535";
    return false;
  }
  if (username.size() > 255 || password.size() > 255) {
    if (error) *error = "SOCKS5 credentials must be at most 255 bytes";
    return false;
  }
  // The runtime only offers one method. A password without a username would
  // silently fall back to no-auth, so reject it before connecting.
  if (username.empty() && !password.empty()) {
    if (error) *error = "SOCKS5 proxy password requires a username";
    return false;
  }
  return true;
}

nlohmann::json socks5_proxy_json(const Socks5Proxy& proxy) {
  if (!proxy.enabled()) return nlohmann::json();
  return {{"host", proxy.host},
          {"port", proxy.port},
          {"authenticated", !proxy.username.empty()}};
}

std::string socks5_error_class(int code, const std::string& detail,
                               bool proxy_connect_failed) {
  const std::string text = lower(detail);
  if (code == UV_ETIMEDOUT || text.find("timed out") != std::string::npos) {
    return "timeout";
  }
  if (proxy_connect_failed) return "proxy_unreachable";
  if (text.find("authentication") != std::string::npos ||
      text.find("invalid username/password") != std::string::npos ||
      text.find("no acceptable authentication") != std::string::npos) {
    return "auth_failed";
  }
  if (code == UV_EPROTO || text.find("malformed") != std::string::npos) {
    return "protocol_error";
  }
  if (text.find("not allowed") != std::string::npos ||
      text.find("unreachable") != std::string::npos ||
      text.find("connection refused") != std::string::npos ||
      text.find("ttl expired") != std::string::npos ||
      text.find("not supported") != std::string::npos) {
    return "target_rejected";
  }
  if (code == UV_EACCES) return "target_rejected";
  if (code == UV_ECONNREFUSED || code == UV_EHOSTUNREACH ||
      code == UV_ENETUNREACH) {
    return "target_rejected";
  }
  return "proxy_error";
}

nlohmann::json socks5_failure_json(const Socks5Proxy& proxy, int code,
                                   const std::string& detail,
                                   bool proxy_connect_failed) {
  nlohmann::json out = socks5_proxy_json(proxy);
  if (!out.is_object()) out = nlohmann::json::object();
  out["enabled"] = proxy.enabled();
  out["errorClass"] = socks5_error_class(code, detail, proxy_connect_failed);
  out["errorCode"] = code;
  out["error"] = detail.empty() ? uv_error_string(code) : detail;
  return out;
}

}  // namespace uvscope
