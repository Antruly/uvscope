#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "store.h"

namespace uvscope {

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
