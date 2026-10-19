#pragma once

#include <optional>
#include <string>

#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

#include "store.h"

namespace uvscope {

struct Principal {
  std::string token;
  User user;
};

class Auth {
 public:
  explicit Auth(Store& store) : store_(store) {}

  std::optional<Principal> current(const uvcpp::uvcpp_web_request& req) const;
  std::optional<Principal> require(const uvcpp::uvcpp_web_request& req,
                                   uvcpp::uvcpp_web_response& resp,
                                   const std::string& permission) const;
  std::optional<Principal> require_module(const uvcpp::uvcpp_web_request& req,
                                          uvcpp::uvcpp_web_response& resp,
                                          const std::string& module,
                                          const std::string& permission) const;
  static void set_cookie(uvcpp::uvcpp_web_response& resp, const std::string& token,
                         long max_age, bool secure);
  static void clear_cookie(uvcpp::uvcpp_web_response& resp);
  static void deny(uvcpp::uvcpp_web_response& resp, int status, const std::string& error,
                   const std::string& code);

 private:
  Store& store_;
};

std::string session_token(const uvcpp::uvcpp_web_request& req);

}  // namespace uvscope
