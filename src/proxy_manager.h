#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "store.h"

namespace uvcpp {
class uvcpp_web_app;
class uvcpp_web_request;
class uvcpp_web_response;
}  // namespace uvcpp

namespace uvscope {

class ProxyChildService;

class ProxyManager {
 public:
  explicit ProxyManager(Store& store, std::string executable_path = "",
                        bool auto_start = true);
  ~ProxyManager();

  bool start(const std::string& id, std::string* error);
  // Start/reload one runtime from an in-memory config. The child proxy
  // process uses these instead of a SQLite-backed Store.
  bool start_config(const ProxyConfig& config, std::string* error);
  bool reload_config(const ProxyConfig& config, std::string* error);
  bool stop(const std::string& id, std::string* error);
  void stop_all();
  bool reload(const ProxyConfig& config, std::string* error);
  bool is_running(const std::string& id) const;

  nlohmann::json snapshot() const;
  nlohmann::json logs(const std::string& id, std::uint64_t after, int limit) const;
  std::uint64_t total_requests() const;
  std::uint64_t total_errors() const;
  std::size_t active_count() const;

 private:
  struct Runtime;
  std::shared_ptr<Runtime> find(const std::string& id) const;
  std::shared_ptr<ProxyChildService> find_child(const std::string& id) const;
  void auto_start();

  Store& store_;
  std::string executable_path_;
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Runtime>> runtimes_;
  std::map<std::string, std::shared_ptr<ProxyChildService>> children_;
  std::atomic<std::uint64_t> total_requests_{0};
  std::atomic<std::uint64_t> total_errors_{0};
};

}  // namespace uvscope
