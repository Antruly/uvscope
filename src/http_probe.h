#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "socks5_proxy.h"

namespace uvscope {

struct HttpHeaderInput {
  std::string name;
  std::string value;
};

struct HttpProbeInput {
  std::string url;
  std::string method = "GET";
  std::vector<HttpHeaderInput> headers;
  std::string body;
  int timeout_ms = 20000;
  std::string protocol = "auto";  // auto | http1 | http2 | http3
  bool insecure = false;
  bool compression = false;
  Socks5Proxy proxy;
  std::size_t max_body_bytes = 4 * 1024 * 1024;
};

nlohmann::json run_http_probe(const HttpProbeInput& input);
std::string http_probe_summary(const nlohmann::json& result);

}  // namespace uvscope
