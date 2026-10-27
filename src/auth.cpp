#include "auth.h"

#include <nlohmann/json.hpp>

namespace uvscope {
namespace {
constexpr const char* kSessionCookie = "uvscope_session";
}

std::string session_token(const uvcpp::uvcpp_web_request& req) {
  const std::string* value = req.cookie(kSessionCookie);
  return value == nullptr ? std::string() : *value;
}

std::optional<Principal> Auth::current(const uvcpp::uvcpp_web_request& req) const {
  const std::string token = session_token(req);
  if (token.empty()) return std::nullopt;
  const auto session = store_.session_by_token(token);
  if (!session) return std::nullopt;
  Principal p;
  p.token = token;
  p.user = session->user;
  return p;
}

std::optional<Principal> Auth::require(const uvcpp::uvcpp_web_request& req,
                                       uvcpp::uvcpp_web_response& resp,
                                       const std::string& permission) const {
  const auto principal = current(req);
  if (!principal) {
    deny(resp, 401, "authentication required", "unauthenticated");
    return std::nullopt;
  }
  if (!role_can(principal->user.role, permission)) {
    deny(resp, 403, "permission denied", "forbidden");
    return std::nullopt;
  }
  return principal;
}

std::optional<Principal> Auth::require_module(const uvcpp::uvcpp_web_request& req,
                                              uvcpp::uvcpp_web_response& resp,
                                              const std::string& module,
                                              const std::string& permission) const {
  auto principal = require(req, resp, permission);
  if (!principal) return std::nullopt;
  if (!store_.module_enabled(module)) {
    deny(resp, 403, "module is disabled", "module_disabled");
    return std::nullopt;
  }
  return principal;
}

void Auth::set_cookie(uvcpp::uvcpp_web_response& resp, const std::string& token, long max_age,
                      bool secure) {
  resp.set_cookie(kSessionCookie, token, "/", max_age, true, secure, "Lax");
}

void Auth::clear_cookie(uvcpp::uvcpp_web_response& resp) {
  resp.clear_cookie(kSessionCookie, "/");
}

void Auth::deny(uvcpp::uvcpp_web_response& resp, int status, const std::string& error,
                const std::string& code) {
  nlohmann::json body = {{"ok", false}, {"error", error}, {"code", code}};
  resp.status(status).set_header("cache-control", "no-store").json_str(body.dump());
  resp.end();
}

}  // namespace uvscope
