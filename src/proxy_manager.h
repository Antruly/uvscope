#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
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

class ProxyManager {
 public:
  explicit ProxyManager(Store& store);
  ~ProxyManager();

  bool start(const std::string& id, std::string* error);
  bool stop(const std::string& id, std::string* error);
  void stop_all();
  bool reload(const ProxyConfig& config, std::string* error);

  nlohmann::json snapshot() const;
  nlohmann::json logs(const std::string& id, std::uint64_t after, int limit) const;
  std::uint64_t total_requests() const;
  std::uint64_t total_errors() const;
  std::size_t active_count() const;

 private:
  struct Runtime;
  std::shared_ptr<Runtime> find(const std::string& id) const;
  void auto_start();

  Store& store_;
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Runtime>> runtimes_;
  std::atomic<std::uint64_t> total_requests_{0};
  std::atomic<std::uint64_t> total_errors_{0};
};

}  // namespace uvscope
