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
  // "text" (current behaviour) or "base64" for binary-safe payloads such as a
  // browser-built multipart/form-data body.
  std::string body_encoding = "text";
  int timeout_ms = 20000;
  std::string protocol = "auto";  // auto | http1 | http2 | http3
  bool insecure = false;
  // Advertise gzip/deflate/br and decode the response in the probe. When
  // false the body stays compressed and content-encoding is preserved.
  bool decompress = true;
  Socks5Proxy proxy;
  std::size_t max_body_bytes = 4 * 1024 * 1024;
};

nlohmann::json run_http_probe(const HttpProbeInput& input);
std::string http_probe_summary(const nlohmann::json& result);

}  // namespace uvscope
