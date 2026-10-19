#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace uvscope {

// Long-lived transport sessions. The manager owns one libuv loop on its own
// thread; web handlers only enqueue commands and read snapshots, so a slow or
// stalled target can never block the control plane.
class TransportManager {
 public:
  TransportManager();
  ~TransportManager();

  TransportManager(const TransportManager&) = delete;
  TransportManager& operator=(const TransportManager&) = delete;

  bool available() const;
  const std::string& start_error() const;

  // All methods are thread-safe. `create_tcp_session` returns immediately with
  // state "connecting"; callers poll `tcp_session_events` for progress.
  nlohmann::json create_tcp_session(const std::string& host, int port,
                                    int connect_timeout_ms, std::string* error);
  nlohmann::json tcp_sessions() const;
  nlohmann::json tcp_session(const std::string& id) const;
  nlohmann::json tcp_session_events(const std::string& id, std::uint64_t after,
                                    int limit) const;
  bool tcp_send(const std::string& id, const std::string& payload,
                const std::string& encoding, std::string* error);
  bool tcp_close(const std::string& id, std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace uvscope
