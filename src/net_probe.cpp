#include "net_probe.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <uv.h>
#include <handle/uvcpp_loop.h>
#include <net/uvcpp_socks5_client.h>
#include <net/uvcpp_socks5_udp.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_udp_client.h>
#include <uvcpp/uvcpp_buf.h>

#include "util.h"

namespace uvscope {
namespace {

std::string uv_error(int code) {
  return uv_error_string(code);
}

bool decode_payload(const std::string& payload, const std::string& encoding,
                    std::string* bytes, std::string* error) {
  if (lower(encoding) == "hex") {
    const auto decoded = hex_decode(payload);
    if (!decoded) {
      if (error) *error = "payload is not valid hex";
      return false;
    }
    *bytes = *decoded;
  } else {
    *bytes = payload;
  }
  return true;
}

nlohmann::json payload_json(const std::string& bytes) {
  const bool printable = std::all_of(bytes.begin(), bytes.end(), [](unsigned char c) {
    return c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c < 0x7F);
  });
  nlohmann::json out;
  out["length"] = bytes.size();
  out["hex"] = hex_encode(bytes, true);
  out["encoding"] = printable ? "utf8" : "base64";
  out["text"] = printable ? bytes : std::string();
  out["base64"] = printable ? std::string() : base64_encode(bytes);
  return out;
}

}  // namespace

nlohmann::json run_tcp_probe(const TcpProbeInput& input) {
  nlohmann::json result;
  result["ok"] = false;
  result["transport"] = "tcp";
  result["host"] = input.host;
  result["port"] = input.port;
  if (input.host.empty() || input.port <= 0 || input.port > 65535) {
    result["error"] = "invalid host or port";
    return result;
  }
  std::string bytes;
  std::string decode_error;
  if (!decode_payload(input.payload, input.encoding, &bytes, &decode_error)) {
    result["error"] = decode_error;
    return result;
  }

  const auto started = std::chrono::steady_clock::now();
  uvcpp::uvcpp_tcp_client client;
  if (input.proxy.enabled()) {
    std::string proxy_error;
    if (!input.proxy.valid(&proxy_error)) {
      result["proxy"] = socks5_failure_json(input.proxy, UV_EINVAL, proxy_error, false);
      result["proxy"]["errorClass"] = "invalid_config";
      result["error"] = proxy_error;
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
  const int connect_rc = client.connect_wait(input.host.c_str(), input.port,
                                              std::max(200, input.connect_timeout_ms));
  const auto connected = std::chrono::steady_clock::now();
  if (connect_rc != 0) {
    if (input.proxy.enabled()) {
      const std::string detail = client.socks5_last_error();
      result["proxy"] = socks5_failure_json(input.proxy, connect_rc, detail,
                                            client.socks5_proxy_connect_failed());
      result["error"] = "connect failed: " + (detail.empty() ? uv_error(connect_rc) : detail);
    } else {
      result["error"] = "connect failed: " + uv_error(connect_rc);
    }
    result["timing"] = {{"totalMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                      connected - started)
                                      .count()}};
    return result;
  }
  result["connected"] = true;
  if (!bytes.empty()) {
    const int write_rc = client.write_wait(bytes.data(), bytes.size(),
                                           std::max(500, input.connect_timeout_ms));
    if (write_rc != 0) {
      result["error"] = "write failed: " + uv_error(write_rc);
      client.close();
      return result;
    }
    result["sent"] = payload_json(bytes);
  }
  uvcpp::uvcpp_buf received;
  int read_rc = UV_ETIMEDOUT;
  if (input.read_timeout_ms > 0) {
    read_rc = client.read_wait(received, input.read_timeout_ms);
  }
  std::string response;
  if (received.size() > 0) {
    response.assign(received.get_const_data(), received.size());
    result["received"] = payload_json(response);
  } else {
    result["received"] = {{"length", 0}, {"hex", ""}, {"encoding", "utf8"}, {"text", ""}, {"base64", ""}};
  }
  if (read_rc != 0 && read_rc != UV_ETIMEDOUT && read_rc != UV_EOF && read_rc != UV_ECANCELED) {
    result["error"] = "read failed: " + uv_error(read_rc);
  } else {
    result["ok"] = true;
    if (read_rc == UV_ETIMEDOUT) result["readTimedOut"] = true;
  }
  if (input.close_after) client.close();
  result["timing"] = {
      {"connectMs", std::chrono::duration_cast<std::chrono::milliseconds>(connected - started).count()},
      {"totalMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count()}};
  return result;
}

nlohmann::json run_udp_probe(const UdpProbeInput& input) {
  nlohmann::json result;
  result["ok"] = false;
  result["transport"] = "udp";
  result["host"] = input.host;
  result["port"] = input.port;
  if (input.host.empty() || input.port <= 0 || input.port > 65535) {
    result["error"] = "invalid host or port";
    return result;
  }
  std::string bytes;
  std::string decode_error;
  if (!decode_payload(input.payload, input.encoding, &bytes, &decode_error)) {
    result["error"] = decode_error;
    return result;
  }

  const auto started = std::chrono::steady_clock::now();
  if (input.proxy.enabled()) {
    std::string proxy_error;
    if (!input.proxy.valid(&proxy_error)) {
      result["proxy"] = socks5_failure_json(input.proxy, UV_EINVAL, proxy_error, false);
      result["proxy"]["errorClass"] = "invalid_config";
      result["error"] = proxy_error;
      return result;
    }
    const int timeout = std::max(200, input.timeout_ms);
    uvcpp::uvcpp_socks5_client client;
    client.set_connect_timeout_ms(timeout);
    if (!input.proxy.username.empty()) {
      client.set_credentials(input.proxy.username, input.proxy.password);
    }
    int rc = client.connect_wait(input.proxy.host, input.proxy.port, timeout);
    if (rc != 0) {
      const std::string detail = client.last_error();
      result["proxy"] = socks5_failure_json(input.proxy, rc, detail, true);
      result["error"] = "proxy handshake failed: " +
                        (detail.empty() ? uv_error(rc) : detail);
      result["timing"] = {{"totalMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started)
                                        .count()}};
      return result;
    }
    uvcpp::uvcpp_socks5_endpoint relay;
    rc = client.udp_associate_wait(relay, timeout);
    if (rc != 0) {
      const std::string detail = client.last_error();
      result["proxy"] = socks5_failure_json(input.proxy, rc, detail, false);
      result["error"] = "UDP ASSOCIATE failed: " +
                        (detail.empty() ? uv_error(rc) : detail);
      result["timing"] = {{"totalMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started)
                                        .count()}};
      client.close();
      return result;
    }
    result["proxy"] = socks5_proxy_json(input.proxy);
    result["proxy"]["relay"] = relay.host + ":" + std::to_string(relay.port);

    auto* udp = client.udp();
    if (udp == nullptr) {
      result["error"] = "UDP relay is unavailable";
      client.close();
      return result;
    }
    std::string response;
    std::string response_from;
    bool received_any = false;
    udp->recv_start([&](int, const uvcpp::uvcpp_socks5_endpoint& from,
                        const void* data, std::size_t len) {
      if (received_any) return;
      received_any = true;
      response_from = from.host + ":" + std::to_string(from.port);
      response.assign(static_cast<const char*>(data), len);
    });
    rc = udp->send_to_wait(input.host, input.port, bytes.data(), bytes.size(), timeout);
    if (rc != 0) {
      const std::string detail = client.last_error();
      result["error"] = "send failed: " + (detail.empty() ? uv_error(rc) : detail);
      client.close();
      return result;
    }
    result["sent"] = payload_json(bytes);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout);
    while (!received_any && std::chrono::steady_clock::now() < deadline) {
      client.get_loop()->run(UV_RUN_NOWAIT);
      if (!received_any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (received_any) {
      result["received"] = payload_json(response);
      result["receivedFrom"] = response_from;
      result["ok"] = true;
    } else {
      result["received"] = {{"length", 0}, {"hex", ""}, {"encoding", "utf8"},
                            {"text", ""}, {"base64", ""}};
      result["ok"] = true;
      result["readTimedOut"] = true;
    }
    result["timing"] = {{"totalMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count()}};
    client.close();
    return result;
  }
  uvcpp::uvcpp_udp_client client;
  int rc = client.bind("0.0.0.0", 0);
  if (rc != 0) {
    result["error"] = "bind failed: " + uv_error(rc);
    return result;
  }
  rc = client.send_wait(input.host.c_str(), input.port, bytes.data(), bytes.size(),
                        std::max(200, input.timeout_ms));
  if (rc != 0) {
    result["error"] = "send failed: " + uv_error(rc);
    return result;
  }
  result["sent"] = payload_json(bytes);
  uvcpp::uvcpp_buf received;
  const int recv_rc = client.recv_wait(received, std::max(100, input.timeout_ms));
  if (received.size() > 0) {
    const std::string response(received.get_const_data(), received.size());
    result["received"] = payload_json(response);
  } else {
    result["received"] = {{"length", 0}, {"hex", ""}, {"encoding", "utf8"}, {"text", ""}, {"base64", ""}};
  }
  if (recv_rc == 0) {
    result["ok"] = true;
  } else if (recv_rc == UV_ETIMEDOUT) {
    result["ok"] = true;
    result["readTimedOut"] = true;
  } else {
    result["error"] = "receive failed: " + uv_error(recv_rc);
  }
  result["timing"] = {{"totalMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count()}};
  return result;
}

}  // namespace uvscope
