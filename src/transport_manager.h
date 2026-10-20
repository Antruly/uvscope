#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "store.h"

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

  // TCP/UDP listeners. Configs are persisted by Store; the manager owns the
  // runtime and exposes snapshots/events for the UI.
  bool add_listener(const ListenerConfig& config, std::string* error);
  bool start_listener(const std::string& id, std::string* error);
  bool stop_listener(const std::string& id, std::string* error);
  bool remove_listener(const std::string& id, std::string* error);
  nlohmann::json listeners() const;
  nlohmann::json listener(const std::string& id) const;
  nlohmann::json listener_events(const std::string& id, std::uint64_t after,
                                 int limit) const;
  nlohmann::json listener_sessions(const std::string& id) const;
  nlohmann::json listener_session_events(const std::string& id,
                                         const std::string& session_id,
                                         std::uint64_t after, int limit) const;
  bool listener_send(const std::string& id, const std::string& session_id,
                     const std::string& peer_host, int peer_port,
                     const std::string& payload, const std::string& encoding,
                     std::string* error);
  bool listener_close_session(const std::string& id,
                              const std::string& session_id,
                              std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace uvscope
