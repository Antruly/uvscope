#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace uvscope {

// Supervises long-lived in-process services (currently reverse proxies).
// Start/stop work is dispatched onto the daemon loop; the watchdog runs on its
// own thread and only reads the managers' atomic running state. Process
// isolation is not part of this layer yet.
class Supervisor {
 public:
  using Dispatch = std::function<void(std::function<void()>)>;

  struct Service {
    std::string id;
    std::string kind;
    std::string name;
    bool desired = false;
    bool pending_restart = false;
    int restarts = 0;
    std::string last_error;
    std::string updated_at;
    std::chrono::steady_clock::time_point next_attempt{};
    std::function<bool(std::string*)> start;
    std::function<bool(std::string*)> stop;
    std::function<bool()> running;
  };

  explicit Supervisor(Dispatch dispatch);
  ~Supervisor();

  Supervisor(const Supervisor&) = delete;
  Supervisor& operator=(const Supervisor&) = delete;

  void register_service(const std::string& kind, const std::string& id,
                        const std::string& name, bool desired,
                        std::function<bool(std::string*)> start,
                        std::function<bool(std::string*)> stop,
                        std::function<bool()> running);
  void unregister_service(const std::string& id);
  void set_desired(const std::string& id, bool desired);
  // Stop then start on the daemon loop. Returns false when the service is unknown.
  bool restart(const std::string& id);
  // Stop the runtime without clearing `desired`; the watchdog restarts it.
  bool stop_runtime(const std::string& id);

  void stop();

  nlohmann::json snapshot() const;
  nlohmann::json events(std::uint64_t after, int limit) const;

 private:
  void loop();
  void add_event(const std::string& type, const std::string& id,
                 const std::string& name, const std::string& message = {});

  Dispatch dispatch_;
  mutable std::mutex mutex_;
  std::map<std::string, Service> services_;
  std::vector<nlohmann::json> events_;
  std::uint64_t next_event_ = 1;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  mutable std::mutex wait_mutex_;
  std::condition_variable wait_cv_;
};

}  // namespace uvscope
