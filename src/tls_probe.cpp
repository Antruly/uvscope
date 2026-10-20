#include "tls_probe.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include <uv.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>

#include "certificate.h"
#include "util.h"

namespace uvscope {
namespace {

std::string pem_from_der(const std::vector<unsigned char>& der) {
  const std::string raw(reinterpret_cast<const char*>(der.data()), der.size());
  const std::string b64 = base64_encode(raw);
  std::string out = "-----BEGIN CERTIFICATE-----\n";
  for (std::size_t i = 0; i < b64.size(); i += 64) {
    out.append(b64, i, std::min<std::size_t>(64, b64.size() - i));
    out.push_back('\n');
  }
  out += "-----END CERTIFICATE-----\n";
  return out;
}

}  // namespace

nlohmann::json run_tls_probe(const TlsProbeInput& input) {
  nlohmann::json result;
  result["ok"] = false;
  result["url"] = input.url;

  const ParsedUrl url = parse_url(input.url);
  if (!url.valid) {
    result["error"] = url.error.empty() ? "invalid URL" : url.error;
    return result;
  }
  if (url.scheme != "https") {
    result["error"] = "live certificate fetch requires an https:// URL";
    return result;
  }
  result["host"] = url.host;
  result["port"] = url.port;

#ifdef UVSCOPE_HAS_PEER_CERT_CHAIN
  const int timeout_ms = std::max(1000, std::min(input.timeout_ms, 60000));
  uvcpp::uvcpp_ssl_context ssl(uvcpp::tls_mode::CLIENT, uvcpp::tls_version::TLS_1_2);
  const std::string ca = find_ca_bundle(executable_dir(nullptr));
  if (!ca.empty()) ssl.load_ca_file(ca);
  ssl.set_verify_mode(input.insecure ? uvcpp::tls_verify_mode::NONE
                                     : uvcpp::tls_verify_mode::PEER_STRICT);

  const auto started = std::chrono::steady_clock::now();
  uvcpp::uvcpp_http_client client;
  client.set_ssl_context(&ssl);
  const int rc = client.connect_wait(url.host.c_str(), url.port, timeout_ms);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  result["timing"] = {{"totalMs", elapsed}};

  const auto chain = client.peer_cert_chain_der();
  const long verify_result = client.peer_verify_result();
  const std::string verify_error = client.peer_verify_error();
  std::string alpn;
#if UVCPP_NGHTTP2_ENABLE || UVCPP_HTTP3_ENABLE
  alpn = client.negotiated_alpn();
#endif
  result["live"] = {{"verified", verify_result == 0 && rc == 0},
                    {"verifyResult", verify_result},
                    {"verifyError", verify_error},
                    {"alpn", alpn},
                    {"insecure", input.insecure},
                    {"handshakeError", rc != 0 ? uv_error_string(rc) : std::string()}};

  if (chain.empty()) {
    result["error"] = rc != 0
                          ? "TLS handshake failed: " + uv_error_string(rc)
                          : "the peer did not present a certificate chain";
    return result;
  }

  std::string pem;
  for (const auto& der : chain) pem += pem_from_der(der);
  std::string parse_error;
  nlohmann::json info = inspect_certificate(pem, &parse_error);
  if (!info.is_object()) {
    result["error"] = parse_error.empty() ? "could not parse the peer chain" : parse_error;
    result["certificateCount"] = chain.size();
    return result;
  }
  result.update(info);
  result["ok"] = true;
  result["live"]["certificateCount"] = chain.size();
  return result;
#else
  (void)input;
  result["error"] = "live certificate fetch requires libuvcpp 1.8.2 or newer";
  result["code"] = "unsupported";
  return result;
#endif
}

}  // namespace uvscope
