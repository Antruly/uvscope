#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace uvscope {

struct QuicProbeInput {
  std::string host;
  int port = 443;
  std::string alpn = "h3";
  std::string payload;
  std::string encoding = "text";  // text | hex
  int timeout_ms = 10000;
  // The QUIC client defaults to not verifying certificates; this is a debugging
  // tool and most test servers use a self-signed certificate.
  bool verify = false;
};

struct QuicServerConfig {
  std::string id;
  std::string name;
  std::string host = "127.0.0.1";
  int port = 4433;
  std::string alpn = "h3";
  std::string cert_path;
  std::string key_path;
  bool echo = true;
  bool auto_start = false;
};

// QUIC client probe and server listener. The whole module is experimental:
// libuvcpp needs a QUIC-enabled build (OpenSSL 3.2+) and release builds keep it
// off. Without that build every entry point reports `unsupported`.
class QuicManager {
 public:
  QuicManager();
  ~QuicManager();

  QuicManager(const QuicManager&) = delete;
  QuicManager& operator=(const QuicManager&) = delete;

  struct Impl;

  static bool compiled();
  static std::string unavailable_reason();

  nlohmann::json probe(const QuicProbeInput& input) const;

  nlohmann::json add_server(const QuicServerConfig& config, std::string* error);
  bool start_server(const std::string& id, std::string* error);
  bool stop_server(const std::string& id, std::string* error);
  bool remove_server(const std::string& id, std::string* error);
  nlohmann::json servers() const;
  nlohmann::json server_events(const std::string& id, std::uint64_t after,
                               int limit) const;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace uvscope
