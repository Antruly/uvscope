#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "socks5_proxy.h"

namespace uvscope {

struct WsConnectInput {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string subprotocol;
  bool insecure = false;
  Socks5Proxy proxy;
  int timeout_ms = 15000;
};

// One dedicated thread + loop per session. libuvcpp's ws client owns its own
// loop, so running each session on its own thread keeps frame events, pings
// and close codes independent of the web control plane.
class WsManager {
 public:
  WsManager();
  ~WsManager();

  WsManager(const WsManager&) = delete;
  WsManager& operator=(const WsManager&) = delete;

  struct Impl;

  bool available() const;
  const std::string& start_error() const;

  std::string create(std::int64_t owner_id, const std::string& owner_name,
                     const WsConnectInput& input, std::string* error);
  bool send(const std::string& id, std::int64_t owner_id, bool is_admin,
            const std::string& type, const std::string& payload,
            std::string* error);
  bool close(const std::string& id, std::int64_t owner_id, bool is_admin,
             int code, const std::string& reason, std::string* error);
  bool remove(const std::string& id, std::int64_t owner_id, bool is_admin,
              std::string* error);
  bool owner_of(const std::string& id, std::int64_t* owner_id) const;

  nlohmann::json sessions(std::int64_t owner_id, bool all) const;
  bool session(const std::string& id, std::int64_t owner_id, bool is_admin,
               nlohmann::json* out) const;
  bool events(const std::string& id, std::int64_t owner_id, bool is_admin,
              std::uint64_t after, std::size_t limit, nlohmann::json* out) const;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace uvscope
