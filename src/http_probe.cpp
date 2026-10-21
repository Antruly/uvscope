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
#if UVCPP_ZLIB_ENABLE || UVCPP_BROTLI_ENABLE
#include <web/uvcpp_http_compress.h>
#endif
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

struct DecompressionInfo {
  bool requested = false;
  bool decoded = false;
  std::string encoding;
  std::string error;
  std::size_t wire_size = 0;
};

// Decode the response here rather than through
// `uvcpp_http_client::set_compression_enabled()`: that path strips
// content-encoding and content-length, so the probe could no longer report the
// negotiated encoding or the wire size. The body stays compressed unless this
// function succeeds.
DecompressionInfo decode_response(uvcpp::uvcpp_http_response& response, bool requested) {
  DecompressionInfo info;
  info.requested = requested;
  if (!requested) return info;
  const std::string encoding = lower(
      trim(uvcpp::http_get_header(response.headers, "content-encoding")));
  if (encoding.empty() || encoding == "identity") return info;
  info.encoding = encoding;
  info.wire_size = response.body.size();
#if UVCPP_ZLIB_ENABLE || UVCPP_BROTLI_ENABLE
  const bool supported =
      encoding == "gzip" || encoding == "x-gzip" || encoding == "deflate" ||
      encoding == "x-deflate"
#if UVCPP_BROTLI_ENABLE
      || encoding == "br"
#endif
      ;
  if (!supported) {
    info.error = "unsupported content-encoding: " + encoding;
    return info;
  }
  const auto result = uvcpp::http_compress::decompress(
      response.body.get_const_data(), response.body.size());
  if (!result.success) {
    info.error = "could not decode " + encoding + " body";
    return info;
  }
  response.body.clear();
  response.body.clone(result.data);
  response.remove_header("content-encoding");
  response.remove_header("content-length");
  info.decoded = true;
#else
  info.error = "this build has no decompression support";
#endif
  return info;
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
  std::string proxy_detail;
  bool proxy_connect_failed = false;
  uvcpp::uvcpp_http_response response;
  std::string alpn;
  bool h2_active = false;
  std::size_t h2_frame_count = 0;
  bool h2_frames_truncated = false;
  nlohmann::json h2_frames = nlohmann::json::array();
};

// One h2 frame summary is a few hundred bytes; capping keeps a busy stream from
// turning a debug response into a capture file.
constexpr std::size_t kMaxH2Frames = 200;

#if UVCPP_NGHTTP2_ENABLE
const char* h2_frame_type_name(int type) {
  switch (type) {
    case uvcpp::H2_FRAME_DATA: return "DATA";
    case uvcpp::H2_FRAME_HEADERS: return "HEADERS";
    case uvcpp::H2_FRAME_PRIORITY: return "PRIORITY";
    case uvcpp::H2_FRAME_RST_STREAM: return "RST_STREAM";
    case uvcpp::H2_FRAME_SETTINGS: return "SETTINGS";
    case uvcpp::H2_FRAME_PUSH_PROMISE: return "PUSH_PROMISE";
    case uvcpp::H2_FRAME_PING: return "PING";
    case uvcpp::H2_FRAME_GOAWAY: return "GOAWAY";
    case uvcpp::H2_FRAME_WINDOW_UPDATE: return "WINDOW_UPDATE";
    case uvcpp::H2_FRAME_CONTINUATION: return "CONTINUATION";
    default: return "UNKNOWN";
  }
}
#endif

void attach_h2_frames(nlohmann::json& result, const AsyncState& state) {
  if (!state.h2_active) return;
  result["h2"] = {{"frameCount", state.h2_frame_count},
                  {"framesTruncated", state.h2_frames_truncated},
                  {"maxFrames", kMaxH2Frames},
                  {"frames", state.h2_frames}};
}

void add_protocol_metadata(nlohmann::json& result,
                           const uvcpp::uvcpp_http_response& response,
                           const uvcpp::uvcpp_http_client& client,
                           bool include_goaway) {
  // HTTP/1.1 leaves these at 0 / false; HTTP/2 and HTTP/3 populate them.
  result["streamId"] = response.stream_id;
  result["retryable"] = response.retryable;
#if UVCPP_NGHTTP2_ENABLE
  if (include_goaway) {
    result["goaway"] = {{"received", client.peer_goaway_received()},
                        {"errorCode", client.peer_goaway_error_code()},
                        {"lastStreamId", client.peer_goaway_last_stream_id()}};
  }
#else
  (void)client;
  (void)include_goaway;
#endif
}

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
  bool has_accept_encoding = false;
  for (const auto& header : input.headers) {
    const std::string name = lower(trim(header.name));
    if (name.empty()) continue;
    if (name == "content-length" || name == "connection" || name == "transfer-encoding") continue;
    if (name == "host") has_host = true;
    if (name == "user-agent") has_user_agent = true;
    if (name == "accept-encoding") has_accept_encoding = true;
    req.set_header(header.name, header.value);
  }
  if (!has_host) req.set_header("host", url.port == (tls ? 443 : 80) ? url.host : url.host + ":" + std::to_string(url.port));
  if (!has_user_agent) {
    // Many sites serve a JS/meta redirect shim (or a bot page) to clients whose
    // UA does not look like a browser; with scripts disabled the preview would
    // be blank. Present a current browser UA and keep uvscope's own token at
    // the end so server logs can still identify the client. An explicit
    // User-Agent header from the request editor always wins.
#if defined(_WIN32)
    constexpr const char* kBrowserUa =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
#elif defined(__APPLE__)
    constexpr const char* kBrowserUa =
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
#else
    constexpr const char* kBrowserUa =
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
#endif
    req.set_header("user-agent", std::string(kBrowserUa) + " uvscope/" UVSCOPE_VERSION);
  }
#if UVCPP_ZLIB_ENABLE || UVCPP_BROTLI_ENABLE
  if (input.decompress && !has_accept_encoding) {
#if UVCPP_BROTLI_ENABLE
    req.set_header("accept-encoding", "gzip, deflate, br");
#else
    req.set_header("accept-encoding", "gzip, deflate");
#endif
  }
#endif
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
  if (input.proxy.enabled()) {
    std::string proxy_error;
    if (!input.proxy.valid(&proxy_error)) {
      result["proxy"] = socks5_failure_json(input.proxy, UV_EINVAL, proxy_error, false);
      result["proxy"]["errorClass"] = "invalid_config";
      result["error"] = proxy_error;
      return result;
    }
    if (protocol == "http3") {
      result["proxy"] = socks5_proxy_json(input.proxy);
      result["proxy"]["errorClass"] = "unsupported";
      result["error"] = "SOCKS5 proxies are not supported with HTTP/3";
      return result;
    }
    const int proxy_rc = client.set_socks5_proxy(
        input.proxy.host, input.proxy.port, input.proxy.username, input.proxy.password);
    if (proxy_rc != 0) {
      result["proxy"] = socks5_failure_json(input.proxy, proxy_rc, "", false);
      result["proxy"]["errorClass"] = "invalid_config";
      result["error"] = "invalid SOCKS5 proxy: " + uv_error(proxy_rc);
      return result;
    }
    result["proxy"] = socks5_proxy_json(input.proxy);
  }

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
      if (input.proxy.enabled()) {
        const std::string detail = client.socks5_last_error();
        result["proxy"] = socks5_failure_json(input.proxy, rc, detail,
                                              client.socks5_proxy_connect_failed());
        result["error"] = "connect failed: " + (detail.empty() ? uv_error(rc) : detail);
      } else {
        result["error"] = "connect failed: " + uv_error(rc);
      }
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
    add_protocol_metadata(result, response, client, protocol != "http3");
    result["timing"] = {{"connectMs", connect_ms},
                        {"totalMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started)
                                        .count()}};
  } else {
    AsyncState state;
#if UVCPP_NGHTTP2_ENABLE
    if (protocol == "http2") {
      state.h2_active = true;
      client.set_http2_enabled(true);
      client.set_h2_frame_callback([&state](const uvcpp::uvcpp_h2_frame_event& frame) {
        ++state.h2_frame_count;
        if (state.h2_frames.size() >= kMaxH2Frames) {
          state.h2_frames_truncated = true;
          return;
        }
        state.h2_frames.push_back({{"type", h2_frame_type_name(frame.type)},
                                   {"typeId", frame.type},
                                   {"flags", frame.flags},
                                   {"streamId", frame.stream_id},
                                   {"length", frame.length},
                                   {"errorCode", frame.error_code},
                                   {"lastStreamId", frame.last_stream_id}});
      });
    }
#endif
#if UVCPP_HTTP3_ENABLE
    if (protocol == "http3") client.set_http3_enabled(true);
#endif
    const int connect_rc = client.connect(url.host.c_str(), url.port, [&](int status) {
      if (status != 0) {
        state.error = status;
        state.proxy_detail = client.socks5_last_error();
        state.proxy_connect_failed = client.socks5_proxy_connect_failed();
        state.error_text = "connect failed: " +
                           (state.proxy_detail.empty() ? uv_error(status)
                                                       : state.proxy_detail);
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
      attach_h2_frames(result, state);
      if (input.proxy.enabled()) {
        result["proxy"] = socks5_failure_json(input.proxy, UV_ETIMEDOUT, "timed out", false);
      }
      result["error"] = "request timed out after " + std::to_string(timeout_ms) + " ms";
      result["timing"] = {{"connectMs", 0}, {"totalMs", timeout_ms}};
      return result;
    }
    if (state.error != 0) {
      attach_h2_frames(result, state);
      if (input.proxy.enabled()) {
        result["proxy"] = socks5_failure_json(input.proxy, state.error,
                                              state.proxy_detail,
                                              state.proxy_connect_failed);
      }
      result["error"] = state.error_text.empty() ? uv_error(state.error) : state.error_text;
      result["timing"] = {{"connectMs", 0},
                          {"totalMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - started)
                                          .count()}};
      return result;
    }
    response = state.response;
    alpn = state.alpn;
    add_protocol_metadata(result, response, client, protocol != "http3");
    attach_h2_frames(result, state);
    const auto connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - connect_started)
                                .count();
    result["timing"] = {{"connectMs", connect_ms},
                        {"totalMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started)
                                        .count()}};
  }

  const DecompressionInfo decompression = decode_response(response, input.decompress);
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
  if (decompression.requested) {
    nlohmann::json compression = {{"requested", true},
                                  {"contentEncoding", decompression.encoding},
                                  {"decoded", decompression.decoded},
                                  {"bodySize", full_size}};
    if (!decompression.encoding.empty()) {
      compression["wireBodySize"] = decompression.wire_size;
    }
    if (!decompression.error.empty()) compression["error"] = decompression.error;
    result["compression"] = std::move(compression);
  }
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
