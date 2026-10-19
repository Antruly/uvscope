#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace uvscope {

struct TcpProbeInput {
  std::string host;
  int port = 0;
  std::string payload;
  std::string encoding = "text";  // text | hex
  int connect_timeout_ms = 8000;
  int read_timeout_ms = 3000;
  bool close_after = true;
};

struct UdpProbeInput {
  std::string host;
  int port = 0;
  std::string payload;
  std::string encoding = "text";
  int timeout_ms = 3000;
};

nlohmann::json run_tcp_probe(const TcpProbeInput& input);
nlohmann::json run_udp_probe(const UdpProbeInput& input);

}  // namespace uvscope
