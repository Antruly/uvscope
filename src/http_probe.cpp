#include "http_probe.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

#include <uv.h>
#include <uvcpp/uvcpp_config.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

#include "util.h"
#include "version.h"

namespace uvscope {
namespace {

bool is_valid_utf8(const std::string& text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    std::size_t extra = 0;
    if (c < 0x80) {
      ++i;
      continue;
    }
    if ((c & 0xE0) == 0xC0) {
      extra = 1;
      if (c < 0xC2) return false;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      if (c > 0xF4) return false;
    } else {
      return false;
    }
    if (i + extra >= text.size()) return false;
    for (std::size_t j = 1; j <= extra; ++j) {
      if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80) return false;
    }
    if (extra == 2) {
      const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
      if (c == 0xE0 && c1 < 0xA0) return false;
      if (c == 0xED && c1 >= 0xA0) return false;
    }
    if (extra == 3) {
      const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
      if (c == 0xF0 && c1 < 0x90) return false;
      if (c == 0xF4 && c1 >= 0x90) return false;
    }
    i += extra + 1;
  }
  return true;
}

uvcpp::http_method method_from_string(const std::string& text) {
  const std::string upper = lower(trim(text));
  using M = uvcpp::http_method;
  if (upper == "GET") return M::HTTP_GET;
  if (upper == "POST") return M::HTTP_POST;
  if (upper == "PUT") return M::HTTP_PUT;
  if (upper == "DELETE") return M::HTTP_DELETE;
  if (upper == "PATCH") return M::HTTP_PATCH;
  if (upper == "HEAD") return M::HTTP_HEAD;
  if (upper == "OPTIONS") return M::HTTP_OPTIONS;
  if (upper == "TRACE") return M::HTTP_TRACE;
  if (upper == "CONNECT") return M::HTTP_CONNECT;
  if (upper == "PROPFIND") return M::HTTP_PROPFIND;
  if (upper == "PROPPATCH") return M::HTTP_PROPPATCH;
  if (upper == "MKCOL") return M::HTTP_MKCOL;
  if (upper == "COPY") return M::HTTP_COPY;
  if (upper == "MOVE") return M::HTTP_MOVE;
  if (upper == "LOCK") return M::HTTP_LOCK;
  if (upper == "UNLOCK") return M::HTTP_UNLOCK;
  if (upper == "REPORT") return M::HTTP_REPORT;
  if (upper == "SEARCH") return M::HTTP_SEARCH;
  return M::HTTP_GET;
}

std::string response_version(const uvcpp::uvcpp_http_response& resp) {
  return uvcpp::uvcpp_http_version_str(resp.version);
}

std::string uv_error(int code) { return uv_error_string(code); }

nlohmann::json headers_json(const uvcpp::http_headers& headers) {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& header : headers) {
    out.push_back({{"name", header.name}, {"value", header.value}});
  }
  return out;
}

std::string clip_body(const std::string& body, std::size_t max_bytes, bool* truncated) {
  if (body.size() <= max_bytes) {
    if (truncated) *truncated = false;
    return body;
  }
  if (truncated) *truncated = true;
  return body.substr(0, max_bytes);
}

struct AsyncState {
  bool done = false;
  int error = 0;
  std::string error_text;
  uvcpp::uvcpp_http_response response;
  std::string alpn;
};

nlohmann::json base_result(const HttpProbeInput& input, const ParsedUrl& url,
                           const std::chrono::steady_clock::time_point& start) {
  nlohmann::json result;
  result["ok"] = false;
  result["request"] = {
      {"method", input.method},
      {"url", input.url},
      {"protocolPreference", input.protocol},
      {"host", url.host},
      {"port", url.port},
      {"path", url_path_with_query(url)}};
  result["startedAt"] = now_iso8601();
  (void)start;
  return result;
}

}  // namespace

nlohmann::json run_http_probe(const HttpProbeInput& input) {
  const auto started = std::chrono::steady_clock::now();
  ParsedUrl url = parse_url(input.url);
  nlohmann::json result = base_result(input, url, started);
  if (!url.valid) {
    result["error"] = url.error.empty() ? "invalid URL" : url.error;
    result["timing"] = {{"totalMs", 0}, {"connectMs", 0}};
    return result;
  }

  const bool tls = url.scheme == "https";
  const std::string protocol = lower(input.protocol.empty() ? "auto" : input.protocol);
  if (protocol != "auto" && protocol != "http1" && protocol != "http2" && protocol != "http3") {
    result["error"] = "unknown protocol preference: " + input.protocol;
    return result;
  }
#if !UVCPP_NGHTTP2_ENABLE
  if (protocol == "http2") {
    result["error"] = "this libuvcpp build has no HTTP/2 support";
    return result;
  }
#endif
#if !UVCPP_HTTP3_ENABLE
  if (protocol == "http3") {
    result["error"] = "this libuvcpp build has no HTTP/3 support";
    return result;
  }
#endif
  if (protocol == "http3") {
    if (!tls) {
      result["error"] = "HTTP/3 requires an https:// URL";
      return result;
    }
  }

  uvcpp::uvcpp_http_request req;
  req.method = method_from_string(input.method);
  req.url = url_path_with_query(url);
  bool has_host = false;
  bool has_user_agent = false;
  for (const auto& header : input.headers) {
    const std::string name = lower(trim(header.name));
    if (name.empty()) continue;
    if (name == "content-length" || name == "connection" || name == "transfer-encoding") continue;
    if (name == "host") has_host = true;
    if (name == "user-agent") has_user_agent = true;
    req.set_header(header.name, header.value);
  }
  if (!has_host) req.set_header("host", url.port == (tls ? 443 : 80) ? url.host : url.host + ":" + std::to_string(url.port));
  if (!has_user_agent) req.set_header("user-agent", "uvscope/" UVSCOPE_VERSION);
  req.body.clone_data(input.body.data(), input.body.size());

  std::unique_ptr<uvcpp::uvcpp_ssl_context> ssl;
  if (tls) {
    ssl.reset(new uvcpp::uvcpp_ssl_context(uvcpp::tls_mode::CLIENT, uvcpp::tls_version::TLS_1_2));
    const std::string ca = find_ca_bundle(executable_dir(nullptr));
    if (!ca.empty()) ssl->load_ca_file(ca);
    ssl->set_verify_mode(input.insecure ? uvcpp::tls_verify_mode::NONE
                                        : uvcpp::tls_verify_mode::PEER_STRICT);
  }

  uvcpp::uvcpp_http_client client;
  if (ssl) client.set_ssl_context(ssl.get());
#if UVCPP_ZLIB_ENABLE
  client.set_compression_enabled(input.compression);
#endif

  const int timeout_ms = std::max(1000, std::min(input.timeout_ms, 120000));
  int rc = 0;
  uvcpp::uvcpp_http_response response;
  std::string alpn;
  const bool async_protocol = protocol == "http2" || protocol == "http3";

  const auto connect_started = std::chrono::steady_clock::now();
  if (!async_protocol) {
    rc = client.connect_wait(url.host.c_str(), url.port, timeout_ms);
    const auto connect_finished = std::chrono::steady_clock::now();
    const auto connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                connect_finished - connect_started)
                                .count();
    if (rc != 0) {
      result["error"] = "connect failed: " + uv_error(rc);
      result["timing"] = {{"totalMs", connect_ms}, {"connectMs", connect_ms}};
      return result;
    }
    rc = ssl ? client.send_wait_ssl(req, response, timeout_ms)
             : client.send_wait_plain(req, response, timeout_ms);
    if (rc != 0) {
      result["error"] = "request failed: " + uv_error(rc);
      result["timing"] = {{"totalMs", connect_ms}, {"connectMs", connect_ms}};
      return result;
    }
    result["timing"] = {{"connectMs", connect_ms},
                        {"totalMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started)
                                        .count()}};
  } else {
    AsyncState state;
#if UVCPP_NGHTTP2_ENABLE
    if (protocol == "http2") client.set_http2_enabled(true);
#endif
#if UVCPP_HTTP3_ENABLE
    if (protocol == "http3") client.set_http3_enabled(true);
#endif
    const int connect_rc = client.connect(url.host.c_str(), url.port, [&](int status) {
      if (status != 0) {
        state.error = status;
        state.error_text = "connect failed: " + uv_error(status);
        state.done = true;
        client.stop();
        return;
      }
      const int send_rc = client.send(req, [&](const uvcpp::uvcpp_http_response& resp, int err) {
        state.response = resp;
        state.error = err;
        if (err != 0) state.error_text = "request failed: " + uv_error(err);
#if UVCPP_NGHTTP2_ENABLE || UVCPP_HTTP3_ENABLE
        state.alpn = client.negotiated_alpn();
#endif
        state.done = true;
        client.stop();
      });
      if (send_rc != 0) {
        state.error = send_rc;
        state.error_text = "request failed: " + uv_error(send_rc);
        state.done = true;
        client.stop();
      }
    });
    if (connect_rc != 0) {
      result["error"] = "could not start connection: " + uv_error(connect_rc);
      result["timing"] = {{"totalMs", 0}, {"connectMs", 0}};
      return result;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!state.done && std::chrono::steady_clock::now() < deadline) {
      client.run(UV_RUN_NOWAIT);
      if (!state.done) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!state.done) {
      client.stop();
      result["error"] = "request timed out after " + std::to_string(timeout_ms) + " ms";
      result["timing"] = {{"connectMs", 0}, {"totalMs", timeout_ms}};
      return result;
    }
    if (state.error != 0) {
      result["error"] = state.error_text.empty() ? uv_error(state.error) : state.error_text;
      result["timing"] = {{"connectMs", 0},
                          {"totalMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - started)
                                          .count()}};
      return result;
    }
    response = state.response;
    alpn = state.alpn;
    const auto connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - connect_started)
                                .count();
    result["timing"] = {{"connectMs", connect_ms},
                        {"totalMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started)
                                        .count()}};
  }

  std::string body = response.body.to_string();
  const std::size_t full_size = body.size();
  bool truncated = false;
  body = clip_body(body, input.max_body_bytes, &truncated);
  const bool text = is_valid_utf8(body);
  result["ok"] = true;
  result["status"] = static_cast<int>(response.status_code);
  result["statusText"] = response.status_message;
  result["httpVersion"] = response_version(response);
  result["protocol"] = response_version(response);
  result["alpn"] = alpn;
  result["headers"] = headers_json(response.headers);
  result["bodySize"] = full_size;
  result["bodyTruncated"] = truncated;
  result["bodyEncoding"] = text ? "utf8" : "base64";
  result["bodyText"] = text ? body : std::string();
  result["bodyBase64"] = text ? std::string() : base64_encode(body);
  result["finishedAt"] = now_iso8601();
  return result;
}

std::string http_probe_summary(const nlohmann::json& result) {
  if (!result.value("ok", false)) {
    return result.value("request", nlohmann::json::object()).value("method", std::string("HTTP")) +
           " " + result.value("request", nlohmann::json::object()).value("url", std::string()) +
           " — " + result.value("error", std::string("failed"));
  }
  return result.value("request", nlohmann::json::object()).value("method", std::string("GET")) + " " +
         result.value("request", nlohmann::json::object()).value("url", std::string()) + " → " +
         std::to_string(result.value("status", 0)) + " " +
         result.value("httpVersion", std::string());
}

}  // namespace uvscope
