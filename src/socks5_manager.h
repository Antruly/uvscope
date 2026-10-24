#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "store.h"

namespace uvscope {

// SOCKS5 server runtimes. Each running server owns its own libuv loop and
// thread (uvcpp_socks5_server keeps its loop private), so a slow handshake or
// an idle session can never stall the control plane. Web handlers only enqueue
// start/stop commands and read snapshots.
class Socks5Manager {
 public:
  explicit Socks5Manager(std::string executable_path = "");
  ~Socks5Manager();

  Socks5Manager(const Socks5Manager&) = delete;
  Socks5Manager& operator=(const Socks5Manager&) = delete;

  // Configs are persisted by Store; the manager owns the runtime. Methods are
  // thread-safe. `start_server` blocks until the listener is bound or failed.
  bool add_server(const Socks5ServerConfig& config, std::string* error);
  bool start_server(const std::string& id, std::string* error);
  bool stop_server(const std::string& id, std::string* error);
  bool remove_server(const std::string& id, std::string* error);
  nlohmann::json servers() const;
  nlohmann::json server(const std::string& id) const;
  nlohmann::json server_events(const std::string& id, std::uint64_t after,
                               int limit) const;
  nlohmann::json server_sessions(const std::string& id) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace uvscope
