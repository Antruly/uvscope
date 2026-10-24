#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "store.h"

namespace uvscope {

// Child mode: run one TCP/UDP listener from IPC commands.
int run_listener_child(const std::string& endpoint);

// Parent-side handle for one listener running in a child process.
class ListenerChildService {
 public:
  ListenerChildService(std::string executable, ListenerConfig config);
  ~ListenerChildService();

  ListenerChildService(const ListenerChildService&) = delete;
  ListenerChildService& operator=(const ListenerChildService&) = delete;

  bool start(std::string* error);
  void stop();

  bool running() const;
  nlohmann::json snapshot() const;
  nlohmann::json events(std::uint64_t after, int limit) const;
  nlohmann::json sessions() const;
  nlohmann::json session_events(const std::string& session_id,
                                std::uint64_t after, int limit,
                                std::string* error) const;
  bool send(const std::string& session_id, const std::string& peer_host,
            int peer_port, const std::string& payload,
            const std::string& encoding, std::string* error);
  bool close_session(const std::string& session_id, std::string* error);
  int pid() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

nlohmann::json listener_config_to_json(const ListenerConfig& config);
bool listener_config_from_json(const nlohmann::json& value, ListenerConfig* config,
                               std::string* error);

}  // namespace uvscope
