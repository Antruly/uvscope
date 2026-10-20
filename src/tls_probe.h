#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace uvscope {

struct TlsProbeInput {
  std::string url;
  int timeout_ms = 10000;
  bool insecure = false;
};

// Connects to an https:// URL and returns the peer certificate chain plus
// verification details. Requires libuvcpp 1.8.2 (peer_cert_chain_der); older
// SDKs return a structured "unsupported" result.
nlohmann::json run_tls_probe(const TlsProbeInput& input);

}  // namespace uvscope
