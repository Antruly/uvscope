#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "store.h"

namespace uvscope {

// Child mode: connect to the parent IPC endpoint, run one SOCKS5 server from
// commands, and exit when the parent stops it or disconnects.
int run_socks5_child(const std::string& endpoint);

// Parent-side handle for one SOCKS5 server running in a child process.
class Socks5ChildService {
 public:
  Socks5ChildService(std::string executable, Socks5ServerConfig config);
  ~Socks5ChildService();

  Socks5ChildService(const Socks5ChildService&) = delete;
  Socks5ChildService& operator=(const Socks5ChildService&) = delete;

  bool start(std::string* error);
  void stop();

  bool running() const;
  nlohmann::json snapshot() const;
  nlohmann::json events(std::uint64_t after, int limit) const;
  nlohmann::json sessions() const;
  int pid() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

nlohmann::json socks5_config_to_json(const Socks5ServerConfig& config);
bool socks5_config_from_json(const nlohmann::json& value, Socks5ServerConfig* config,
                             std::string* error);

}  // namespace uvscope
