#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "store.h"

namespace uvscope {

// Parent-side handle for one proxy running in a child process. It owns the
// IPC loop thread, the child process, the latest telemetry snapshot and a
// bounded cached log window.
class ProxyChildService {
 public:
  ProxyChildService(std::string executable, ProxyConfig config);
  ~ProxyChildService();

  ProxyChildService(const ProxyChildService&) = delete;
  ProxyChildService& operator=(const ProxyChildService&) = delete;

  bool start(std::string* error);
  void stop();
  bool reload(const ProxyConfig& config, std::string* error);

  bool running() const;
  nlohmann::json snapshot() const;
  nlohmann::json logs(std::uint64_t after, int limit) const;
  ProxyConfig config_snapshot() const;
  std::uint64_t config_version() const;
  int pid() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Child mode: connect to the parent IPC endpoint, run one proxy runtime from
// commands, and exit when the parent stops it or disconnects.
int run_proxy_child(const std::string& endpoint);

// Parent-side self-check: spawn --proxy-child, start a proxy that targets
// 127.0.0.1:<target_port>, request /hello through it, take a snapshot and stop
// it. Used by --self-check on every platform.
bool run_proxy_child_self_check(const std::string& executable, int target_port,
                                std::string* error);

bool proxy_config_from_json(const nlohmann::json& json, ProxyConfig* config,
                            std::string* error);
nlohmann::json proxy_config_to_json(const ProxyConfig& config);

}  // namespace uvscope
